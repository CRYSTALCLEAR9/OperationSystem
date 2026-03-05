#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"

#define HEAP_MAX_BLOCKS 256

typedef struct heap_block_t {
  uint64 va;
  uint64 size;
  int used;
  int valid;
} heap_block;

typedef struct trapframe_t {
  // space to store context (all common registers)
  /* offset:0   */ riscv_regs regs;

  // process's "user kernel" stack
  /* offset:248 */ uint64 kernel_sp;
  // pointer to smode_trap_handler
  /* offset:256 */ uint64 kernel_trap;
  // saved user process counter
  /* offset:264 */ uint64 epc;

  // kernel page table. added @lab2_1
  /* offset:272 */ uint64 kernel_satp;
}trapframe;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // user page table
  pagetable_t pagetable;
  // trapframe storing the context of a (User mode) process.
  trapframe* trapframe;

  // user heap range: [heap_base, heap_end)
  uint64 heap_base;
  uint64 heap_end;
  int heap_initialized;
  heap_block heap_blocks[HEAP_MAX_BLOCKS];
}process;

// switch to run user app
void switch_to(process*);

// current running process
extern process* current;

void process_heap_init(process* proc);
uint64 process_heap_alloc(process* proc, uint64 size);
int process_heap_free(process* proc, uint64 va);

#endif
