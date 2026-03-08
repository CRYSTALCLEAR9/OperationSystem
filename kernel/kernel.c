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
static volatile int g_smode_init_ready = 0;

static int boot_participating_harts(void) {
  return g_boot_app_count < g_active_harts ? g_boot_app_count : g_active_harts;
}

static void idle_hart_forever(void) {
  intr_off();
  write_csr(sie, 0);
  while (1) asm volatile("wfi");
}

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
process* load_user_program() {
  process* proc;

  proc = alloc_process();
  sprint("User application is loading.\n");

  arg_buf arg_bug_msg;

  // retrieve command line arguements
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  load_bincode_from_host_elf(proc, arg_bug_msg.argv[0]);
  return proc;
}

static void load_user_program_for_hart(process *proc, int app_index) {
  arg_buf arg_bug_msg;
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc || app_index >= (int)argc) panic("Missing application for hart %d.\n", app_index);

  sprint("hartid = %ld: User application is loading.\n", read_tp());
  load_bincode_from_host_elf(proc, arg_bug_msg.argv[app_index]);
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
    g_multicore_boot_mode = (g_active_harts > 1 && argc > 1) ? 1 : 0;
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

  if (!g_multicore_boot_mode && hartid != 0) idle_hart_forever();

  // now, switch to paging mode by turning on paging (SV39)
  enable_paging();
  // the code now formally works in paging mode, meaning the page table is now in use.
  if (!g_multicore_boot_mode || hartid == 0) sprint("kernel page table is on \n");

  if (g_multicore_boot_mode) {
    if ((int)hartid < boot_participating_harts()) {
      process *proc = alloc_process();
      sprint("hartid = %ld: user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
             hartid, proc->trapframe, proc->trapframe->regs.sp, proc->kstack);
      load_user_program_for_hart(proc, hartid);
      sprint("hartid = %ld: Switch to user mode...\n", hartid);
      vm_alloc_stage[hartid] = 1;
      switch_to(proc);
    }
    idle_hart_forever();
  } else {
    process *proc = load_user_program();
    sprint("Switch to user mode...\n");
    vm_alloc_stage[hartid] = 1;
    insert_to_ready_queue(proc);
    schedule();
  }

  // we should never reach here.
  return 0;
}
