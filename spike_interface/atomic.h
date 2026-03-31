// See LICENSE for license details.
// borrowed from https://github.com/riscv/riscv-pk:
// machine/atomic.h

#ifndef _RISCV_ATOMIC_H_
#define _RISCV_ATOMIC_H_

// todo: for PKE, we turn on irq in lab_1_3_timer, so we have to implement these two functions.
#define disable_irqsave() (0)
#define enable_irqrestore(flags) ((void)(flags))

typedef struct {
  volatile int lock;
  // For debugging:
  char* name;       // Name of lock.
  struct cpu* cpu;  // The cpu holding the lock.
} spinlock_t;

#define SPINLOCK_INIT \
  { 0 }

#define mb() asm volatile("fence" ::: "memory")
#define atomic_set(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_read(ptr) __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
#define atomic_add(ptr, inc) __sync_fetch_and_add((ptr), (inc))
#define atomic_or(ptr, inc) __sync_fetch_and_or((ptr), (inc))
#define atomic_swap(ptr, inc) __sync_lock_test_and_set((ptr), (inc))
#define atomic_cas(ptr, cmp, swp) __sync_val_compare_and_swap((ptr), (cmp), (swp))

static inline int spinlock_trylock(spinlock_t* lock) {
  int res = atomic_swap(&lock->lock, -1);
  mb();
  return res;
}

static inline void spinlock_lock(spinlock_t* lock) {
  do {
    while (atomic_read(&lock->lock))
      ;
  } while (spinlock_trylock(lock));
}

static inline void spinlock_unlock(spinlock_t* lock) {
  mb();
  atomic_set(&lock->lock, 0);
}

static inline long spinlock_lock_irqsave(spinlock_t* lock) {
  long flags = disable_irqsave();
  spinlock_lock(lock);
  return flags;
}

static inline void spinlock_unlock_irqrestore(spinlock_t* lock, long flags) {
  spinlock_unlock(lock);
  enable_irqrestore(flags);
}

#endif
