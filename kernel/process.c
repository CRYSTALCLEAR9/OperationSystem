/*
 * Utility functions for process management. 
 *
 * Note: in Lab1, only one process (i.e., our user application) exists. Therefore, 
 * PKE OS at this stage will set "current" to the loaded user application, and also
 * switch to the old "current" process after trap handling.
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
#include "spike_interface/spike_utils.h"

//Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// current points to the currently running user-mode application.
process* current = NULL;

#define HEAP_ALIGN 16

static uint64 align_up(uint64 x, uint64 align) {
  return (x + align - 1) & ~(align - 1);
}

static int heap_alloc_slot(process* proc) {
  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    if (!proc->heap_blocks[i].valid) return i;
  }
  return -1;
}

static int heap_find_best_fit(process* proc, uint64 size) {
  int best = -1;
  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    heap_block* b = &proc->heap_blocks[i];
    if (!b->valid || b->used || b->size < size) continue;
    if (best < 0 || b->size < proc->heap_blocks[best].size) best = i;
  }
  return best;
}

static void heap_coalesce_free_blocks(process* proc) {
  int merged = 1;
  while (merged) {
    merged = 0;
    for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
      heap_block* a = &proc->heap_blocks[i];
      if (!a->valid || a->used) continue;

      for (int j = 0; j < HEAP_MAX_BLOCKS; j++) {
        if (i == j) continue;
        heap_block* b = &proc->heap_blocks[j];
        if (!b->valid || b->used) continue;

        if (a->va + a->size == b->va) {
          a->size += b->size;
          b->valid = 0;
          merged = 1;
          break;
        }
      }
      if (merged) break;
    }
  }
}

static int heap_extend(process* proc, uint64 required_size) {
  int block_slot = heap_alloc_slot(proc);
  if (block_slot < 0) return -1;

  uint64 bytes = ROUNDUP(required_size, PGSIZE);
  uint64 va = proc->heap_end;

  for (uint64 off = 0; off < bytes; off += PGSIZE) {
    void* pa = alloc_page();
    if (pa == 0) return -1;
    user_vm_map((pagetable_t)proc->pagetable, va + off, PGSIZE, (uint64)pa,
                prot_to_type(PROT_WRITE | PROT_READ, 1));
  }

  proc->heap_end += bytes;
  proc->heap_blocks[block_slot].va = va;
  proc->heap_blocks[block_slot].size = bytes;
  proc->heap_blocks[block_slot].used = 0;
  proc->heap_blocks[block_slot].valid = 1;

  heap_coalesce_free_blocks(proc);
  return 0;
}

void process_heap_init(process* proc) {
  proc->heap_base = USER_FREE_ADDRESS_START;
  proc->heap_end = USER_FREE_ADDRESS_START;
  proc->heap_initialized = 1;
  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    proc->heap_blocks[i].valid = 0;
  }
}

uint64 process_heap_alloc(process* proc, uint64 size) {
  if (size == 0) return 0;
  if (!proc->heap_initialized) process_heap_init(proc);

  uint64 alloc_size = align_up(size, HEAP_ALIGN);
  int idx = heap_find_best_fit(proc, alloc_size);
  if (idx < 0) {
    if (heap_extend(proc, alloc_size) != 0) return 0;
    idx = heap_find_best_fit(proc, alloc_size);
    if (idx < 0) return 0;
  }

  heap_block* b = &proc->heap_blocks[idx];
  if (b->size >= alloc_size + HEAP_ALIGN) {
    int split_slot = heap_alloc_slot(proc);
    if (split_slot >= 0) {
      proc->heap_blocks[split_slot].va = b->va + alloc_size;
      proc->heap_blocks[split_slot].size = b->size - alloc_size;
      proc->heap_blocks[split_slot].used = 0;
      proc->heap_blocks[split_slot].valid = 1;
      b->size = alloc_size;
    }
  }

  b->used = 1;
  return b->va;
}

int process_heap_free(process* proc, uint64 va) {
  if (!proc->heap_initialized) return -1;

  for (int i = 0; i < HEAP_MAX_BLOCKS; i++) {
    heap_block* b = &proc->heap_blocks[i];
    if (!b->valid || !b->used) continue;
    if (b->va != va) continue;

    b->used = 0;
    heap_coalesce_free_blocks(proc);
    return 0;
  }

  return -1;
}

//
// switch to a user-mode process
//
void switch_to(process* proc) {
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;      // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp);  // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE;  // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added @lab2_1
  uint64 user_satp = MAKE_SATP(proc->pagetable);

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  // note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}
