/*! @file excp.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief Exception handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#include <stddef.h>

#include "console.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "trap.h"

// EXPORTED FUNCTION DECLARATIONS
//

// The following two functions, defined below, are called to handle an exception
// from trap.s.

extern void handle_smode_exception(unsigned int cause, struct trap_frame* tfr);
extern void handle_umode_exception(unsigned int cause, struct trap_frame* tfr);

// IMPORTED FUNCTION DECLARATIONS
//
/**
 * @brief Imported function definition from syscall.c that handles system calls from user mode.
 * @param tfr Pointer to the trapframe
 * @return None
 */
extern void handle_syscall(struct trap_frame* tfr);  // syscall.c

// INTERNAL GLOBAL VARIABLES
//

/**
 * @brief Array of exception names indexed by their exception code
 */
static const char* const excp_names[] = {
    [RISCV_SCAUSE_INSTR_ADDR_MISALIGNED] = "Misaligned instruction address",
    [RISCV_SCAUSE_INSTR_ACCESS_FAULT] = "Instruction access fault",
    [RISCV_SCAUSE_ILLEGAL_INSTR] = "Illegal instruction",
    [RISCV_SCAUSE_BREAKPOINT] = "Breakpoint",
    [RISCV_SCAUSE_LOAD_ADDR_MISALIGNED] = "Misaligned load address",
    [RISCV_SCAUSE_LOAD_ACCESS_FAULT] = "Load access fault",
    [RISCV_SCAUSE_STORE_ADDR_MISALIGNED] = "Misaligned store address",
    [RISCV_SCAUSE_STORE_ACCESS_FAULT] = "Store access fault",
    [RISCV_SCAUSE_ECALL_FROM_UMODE] = "Environment call from U mode",
    [RISCV_SCAUSE_ECALL_FROM_SMODE] = "Environment call from S mode",
    [RISCV_SCAUSE_INSTR_PAGE_FAULT] = "Instruction page fault",
    [RISCV_SCAUSE_LOAD_PAGE_FAULT] = "Load page fault",
    [RISCV_SCAUSE_STORE_PAGE_FAULT] = "Store page fault"};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Handles exceptions from supervisor mode. Ensures that each
 * specfic cause is handled appropriately. Creates a panic.
 * @param cause Exception code
 * @param tfr Pointer to the trap frame
 * @return None
 */
void handle_smode_exception(unsigned int cause, struct trap_frame* tfr) {
    const char* name = NULL;
    char msgbuf[80];

    if (0 <= cause && cause < sizeof(excp_names) / sizeof(excp_names[0])) name = excp_names[cause];

    if (name != NULL) {
        switch (cause) {
            case RISCV_SCAUSE_LOAD_PAGE_FAULT:
            case RISCV_SCAUSE_STORE_PAGE_FAULT:
            case RISCV_SCAUSE_INSTR_PAGE_FAULT:
            case RISCV_SCAUSE_LOAD_ADDR_MISALIGNED:
            case RISCV_SCAUSE_STORE_ADDR_MISALIGNED:
            case RISCV_SCAUSE_INSTR_ADDR_MISALIGNED:
            case RISCV_SCAUSE_LOAD_ACCESS_FAULT:
            case RISCV_SCAUSE_STORE_ACCESS_FAULT:
            case RISCV_SCAUSE_INSTR_ACCESS_FAULT:
                kprintf("Register dump:\n");
                kprintf("  a0=%016lx a1=%016lx a2=%016lx a3=%016lx\n", tfr->a0, tfr->a1, tfr->a2, tfr->a3);
                kprintf("  a4=%016lx a5=%016lx a6=%016lx a7=%016lx\n", tfr->a4, tfr->a5, tfr->a6, tfr->a7);
                kprintf("  t0=%016lx t1=%016lx t2=%016lx t3=%016lx\n", tfr->t0, tfr->t1, tfr->t2, tfr->t3);
                kprintf("  t4=%016lx t5=%016lx t6=%016lx\n", tfr->t4, tfr->t5, tfr->t6);
                kprintf("  sp=%p gp=%p tp=%p ra=%p\n", tfr->sp, tfr->gp, tfr->tp, tfr->ra);
                kprintf("  sstatus=%016lx (SPP=%d)\n", tfr->sstatus, (int)((tfr->sstatus >> 8) & 1));
                snprintf(msgbuf, sizeof(msgbuf), "%s at %p for %p in S mode", name,
                         (void*)tfr->sepc, (void*)csrr_stval());
                break;
            default:
                snprintf(msgbuf, sizeof(msgbuf), "%s at %p in S mode", name, (void*)tfr->sepc);
        }
    } else {
        snprintf(msgbuf, sizeof(msgbuf), "Exception %d at %p in S mode", cause, (void*)tfr->sepc);
    }

    panic(msgbuf);
}

/**
 * @brief Handles exceptions from user mode to ensure proper system functionality. The handler
 * redirects certain exceptions to support lazy allocation and system calls from user mode.
 * Otherwise, the handler exits the current process after providing information on where the
 * exception occurred.
 * @param cause Exception code
 * @param tfr Trap frame pointer
 * @return None
 */
void handle_umode_exception(unsigned int cause, struct trap_frame* tfr) {
    if (cause == RISCV_SCAUSE_ECALL_FROM_UMODE) {
        handle_syscall(tfr);
        return;
    }
    
    if (cause == RISCV_SCAUSE_LOAD_PAGE_FAULT ||
        cause == RISCV_SCAUSE_STORE_PAGE_FAULT ||
        cause == RISCV_SCAUSE_INSTR_PAGE_FAULT) {
        
        uintptr_t vma = csrr_stval();
        if (handle_umode_page_fault(tfr, vma)) {
            return;
        }
    }

    // Otherwise, kill process
    kprintf("Unhandled user exception: %d at %p stval=%p\n", cause, tfr->sepc, csrr_stval());
    process_exit();
}