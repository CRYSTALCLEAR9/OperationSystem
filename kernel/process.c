/*
 * Utility functions for process management. 
 *
 * Note: in Lab1, only one process (i.e., our user application) exists. Therefore, 
 * PKE OS at this stage will set "current" to the loaded user application, and also
 * switch to the old "current" process after trap handling.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"

#include "spike_interface/spike_utils.h"
#include "spike_interface/spike_file.h"

//Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe*);

// current points to the currently running user-mode application.
process* current = NULL;

static int find_addr_line_entry(process *proc, uint64 fault_pc, addr_line *entry) {
  if (!proc || !proc->line || proc->line_ind <= 0) return 0;

  int best = -1;
  for (int i = 0; i < proc->line_ind; i++) {
    if (proc->line[i].addr > fault_pc) break;
    best = i;
  }

  if (best < 0) return 0;
  *entry = proc->line[best];
  return 1;
}

static void print_source_line(const char *path, uint64 target_line) {
  if (!path || target_line == 0) return;

  spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) return;

  char buf[256];
  char source_line[256];
  uint64 line_no = 1;
  uint64 line_pos = 0;
  uint64 offset = 0;
  int found = 0;

  for (;;) {
    ssize_t n = spike_file_pread(f, buf, sizeof(buf), offset);
    if (n <= 0) break;
    offset += n;

    for (ssize_t i = 0; i < n; i++) {
      char c = buf[i];

      if (line_no == target_line && c != '\n' && c != '\r') {
        if (line_pos < sizeof(source_line) - 1) source_line[line_pos++] = c;
      }

      if (c == '\n') {
        if (line_no == target_line) {
          found = 1;
          break;
        }
        line_no++;
      }
    }

    if (found) break;
  }

  if (line_no == target_line && line_pos > 0) found = 1;
  if (found) {
    source_line[line_pos] = '\0';
    sprint("%s\n", source_line);
  }

  spike_file_close(f);
}

void print_runtime_error(uint64 fault_pc) {
  if (!current) return;

  addr_line entry;
  if (!find_addr_line_entry(current, fault_pc, &entry)) return;
  if (!current->file || !current->dir) return;
  if (entry.file >= 64) return;

  code_file cf = current->file[entry.file];
  if (cf.dir >= 64) return;

  const char *dir = current->dir[cf.dir];
  const char *file = cf.file;
  if (!dir || !file) return;

  char full_path[256];
  if (dir[0] == '\0') {
    sprint("Runtime error at %s:%ld\n", file, entry.line);
    print_source_line(file, entry.line);
    return;
  }

  sprint("Runtime error at %s/%s:%ld\n", dir, file, entry.line);
  memset(full_path, 0, sizeof(full_path));
  strcpy(full_path, dir);
  uint64 pos = strlen(full_path);
  if (pos < sizeof(full_path) - 1) full_path[pos++] = '/';
  for (uint64 i = 0; file[i] != '\0' && pos < sizeof(full_path) - 1; i++) full_path[pos++] = file[i];
  full_path[pos] = '\0';
  print_source_line(full_path, entry.line);
}

//
// switch to a user-mode process
//
void switch_to(process* proc) {
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;  // process's kernel stack
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE;  // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  return_to_user(proc->trapframe);
}
