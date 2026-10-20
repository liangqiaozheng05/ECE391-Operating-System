/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
#ifdef PROCESS_TRACE
#define TRACE
#endif

/*!
 * @brief Enables debug messages for process.c
 */
#ifdef PROCESS_DEBUG
#define DEBUG
#endif

/*!
 *@brief trap frame size from trap.h
 */
#ifndef TFRSZ
#define TFRSZ (34 * 8)
#endif

#include "process.h"

#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"
#include "console.h" // for kprintf
#include "uioimpl.h"

// COMPILE-TIME PARAMETERS
//

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void* stack, int argc, char** argv);

static void fork_func(struct condition* forked, struct trap_frame* tfr);

// INTERNAL GLOBAL VARIABLES
//

/*!
 * @brief The main user process struct
 */
static struct process main_proc;

static struct process* proctab[NPROC] = {&main_proc};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    
    main_proc.uiotab[0] = create_console_uio();
    main_proc.uiotab[1] = create_console_uio();
    main_proc.uiotab[2] = create_console_uio();
    
    procmgr_initialized = 1;
}

// Helper to duplicate string in kernel memory
static char *kstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *new_s = kmalloc(len);
    if (!new_s) return NULL;
    memcpy(new_s, s, len);
    return new_s;
}

int process_exec(struct uio* exefile, int argc, char** argv) {
    struct process* proc;
    struct trap_frame tfr;
    void* stack_page;
    int stksz;
    int result;
    void (*entry)(void);
    char **kargv = NULL;

    trace("%s(argc=%d) in thread %d", __func__, argc, running_thread());
    
    // Get current process
    proc = running_thread_process();
    if (proc == NULL) {
        kprintf("No process associated with running thread\n");
        return -EINVAL;
    }
    
    // 1. Deep copy argv to kernel space BEFORE unmapping user memory
    // Because argc/argv reside in user memory which will be gone after reset_active_mspace()
    if (argc > 0 && argv != NULL) {
        if (argc > 1024) return -EINVAL; 

        kargv = kcalloc(argc + 1, sizeof(char*));
        if (!kargv) return -ENOMEM;

        for (int i = 0; i < argc; i++) {
            // Note: argv[i] is a user pointer. We can access it because SUM is set.
            kargv[i] = kstrdup(argv[i]); 
            if (!kargv[i]) {
                // cleanup on error
                for (int j=0; j<i; j++) kfree(kargv[j]);
                kfree(kargv);
                return -ENOMEM;
            }
        }
        kargv[argc] = NULL;
    }

    //Unmap any existing user memory mappings
    reset_active_mspace();
    
    // Allocate stack page for the user process (physical page)
    stack_page = alloc_phys_page();
    if (stack_page == NULL) {
        kprintf("Failed to allocate stack page\n");
        // cleanup kargv
        if (kargv) {
             for (int i = 0; i < argc; i++) kfree(kargv[i]);
             kfree(kargv);
        }
        process_exit();
        return -ENOMEM;
    }
    memset(stack_page, 0, PAGE_SIZE);
    
    // Build the user stack with kargv (from kernel memory)
    stksz = build_stack(stack_page, argc, kargv);
    
    // Free kargv now that we copied it to the new stack page
    if (kargv) {
        for (int i = 0; i < argc; i++) kfree(kargv[i]);
        kfree(kargv);
    }

    if (stksz < 0) {
        kprintf("Failed to build stack: %d\n", stksz);
        free_phys_page(stack_page);
        process_exit();
        return stksz;
    }
    
    // Map the stack page to the top of user memory
    // User stack is at [UMEM_END_VMA - PAGE_SIZE, UMEM_END_VMA)
    void* mapped = map_page(UMEM_END_VMA - PAGE_SIZE, stack_page, 
                            PTE_R | PTE_W | PTE_U);
    if (mapped == NULL) {
        kprintf("Failed to map stack page\n");
        free_phys_page(stack_page);
        process_exit();
        return -ENOMEM;
    }
    
    //Load the ELF executable from the UIO interface
    entry = NULL;
    result = elf_load(exefile, &entry);
    
    if (result < 0) {
        kprintf("Failed to load executable: %d\n", result);
        free_phys_page(stack_page);
        process_exit();
        return result;
    }
    
    // Close the executable file after loading and change to user mode
    uio_close(exefile);
    
    memset(&tfr, 0, sizeof(tfr));
    
    // Set program counter & stack pointer & a0 & a1
    tfr.sepc = (void *) entry;
    tfr.sp = (void *) (UMEM_END_VMA - stksz); 
    tfr.a0 = argc;
    tfr.a1 = (long int) tfr.sp;
    
    // Set up sstatus for user mode
    // SPP = 0 (return to user mode) - bit 8
    // SPIE = 1 (enable interrupts when returning to user mode) - bit 5
    // We need to explicitly clear SPP and set SPIE
    tfr.sstatus = (1UL << 5);  // SPIE bit
    tfr.sstatus &= ~(1UL << 8); // Clear SPP bit to ensure user mode
    void* stack_anchor = running_thread_stack_base();
    void* sscratch_value = stack_anchor;
 
    // Jump to user mode - this function does not return
    trap_frame_jump(&tfr, sscratch_value);
    
    // Should never reach here
    halt_failure();
    return 0;
}

int process_fork(const struct trap_frame* tfr) {
    struct process* parent_proc;
    struct process* child_proc;
    struct trap_frame* child_tfr;
    struct condition forked;
    int child_tid;
    int i;
    
    trace("%s() in thread %d", __func__, running_thread());
    
    // Get parent process
    parent_proc = running_thread_process();
    if (parent_proc == NULL) {
        return -EINVAL;
    }
    
    // Find free slot in process table
    child_proc = NULL;
    for (i = 0; i < NPROC; i++) {
        if (proctab[i] == NULL) {
            child_proc = kcalloc(1, sizeof(struct process));
            if (child_proc == NULL) {
                return -ENOMEM;
            }
            proctab[i] = child_proc;
            break;
        }
    }
    
    if (child_proc == NULL) {
        return -EMPROC;  // No free process slots
    }
    
    // Copy parent's UIO table and increment reference counts
    for (i = 0; i < PROCESS_UIOMAX; i++) {
        child_proc->uiotab[i] = parent_proc->uiotab[i];
        if (child_proc->uiotab[i] != NULL) {
            uio_addref(child_proc->uiotab[i]);  // Increment reference count
        }
    }
    
    // Allocate trap frame for child
    child_tfr = kmalloc(sizeof(struct trap_frame));
    if (child_tfr == NULL) {
        kfree(child_proc);
        proctab[i] = NULL;
        return -ENOMEM;
    }

    memcpy(child_tfr, tfr, sizeof(struct trap_frame));
    
    // Set child's return value to 0
    child_tfr->a0 = 0;
    
    // Initialize condition variable for synchronization
    condition_init(&forked, "fork_sync");
    
    // Spawn child thread
    child_tid = spawn_thread("forked_process", (void(*)(void))fork_func, &forked, child_tfr);
    
    if (child_tid < 0) {
        kfree(child_tfr);
        kfree(child_proc);
        proctab[i] = NULL;
        return child_tid;
    }
    
    // Set child process fields
    child_proc->tid = child_tid;
    child_proc->mtag = clone_active_mspace();
    
    // Set process pointer in child thread
    thread_set_process(child_tid, child_proc);
    
    condition_wait(&forked);
    
    // Free the trap frame and return TID
    kfree(child_tfr);
    return child_tid;
}

/** \brief
 *
 *
 *  Discard memory space, close your associated uio, free the memory you're supposed to free.
 *
 *
 */
void process_exit(void) {
    struct process* proc;
    
    // Get current process
    proc = running_thread_process();
    if (proc == NULL) {
        // No process to clean up, just exit thread
        running_thread_exit();
    }
    else{
    // Close all open UIO objects
        debug("Closing related UIO %d", proc->tid);
        for (int i = 0; i < PROCESS_UIOMAX; i++) {
            if (proc->uiotab[i] != NULL) {
                uio_close(proc->uiotab[i]);
                proc->uiotab[i] = NULL;
            }
        }
    
        // Discard the memory space (free user pages)
        if (proc->mtag != 0) {
            discard_active_mspace();
        }
    
        // Remove from process table (if not main process)
        if (proc != &main_proc) {
            for (int i = 0; i < PROCESS_UIOMAX; i++) {
                if (proctab[i] == proc) {
                    proctab[i] = NULL;
                    break;
                }
            }
            kfree(proc);
        }
    
        // Exit the thread
        running_thread_exit();
    }
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * \brief Builds the initial user stack for a new process.
 *
 * Builds the stack for a new process, including the argument vector (\p argv)
 * and the strings it points to. Note that \p argv must contain \p argc + 1
 * elements (the last one is a NULL pointer).
 *
 * Remember to round the final stack size up to a multiple of 16 bytes
 * (RISC-V ABI requirement).
 *
 * \param[in,out] stack  Pointer to the stack page (destination buffer).
 * \param[in]     argc   Number of arguments in \p argv.
 * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
 *                       \p argv[argc] must be NULL.
 *
 * \return Size of the stack page on success; negative error code on failure.
 */
int build_stack(void* stack, int argc, char** argv) {
    size_t stksz, argsz;
    uintptr_t* newargv;
    char* p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc) return -ENOMEM;

    stksz = (argc + 1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i]) + 1;
        if (PAGE_SIZE - stksz < argsz) return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert(stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv + argc + 1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i]) + 1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

/**
 * \brief Function to be executed by the child process after fork.
 * This is a very beautiful function.
 * Tell the parent process that it is done with the trap frame, then jumps to user space (hint:
 * which function should we use?)
 *
 * \param[in] done  Pointer to a condition variable to signal parent
 * \param[in] tfr   Pointer to a trap frame
 *
 * \return NONE (very important, this is a hint)
 */
void fork_func(struct condition* done, struct trap_frame* tfr) {
    condition_broadcast(done);
    
    // Calculate sscratch value for the child process
    void* stack_anchor = running_thread_stack_base();
    void* sscratch_value = stack_anchor;
    
    // Jump to user space - this function does not return
    trap_frame_jump(tfr, sscratch_value);
    
    // Should never reach here
    halt_failure();
}

static long console_uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    (void)uio;
    const char* cbuf = (const char*)buf;
    for (unsigned long i = 0; i < buflen; i++) {
        kputc(cbuf[i]);
    }
    return (long)buflen;
}

static long console_uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    (void)uio;
    char* cbuf = (char*)buf;
    if (bufsz > 0) {
        cbuf[0] = kgetc();
        return 1;
    }
    return 0;
}

static void console_uio_close(struct uio* uio) {
    kfree(uio);
}

static int console_uio_cntl(struct uio* uio, int op, void* arg) {
    (void)uio;
    (void)op;
    (void)arg;
    return -ENOTSUP;
}

static const struct uio_intf console_uio_intf = {
    .close = console_uio_close,
    .read  = console_uio_read,
    .write = console_uio_write,
    .cntl  = console_uio_cntl,
};

struct uio* create_console_uio(void) {
    struct uio* uio = kmalloc(sizeof(struct uio));
    if (!uio) return NULL;
    return uio_init1(uio, &console_uio_intf);
}
