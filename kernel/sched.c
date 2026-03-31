/*
 * implementing the scheduler
 */

#include "sched.h"
#include "config.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"
#include "sync_utils.h"

process* ready_queue_head = NULL;
static volatile int g_ready_queue_lock __attribute__((aligned(8))) = 0;
static int first_schedule_done[NCPU] = {0};
static int proc_can_run_on_hart(process *proc, uint64 hartid) {
  return (proc->hart_mask & (1ULL << hartid)) != 0;
}

static process *dequeue_ready_for_hart(uint64 hartid) {
  process *prev = NULL;
  process *p = ready_queue_head;
  while (p) {
    if (proc_can_run_on_hart(p, hartid)) {
      if (prev)
        prev->queue_next = p->queue_next;
      else
        ready_queue_head = p->queue_next;
      p->queue_next = NULL;
      return p;
    }
    prev = p;
    p = p->queue_next;
  }
  return NULL;
}

//
// insert a process, proc, into the END of ready queue.
//
void insert_to_ready_queue( process* proc ) {
  spin_lock(&g_ready_queue_lock);
  if (proc->status == RUNNING && proc != current) {
    spin_unlock(&g_ready_queue_lock);
    return;
  }
  if (!g_quiet_mode && !(current == NULL && proc->pid == 0)) {
    sprint("going to insert process %ld to ready queue.\n", proc->pid);
  }
  // if the queue is empty in the beginning
  if( ready_queue_head == NULL ){
    proc->status = READY;
    proc->queue_next = NULL;
    ready_queue_head = proc;
    spin_unlock(&g_ready_queue_lock);
    return;
  }

  // ready queue is not empty
  process *p;
  // browse the ready queue to see if proc is already in-queue
  for( p=ready_queue_head; p->queue_next!=NULL; p=p->queue_next )
    if( p == proc ) {
      spin_unlock(&g_ready_queue_lock);
      return;  //already in queue
    }

  // p points to the last element of the ready queue
  if( p==proc ) {
    spin_unlock(&g_ready_queue_lock);
    return;
  }
  p->queue_next = proc;
  proc->status = READY;
  proc->queue_next = NULL;

  spin_unlock(&g_ready_queue_lock);
  return;
}

//
// choose a proc from the ready queue, and put it to run.
// note: schedule() does not take care of previous current process. If the current
// process is still runnable, you should place it into the ready queue (by calling
// ready_queue_insert), and then call schedule().
//
extern process procs[NPROC];
void schedule() {
  for (;;) {
    spin_lock(&g_ready_queue_lock);
    uint64 hartid = read_tp();
    process *next = dequeue_ready_for_hart(hartid);
    if (next) {
      next->status = RUNNING;
      current = next;
      spin_unlock(&g_ready_queue_lock);

      if (first_schedule_done[hartid]) {
        if (!g_quiet_mode) sprint("going to schedule process %ld to run.\n", current->pid);
      } else {
        first_schedule_done[hartid] = 1;
      }
      switch_to(current);
      return;
    }
    spin_unlock(&g_ready_queue_lock);

    int active = 0;
    for (int i = 0; i < NPROC; i++) {
      if (procs[i].status != FREE && procs[i].status != ZOMBIE) {
        active = 1;
        break;
      }
    }

    if (!active) {
      if (read_tp() == 0) {
        sprint("no more ready processes, system shutdown now.\n");
        shutdown(0);
      }
      intr_off();
      write_csr(sie, 0);
      while (1) asm volatile("wfi");
    }

    // No runnable process for this hart right now.
    // Keep polling: secondary harts may not get a wakeup source in all paths.
    asm volatile("nop");
  }
}
