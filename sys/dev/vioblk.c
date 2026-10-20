/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include <stdint.h> // Added by xinyi39

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

// By xinyi39 BEGIN
#define VQ_SIZE 128
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_UNSUPP 2

// Driver-private Data Structures
//

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct vioblk_reqctx {
    struct virtio_blk_req_hdr hdr;
    uint8_t status;
    struct condition done;
    volatile int finished;
};

struct vioblk_storage {
    struct storage sto;     // MUST be first, to fit in the interface

    volatile struct virtio_mmio_regs *regs;
    int irqno;
    unsigned blksz;
    unsigned long long capacity; // Bytes

    // Virtual queue
    struct virtq_desc *desc;   // Descriptor table
    struct virtq_avail *avail; // Available
    struct virtq_used *used;
    uint16_t qid;
    uint16_t qsize;
    uint16_t last_used_idx;

    struct vioblk_reqctx ioctx; // One-request-each-time version
    struct lock qlock;          // Serialize submissions

    int opened; // 1 = opened, 0 = closed
};
// By xinyi39 END

// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
 * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
 * device.
 * @param sto Storage IO struct for the storage device
 * @return Return 0 on success or negative error code if error. If the given sto is already opened,
 * then return -EBUSY.
 */
static int vioblk_storage_open(struct storage* sto);

/**
 * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
 * the given sto is not opened, this function does nothing.
 * @param sto Storage IO struct for the storage device
 * @return None
 */
static void vioblk_storage_close(struct storage* sto);

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
 * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
 * data has been populated in block buffer cache, and then writes that data out to buf. Thread
 * sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the read within the VirtIO device
 * @param buf A pointer to the buffer to fill with the read data
 * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
 * device should not change. You should only overwrite existing data. Write should also not create
 * any new files. Achieves this by filling up the block buffer cache and then setting the
 * appropriate registers to request the disk write the contents of the cache to the specified block
 * location. Thread sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the write within the VirtIO device
 * @param buf A pointer to the buffer with the data to write
 * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
 * @return The number of bytes written to the device, or negative error code if error
 */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions on the VirtIO block device.
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage IO struct for the storage device
 * @param op Operation to execute. vioblk should support FCNTL_GETEND.
 * @param arg Argument specific to the operation being performed
 * @return Status code on the operation performed
 */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
 * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
 * call this function.
 * @param irqno The interrupt request number for the VirtIO device
 * @param aux A generic pointer for auxiliary data.
 * @return None
 */
static void vioblk_isr(int irqno, void* aux);

// By xinyi39 BEGIN
// Helper function
/**
 * @brief A helper function for vioblk_storage_fetch and vioblk_storage_store.
 * @param vbd vioblk struct for the storage device
 * @param pos The starting position for the read/write within the VirtIO device
 * @param buf A pointer to the buffer with the data to read/write
 * @param bytecnt The number of bytes to read from/write to the VirtIO device from the buffer
 * @param is_write A boolean controls whether it is reading or writing. 1 = write, 0 = read
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_io(struct vioblk_storage* vbd, uint64_t pos, void* buf, uint32_t bytecnt, int is_write);

// Constant interface template
/*
 * I used to stick to Allman style bracket break, but for this project I won't use it, for 
 * overall code tidiness.  -- xinyi39
 */
static const struct storage_intf vioblk_intf_template = {
    .blksz = 512,  // will be changed at attach
    .open = vioblk_storage_open,
    .close = vioblk_storage_close,
    .fetch = vioblk_storage_fetch,
    .store = vioblk_storage_store,
    .cntl = vioblk_storage_cntl,
};
// By xinyi39 END

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
 * @brief Initializes virtio block device with the necessary IO operation functions and sets the
 * required feature bits.
 * @param regs Memory mapped register of Virtio
 * @param irqno Interrupt request number of the device
 * @return None
 */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // By xinyi39 BEGIN
    // Allocate device
    vbd = (struct vioblk_storage*)kcalloc(1, sizeof(struct vioblk_storage));
    if (!vbd) {
        kprintf("%p: VIOBLK kcalloc failed", regs); // Allocation error
        return;
    }
    
    // Capacity is in 512B sectors, so the number of bytes is sectors * 512
    uint64_t capacity_bytes = regs->config.blk.capacity * 512ULL;

    vbd->regs = regs;
    vbd->irqno = irqno;
    vbd->blksz = blksz;
    vbd->capacity = capacity_bytes;
    vbd->qid = 0;

    // Initialize lock and condition
    lock_init(&vbd->qlock);
    condition_init(&vbd->ioctx.done, "vioblk-io");
    vbd->ioctx.finished = 0;
    vbd->opened = 0;
    
    // Build the queue, and the size will be min(VQ_SIZE, queue_num_max)
    regs->queue_sel = vbd->qid;
    __sync_synchronize();
    uint16_t qmax = regs->queue_num_max;
    if (qmax == 0) {
        kprintf("%p: VIOBLK queue_num_max is 0\n", regs); // Queue size error
        kfree(vbd);
        return;
    }
    vbd->qsize = (VQ_SIZE <= qmax) ? VQ_SIZE : qmax;
    
    // Initialize the tables/rings
    size_t desc_bytes = sizeof(struct virtq_desc) * vbd->qsize;
    size_t avail_bytes = VIRTQ_AVAIL_SIZE(vbd->qsize);
    size_t used_bytes = VIRTQ_USED_SIZE(vbd->qsize);

    vbd->desc = (struct virtq_desc*)kcalloc(1, desc_bytes);
    vbd->avail = (struct virtq_avail*)kcalloc(1, avail_bytes);
    vbd->used = (struct virtq_used*)kcalloc(1, used_bytes);
    if (!vbd->desc || !vbd->avail || !vbd->used) {
        kprintf("%p: VIOBLK queue alloc failed\n", regs);
        return;
    }
    
    regs->queue_num = vbd->qsize;

    // Attach the tables/rings to the device
    virtio_attach_virtq(regs, 0, vbd->qsize, (uint64_t)(uintptr_t)vbd->desc,
                        (uint64_t)(uintptr_t)vbd->used, (uint64_t)(uintptr_t)vbd->avail);

    // Initialize storage face
    struct storage_intf *intf_copy = kmalloc(sizeof(*intf_copy));
    memcpy(intf_copy, &vioblk_intf_template, sizeof(*intf_copy));
    intf_copy->blksz = blksz;

    storage_init(&vbd->sto, intf_copy, capacity_bytes);

    // Register the device
    register_device(VIOBLK_NAME, DEV_STORAGE, &vbd->sto);
    // By xinyi39 END
}

// By xinyi39 BEGIN
// Helper to submit a single request
static int vioblk_io_sub_one(struct vioblk_storage *vbd, struct virtio_blk_req_hdr *hdr_ptr, void *data_ptr,
    uint32_t data_len, int data_is_write) {
    struct vioblk_reqctx *ctx = &vbd->ioctx;

    ctx->hdr = *hdr_ptr;
    ctx->status = 0xFF;
    ctx->finished = 0;

    vbd->desc[0].addr = (uint64_t)(uintptr_t)&ctx->hdr;
    vbd->desc[0].len = sizeof(ctx->hdr);
    vbd->desc[0].flags = VIRTQ_DESC_F_NEXT;
    vbd->desc[0].next = 1;

    vbd->desc[1].addr = (uint64_t)(uintptr_t)data_ptr;
    vbd->desc[1].len = data_len;
    vbd->desc[1].flags = VIRTQ_DESC_F_NEXT | (data_is_write ? VIRTQ_DESC_F_WRITE : 0);
    vbd->desc[1].next = 2;

    vbd->desc[2].addr = (uint64_t)(uintptr_t)&ctx->status;
    vbd->desc[2].len = 1;
    vbd->desc[2].flags = VIRTQ_DESC_F_WRITE;

    // Publish to avail ring
    vbd->avail->ring[vbd->avail->idx%vbd->qsize] = 0; // head descriptor = 0
    __sync_synchronize();
    vbd->avail->idx++;

    virtio_notify_avail(vbd->regs, vbd->qid);

    // Wait for completion
    int pie = disable_interrupts();
    while (!ctx->finished) {
        condition_wait(&ctx->done);
    }
    restore_interrupts(pie);

    if (ctx->status == VIRTIO_BLK_S_OK) {
        return 0; // Success
    }
    if (ctx->status == VIRTIO_BLK_S_UNSUPP) {
        return -ENOTSUP;
    }
    return -EIO;
}

// Helper function for IO
static long vioblk_io(struct vioblk_storage* vbd, uint64_t pos, void* buf, uint32_t bytecnt, int is_write) {
    const uint32_t blksz = vbd->sto.intf->blksz;
    if (bytecnt == 0) {
        return 0;
    }

    // When reading, if bytecnt < blksz then return 0
    if (!is_write && bytecnt < blksz) {
        return 0;
    }

    // Round down bytecnt to multiple of blksz and 512
    uint32_t rounded = ROUND_DOWN(bytecnt, blksz);
    if (rounded == 0 && is_write == 0) {
        return 0;
    }
    if (rounded & 511U) {
        rounded &= ~511U;
    }

    // pos must be aligned
    if (pos & 511ULL) {
        return -EINVAL;
    }
    
    // 512B per sector
    uint64_t sector = pos / 512ULL;

    lock_acquire(&vbd->qlock);

    if (rounded >= blksz) {
        // prepare header
        struct virtio_blk_req_hdr hdr;
        hdr.type = is_write;
        hdr.reserved = 0;
        hdr.sector = sector;

        int ret = vioblk_io_sub_one(vbd, &hdr, buf, rounded, !is_write);

        lock_release(&vbd->qlock);
        if (ret == 0) return (long)rounded;
        return ret; // error
    }

    void *tmp = kmalloc(blksz);
    if (!tmp) {
        lock_release(&vbd->qlock);
        return -ENOMEM;
    }

    // Read full block into tmp
    struct virtio_blk_req_hdr hdr_read;
    hdr_read.type = 0; // Read
    hdr_read.reserved = 0;
    hdr_read.sector = sector;

    int r = vioblk_io_sub_one(vbd, &hdr_read, tmp, blksz, 1);
    if (r != 0) {
        kfree(tmp);
        lock_release(&vbd->qlock);
        return r; // Error
    }

    uint32_t offset_in_block = pos % blksz;
    memcpy((uint8_t*)tmp + offset_in_block, buf, bytecnt);

    // Write the whole block back
    struct virtio_blk_req_hdr hdr_write;
    hdr_write.type = 1; // Write
    hdr_write.reserved = 0;
    hdr_write.sector = sector;

    r = vioblk_io_sub_one(vbd, &hdr_write,tmp, blksz, 0);
    kfree(tmp);
    lock_release(&vbd->qlock);

    if (r != 0) return r;

    // Return requested bytecnt
    return (long)bytecnt;
}
// By xinyi39 end

static int vioblk_storage_open(struct storage* sto) {
    // By xinyi39 BEGIN
    trace("%s(sto=%p)", __func__, sto);

    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;
    if (vbd->opened) 
    {
        return -EBUSY;
    }

    volatile struct virtio_mmio_regs *regs = vbd->regs;

    // Reset runtime indices and rings
    vbd->last_used_idx = 0;
    vbd->avail->idx = 0;
    vbd->used->idx = 0;

    vbd->avail->flags = 0;
    vbd->used->flags = 0;
    __sync_synchronize();

    // Select queue and mark it ready
    regs->queue_sel = vbd->qid;
    __sync_synchronize();
    virtio_enable_virtq(regs, vbd->qid);  // sets queue_ready per virtio.h helper

    // Enable the device’s interrupt source and point it at our ISR
    enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);

    // Set driver OK
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();

    vbd->opened = 1;
    return 0;
    // By xinyi39 END
}

static void vioblk_storage_close(struct storage* sto) {
    // By xinyi39 BEGIN
    trace("%s(sto=%p)", __func__, sto);

    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;
    if (!vbd->opened) {
        return;
    }

    volatile struct virtio_mmio_regs *regs = vbd->regs;
    const int qid = vbd->qid;

    // Disable device interrupt delivery for this source
    disable_intr_source(vbd->irqno);

    // Reset the virtqueue (transport-level virtqueue reset)
    regs->queue_sel = qid;
    __sync_synchronize();
    virtio_reset_virtq(regs, qid);

    // Clear our in-memory ring bookkeeping
    vbd->last_used_idx = 0;
    vbd->avail->idx = 0;
    vbd->used->idx  = 0;
    vbd->avail->flags = 0;
    vbd->used->flags  = 0;

    // Acknowledge any pending interrupt from the device
    if (regs->interrupt_status) {
        regs->interrupt_ack = regs->interrupt_status;
        __sync_synchronize();
    }

    // Clear DRIVER_OK
    regs->status &= ~VIRTIO_STAT_DRIVER_OK; 

    vbd->opened = 0;
    // By xinyi39 END
    return;
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // By xinyi39 BEGIN
    trace("%s(sto=%p, pos=%llu, buf=%p, bytecnt=%lu)", __func__, sto, pos, buf, bytecnt);

    return vioblk_io((struct vioblk_storage*)sto, pos, buf, bytecnt, 0); // is_write = 0
    // By xinyi39 END
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // By xinyi39 BEGIN
    trace("%s(sto=%p, pos=%llu, buf=%p, bytecnt=%lu)", __func__, sto, pos, buf, bytecnt);

    return vioblk_io((struct vioblk_storage*)sto, pos, (void*)buf, bytecnt, 1); // is_write = 1
    // By xinyi39 END
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // By xinyi39 BEGIN
    trace("%s(sto=%p, op=%d, arg=%p)", __func__, sto, op, arg);

    struct vioblk_storage* vbd = (struct vioblk_storage*)sto;
    if (op == FCNTL_GETEND) {
        if (!arg) return -EINVAL;
        *(unsigned long long*)arg = vbd->capacity;
        return 0;
    }
    // By xinyi39 END
    return -ENOTSUP;
}

static void vioblk_isr(int irqno, void* aux) {
    // By xinyi39 BEGIN
    trace("%s(irqno=%d)", __func__, irqno);

    struct vioblk_storage* vbd = (struct vioblk_storage*)aux;

    while (vbd->last_used_idx != vbd->used->idx) {
        // Not used in single-request implementation
        // struct virtq_used_elem* ue = &vbd->used->ring[vbd->last_used_idx % vbd->qsize];

        vbd->ioctx.finished = 1; // Awake io
        condition_broadcast(&vbd->ioctx.done);

        vbd->last_used_idx++;
    }

    // Acknowledge interrupt
    vbd->regs->interrupt_ack = vbd->regs->interrupt_status;
    __sync_synchronize();
    // By xinyi39 END
}
