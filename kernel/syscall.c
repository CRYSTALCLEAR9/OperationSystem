/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"

#include "spike_interface/spike_utils.h"

#define NSEM 16

typedef struct semaphore_t {
  int used;
  int value;
  process* wait_queue_head;
  process* wait_queue_tail;
} semaphore;

static semaphore sem_pool[NSEM];

static int sem_id_valid(int sem_id) {
  return sem_id >= 0 && sem_id < NSEM && sem_pool[sem_id].used;
}

static void sem_waitq_push(semaphore* sem, process* proc) {
  proc->queue_next = 0;
  if (sem->wait_queue_tail == 0) {
    sem->wait_queue_head = proc;
    sem->wait_queue_tail = proc;
    return;
  }

  sem->wait_queue_tail->queue_next = proc;
  sem->wait_queue_tail = proc;
}

static process* sem_waitq_pop(semaphore* sem) {
  process* proc = sem->wait_queue_head;
  if (proc == 0) {
    return 0;
  }

  sem->wait_queue_head = proc->queue_next;
  if (sem->wait_queue_head == 0) {
    sem->wait_queue_tail = 0;
  }
  proc->queue_next = 0;
  return proc;
}

static int sem_new(int init_value) {
  if (init_value < 0) {
    return -1;
  }

  for (int i = 0; i < NSEM; i++) {
    if (!sem_pool[i].used) {
      sem_pool[i].used = 1;
      sem_pool[i].value = init_value;
      sem_pool[i].wait_queue_head = 0;
      sem_pool[i].wait_queue_tail = 0;
      return i;
    }
  }

  return -1;
}

static int sem_free(int sem_id) {
  if (!sem_id_valid(sem_id)) {
    return -1;
  }

  semaphore* sem = &sem_pool[sem_id];
  if (sem->wait_queue_head != 0) {
    return -1;
  }

  sem->used = 0;
  sem->value = 0;
  sem->wait_queue_tail = 0;
  return 0;
}

static int sem_P(int sem_id) {
  if (!sem_id_valid(sem_id)) {
    return -1;
  }

  semaphore* sem = &sem_pool[sem_id];
  sem->value--;
  if (sem->value < 0) {
    current->status = BLOCKED;
    sem_waitq_push(sem, current);
    schedule();
  }

  return 0;
}

static int sem_V(int sem_id) {
  if (!sem_id_valid(sem_id)) {
    return -1;
  }

  semaphore* sem = &sem_pool[sem_id];
  sem->value++;
  if (sem->value <= 0) {
    process* proc = sem_waitq_pop(sem);
    if (proc) {
      insert_to_ready_queue(proc);
    }
  }

  return 0;
}

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert( current );
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // reclaim the current process, and reschedule. added @lab3_1
  free_process( current );
  schedule();
  return 0;
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va;
  // if there are previously reclaimed pages, use them first (this does not change the
  // size of the heap)
  if (current->user_heap.free_pages_count > 0) {
    va =  current->user_heap.free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    // otherwise, allocate a new page (this increases the size of the heap by one page)
    va = current->user_heap.heap_top;
    current->user_heap.heap_top += PGSIZE;

    current->mapped_info[HEAP_SEGMENT].npages++;
  }
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));

  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  // add the reclaimed page to the free page list
  current->user_heap.free_pages_address[current->user_heap.free_pages_count++] = va;
  return 0;
}

//
// kerenl entry point of naive_fork
//
ssize_t sys_user_fork() {
  sprint("User call fork.\n");
  return do_fork( current );
}

//
// kerenl entry point of yield. added @lab3_2
//
ssize_t sys_user_yield() {
  // TODO (lab3_2): implment the syscall of yield.
  // hint: the functionality of yield is to give up the processor. therefore,
  // we should set the status of currently running process to READY, insert it in
  // the rear of ready queue, and finally, schedule a READY process to run.
  insert_to_ready_queue(current);
  schedule();

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

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    // added @lab2_2
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    case SYS_user_fork:
      return sys_user_fork();
    case SYS_user_yield:
      return sys_user_yield();
    case SYS_user_sem_new:
      return sys_user_sem_new(a1);
    case SYS_user_sem_free:
      return sys_user_sem_free(a1);
    case SYS_user_sem_P:
      return sys_user_sem_P(a1);
    case SYS_user_sem_V:
      return sys_user_sem_V(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
