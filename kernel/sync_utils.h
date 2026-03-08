#ifndef _SYNC_UTILS_H_
#define _SYNC_UTILS_H_

#include "config.h"

extern volatile int g_active_harts;
extern volatile int g_multicore_boot_mode;
extern volatile int g_boot_app_count;

static inline void spin_lock(volatile int *lock) {
  int old;
  do {
    asm volatile("amoswap.w.aq %0, %2, (%1)\n"
                 : "=r"(old)
                 : "r"(lock), "r"(1)
                 : "memory");
  } while (old != 0);
}

static inline void spin_unlock(volatile int *lock) {
  asm volatile("amoswap.w.rl x0, x0, (%0)\n" : : "r"(lock) : "memory");
}

static inline void sync_barrier(volatile int *counter, int all) {
  int local;

  asm volatile("amoadd.w %0, %2, (%1)\n"
               : "=r"(local)
               : "r"(counter), "r"(1)
               : "memory");

  if (local + 1 < all) {
    do {
      asm volatile("lw %0, (%1)\n" : "=r"(local) : "r"(counter) : "memory");
    } while (local < all);
  }
}

#endif
