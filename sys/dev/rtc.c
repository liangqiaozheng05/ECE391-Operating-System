// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "misc.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
// 

// int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz)
// Inputs: mmio_base - base address of RTC registers
// Outputs: nothing
// Description: Attach RTC device
// Side Effects: none

void rtc_attach(void * mmio_base) {
    struct rtc_device * dev;

    trace("%s(%p)", __func__, mmio_base);

    /* allocate device struct */
    dev = kcalloc(1, sizeof(*dev));
    assert(dev != NULL);

    /* bind interface and mmio base */
    serial_init(&dev->base, &rtc_serial_intf);
    dev->regs = (volatile struct rtc_regs *) mmio_base;

    /* register device under name "rtc" as a serial device */
    register_device("rtc", DEV_SERIAL, &dev->base);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}



// int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz)
// Inputs: ser - pointer to RTC serial device
// Outputs: the number of bytes written on success, or -EINVAL on error 
// Description: Read the current time
// Side Effects: none

int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct rtc_device* device;

    trace("%s(buf=%p, bufsz=%u)", __func__, buf, bufsz);

    if(buf == NULL || bufsz == 0) return 0; 
    if(bufsz < sizeof(uint64_t)) return 0;
    
    device = (struct rtc_device*)((char*)ser - offsetof(struct rtc_device, base));
    if(device == NULL || device->regs == NULL) return -EINVAL;

    uint64_t t= read_real_time(device->regs);
    memcpy(buf, &t, sizeof(t));
    return sizeof(t);
}


// uint64_t read_real_time(volatile struct rtc_regs * regs)
// Inputs: volatile struct rtc_regs * regs - pointer to ret_regs
// Outputs: real time
// Description: Get time from registers
// Side Effects: none

uint64_t read_real_time(volatile struct rtc_regs * regs) {
    // FIXME your code goes here
   
    uint32_t low, high;

    //read low latches high
    low= regs->time_low;
    high= regs->time_high;

    return (((uint64_t)high) << 32) | (uint64_t)low;
}