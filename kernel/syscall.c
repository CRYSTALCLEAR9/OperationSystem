/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "elf.h"
#include "util/functions.h"

#include "spike_interface/spike_utils.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  sprint(buf);
  return 0;
}

//
// implement the SYS_user_backtrace syscall
//
ssize_t sys_user_backtrace(int64_t n) {
  uint64 fp = current->trapframe->regs.s0;
  
  // Skip do_user_call frame
  // do_user_call saves s0 at s0 - 8
  if (fp < DRAM_BASE) return 0;
  fp = *(uint64*)(fp - 8);
  
  for (int i = 0; i < n; i++) {
    if (fp < DRAM_BASE) break;
    // For standard functions (like print_backtrace, f8, etc.):
    // ra is at s0 - 8
    // prev_fp is at s0 - 16
    uint64 ra = *(uint64*)(fp - 8);
    uint64 prev_fp = *(uint64*)(fp - 16);
    
    char *name = find_symbol_name(ra, current);
    if (name) {
      sprint("%s\n", name);
    } else {
      sprint("0x%lx\n", ra);
    }
    fp = prev_fp;
  }
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process). 
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
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
    case SYS_user_backtrace:
      return sys_user_backtrace(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
