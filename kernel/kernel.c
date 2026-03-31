/*
 * Supervisor-mode startup codes
 */

#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"
#include "util/types.h"
#include "vfs.h"
#include "rfs.h"
#include "ramdev.h"
#include "sync_utils.h"

volatile int g_multicore_boot_mode = 0;
volatile int g_boot_app_count = 0;
volatile int g_quiet_mode = 0;
static volatile int g_smode_init_ready __attribute__((aligned(8))) = 0;
static volatile int g_smp_sched_ready __attribute__((aligned(8))) = 0;

//
// trap_sec_start points to the beginning of S-mode trap segment (i.e., the entry point of
// S-mode trap vector). added @lab2_1
//
extern char trap_sec_start[];

//
// turn on paging. added @lab2_1
//
void enable_paging() {
  // write the pointer to kernel page (table) directory into the CSR of "satp".
  write_csr(satp, MAKE_SATP(g_kernel_pagetable));

  // refresh tlb to invalidate its content.
  flush_tlb();
}

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

//
// returns the number (should be 1) of string(s) after PKE kernel in command line.
// and store the string(s) in arg_bug_msg.
//
static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input) *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
      sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1;  // skip the PKE OS kernel string, leave behind only the application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  //returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

//
// load the elf, and construct a "process" (with only a trapframe).
// load_bincode_from_host_elf is defined in elf.c
//
static void spawn_process_from_path(const char *path, uint64 hart_mask) {
  process *proc = alloc_process();
  sprint("User application is loading.\n");
  load_bincode_from_host_elf(proc, (char *)path);
  proc->hart_mask = hart_mask;
  insert_to_ready_queue(proc);
}

//
// s_start: S-mode entry point of riscv-pke OS kernel.
//
int s_start(void) {
  uint64 hartid = read_tp();
  arg_buf arg_bug_msg;
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");
  if (hartid == 0) {
    g_boot_app_count = argc;
    g_multicore_boot_mode = (g_active_harts > 1) ? 1 : 0;
  }

  // in the beginning, we use Bare mode (direct) memory mapping as in lab1.
  // but now, we are going to switch to the paging mode @lab2_1.
  // note, the code still works in Bare mode when calling pmm_init() and kern_vm_init().
  write_csr(satp, 0);

  while (g_boot_app_count == 0)
    ;

  if (g_multicore_boot_mode)
    sprint("hartid = %ld: Enter supervisor mode...\n", hartid);
  else if (hartid == 0)
    sprint("Enter supervisor mode...\n");

  if (hartid == 0) {
    pmm_init();
    kern_vm_init();
    init_proc_pool();
    fs_init();
  }
  sync_barrier(&g_smode_init_ready, g_active_harts);

  // now, switch to paging mode by turning on paging (SV39)
  enable_paging();
  // the code now formally works in paging mode, meaning the page table is now in use.
  if (!g_multicore_boot_mode || hartid == 0) sprint("kernel page table is on \n");

  vm_alloc_stage[hartid] = 1;

  uint64 all_mask = (g_active_harts >= 64) ? ~0ULL : ((1ULL << g_active_harts) - 1);
  if ((size_t)hartid < argc) {
    uint64 mask = (hartid < 64) ? (1ULL << hartid) : all_mask;
    spawn_process_from_path(arg_bug_msg.argv[hartid], mask);
  }
  if (hartid == 0) {
    for (size_t i = g_active_harts; i < argc; i++) {
      spawn_process_from_path(arg_bug_msg.argv[i], all_mask);
    }
  }
  sync_barrier(&g_smp_sched_ready, g_active_harts);
  schedule();

  // we should never reach here.
  return 0;
}
