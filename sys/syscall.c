/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysfsdelete(const char *path);
static int sysfscreate(const char *path);

static int sysopen(int fd, const char *path);
static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysfcntl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);
static int sysuiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Initiates syscall present in trap frame struct and stores the return address into the sepc
 * @details sepc will be used to return back to program execution after interrupt is handled and
 * sret is called
 * @param tfr pointer to trap frame struct
 * @return void
 */

void handle_syscall(struct trap_frame *tfr) {
    // Continue from the next instruction since each instruction is 4 bytes
    // We need to update BOTH trap frame and CSR because trap return reloads from frame
    void *new_sepc = (char*)tfr->sepc + 4;
    tfr->sepc = new_sepc;
    csrw_sepc(new_sepc);
    
    // Enable SUM (Supervisor User Memory access) bit to allow kernel to access user memory
    csrs_sstatus(RISCV_SSTATUS_SUM);
    
    // Execute call and return the value to a0
    tfr->a0 = syscall(tfr);
    
    // Restore original sstatus by clearing SUM bit
    csrc_sstatus(RISCV_SSTATUS_SUM);
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Calls specified syscall and passes arguments
 * @details Function uses register a7 to determine syscall number and arguments are passed in from
 * a0-a5 depending on the function
 * @param tfr pointer to trap frame struct
 * @return result of syscall
 */

int64_t syscall(const struct trap_frame *tfr) {
    switch (tfr->a7) { // syscall number in register a7
    case SYSCALL_EXIT:
        return sysexit();

    case SYSCALL_EXEC:
        return sysexec(tfr->a0, tfr->a1, (char**)tfr->a2);

    case SYSCALL_FORK:
        return sysfork(tfr);
    
    case SYSCALL_WAIT:
        return syswait(tfr->a0);

    case SYSCALL_PRINT:
        return sysprint((const char*)tfr->a0);

    case SYSCALL_USLEEP:
        return sysusleep(tfr->a0);

    case SYSCALL_FSCREATE:
        return sysfscreate((const char*)tfr->a0);

    case SYSCALL_FSDELETE:
        return sysfsdelete((const char*)tfr->a0);

    case SYSCALL_OPEN:
        return sysopen(tfr->a0, (const char*)tfr->a1);

    case SYSCALL_CLOSE:
        return sysclose(tfr->a0);

    case SYSCALL_READ:
        return sysread(tfr->a0, (void*)tfr->a1, (size_t)tfr->a2);

    case SYSCALL_WRITE:
        return syswrite(tfr->a0, (const void*)tfr->a1, (size_t)tfr->a2);

    case SYSCALL_FCNTL:
        return sysfcntl(tfr->a0, tfr->a1, (void*)tfr->a2);

    case SYSCALL_PIPE:
        return syspipe((int*)tfr->a0, (int*)tfr->a1);

    case SYSCALL_UIODUP:
        return sysuiodup(tfr->a0, tfr->a1);

    default:
        return -ENOTSUP;
    }
}

/**
 * @brief Calls process exit
 * @return void
 */

int sysexit(void) { 
    process_exit(); // This function should not return
    return -EINVAL;
}

/**
 * @brief Executes new process given a executable and arguments
 * @details Valid fd checks, get current process struct, close fd being executed, finally calls
 * process_exec with arguments and executable io "file"
 * @param fd file descripter idx
 * @param argc number of arguments in argv
 * @param argv array of arguments for multiple args
 * @return result of process_exec, else -EBADFD on invalid file descriptors
 */

int sysexec(int fd, int argc, char **argv) {
    struct process *proc = current_process();

    // Validate fd
    if (fd < 0 || fd >= PROCESS_UIOMAX || proc->uiotab[fd] == NULL) {
        return -EBADFD;
    }
    struct uio *exefile = proc->uiotab[fd];

    // Always closes io object
    proc->uiotab[fd] = NULL;

    process_exec(exefile, argc, argv); // This function should not return
    
    return -EINVAL;
}

/**
 * @brief Forks a new child process using process_fork
 * @param tfr pointer to the trap frame
 * @return result of process_fork
 */

int sysfork(const struct trap_frame *tfr) {
    return process_fork(tfr);
}

/**
 * @brief Sleeps till a specified child process completes
 * @details Calls thread_join with the thread id the process wishes to wait for
 * @param tid thread_id
 * @return result of thread_join else invalid on invalid thread id
 */

int syswait(int tid) { // Won't be tested until CP3

    
    if (0 <= tid) {
        return thread_join(tid);
    }
    else {
        return -EINVAL; // Invalid thread id
    }
}

/**
 * @brief Prints to console via kprintf
 * @details Validates that msg string is valid via validate_vstr and pages are mapped, calls kprintf
 * on current running process
 * @param msg string msg in userspace
 * @return 0 on sucess else error from validate_vstr
 */

int sysprint(const char *msg) {
    // Trace the function
    trace("%s(msg=%p)", __func__, msg);
    
    // Validate that msg string is valid via validate_vstr and pages are mapped
    int isvalid;
    isvalid = validate_vstr(msg, PTE_U);
    if (isvalid != 0){ // Not success
        return isvalid;
    }

    // Call kprintf, c.f. Slides
    kprintf("Thread <%s:%d> says: %s\n", thread_name(running_thread()), running_thread(), msg);

    return 0;
}

/**
 * @brief Sleeps process till specificed amount of time has passed
 * @details Creates alarm struct, inits struct with name usleep, which sets the current time via the
 * rd_time() function, taking values from the csr, makes frequency calcuation to determine us has
 * passed before waking process
 * @param us time in us for process to sleep
 * @return 0
 */

int sysusleep(unsigned long us) {
    // Trace the function
    trace("%s(us=%lu)", __func__, us);

    // Call sleep_us
    sleep_us(us);
    return 0;
}

/**
 * @brief Creates a new file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * create_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfscreate(const char *path) { 
    trace("%s(path=%p)", __func__, path); // Trace the function

    // Validate the path
    int valid = validate_vstr(path, PTE_U);
    if (valid != 0) {
        return valid; // Return error
    }

    // Parse the path to get mountpoint name and file name
    char *mpname, *flname;
    int parse = parse_path((char*)path, &mpname, &flname);
    if (parse != 0) {
        return parse; // Return error
    }

    // Call create_file and return
    return create_file(mpname, flname);
}

/**
 * @brief Deletes a file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * delete_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfsdelete(const char *path) { 
    trace("%s(path=%p)", __func__, path); // Trace the function

    // Validate the path
    int valid = validate_vstr(path, PTE_U);
    if (valid != 0) {
        return valid; // Return error
    }

    // Parse the path to get mountpoint name and file name
    char *mpname, *flname;
    int parse = parse_path((char*)path, &mpname, &flname);
    if (parse != 0) {
        return parse; // Return error
    }

    // Call delete_file and return
    return delete_file(mpname, flname);
}

/**
 * @brief Opens a file or device of specified fd for given process
 * @details gets current process, allocates file descriptor (if fd = -1) or uses valid file
 * descriptor given, validates and parses user provided path, calls open_file
 * @param fd file descriptor number
 * @param path User provided path string
 * @return fd number if sucessful else return error that occured -EMFILE or -EBADFD
 */

int sysopen(int fd, const char *path) { 


    struct process *proc = current_process();
    
    // Validate the path
    int valid = validate_vstr(path, PTE_U);
    if (valid != 0) {
        return valid;
    }
    
    // Parse the path to get mountpoint and filename
    char *mpname, *flname;
    int parse = parse_path((char*)path, &mpname, &flname);
    if (parse != 0) {
        return parse; // Return error
    }
    
    // Validate fd
    if (fd >= 0) {
        if (fd >= PROCESS_UIOMAX || proc->uiotab[fd] != NULL) {
            return -EBADFD; // FD out of range or already in use
        }
    }

    // Find available file descriptor
    int target_fd = fd;
    if (fd < 0) { // If fd < 0, requests next available descriptor number
        for (target_fd = 0; target_fd < PROCESS_UIOMAX; target_fd++) {
            if (proc->uiotab[target_fd] == NULL) {
                break;
            }
        }
        if (target_fd >= PROCESS_UIOMAX) {
            return -EMFILE; // Too many open files
        }
    }
    
    // Open the file
    struct uio *io;
    int open = open_file(mpname, flname, &io);
    if (open != 0) {
        return open; // Return error
    }
    
    // Assign to file descriptor
    proc->uiotab[target_fd] = io;
    
    return target_fd;
}

/**
 * @brief Closes file or device of specified fd for given process
 * @details gets current process, calls close function of the io, deallocates the file descriptor
 * @param fd file descriptor
 * @return 0 on success, error on invalid file descriptor or empty file descriptor
 */

int sysclose(int fd) { 
    // Trace the function
    trace("%s(fd=%d)", __func__, fd);

    struct process *proc = current_process();
    
    // Validate fd
    if (fd < 0 || fd >= PROCESS_UIOMAX || proc->uiotab[fd] == NULL) {
        return -EBADFD;
    }

    // Close the uio object
    struct uio *io = proc->uiotab[fd];
    uio_close(io);

    // Deallocate hte file descriptor
    proc->uiotab[fd] = NULL;
    return 0; 
}

/**
 * @brief Calls read function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call ioread with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param bufsz number of bytes to be read
 * @return number of bytes read
 */

long sysread(int fd, void *buf, size_t bufsz) { 
    // Trace the function
    trace("%s(fd=%d, buf=%p, bufsz=%zu)", __func__, fd, buf, bufsz);
    
    struct process *proc = current_process();
    
    // Validate fd
    if (fd < 0 || fd >= PROCESS_UIOMAX || proc->uiotab[fd] == NULL) {
        return -EBADFD;
    }
    
    // Validate buf
    int valid = validate_vptr(buf, bufsz, PTE_U | PTE_W); // The buffer should be writable
    if (valid != 0) {
        return valid;
    }
    
    // Call uio_read and return
    struct uio *io = proc->uiotab[fd];
    return uio_read(io, buf, bufsz); 
}

/**
 * @brief Calls write function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call iowrite with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param len number of bytes to be written
 * @return number of bytes written
 */

long syswrite(int fd, const void *buf, size_t len) { 
    // Trace the function
    trace("%s(fd=%d, buf=%p, len=%zu)", __func__, fd, buf, len);
    
    struct process *proc = current_process();
    
    // Validate fd
    if (fd < 0 || fd >= PROCESS_UIOMAX || proc->uiotab[fd] == NULL) {
        return -EBADFD;
    }
    
    // Validate buf
    int valid = validate_vptr((void*)buf, len, PTE_U | PTE_R); // The buffer should be readable
    if (valid != 0) {
        return valid;
    }
    
    // Call uio_write and return
    struct uio *io = proc->uiotab[fd];
    return uio_write(io, buf, len);
}

/**
 * @brief Calls device input output commands for a given device instance
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * ensure that fcntl type exists, validate argument pointer, issue fcntl
 * @param fd file descriptor number
 * @param cmd selection of fcntl
 * @param arg pointer to arguments
 * @return number of bytes written
 */

int sysfcntl(int fd, int cmd, void *arg) { 
    // Trace the function
    trace("%s(fd=%d, cmd=%d, arg=%p)", __func__, fd, cmd, arg);
    
    struct process *proc = current_process();
    
    // Validate fd
    if (fd < 0 || fd >= PROCESS_UIOMAX || proc->uiotab[fd] == NULL) {
        return -EBADFD;
    }
    
    // Validate argument if provided (depends on command)
    if (arg != NULL) {
        int valid = validate_vptr(arg, sizeof(void*), PTE_U | PTE_R); // Should be readable
        if (valid != 0) {
            return valid;
        }
    }
    
    // Call uio_cntl and return
    struct uio *io = proc->uiotab[fd];
    return uio_cntl(io, cmd, arg);
}

/**
 * @brief Creates a pipe for the current process
 * @details The function retrieves the current process. If either the write or read descriptor
 * pointer stores a negative value, an unused descriptor is assigned. If both file descriptors are
 * unused and valid, the function connects them via create_pipe function.
 * @param wfdptr pointer to write file descriptor
 * @param rfdptr pointer to read file descriptor
 * @return 0 on success. Else, negative error code on invalid file descriptor, or if a file
 * descriptor is already in use, or if no descriptors are found available.
 */
int syspipe(int *wfdptr, int *rfdptr) {
    // Trace the function
    trace("%s(wfdptr=%p, rfdptr=%p)", __func__, wfdptr, rfdptr);
    
    struct process *proc = current_process();
    
    // Validate pointers
    if (validate_vptr(wfdptr, sizeof(int), PTE_U | PTE_W) != 0) {
        return -EINVAL;
    }
    if (validate_vptr(rfdptr, sizeof(int), PTE_U | PTE_W) != 0) {
        return -EINVAL;
    }
    
    // Get initial FD values from user space
    int write_fd = *wfdptr;
    int read_fd = *rfdptr;
    
    // Find available file descriptors
    if (write_fd < 0) {
        for (write_fd = 0; write_fd < PROCESS_UIOMAX; write_fd++) {
            if (proc->uiotab[write_fd] == NULL) {
                break;
            }
        }
        if (write_fd >= PROCESS_UIOMAX) {
            return -EMFILE;
        }
    } else {
        if (write_fd >= PROCESS_UIOMAX || proc->uiotab[write_fd] != NULL) {
            return -EBADFD;
        }
    }
    
    if (read_fd < 0) {
        for (read_fd = 0; read_fd < PROCESS_UIOMAX; read_fd++) {
            if (read_fd != write_fd && proc->uiotab[read_fd] == NULL) {
                break;
            }
        }
        if (read_fd >= PROCESS_UIOMAX) {
            return -EMFILE;
        }
    } else {
        if (read_fd >= PROCESS_UIOMAX || read_fd == write_fd || proc->uiotab[read_fd] != NULL) {
            return -EBADFD;
        }
    }
    
    // Create the pipe
    struct uio *write_uio = NULL;
    struct uio *read_uio = NULL;
    create_pipe(&write_uio, &read_uio);
    
    if (write_uio == NULL || read_uio == NULL) {
        return -ENOMEM;
    }
    
    // Assign to file descriptors
    proc->uiotab[write_fd] = write_uio;
    proc->uiotab[read_fd] = read_uio;
    
    // Write back to user space
    *wfdptr = write_fd;
    *rfdptr = read_fd;
    
    return 0;
}

/**
 * @brief Duplicates a file description
 * @details Allocates a new file descriptor that refers to the same open _uio_ as the descriptor
 * _oldfd_. Increments the _refcnt_ if successful.
 * @param oldfd old file descriptor number
 * @param newfd new file descriptor number
 * @return fd number if sucessful else return error on invalid file descriptor or empty file
 * descriptor
 */

int sysuiodup(int oldfd, int newfd) {
    // Trace the function
    trace("%s(oldfd=%d, newfd=%d)", __func__, oldfd, newfd);

    struct process *proc = current_process();

    // Validate oldfd
    if (oldfd < 0 || oldfd >= PROCESS_UIOMAX || proc->uiotab[oldfd] == NULL) {
        return -EBADFD;
    }

    // Validate newfd
    if (newfd >= PROCESS_UIOMAX || proc->uiotab[newfd] != NULL) {
        return -EBADFD; // FD not in range or already in use
    }

    // Find target file descriptor
    int target_fd = newfd;
    if (newfd < 0) {
        // Find first available fd
        for (target_fd = 0; target_fd < PROCESS_UIOMAX; target_fd++) {
            if (proc->uiotab[target_fd] == NULL) {
                break; // If newfd < 0, picks first available descriptor number
            }
        }
        if (target_fd >= PROCESS_UIOMAX) {
            return -EMFILE; // Too many open files
        }
    }

    // Duplicate the uio object
    struct uio *old_uio = proc->uiotab[oldfd];
    
    // Increment the refcnt
    int refcnt = uio_addref(old_uio);
    if (refcnt <= 0) {
        return -EINVAL; // Fail
    }

    // Assign the same uio object to the new file descriptor
    proc->uiotab[target_fd] = old_uio;

    return target_fd;
}
