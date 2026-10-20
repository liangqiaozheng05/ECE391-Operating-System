/*! @file ramdisk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief Memory-backed storage implementation
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef RAMDISK_DEBUG
#define DEBUG
#endif

#ifdef RAMDISK_TRACE
#define TRACE
#endif

#include <stddef.h>

#include "console.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "uio.h"

#ifndef RAMDISK_NAME
#define RAMDISK_NAME "ramdisk"
#endif

// INTERNAL TYPE DEFINITIONS
//

/**
 * @brief Storage device backed by a block of memory. Allows modification of the backing memory
 * block.
 */
struct ramdisk {
    struct storage storage;  ///< Storage struct of memory storage
    void *buf;               ///< Block of memory
    size_t size;             ///< Size of memory block
};

// INTERNAL FUNCTION DECLARATIONS
//

static int ramdisk_open(struct storage *sto);
static void ramdisk_close(struct storage *sto);
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt);
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

// INTERNAL GLOBAL CONSTANTS
//

static const struct storage_intf ramdisk_intf = {
    .blksz = 1,
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL,  // Read-only storage (blob data in .rodata)
    .cntl = &ramdisk_cntl};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Creates and registers a memory-backed storage device
 * @return None
 */
void ramdisk_attach() {
    // External symbols from linker script for embedded blob data
    extern char _kimg_blob_start[], _kimg_blob_end[];

    // FIXME
    struct ramdisk *rd = kmalloc(sizeof(struct ramdisk));
    if (rd == NULL) {
        kprintf("ramdisk_attach allocate ramdisk structure failed\n");
        return;
    }

    rd->buf = (void *)_kimg_blob_start;
    rd->size = (size_t)(_kimg_blob_end - _kimg_blob_start);
    rd->storage.intf = &ramdisk_intf;

    register_device(RAMDISK_NAME, DEV_STORAGE, &rd->storage);

    debug("ramdisk_attach: registered ramdisk with size %lu bytes", rd->size);
    return;
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Opens the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 * @return 0 on success
 */
static int ramdisk_open(struct storage *sto) {
    // FIXME
    if (sto == NULL) {
        return -EINVAL;
    }
    
    debug("ramdisk_open: opened ramdisk device");
    return 0;
}

/**
 * @brief Closes the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 */
static void ramdisk_close(struct storage *sto) {
    // FIXME
    if (sto == NULL) {
        return;
    }
    
    debug("ramdisk_close: closed ramdisk device");
    return;
}

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf.
 * @details Performs proper bounds checks, then copies data from memory block to passed buffer
 * @param sto Storage struct pointer for memory storage
 * @param pos Position in storage to read from
 * @param buf Buffer to copy data from memory to
 * @param bytecnt Number of bytes to read from memory
 * @return Number of bytes successfully read
 */
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt) {
    // FIXME
    if (sto == NULL || buf == NULL) {
        return -EINVAL;
    }

    struct ramdisk *rd = (struct ramdisk *)sto;

    // Check if position is within bounds
    if (pos >= rd->size) {
        return 0;  // Position beyond end of device
    }

    // Calculate actual bytes to read (don't read past end)
    unsigned long bytes_to_read = bytecnt;
    if (pos + bytecnt > rd->size) {
        bytes_to_read = rd->size - pos;
    }

    // Copy data from ramdisk buffer to output buffer
    memcpy(buf, (char *)rd->buf + pos, bytes_to_read);

    debug("ramdisk_fetch: read %lu bytes from position %llu", bytes_to_read, pos);
    return bytes_to_read;
}

/**
 * @brief _cntl_ functions for memory storage.
 * @details Memory storage supports basic control operations
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage struct pointer for memory storage
 * @param cmd command to execute. ramdisk should support FCNTL_GETEND.
 * @param arg Argument for commands
 * @return 0 on success, error on failure or unsupported command
 */
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg) {
    // FIXME
    if (sto == NULL) {
        return -EINVAL;
    }

    struct ramdisk *rd = (struct ramdisk *)sto;

    switch (cmd) {
        case FCNTL_GETEND:
            if (arg == NULL) {
                return -EINVAL;
            }
            *(unsigned long long *)arg = rd->size;
            debug("ramdisk_cntl: FCNTL_GETEND returned size %lu", rd->size);
            return 0;
        
        case FCNTL_GETPOS:
            debug("ramdisk_cntl: unrelated command %d", cmd);
            return -ENOTSUP;

        case FCNTL_SETPOS:
            // These commands are not supported for ramdisk
            debug("ramdisk_cntl: unrelated command %d", cmd);
            return -ENOTSUP;


        default:
            debug("ramdisk_cntl: unsupported command %d", cmd);
            return -ENOTSUP;
    }
}
