/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "riscv.h"
#include "util/string.h"
#include "process.h"
#include "elf.h"
#include "spike_interface/spike_htif.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "proc_file.h"
#include "sync_utils.h"

#include "spike_interface/spike_utils.h"

#define NSEM 16

typedef struct semaphore_t {
  int used;
  int value;
  process *wait_queue_head;
  process *wait_queue_tail;
} semaphore;

static semaphore sem_pool[NSEM];

static int sem_id_valid(int sem_id) {
  return sem_id >= 0 && sem_id < NSEM && sem_pool[sem_id].used;
}

static void sem_waitq_push(semaphore *sem, process *proc) {
  proc->queue_next = 0;
  if (sem->wait_queue_tail == 0) {
    sem->wait_queue_head = proc;
    sem->wait_queue_tail = proc;
    return;
  }

  sem->wait_queue_tail->queue_next = proc;
  sem->wait_queue_tail = proc;
}

static process* sem_waitq_pop(semaphore *sem) {
  process *proc = sem->wait_queue_head;
  if (proc == 0) return 0;

  sem->wait_queue_head = proc->queue_next;
  if (sem->wait_queue_head == 0) sem->wait_queue_tail = 0;
  proc->queue_next = 0;
  return proc;
}

static int sem_new(int init_value) {
  if (init_value < 0) return -1;

  for (int i = 0; i < NSEM; i++) {
    if (sem_pool[i].used) continue;
    sem_pool[i].used = 1;
    sem_pool[i].value = init_value;
    sem_pool[i].wait_queue_head = 0;
    sem_pool[i].wait_queue_tail = 0;
    return i;
  }
  return -1;
}

static int sem_free(int sem_id) {
  if (!sem_id_valid(sem_id)) return -1;
  if (sem_pool[sem_id].wait_queue_head != 0) return -1;

  sem_pool[sem_id].used = 0;
  sem_pool[sem_id].value = 0;
  sem_pool[sem_id].wait_queue_head = 0;
  sem_pool[sem_id].wait_queue_tail = 0;
  return 0;
}

static int sem_P(int sem_id) {
  if (!sem_id_valid(sem_id)) return -1;

  semaphore *sem = &sem_pool[sem_id];
  sem->value--;
  if (sem->value < 0) {
    current->status = BLOCKED;
    sem_waitq_push(sem, current);
    schedule();
  }
  return 0;
}

static int sem_V(int sem_id) {
  if (!sem_id_valid(sem_id)) return -1;

  semaphore *sem = &sem_pool[sem_id];
  sem->value++;
  if (sem->value <= 0) {
    process *proc = sem_waitq_pop(sem);
    if (proc) insert_to_ready_queue(proc);
  }
  return 0;
}

static int read_user_u64(uint64 uva, uint64 *value) {
  uint64 pa = (uint64)user_va_to_pa((pagetable_t)current->pagetable, (void *)uva);
  if (pa == 0) return 0;
  *value = *(uint64 *)pa;
  return 1;
}

ssize_t sys_user_print(const char* buf, size_t n) {
  assert(current);
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  if (pa == 0) return -1;
  if (current->stdout_fd >= 0) return do_write(current->stdout_fd, pa, n);
  if (g_multicore_boot_mode && !g_quiet_mode) {
    sprint("hartid = %ld: %s", read_tp(), pa);
    return n;
  }
  sprint("%s", pa);
  return n;
}

ssize_t sys_user_exit(uint64 code) {
  if (!g_quiet_mode || code != 0) {
    if (g_multicore_boot_mode) {
      uint64 hartid = read_tp();
      sprint("hartid = %ld: User exit with code:%ld.\n", hartid, code);
    } else {
      sprint("User exit with code:%ld.\n", code);
    }
  }
  if (current->parent && current->parent->status == BLOCKED)
    insert_to_ready_queue(current->parent);
  free_process(current);
  schedule();
  return 0;
}

uint64 sys_user_allocate_page(uint64 size) {
  assert(current);
  if (size == 0) size = PGSIZE;
  uint64 va = process_heap_alloc(current, size);
  if (!g_quiet_mode && g_multicore_boot_mode && va != 0) {
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, va);
    sprint("hartid = %ld: vaddr 0x%x is mapped to paddr 0x%x\n", read_tp(), va, pa);
  }
  return va;
}

uint64 sys_user_free_page(uint64 va) {
  assert(current);
  return process_heap_free(current, va);
}

ssize_t sys_user_fork() {
  if (!g_quiet_mode) sprint("User call fork.\n");
  return do_fork(current);
}

ssize_t sys_user_yield() {
  insert_to_ready_queue(current);
  schedule();
  return 0;
}

ssize_t sys_user_backtrace(int depth) {
  uint64 fp = current->trapframe->regs.s0;
  int printed = 0;
  int guard = 0;

  if (fp == 0 || !read_user_u64(fp - 8, &fp)) return 0;

  while (fp != 0 && printed < depth && guard < depth + 8) {
    uint64 ra = 0;
    uint64 prev_fp = 0;
    if (!read_user_u64(fp - 8, &ra) || !read_user_u64(fp - 16, &prev_fp)) break;

    const char *name = find_symbol_name(ra);
    if (name && strcmp(name, "do_user_call") != 0 && strcmp(name, "print_backtrace") != 0) {
      sprint("%s\n", name);
      printed++;
      if (strcmp(name, "main") == 0) break;
    }

    if (prev_fp <= fp) break;
    fp = prev_fp;
    guard++;
  }
  return 0;
}

ssize_t sys_user_printpa(uint64 va) {
  uint64 pa = (uint64)user_va_to_pa((pagetable_t)(current->pagetable), (void*)va);
  sprint("%lx\n", pa);
  return 0;
}

ssize_t sys_user_sem_new(int init_value) {
  return sem_new(init_value);
}

ssize_t sys_user_sem_free(int sem_id) {
  return sem_free(sem_id);
}

ssize_t sys_user_sem_P(int sem_id) {
  return sem_P(sem_id);
}

ssize_t sys_user_sem_V(int sem_id) {
  return sem_V(sem_id);
}

ssize_t sys_user_open(char *pathva, int flags) {
  char* pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  char abs_path[MAX_PATH_LEN];
  make_abs_path(current->cwd, pathpa, abs_path);
  return do_open(abs_path, flags);
}

ssize_t sys_user_read(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) {
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_read(fd, (char *)pa + off, len);
    i += r;
    if (r < len) return i;
  }
  return count;
}

ssize_t sys_user_write(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) {
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_write(fd, (char *)pa + off, len);
    i += r;
    if (r < len) return i;
  }
  return count;
}

ssize_t sys_user_lseek(int fd, int offset, int whence) {
  return do_lseek(fd, offset, whence);
}

ssize_t sys_user_stat(int fd, struct istat *istat) {
  struct istat *pistat = (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_stat(fd, pistat);
}

ssize_t sys_user_disk_stat(int fd, struct istat *istat) {
  struct istat *pistat = (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_disk_stat(fd, pistat);
}

ssize_t sys_user_close(int fd) {
  return do_close(fd);
}

ssize_t sys_user_opendir(char *pathva) {
  char *pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  char abs_path[MAX_PATH_LEN];
  make_abs_path(current->cwd, pathpa, abs_path);
  return do_opendir(abs_path);
}

ssize_t sys_user_readdir(int fd, struct dir *vdir) {
  struct dir *pdir = (struct dir *)user_va_to_pa((pagetable_t)(current->pagetable), vdir);
  return do_readdir(fd, pdir);
}

ssize_t sys_user_mkdir(char *pathva) {
  char *pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  char abs_path[MAX_PATH_LEN];
  make_abs_path(current->cwd, pathpa, abs_path);
  return do_mkdir(abs_path);
}

ssize_t sys_user_closedir(int fd) {
  return do_closedir(fd);
}

ssize_t sys_user_link(char *vfn1, char *vfn2) {
  char *pfn1 = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vfn1);
  char *pfn2 = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vfn2);
  char abs_path1[MAX_PATH_LEN];
  char abs_path2[MAX_PATH_LEN];
  make_abs_path(current->cwd, pfn1, abs_path1);
  make_abs_path(current->cwd, pfn2, abs_path2);
  return do_link(abs_path1, abs_path2);
}

ssize_t sys_user_unlink(char *vfn) {
  char *pfn = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vfn);
  char abs_path[MAX_PATH_LEN];
  make_abs_path(current->cwd, pfn, abs_path);
  return do_unlink(abs_path);
}

ssize_t sys_user_exec(char *vpath, char *varg) {
  char *upath = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)vpath);
  char *uarg = varg ? (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)varg) : 0;
  if (!upath) return -1;

  char path[MAX_PATH_LEN];
  char abs_path[MAX_PATH_LEN];
  char arg[256];
  safestrcpy(path, upath, sizeof(path));
  make_abs_path(current->cwd, path, abs_path);
  if (uarg) safestrcpy(arg, uarg, sizeof(arg));
  else arg[0] = '\0';

  if (do_exec(current, abs_path, arg) < 0) return -1;
  return current->trapframe->regs.a0;
}

ssize_t sys_user_wait(int pid) {
  return do_wait(current, pid);
}

ssize_t sys_user_rcwd(char *pathva) {
  char *pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  strcpy(pathpa, current->cwd);
  return 0;
}

ssize_t sys_user_ccwd(char *pathva) {
  char *pathpa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  char abs_path[MAX_PATH_LEN];
  make_abs_path(current->cwd, pathpa, abs_path);

  struct file *dir = vfs_opendir(abs_path);
  if (dir == NULL) return -1;
  vfs_closedir(dir);
  strcpy(current->cwd, abs_path);
  return 0;
}

ssize_t sys_user_pipe(int *fdsva) {
  int fds[2];
  int rc = do_pipe(fds);
  if (rc < 0) return rc;

  int *fdspa = (int *)user_va_to_pa((pagetable_t)(current->pagetable), fdsva);
  if (!fdspa) return -1;
  fdspa[0] = fds[0];
  fdspa[1] = fds[1];
  return 0;
}

ssize_t sys_user_set_stdin(int fd) {
  if (fd < 0) {
    current->stdin_fd = -1;
    return 0;
  }
  if (fd >= MAX_FILES || current->pfiles->opened_files[fd].status == FD_NONE) return -1;
  current->stdin_fd = fd;
  return 0;
}

ssize_t sys_user_set_stdout(int fd) {
  if (fd < 0) {
    current->stdout_fd = -1;
    return 0;
  }
  if (fd >= MAX_FILES || current->pfiles->opened_files[fd].status == FD_NONE) return -1;
  current->stdout_fd = fd;
  return 0;
}

ssize_t sys_user_set_affinity(int hartid) {
  if (hartid < 0) {
    if (g_active_harts >= 64)
      current->hart_mask = ~0ULL;
    else
      current->hart_mask = (1ULL << g_active_harts) - 1;
    return 0;
  }
  if (hartid >= g_active_harts) return -1;
  current->hart_mask = (1ULL << hartid);
  return 0;
}

ssize_t sys_user_set_quiet(int on) {
  g_quiet_mode = on ? 1 : 0;
  return 0;
}

static int console_getchar_blocking(void) {
  int ch;
  do {
    ch = htif_console_getchar();
  } while (ch < 0);
  return ch;
}

ssize_t sys_user_read_stdin(char *bufva, uint64 count) {
  if (count == 0) return 0;

  if (current->stdin_fd < 0) {
    uint64 i = 0;
    while (i < count) {
      uint64 addr = (uint64)bufva + i;
      uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
      if (!pa) return i;
      uint64 off = addr - ROUNDDOWN(addr, PGSIZE);

      char ch = (char)console_getchar_blocking();
      *((char *)pa + off) = ch;
      i++;
      if (ch == '\n') break;
    }
    return i;
  }

  int i = 0;
  while (i < count) {
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_read(current->stdin_fd, (char *)pa + off, len);
    i += r;
    if (r < len) return i;
  }
  return count;
}

long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    case SYS_user_allocate_page:
      return sys_user_allocate_page(a1);
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    case SYS_user_fork:
      return sys_user_fork();
    case SYS_user_yield:
      return sys_user_yield();
    case SYS_user_open:
      return sys_user_open((char *)a1, a2);
    case SYS_user_read:
      return sys_user_read(a1, (char *)a2, a3);
    case SYS_user_write:
      return sys_user_write(a1, (char *)a2, a3);
    case SYS_user_lseek:
      return sys_user_lseek(a1, a2, a3);
    case SYS_user_stat:
      return sys_user_stat(a1, (struct istat *)a2);
    case SYS_user_disk_stat:
      return sys_user_disk_stat(a1, (struct istat *)a2);
    case SYS_user_close:
      return sys_user_close(a1);
    case SYS_user_opendir:
      return sys_user_opendir((char *)a1);
    case SYS_user_readdir:
      return sys_user_readdir(a1, (struct dir *)a2);
    case SYS_user_mkdir:
      return sys_user_mkdir((char *)a1);
    case SYS_user_closedir:
      return sys_user_closedir(a1);
    case SYS_user_link:
      return sys_user_link((char *)a1, (char *)a2);
    case SYS_user_unlink:
      return sys_user_unlink((char *)a1);
    case SYS_user_exec:
      return sys_user_exec((char *)a1, (char *)a2);
    case SYS_user_wait:
      return sys_user_wait(a1);
    case SYS_user_backtrace:
      return sys_user_backtrace(a1);
    case SYS_user_printpa:
      return sys_user_printpa(a1);
    case SYS_user_sem_new:
      return sys_user_sem_new(a1);
    case SYS_user_sem_P:
      return sys_user_sem_P(a1);
    case SYS_user_sem_V:
      return sys_user_sem_V(a1);
    case SYS_user_sem_free:
      return sys_user_sem_free(a1);
    case SYS_user_rcwd:
      return sys_user_rcwd((char *)a1);
    case SYS_user_ccwd:
      return sys_user_ccwd((char *)a1);
    case SYS_user_pipe:
      return sys_user_pipe((int *)a1);
    case SYS_user_set_stdin:
      return sys_user_set_stdin(a1);
    case SYS_user_set_stdout:
      return sys_user_set_stdout(a1);
    case SYS_user_read_stdin:
      return sys_user_read_stdin((char *)a1, a2);
    case SYS_user_set_affinity:
      return sys_user_set_affinity(a1);
    case SYS_user_set_quiet:
      return sys_user_set_quiet(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
