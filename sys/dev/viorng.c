// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//
struct viorng_serial {
    //important fields from UART
    struct serial base;
    volatile struct virtio_mmio_regs *regs;
    int irqno;
    char opened;  //0 closed, 1 opened

    //avail, used virtqueues,and descriptors
    struct virtq_desc* desc;
    struct virtq_avail* avail;
    struct virtq_used* used;


    //length of the queue
    uint16_t length;


    //buffer for random data byted generated
    char buffer[VIORNG_BUFSZ];

    //CP3
    struct condition cond;


};

// INTERNAL FUNCTION DECLARATIONS
//

static int  viorng_serial_open (struct serial * ser);
static void viorng_serial_close(struct serial * ser);
static int  viorng_serial_recv (struct serial * ser, void * buf, unsigned int bufsz);
static void viorng_isr         (int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open  = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv  = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS



// void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
// Inputs: volatile struct virtio_mmio_regs * regs - points to VIRTO mapped MIMO
//         int irqno -interrupt src number
// Outputs: nothing
// Description: Attach RNG device
// Side Effects: none
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    
    vrng = (struct viorng_serial *)kcalloc(1, sizeof(*vrng));
    assert(vrng != NULL);


    //initialize with given parameters
    vrng->regs= regs;
    vrng->irqno = irqno;

    vrng->length= 16;  //set the length of the queue to 16

    vrng->opened = 0;

    // CP3 init
    condition_init(&vrng->cond, "vrng.condition");

    //initialize desc, avail, used
    vrng->desc = (struct virtq_desc*)kcalloc(vrng->length, sizeof(struct virtq_desc));
    vrng->avail= (struct virtq_avail*)kcalloc(1, VIRTQ_AVAIL_SIZE(vrng->length));
    vrng->used = (struct virtq_used*)kcalloc(1, VIRTQ_USED_SIZE(vrng->length));
    


    //It attaches the virtq avail and virtq used structs using the virtio attach virtq function
    virtio_attach_virtq(regs, 0, vrng->length, (uint64_t)vrng->desc, (uint64_t)vrng->used, (uint64_t)vrng->avail);

    //the serial struct is initialized with the correct interface
    serial_init(&vrng->base, &viorng_serial_intf);

    //the device is registered
    register_device(VIORNG_NAME, DEV_SERIAL, &vrng->base);

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    virtio_enable_virtq(regs, 0);
    __sync_synchronize();
    

}



// int viorng_serial_open(struct serial * ser)
// Inputs: struct serial * ser - pointer to viorng_serial device
// Outputs: return 0 on success, or -EBUSY
// Description: This function makes the virtq avail and virtq used queues available for use.
// Side Effects: none
int viorng_serial_open(struct serial * ser) {
    struct viorng_serial * vrng;
    vrng = (struct viorng_serial *)ser;
    
    // If the given ser is already opened, then return -EBUSY
    if(vrng->opened) return -EBUSY;

    //Enables the interrupt source for the device, with the correct ISR
    enable_intr_source(vrng->irqno, VIORNG_IRQ_PRIO, viorng_isr, vrng);
    
    //signael the device is open
    vrng->opened = 1;

    //return 0 on success
    return 0;  
}




// void viorng_serial_close(struct serial * ser)
// Inputs: struct serial * ser - pointer to viorng_serial device
// Outputs: return 0 on success, or -EBUSY
// Description: This function resets the virtq avail and virtq used queues and prevents further interrupts.
// Side Effects: none
void viorng_serial_close(struct serial * ser) {
    struct viorng_serial * vrng;
    vrng = (struct viorng_serial *)ser;
    
    // If the given ser is not opened, then this function does nothing
    if(!vrng->opened) return;

    //resets the virtq avail and virtq used queues 
    virtio_reset_virtq(vrng->regs, 0);

    //prevents further interrupts
    disable_intr_source(vrng->irqno);

    //indicate the device is closed
    vrng->opened = 0;  
 
}






// int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
// Inputs: struct serial * ser - pointer to viorng_serial device
//          void *buf- destination buffer
//          unsigned int bufsz- number of bytes to read
// Outputs: return number of bytes reads
// Description: This function reads up to bufsz bytes from the VirtIO Entropy device and writes them to bu
// Side Effects: none

int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {

    //the recive_none case
    if(bufsz == 0 || buf == NULL) return 0;

    struct viorng_serial * vrng = (struct viorng_serial *)ser;

    if(!vrng->opened) return -EINVAL;
   
    //reads up to bufsz
    unsigned int bytes_received = bufsz;
    if(bytes_received >= VIORNG_BUFSZ) bytes_received = VIORNG_BUFSZ;

    //record the previous status
    uint16_t used_before = vrng->used->idx;

    
    //describe the randomness
    vrng->desc[0].addr = (uint64_t)vrng->buffer;
    vrng->desc[0].len = bytes_received;
    vrng->desc[0].flags = VIRTQ_DESC_F_WRITE;
    vrng->desc[0].next = 0;

    //setting the appropriate registers to request entropy from the device
    vrng->avail->ring[vrng->avail->idx % vrng->length]= 0;

    vrng->avail->idx = vrng->avail->idx + 1;

    virtio_notify_avail(vrng->regs, 0);

    int pie = disable_interrupts();
    // waiting until the randomness has been placed into a buffer
    while(vrng->used->idx == used_before){
        // CP3
        condition_wait(&vrng->cond);
    }
    restore_interrupts(pie);
    // writing that data out to buf
    memcpy(buf, vrng->buffer, bytes_received);
    //Returns the number of bytes of randomness successfully obtained.
    return bytes_received;  
}


// void viorng_isr(int irqno, void * aux)
// Inputs: aux - pointer to viorng_serial device
//         riqno - interrupt source number
// Outputs:nothing
// Description:This function sets the appropriate device registers and wakes the thread up after waiting for the device to finish
// Side Effects: none
void viorng_isr(int irqno, void * aux) {

    struct viorng_serial * vrng;
    vrng = (struct viorng_serial *)aux;   

    // sets the appropriate device registers 
    //wakes the thread up after waiting for the device to finish
    //servicing a request.
    vrng->regs->interrupt_ack = vrng->regs->interrupt_status; 


    // CP3
    condition_broadcast(&vrng->cond);
}