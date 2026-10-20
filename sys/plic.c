// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "misc.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

// PLIC function implementations

// static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
// Inputs: uint_fast32_t srcno - interrupt source number
//         uint_fast32_t level - priority level 
// Outputs: nothing
// Description:Set the priority level of an interrupt source
// Side Effects: none

static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
    if(srcno < PLIC_SRC_CNT){
        PLIC.priority[srcno]= level;
    }   
}



// static inline int plic_source_pending(uint_fast32_t srcno)
// Inputs: uint_fast32_t srcno - interrupt source number
// Outputs: return 1 if pending
// Description:Check if an interrupt source is pending
// Side Effects: none

static inline int plic_source_pending(uint_fast32_t srcno) {
    if(srcno >= PLIC_SRC_CNT) return 0;
  
    uint32_t index= srcno/32;
    uint32_t offset= srcno%32;
    
    return (PLIC.pending[index] >> offset) & 1;
}




//static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t srcno - interrupt source number
// Outputs: nothing
// Description:Enable an interrupt source for a context
// Side Effects: none

static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
    if(ctxno >= PLIC_CTX_CNT || srcno >= PLIC_SRC_CNT) return;

    
    uint32_t index= srcno/32;
    uint32_t offset= srcno%32;
    
    PLIC.enable[ctxno][index] |= (1U << offset);
}


//static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t srcid - interrupt source number
// Outputs: nothing
// Description:Disable an interrupt source for a context
// Side Effects: none
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
    if(ctxno >= PLIC_CTX_CNT || srcid >= PLIC_SRC_CNT) return;
  
    
    uint32_t index= srcid/32;
    uint32_t offset= srcid%32;
    
    PLIC.enable[ctxno][index] &= ~(1U << offset);
}



//static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t level - minimum priority
// Outputs: nothing
// Description: Set the interrupt priority threshold for a contex
// Side Effects: none

static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
    if(ctxno < PLIC_CTX_CNT)  PLIC.ctx[ctxno].threshold= level;

}


//static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: nothing
// Description: return interrupt source number
// Side Effects: none

static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
    if(ctxno >= PLIC_CTX_CNT) return 0;
  
    return PLIC.ctx[ctxno].claim;
}



//static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t srcno - source number  of the interrupt  
// Outputs: nothing
// Description: Complete the handling of an interrupt for a given context
// Side Effects: none

static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
    if(ctxno < PLIC_CTX_CNT){
        PLIC.ctx[ctxno].claim= srcno;
    }   
}


//static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: nothing
// Description: Enable all interrupt sources for a context
// Side Effects: none
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
    if(ctxno >= PLIC_CTX_CNT) return;
  
    
    // Set all bits in all enable words for this context
    for(int i = 0; i< 32; ++i){
        PLIC.enable[ctxno][i]= 0xFFFFFFFF;
    }   
}




// static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: nothing
// Description: Disable all interrupt sources for a context
// Side Effects: none

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
    if(ctxno >= PLIC_CTX_CNT) return;
 
    
    // Clear all bits in all enable words for this context
    for(int i = 0; i< 32; ++i){
        PLIC.enable[ctxno][i]= 0;
    }   
}


