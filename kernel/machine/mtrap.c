#include "kernel/riscv.h"
#include "kernel/process.h"
#include "kernel/elf.h"
#include "spike_interface/spike_utils.h"

static void handle_instruction_access_fault() { panic("Instruction access fault!"); }

static void handle_load_access_fault() { panic("Load access fault!"); }

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

static void handle_illegal_instruction() { panic("Illegal instruction!"); }

static void handle_misaligned_load() { panic("Misaligned Load!"); }

static void handle_misaligned_store() { panic("Misaligned AMO!"); }

// added @lab1_3
static void handle_timer() {
  uint64 hartid = read_tp();
  // keep each hart's timer independent in SMP mode
  *(uint64*)CLINT_MTIMECMP(hartid) = *(uint64*)CLINT_MTIME + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in S-mode
  write_csr(sip, SIP_SSIP);
}

//
// handle_mtrap calls a handling function according to the type of a machine mode interrupt (trap).
//
void handle_mtrap() {
  uint64 mcause = read_csr(mcause);
  uint64 mepc = read_csr(mepc);
  switch (mcause) {
    case CAUSE_MTIMER:
      handle_timer();
      break;
    case CAUSE_FETCH_ACCESS:
      print_runtime_error(mepc);
      handle_instruction_access_fault();
      break;
    case CAUSE_LOAD_ACCESS:
      print_runtime_error(mepc);
      handle_load_access_fault();
      break;
    case CAUSE_STORE_ACCESS:
      print_runtime_error(mepc);
      handle_store_access_fault();
      break;
    case CAUSE_ILLEGAL_INSTRUCTION:
      // TODO (lab1_2): call handle_illegal_instruction to implement illegal instruction
      // interception, and finish lab1_2.
      print_runtime_error(mepc);
      handle_illegal_instruction();

      break;
    case CAUSE_MISALIGNED_LOAD:
      print_runtime_error(mepc);
      handle_misaligned_load();
      break;
    case CAUSE_MISALIGNED_STORE:
      print_runtime_error(mepc);
      handle_misaligned_store();
      break;

    default:
      sprint("machine trap(): unexpected mscause %p\n", mcause);
      sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
      panic( "unexpected exception happened in M-mode.\n" );
      break;
  }
}
