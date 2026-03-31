/*
 * Utility functions for process management.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "memlayout.h"
#include "util/functions.h"
#include "sched.h"
#include "sync_utils.h"
#include "spike_interface/spike_utils.h"

extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);
extern char trap_sec_start[];

process procs[NPROC];
process* g_current[NCPU] = {0};
static volatile int g_proc_alloc_lock __attribute__((aligned(8))) = 0;

#define HEAP_ALIGN 16

static uint64 align_up(uint64 value, uint64 align) {
  return (value + align - 1) & ~(align - 1);
}

static int heap_alloc_slot(process *proc) {
  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    if (!proc->heap_blocks[i].valid) return i;
  }
  return -1;
}

static int heap_find_best_fit(process *proc, uint64 size) {
  int best = -1;
  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    heap_block *block = &proc->heap_blocks[i];
    if (!block->valid || block->used || block->size < size) continue;
    if (best < 0 || block->size < proc->heap_blocks[best].size) best = i;
  }
  return best;
}

static void heap_coalesce_free_blocks(process *proc) {
  int merged = 1;
  while (merged) {
    merged = 0;
    for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
      heap_block *left = &proc->heap_blocks[i];
      if (!left->valid || left->used) continue;

      for (int j = 0; j < HEAP_MAX_BLOCKS; j++) {
        if (i == j) continue;
        heap_block *right = &proc->heap_blocks[j];
        if (!right->valid || right->used) continue;

        if (left->va + left->size == right->va) {
          left->size += right->size;
          right->valid = 0;
          merged = 1;
          break;
        }
      }
      if (merged) break;
    }
  }
}

static int heap_extend(process *proc, uint64 size) {
  int slot = heap_alloc_slot(proc);
  if (slot < 0) return -1;

  uint64 bytes = ROUNDUP(size, PGSIZE);
  uint64 base = proc->user_heap.heap_top;

  for (uint64 off = 0; off < bytes; off += PGSIZE) {
    void *pa = alloc_page();
    if (pa == 0) return -1;
    user_vm_map((pagetable_t)proc->pagetable, base + off, PGSIZE, (uint64)pa,
                prot_to_type(PROT_WRITE | PROT_READ, 1));
    proc->mapped_info[HEAP_SEGMENT].npages++;
  }

  proc->user_heap.heap_top += bytes;
  proc->heap_blocks[slot].va = base;
  proc->heap_blocks[slot].size = bytes;
  proc->heap_blocks[slot].used = 0;
  proc->heap_blocks[slot].valid = 1;
  heap_coalesce_free_blocks(proc);
  return 0;
}

static void share_region_with_child_cow(process *parent, process *child, mapped_region *region) {
  for (uint32 page = 0; page < region->npages; page++) {
    uint64 va = region->va + (uint64)page * PGSIZE;
    pte_t *parent_pte = page_walk(parent->pagetable, va, 0);
    if (parent_pte == 0 || (*parent_pte & PTE_V) == 0) continue;

    uint64 pa = PTE2PA(*parent_pte);
    uint64 flags = PTE_FLAGS(*parent_pte);
    if (flags & PTE_W) {
      flags = (flags & (~PTE_W)) | PTE_COW;
      *parent_pte = PA2PTE(pa) | flags;
    }

    inc_page_ref((void *)pa);
    map_pages(child->pagetable, va, PGSIZE, pa, flags & (~PTE_V));
  }
}

void process_heap_init(process *proc) {
  proc->user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  proc->user_heap.heap_top = USER_FREE_ADDRESS_START;
  proc->user_heap.free_pages_count = 0;
  memset(proc->user_heap.free_pages_address, 0, sizeof(proc->user_heap.free_pages_address));
  memset(proc->heap_blocks, 0, sizeof(proc->heap_blocks));
}

uint64 process_heap_alloc(process *proc, uint64 size) {
  if (size == 0) size = PGSIZE;
  uint64 alloc_size = align_up(size, HEAP_ALIGN);

  int slot = heap_find_best_fit(proc, alloc_size);
  if (slot < 0) {
    if (heap_extend(proc, alloc_size) != 0) return 0;
    slot = heap_find_best_fit(proc, alloc_size);
    if (slot < 0) return 0;
  }

  heap_block *block = &proc->heap_blocks[slot];
  if (block->size >= alloc_size + HEAP_ALIGN) {
    int split_slot = heap_alloc_slot(proc);
    if (split_slot >= 0) {
      proc->heap_blocks[split_slot].va = block->va + alloc_size;
      proc->heap_blocks[split_slot].size = block->size - alloc_size;
      proc->heap_blocks[split_slot].used = 0;
      proc->heap_blocks[split_slot].valid = 1;
      block->size = alloc_size;
    }
  }

  block->used = 1;
  return block->va;
}

int process_heap_free(process *proc, uint64 va) {
  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    heap_block *block = &proc->heap_blocks[i];
    if (!block->valid || !block->used) continue;
    if (block->va != va) continue;

    block->used = 0;
    heap_coalesce_free_blocks(proc);
    return 0;
  }
  return -1;
}

void switch_to(process* proc) {
  assert(proc);
  current = proc;

  write_csr(stvec, (uint64)smode_trap_vector);

  proc->trapframe->kernel_sp = proc->kstack;
  proc->trapframe->kernel_satp = read_csr(satp);
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;
  proc->trapframe->kernel_hartid = read_tp();

  unsigned long status = read_csr(sstatus);
  status &= ~SSTATUS_SPP;
  status |= SSTATUS_SPIE;
  write_csr(sstatus, status);

  write_csr(sepc, proc->trapframe->epc);
  return_to_user(proc->trapframe, MAKE_SATP(proc->pagetable));
}

void init_proc_pool() {
  memset(procs, 0, sizeof(process) * NPROC);
  for (int i = 0; i < NPROC; ++i) {
    procs[i].status = FREE;
    procs[i].pid = i;
  }
}

process* alloc_process() {
  spin_lock(&g_proc_alloc_lock);
  int index;
  for (index = 0; index < NPROC; index++)
    if (procs[index].status == FREE) break;

  if (index >= NPROC) {
    spin_unlock(&g_proc_alloc_lock);
    panic("cannot find any free process structure.\n");
    return 0;
  }
  procs[index].status = BLOCKED;
  spin_unlock(&g_proc_alloc_lock);

  procs[index].trapframe = (trapframe *)alloc_page();
  memset(procs[index].trapframe, 0, sizeof(trapframe));

  procs[index].pagetable = (pagetable_t)alloc_page();
  memset((void *)procs[index].pagetable, 0, PGSIZE);

  procs[index].kstack = (uint64)alloc_page() + PGSIZE;
  uint64 user_stack = (uint64)alloc_page();
  procs[index].trapframe->regs.sp = USER_STACK_TOP;

  procs[index].mapped_info = (mapped_region*)alloc_page();
  memset(procs[index].mapped_info, 0, PGSIZE);

  user_vm_map((pagetable_t)procs[index].pagetable, USER_STACK_TOP - PGSIZE, PGSIZE,
              user_stack, prot_to_type(PROT_WRITE | PROT_READ, 1));
  procs[index].mapped_info[STACK_SEGMENT].va = USER_STACK_TOP - PGSIZE;
  procs[index].mapped_info[STACK_SEGMENT].npages = 1;
  procs[index].mapped_info[STACK_SEGMENT].seg_type = STACK_SEGMENT;

  user_vm_map((pagetable_t)procs[index].pagetable, (uint64)procs[index].trapframe, PGSIZE,
              (uint64)procs[index].trapframe, prot_to_type(PROT_WRITE | PROT_READ, 0));
  procs[index].mapped_info[CONTEXT_SEGMENT].va = (uint64)procs[index].trapframe;
  procs[index].mapped_info[CONTEXT_SEGMENT].npages = 1;
  procs[index].mapped_info[CONTEXT_SEGMENT].seg_type = CONTEXT_SEGMENT;

  user_vm_map((pagetable_t)procs[index].pagetable, (uint64)trap_sec_start, PGSIZE,
              (uint64)trap_sec_start, prot_to_type(PROT_READ | PROT_EXEC, 0));
  procs[index].mapped_info[SYSTEM_SEGMENT].va = (uint64)trap_sec_start;
  procs[index].mapped_info[SYSTEM_SEGMENT].npages = 1;
  procs[index].mapped_info[SYSTEM_SEGMENT].seg_type = SYSTEM_SEGMENT;

  process_heap_init(&procs[index]);
  procs[index].mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  procs[index].mapped_info[HEAP_SEGMENT].npages = 0;
  procs[index].mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;

  procs[index].total_mapped_region = HEAP_SEGMENT + 1;
  procs[index].parent = NULL;
  procs[index].queue_next = NULL;
  procs[index].tick_count = 0;
  if (g_active_harts >= 64)
    procs[index].hart_mask = ~0ULL;
  else
    procs[index].hart_mask = (1ULL << g_active_harts) - 1;
  procs[index].stdin_fd = -1;
  procs[index].stdout_fd = -1;
  strcpy(procs[index].cwd, "/");

  procs[index].pfiles = init_proc_file_management();
  if (current != NULL && !g_quiet_mode) {
    sprint("in alloc_proc. user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
           procs[index].trapframe, procs[index].trapframe->regs.sp, procs[index].kstack);
    sprint("in alloc_proc. build proc_file_management successfully.\n");
  }

  return &procs[index];
}

int free_process(process* proc) {
  if (proc == current && proc->pfiles) {
    for (int fd = 0; fd < MAX_FILES; ++fd) {
      if (proc->pfiles->opened_files[fd].status != FD_NONE) do_close(fd);
    }
  }
  proc->status = ZOMBIE;
  return 0;
}

int do_fork(process* parent) {
  if (!g_quiet_mode) sprint("will fork a child from parent %ld.\n", parent->pid);
  process* child = alloc_process();

  strcpy(child->cwd, parent->cwd);
  child->stdin_fd = parent->stdin_fd;
  child->stdout_fd = parent->stdout_fd;
  child->hart_mask = parent->hart_mask;
  copy_proc_file_management(child->pfiles, parent->pfiles);
  child->user_heap = parent->user_heap;
  memcpy(child->heap_blocks, parent->heap_blocks, sizeof(parent->heap_blocks));
  child->mapped_info[HEAP_SEGMENT] = parent->mapped_info[HEAP_SEGMENT];

  for (int i = 0; i < parent->total_mapped_region; i++) {
    switch (parent->mapped_info[i].seg_type) {
      case CONTEXT_SEGMENT:
        *child->trapframe = *parent->trapframe;
        break;
      case STACK_SEGMENT:
        child->mapped_info[STACK_SEGMENT].va = parent->mapped_info[i].va;
        child->mapped_info[STACK_SEGMENT].npages = parent->mapped_info[i].npages;
        for (uint32 page = 0; page < parent->mapped_info[i].npages; page++) {
          uint64 va = parent->mapped_info[i].va + (uint64)page * PGSIZE;
          uint64 parent_pa = lookup_pa(parent->pagetable, va);
          if (parent_pa == 0) continue;
          uint64 child_pa = lookup_pa(child->pagetable, va);
          if (child_pa == 0) {
            child_pa = (uint64)alloc_page();
            map_pages(child->pagetable, va, PGSIZE, child_pa,
                      prot_to_type(PROT_READ | PROT_WRITE, 1));
          }
          memcpy((void*)child_pa, (void*)parent_pa, PGSIZE);
        }
        break;
      case CODE_SEGMENT:
        if (!g_quiet_mode)
          sprint("do_fork map code segment at pa:%lx of parent to child at va:%lx.\n",
                 lookup_pa(parent->pagetable, parent->mapped_info[i].va),
                 parent->mapped_info[i].va);
        share_region_with_child_cow(parent, child, &parent->mapped_info[i]);
        child->mapped_info[child->total_mapped_region] = parent->mapped_info[i];
        child->total_mapped_region++;
        break;
      case DATA_SEGMENT:
        share_region_with_child_cow(parent, child, &parent->mapped_info[i]);
        child->mapped_info[child->total_mapped_region] = parent->mapped_info[i];
        child->total_mapped_region++;
        break;
      case HEAP_SEGMENT:
        share_region_with_child_cow(parent, child, &parent->mapped_info[i]);
        break;
      default:
        break;
    }
  }

  flush_tlb();

  child->status = READY;
  child->trapframe->regs.a0 = 0;
  child->parent = parent;
  insert_to_ready_queue(child);
  return child->pid;
}

int do_exec(process* proc, const char* path, const char* arg) {
  if (!proc || !path) return -1;

  for (int i = 0; i < proc->total_mapped_region; i++) {
    if (proc->mapped_info[i].npages == 0) continue;

    switch (proc->mapped_info[i].seg_type) {
      case CODE_SEGMENT:
        user_vm_unmap((pagetable_t)proc->pagetable, proc->mapped_info[i].va,
                      proc->mapped_info[i].npages * PGSIZE, 0);
        break;
      case DATA_SEGMENT:
      case HEAP_SEGMENT:
        user_vm_unmap((pagetable_t)proc->pagetable, proc->mapped_info[i].va,
                      proc->mapped_info[i].npages * PGSIZE, 1);
        break;
      default:
        break;
    }

    if (proc->mapped_info[i].seg_type >= CODE_SEGMENT) {
      proc->mapped_info[i].va = 0;
      proc->mapped_info[i].npages = 0;
      proc->mapped_info[i].seg_type = 0;
    }
  }

  proc->total_mapped_region = HEAP_SEGMENT + 1;
  process_heap_init(proc);
  proc->mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  proc->mapped_info[HEAP_SEGMENT].npages = 0;
  proc->mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;
  proc->tick_count = 0;

  memset((void*)lookup_pa(proc->pagetable, USER_STACK_TOP - PGSIZE), 0, PGSIZE);
  load_bincode_from_host_elf(proc, (char*)path);

  if (!arg) arg = "";

  if (arg[0] == '\0') {
    proc->trapframe->regs.sp = USER_STACK_TOP;
    proc->trapframe->regs.a0 = 0;
    proc->trapframe->regs.a1 = 0;
    return 0;
  }

  uint64 sp = USER_STACK_TOP;
  size_t arg_len = strlen(arg) + 1;
  if (arg_len > PGSIZE / 2) return -1;

  sp -= arg_len;
  char* arg_user = (char*)sp;
  char* arg_pa = (char*)user_va_to_pa((pagetable_t)proc->pagetable, arg_user);
  if (!arg_pa) return -1;
  memcpy(arg_pa, arg, arg_len);

  sp = ROUNDDOWN(sp, 16);
  sp -= 2 * sizeof(uint64);
  uint64* argv_pa = (uint64*)user_va_to_pa((pagetable_t)proc->pagetable, (void*)sp);
  if (!argv_pa) return -1;
  argv_pa[0] = (uint64)arg_user;
  argv_pa[1] = 0;

  proc->trapframe->regs.sp = sp;
  proc->trapframe->regs.a0 = 1;
  proc->trapframe->regs.a1 = sp;
  return 0;
}

int do_wait(process* proc, int pid) {
  if (!proc) return -1;

  for (;;) {
    int has_child = 0;
    for (int i = 0; i < NPROC; i++) {
      if (procs[i].parent != proc) continue;
      if (pid != -1 && procs[i].pid != pid) continue;

      has_child = 1;
      if (procs[i].status == ZOMBIE) {
        procs[i].status = FREE;
        procs[i].parent = NULL;
        procs[i].queue_next = NULL;
        return procs[i].pid;
      }
    }

    if (!has_child) return -1;
    proc->status = BLOCKED;
    schedule();
  }
}
