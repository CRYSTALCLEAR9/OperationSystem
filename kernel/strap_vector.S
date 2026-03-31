.section trapsec
.globl trap_sec_start
trap_sec_start:

#include "util/load_store.S"

#
# When a trap (e.g., a syscall from User mode in this lab) happens and the computer
# enters the Supervisor mode, the computer will continue to execute the following
# function (smode_trap_vector) to actually handle the trap.
#
# NOTE: sscratch points to the trapframe of current process before entering
# smode_trap_vector. It is done by reture_to_user function (defined below) when
# scheduling a user-mode application to run.
#
.globl smode_trap_vector
.align 4
smode_trap_vector:
    # Distinguish trap origin:
    #   SPP=0: trap from user mode, save user context into trapframe
    #   SPP=1: trap from supervisor mode, save context on current kernel stack
    csrr t0, sstatus
    andi t0, t0, 0x100
    bnez t0, .Ltrap_from_kernel

    # -------------------- trap from user --------------------
    # swap a0 and sscratch, so that points a0 to the trapframe of current process
    csrrw a0, sscratch, a0

    # save the context (user registers) of current process in its trapframe.
    addi t6, a0 , 0

    # store_all_registers is a macro defined in util/load_store.S, it stores contents
    # of all general purpose registers into a piece of memory started from [t6].
    store_all_registers

    # come back to save a0 register before entering trap handling in trapframe
    # [t0]=[sscratch]
    csrr t0, sscratch
    sd t0, 72(a0)

    # use the "user kernel" stack (whose pointer stored in p->trapframe->kernel_sp)
    ld sp, 248(a0)

    # load the address of smode_trap_handler() from p->trapframe->kernel_trap
    ld t0, 256(a0)

    # restore kernel page table from p->trapframe->kernel_satp. added @lab2_1
    ld t1, 272(a0)
    csrw satp, t1
    sfence.vma zero, zero

    # restore kernel hartid into tp, because user mode may clobber tp.
    ld tp, 280(a0)

    # jump to smode_trap_handler() that is defined in kernel/trap.c
    jr t0

.Ltrap_from_kernel:
    # save full supervisor context on current kernel stack
    addi sp, sp, -256
    addi t6, sp, 0
    store_all_registers

    # handle kernel-origin trap in C
    call smode_kernel_trap_handler

    # restore context and return to supervisor mode
    addi t6, sp, 0
    restore_all_registers
    addi sp, sp, 256
    sret

#
# return from Supervisor mode to User mode, transition is made by using a trapframe,
# which stores the context of a user application.
# return_to_user() takes one parameter, i.e., the pointer (a0 register) pointing to a
# trapframe (defined in kernel/process.h) of the process.
#
.globl return_to_user
return_to_user:
    # a0: TRAPFRAME
    # a1: user page table, for satp.

    # switch to the user page table. added @lab2_1
    csrw satp, a1
    sfence.vma zero, zero

    # [sscratch]=[a0], save a0 in sscratch, so sscratch points to a trapframe now.
    csrw sscratch, a0

    # let [t6]=[a0]
    addi t6, a0, 0

    # restore_all_registers is a assembly macro defined in util/load_store.S.
    # the macro restores all registers from trapframe started from [t6] to all general
    # purpose registers, so as to resort the execution of a process.
    restore_all_registers 

    # return to user mode and user pc.
    sret
