/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"

#include "conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE)  // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE)  // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk *next;  ///< Next page in list
    unsigned long pagecnt;    ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//

static void ptab_discard(struct pte *ptab  // page table to discard
);

struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn);

static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);

static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk *free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init(void) {
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;

    void *heap_start;
    void *heap_end;

    uintptr_t pma;
    const void *pp;

    trace("%s()", __func__);

    assert(RAM_START == _kimg_start);

    debug("           RAM: [%p,%p): %zu MB", RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    debug("  Kernel image: [%p,%p)", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB

    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages
    // Map up to RAM_END (inclusive of the last complete megapage)
    
    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }
    // Also map any remaining pages at the end if RAM_END is not aligned to MEGA_SIZE
    if ((uintptr_t)RAM_END % MEGA_SIZE != 0) {
        // Round down to last megapage boundary
        pp = (void*)((uintptr_t)RAM_END - ((uintptr_t)RAM_END % MEGA_SIZE));
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end) panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
          (heap_end - heap_start) / 1024);

    // FIXME: Initialize the free chunk list here

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).
    size_t free_bytes = (uintptr_t)RAM_END - (uintptr_t)heap_end;
    unsigned long free_pages = free_bytes / PAGE_SIZE;
    if (free_pages > 1) {
        free_pages -= 2;
    }
    else free_pages = 0;
    
    if (free_pages == 0) {
        free_chunk_list = NULL;
    }
    else{
        struct page_chunk *first_chunk = (struct page_chunk *)heap_end;
        first_chunk->pagecnt = free_pages;
        first_chunk->next = NULL;
        free_chunk_list = first_chunk;
    }
    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) { return active_space_mtag(); }

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;

    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

mtag_t clone_active_mspace(void) {
    // FIXME (CP3)
    struct pte *parent_pt2 = active_space_ptab();
    struct pte *child_pt2 = NULL;
    mtag_t child_mtag;
    
    // Allocate new root page table for child
    child_pt2 = alloc_phys_page();
    if (child_pt2 == NULL) {
        return 0;
    }
    memset(child_pt2, 0, PAGE_SIZE);
    
    // Copy global kernel mappings from parent
    for (int i = 0; i < PTE_CNT; i++) {
        struct pte p2 = parent_pt2[i];
        if (PTE_VALID(p2) && PTE_GLOBAL(p2)) {
            child_pt2[i] = p2;
        }
    }
    
    // Clone user memory mappings (non-global pages)
    for (int i2 = 0; i2 < PTE_CNT; i2++) {
        struct pte p2 = parent_pt2[i2];
        if (!PTE_VALID(p2) || PTE_GLOBAL(p2)) { // Skip invalid or global entries
            continue;
        }
        
        if (PTE_LEAF(p2)) {
            // Leaf at level 2
            void *parent_page = pageptr(p2.ppn);
            void *child_page = alloc_phys_page();
            if (child_page == NULL) {   //NULL - clean and return fail
                ptab_discard(child_pt2);
                return 0;
            }
            memcpy(child_page, parent_page, PAGE_SIZE);
            child_pt2[i2] = leaf_pte(child_page, p2.flags);
        }
        else {
            // Non-leaf at level 2
            struct pte *parent_pt1 = (struct pte *)pageptr(p2.ppn);
            struct pte *child_pt1 = alloc_phys_page();
            if (child_pt1 == NULL) {
                ptab_discard(child_pt2);
                return 0;
            }
            memset(child_pt1, 0, PAGE_SIZE);
            child_pt2[i2] = ptab_pte(child_pt1, 0);
            
            // Process level 1
            for (int i1 = 0; i1 < PTE_CNT; i1++) {
                struct pte p1 = parent_pt1[i1];
                if (!PTE_VALID(p1) || PTE_GLOBAL(p1)) {
                    continue;
                }
                
                if (PTE_LEAF(p1)) { // copy the page if leaf at level 1
                    void *parent_page = pageptr(p1.ppn);
                    void *child_page = alloc_phys_page();
                    if (child_page == NULL) {
                        ptab_discard(child_pt2);
                        return 0;
                    }
                    memcpy(child_page, parent_page, PAGE_SIZE);
                    child_pt1[i1] = leaf_pte(child_page, p1.flags);
                }
                else {
                    // need level 0 page table if no leaf at lavel 1
                    struct pte *parent_pt0 = (struct pte *)pageptr(p1.ppn);
                    struct pte *child_pt0 = alloc_phys_page();
                    if (child_pt0 == NULL) {
                        ptab_discard(child_pt2);
                        return 0;
                    }
                    memset(child_pt0, 0, PAGE_SIZE);
                    child_pt1[i1] = ptab_pte(child_pt0, 0);
                    
                    // Process level 0
                    for (int i0 = 0; i0 < PTE_CNT; i0++) {
                        struct pte p0 = parent_pt0[i0];
                        if (!PTE_VALID(p0) || PTE_GLOBAL(p0)) {
                            continue;
                        }
                        
                        if (PTE_LEAF(p0)) {
                            // copy the page if leaf at level 0
                            void *parent_page = pageptr(p0.ppn);
                            void *child_page = alloc_phys_page();
                            if (child_page == NULL) {
                                ptab_discard(child_pt2);
                                return 0;
                            }
                            memcpy(child_page, parent_page, PAGE_SIZE);
                            child_pt0[i0] = leaf_pte(child_page, p0.flags);
                        }
                    }
                }
            }
        }
    }
    
    // Create memory tag for child
    child_mtag = ptab_to_mtag(child_pt2, 0);
    return child_mtag;
}

static void ptab_discard(struct pte *ptab) {
    if (!ptab) return;

    /* Walk all entries in this page table. For any non-global, valid
     * leaf entries free the referenced physical page. For any non-global,
     * non-leaf entries recursively discard the child page table. After
     * cleaning children, free this page-table page itself. */
    for (unsigned int i = 0; i < PTE_CNT; i++) {
        struct pte entry = ptab[i];

        if (!PTE_VALID(entry) || PTE_GLOBAL(entry)){
            continue;
        }

        if (PTE_LEAF(entry)) {
            void *pp = pageptr(entry.ppn);
            if (pp){
                free_phys_page(pp);
            }
            else{
                continue;
            }
        }
        else {
            struct pte *child = (struct pte *)pageptr(entry.ppn);
            if (child){
                ptab_discard(child);
            }
            else{
                continue;
            }
        }
    }

    free_phys_page((void *)ptab);
}

void reset_active_mspace(void) {
    // FIXME
    unmap_and_free_range((void*)UMEM_START_VMA, UMEM_SIZE);
}

mtag_t discard_active_mspace(void) {
    // FIXME
    mtag_t old = active_space_mtag();
    csrrw_satp(main_mtag);
    sfence_vma();
    return old;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

void *map_page(uintptr_t vma, void *pp, int rwxug_flags) {
    // FIXME
    if (vma % PAGE_SIZE != 0) return NULL;
    unsigned long vpn = VPN(vma);
    struct pte *pt2 = active_space_ptab();
    struct pte p2 = pt2[PT_INDEX(2, vpn)];
    if (!PTE_VALID(p2)) {
        void *newpt = alloc_phys_page();
        if (!newpt) return NULL;
        memset(newpt, 0, PAGE_SIZE);
        pt2[PT_INDEX(2, vpn)] = ptab_pte(newpt, 0);
        p2 = pt2[PT_INDEX(2, vpn)];
    } else if (PTE_LEAF(p2)) {
        return NULL;
    }
    struct pte *pt1 = (struct pte *)pageptr(p2.ppn);
    struct pte p1 = pt1[PT_INDEX(1, vpn)];
    if (!PTE_VALID(p1)) {
        void *newpt = alloc_phys_page();
        if (!newpt) return NULL;
        memset(newpt, 0, PAGE_SIZE);
        pt1[PT_INDEX(1, vpn)] = ptab_pte(newpt, 0);
        p1 = pt1[PT_INDEX(1, vpn)];
    } else if (PTE_LEAF(p1)) {
        return NULL;
    }
    struct pte *pt0 = (struct pte *)pageptr(p1.ppn);
    pt0[PT_INDEX(0, vpn)] = leaf_pte(pp, rwxug_flags);

    sfence_vma();
    return (void *)vma;
}

void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags) {
    // FIXME
    if (size == 0) return (void*)vma;
    size_t n = ROUND_UP(size, PAGE_SIZE);
    uintptr_t a = vma;
    uint8_t *p = pp;
    for (size_t i = 0; i < n; i += PAGE_SIZE) {
        map_page(a, p, rwxug_flags);
        a += PAGE_SIZE;
        p += PAGE_SIZE;
    }
    return (void*)vma;
}

void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // FIXME
    if (size == 0) return NULL;
    size_t nbytes = ROUND_UP(size, PAGE_SIZE);
    unsigned int npages = (unsigned int)(nbytes / PAGE_SIZE);
    void *pp = alloc_phys_pages(npages);
    if (pp == NULL) return NULL;
    map_range(vma, nbytes, pp, rwxug_flags);
    return pp;  // Return physical address so kernel can access it
}

void set_range_flags(const void *vp, size_t size, int rwxug_flags) { 
    // should completely overwrite any existing flags in that range. -- Errata
    // FIXME
    if (size == 0) return;
    uintptr_t vma = (uintptr_t)vp;
    assert((vma % PAGE_SIZE) == 0);
    size_t nbytes = ROUND_UP(size, PAGE_SIZE);
    struct pte *pt2 = active_space_ptab();
    for (size_t off = 0; off < nbytes; off += PAGE_SIZE) {
        unsigned long vpn = VPN(vma + off);
        struct pte p2 = pt2[PT_INDEX(2, vpn)];
        assert(PTE_VALID(p2) && !PTE_LEAF(p2));
        struct pte *pt1 = (struct pte*)pageptr(((struct pte)p2).ppn);
        struct pte p1 = pt1[PT_INDEX(1, vpn)];
        assert(PTE_VALID(p1) && !PTE_LEAF(p1));
        struct pte *pt0 = (struct pte*)pageptr(((struct pte)p1).ppn);
        struct pte old = pt0[PT_INDEX(0, vpn)];
        assert(PTE_VALID(old) && PTE_LEAF(old));
        void *pp = pageptr(old.ppn);
        pt0[PT_INDEX(0, vpn)] = leaf_pte(pp, (uint_fast8_t)rwxug_flags);
    }
    sfence_vma();
    return;
}

void unmap_and_free_range(void *vp, size_t size) {
    // FIXME
    if (size == 0) return;
    uintptr_t vma = (uintptr_t)vp;
    assert((vma % PAGE_SIZE) == 0);
    size_t nbytes = ROUND_UP(size, PAGE_SIZE);
    struct pte *pt2 = active_space_ptab();
    for (size_t off = 0; off < nbytes; off += PAGE_SIZE) {
        unsigned long vpn = VPN(vma + off);
        struct pte p2 = pt2[PT_INDEX(2, vpn)];
        if (!PTE_VALID(p2) || PTE_LEAF(p2)) continue;
        struct pte *pt1 = (struct pte*)pageptr(((struct pte)p2).ppn);
        struct pte p1 = pt1[PT_INDEX(1, vpn)];
        if (!PTE_VALID(p1) || PTE_LEAF(p1)) continue;
        struct pte *pt0 = (struct pte*)pageptr(((struct pte)p1).ppn);
        struct pte leaf = pt0[PT_INDEX(0, vpn)];
        if (PTE_VALID(leaf) && PTE_LEAF(leaf) && !PTE_GLOBAL(leaf)) {
            void *pp = pageptr(leaf.ppn);
            pt0[PT_INDEX(0, vpn)] = null_pte();
            free_phys_page(pp);
        }
    }
    sfence_vma();
    return;
}

int validate_vptr(const void *vp, size_t len, int rwxug_flags) {
    // FIXME
    extern int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma);
    
    uintptr_t start = (uintptr_t)vp;
    if (!wellformed(start)) return -1;
    uintptr_t end = start + len;
    if (end < start) return -1; 
    if (len == 0) return 0;

    struct pte *pt2 = active_space_ptab();
    uintptr_t cur = start & ~(PAGE_SIZE - 1);
    uintptr_t last = ((end - 1) & ~(PAGE_SIZE - 1));
    for (;;) {
        unsigned long vpn = VPN(cur);

        struct pte p2 = pt2[PT_INDEX(2, vpn)];
        if (!PTE_VALID(p2) || PTE_LEAF(p2)) {
            // Attempt to demand page
            if (!handle_umode_page_fault(NULL, cur)) return -1;
            p2 = pt2[PT_INDEX(2, vpn)];
            if (!PTE_VALID(p2) || PTE_LEAF(p2)) return -1;
        }
        struct pte *pt1 = (struct pte*)pageptr(((struct pte)p2).ppn);

        struct pte p1 = pt1[PT_INDEX(1, vpn)];
        if (!PTE_VALID(p1) || PTE_LEAF(p1)) {
            // Attempt to demand page
            if (!handle_umode_page_fault(NULL, cur)) return -1;
            p1 = pt1[PT_INDEX(1, vpn)];
            if (!PTE_VALID(p1) || PTE_LEAF(p1)) return -1;
        }
        struct pte *pt0 = (struct pte*)pageptr(((struct pte)p1).ppn);

        struct pte leaf = pt0[PT_INDEX(0, vpn)];
        if (!PTE_VALID(leaf) || !PTE_LEAF(leaf)) {
             // Attempt to demand page
            if (!handle_umode_page_fault(NULL, cur)) return -1;
            leaf = pt0[PT_INDEX(0, vpn)];
            if (!PTE_VALID(leaf) || !PTE_LEAF(leaf)) return -1;
        }
        if ((leaf.flags & rwxug_flags) != (uint8_t)rwxug_flags) return -1;
        if (cur == last) break;
        cur += PAGE_SIZE;
    }
    return 0;
}

int validate_vstr(const char *vs, int rug_flags) {
    // FIXME
    const char *p = vs;
    for (;;) {
        if (validate_vptr(p, 1, rug_flags) != 0) return -1;
        char c = *p;  
        if (c == '\0') break;
        p++;
    }
    return 0;
}

void *alloc_phys_page(void) {
    // FIXME
    return alloc_phys_pages(1);
}

void free_phys_page(void *pp) {
    // FIXME
    free_phys_pages(pp, 1);
    return;
}

void *alloc_phys_pages(unsigned int cnt) {
    // FIXME
    assert(cnt > 0);
    struct page_chunk *best = NULL, *best_prev = NULL;
    struct page_chunk *prev = NULL, *cur = free_chunk_list;
    while (cur != NULL) {
        if ((void*)cur >= RAM_END) {
             kprintf("CORRUPTION: chunk %p >= RAM_END\n", cur);
             break;
        }
        if (cur->pagecnt >= cnt && (!best || cur->pagecnt < best->pagecnt)) {
            best = cur;
            best_prev = prev;
            if (best->pagecnt == cnt) break; 
        }
        prev = cur;
        cur = cur->next;
    }
    if (!best) return NULL;
    void *ret;
    if (best->pagecnt == cnt) {
        if (best_prev) best_prev->next = best->next;
        else free_chunk_list = best->next;
        ret = (void*)best;
    } else {
        // Allocate from the beginning of the chunk (low address)
        ret = (void*)best;
        // Move the chunk pointer forward
        struct page_chunk *new_chunk = (struct page_chunk*)((uintptr_t)best + cnt * PAGE_SIZE);
        if ((void*)new_chunk >= RAM_END) {
            // kprintf("alloc BUG: best=%p size=%d cnt=%d new=%p\n", best, best->pagecnt, cnt, new_chunk);
            panic("memory corruption");
        }
        new_chunk->pagecnt = best->pagecnt - cnt;
        new_chunk->next = best->next;
        if (best_prev) best_prev->next = new_chunk;
        else free_chunk_list = new_chunk;
    }
    return ret;
}

void free_phys_pages(void *pp, unsigned int cnt) {
    // FIXME
    if (!pp || cnt == 0) return;
    struct page_chunk *node = (struct page_chunk*)pp;
    node->pagecnt = cnt;
    node->next = free_chunk_list;
    free_chunk_list = node;
    return;
}

unsigned long free_phys_page_count(void) {
    // FIXME
    unsigned long total = 0;
    for (struct page_chunk *cur = free_chunk_list; cur; cur = cur->next) {
        total += cur->pagecnt;
    }
    return total;
}

int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma) {
    // FIXME
    (void)tfr;
    if (!wellformed(vma)) {
        return 0;
    }
    if (vma < UMEM_START_VMA || vma >= UMEM_END_VMA) {
        return 0;
    }
    uintptr_t va_page = (uintptr_t)ROUND_DOWN(vma, PAGE_SIZE);
    void *pp = alloc_phys_page();
    if (!pp) {
        return 0;
    }
    void *result = map_page(va_page, pp, PTE_R | PTE_W | PTE_U);
    if (!result) {
        free_phys_page(pp);
        return 0;
    }
    return 1;
}

/**
 * @brief Reads satp to retrieve tag for active memory space
 * @return Tag for active memory space
 */
mtag_t active_space_mtag(void) { return csrr_satp(); }

/**
 * @brief Constructs tag from page table address and address space identifier
 * @param ptab Pointer to page table to use in tag
 * @param asid Address space identifier to use in tag
 * @return Memory tag formed from paging mode, page table address, and ASID
 */
static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid) {
    return (((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
            ((unsigned long)asid << RISCV_SATP_ASID_shift) | pagenum(ptab) << RISCV_SATP_PPN_shift);
}

/**
 * @brief Retrives a page table address from a tag
 * @param mtag Tag to extract page table address from
 * @return Pointer to page table retrieved from tag
 */
static inline struct pte *mtag_to_ptab(mtag_t mtag) { return (struct pte *)((mtag << 20) >> 8); }

/**
 * @brief Returns the address of the page table corresponding to the active memory space
 * @return Pointer to page table extracted from active memory space tag
 */
static inline struct pte *active_space_ptab(void) { return mtag_to_ptab(active_space_mtag()); }

/**
 * @brief Constructs a physical pointer from a physical page number
 * @param n Physical page number to derive physical pointer from
 * @return Pointer to memory corresponding to physical page
 */
static inline void *pageptr(uintptr_t n) { return (void *)(n << PAGE_ORDER); }

/**
 * @brief Constructs a physical page number from a pointer
 * @param p Pointer to derive physical page number from
 * @return Physical page number corresponding to pointer
 */
static inline unsigned long pagenum(const void *p) { return (unsigned long)p >> PAGE_ORDER; }

/**
 * @brief Checks if bits 63:38 of passed virtual memory address are all 1 or all 0
 * @param vma Virtual memory address to check well-formedness of
 * @return 1 if pointer is well-formed, 0 otherwise
 */
static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}

/**
 * @brief Constructs a page table entry corresponding to a leaf
 * @details For our purposes, a leaf PTE has the A, D, and V flags set
 * @param pp Physical address to set physical page number of PTE from
 * @param rwxug_flags Flags to set on PTE
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags) {
    return (struct pte){.flags = rwxug_flags | PTE_A | PTE_D | PTE_V, .ppn = pagenum(pp)};
}

/**
 * @brief Constructs a page table entry corresponding to a page table
 * @param pt Physical address to set physical page number of PTE from
 * @param g_flag Flags to set on PTE (should either be G flag or nothing)
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag) {
    return (struct pte){.flags = g_flag | PTE_V, .ppn = pagenum(pt)};
}

/**
 * @brief Returns an empty pte
 * @return An empty pte
 */
static inline struct pte null_pte(void) { return (struct pte){}; }
