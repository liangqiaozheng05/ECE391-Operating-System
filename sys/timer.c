// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

#include <stddef.h>
// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;


#define TIMESLICE_TICKS (TIMER_FREQ / 100)
// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    unsigned long long now = rdtime();
    unsigned long long first_slice = now + TIMESLICE_TICKS;
    set_stcmp(first_slice);
    timer_initialized = 1;
}



// void alarm_init(struct alarm * al, const char * name) 
// Inputs: struct alarm * al -  a pointer to the alarm object to be initialized
//         const char * name - which may be null, is an optional name that can be passed to the condition variable
// Outputs: nothing
// Description: Initialize the alarm
// Side Effects: none

void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here

    //If no name is given (i.e.,name == NUL), give your alarm a sensible name (e.g., “alarm”).
    if(name == NULL) name = "alarm";

    // initilization of condition variable cond
    condition_init(&al->cond, name);

    //the alarm is not yet in the list
    al->next = NULL;

    // keep track of the most recent time in ticks
    al->twake = rdtime();
}




// void alarm_sleep(struct alarm * al, unsigned long long tcnt)
// Inputs: struct alarm * al -  a pointer to the alarm object to be initialized
//         unsigned long long tcnt - the number of ticks to put the thread to sleep for relative to the most recent initialization, reset, or wake-up
// Outputs: nothing
// Description: Put the alarm to sleep
// Side Effects: none
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    // FIXME your code goes here

    // critical section
    // pie: previous interrupt enable
    pie = disable_interrupts(); 
    
    //find where to insert in sleep_list
    struct alarm* current = sleep_list;

    prev = NULL;
    
    while(current != NULL && current->twake <= al->twake){
        prev = current;
        current = current->next;
    } 


    if(current != sleep_list){

        //insert not at the begining
        prev->next = al;
        al->next = current;

        // Enable timer interrupts when adding new alarms in s mode 
        csrs_sie(RISCV_SIE_STIE);
    }
    else{
        //adds the alarm to sleep list
        al->next = sleep_list;
        //insert at the begining of the list
        sleep_list = al;
        // set mtimecmp as needed
        set_stcmp(al->twake); // It determines when the next timer interrupt will be triggered.

        // Enable timer interrupts when adding new alarms in s mode 
        csrs_sie(RISCV_SIE_STIE);
    }


    restore_interrupts(pie);     

    //Put current thread to sleep.
    condition_wait(&al->cond);
   

}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}



// void handle_timer_interrupt(void)
// Inputs: nothing
// Outputs: nothing
// Description: Service timer interrupts
// Side Effects: none

void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // FIXME your code goes here
    while (head && head->twake <= now) {
        head->twake = now;
        condition_broadcast(&head->cond);
        next = head->next;
        sleep_list = next;
        head->next = NULL;
        head = sleep_list;
    }
    unsigned long long next_preempt_time = now + TIMESLICE_TICKS;
    unsigned long long next_cmp = next_preempt_time;
    if (sleep_list && sleep_list->twake < next_cmp) {
        next_cmp = sleep_list->twake;
    }
    set_stcmp(next_cmp);
    csrs_sie(RISCV_SIE_STIE);
    }
