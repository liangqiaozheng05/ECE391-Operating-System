/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"

#include <stdint.h>

#include "conf.h"
#include "console.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"
#include "thread.h"
#include "heap.h"

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };

/*! @struct elf64_ehdr
    @brief ELF header struct
*/
struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/*! @enum elf_pt
    @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
// Memory range for loaded programs
#define PROG_MEM_START UMEM_START_VMA
#define PROG_MEM_END   UMEM_END_VMA

/*! @struct elf64_phdr
    @brief Program header struct
*/
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243
/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * The loader processes only program header entries of type `PT_LOAD`. The layouts
 * of structures and magic values can be found in the Linux ELF header file
 * `<uapi/linux/elf.h>`
 * The implementation should ensure that all loaded sections of the program are
 * mapped within the memory range `0x80100000` to `0x81000000`.
 *
 * Let's do some reading! The following documentation will be very helpful!
 * [Helpful doc](https://linux.die.net/man/5/elf)
 * Good luck!
 * [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */

struct lock elf_load_lock;

int elf_load(struct uio* uio, void (**eptr)(void)) {
    // FIXME
    lock_acquire(&elf_load_lock);

    if (uio == NULL || eptr == NULL) {
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    struct elf64_ehdr ehdr;
    long bytes_read;

    // Read ELF header
    bytes_read = uio_read(uio, &ehdr, sizeof(ehdr));
    
    if (bytes_read != sizeof(ehdr)) {
        debug("elf_load: ELF header read failed");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    // Validate ELF magic number
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        debug("elf_load: magic number fail");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) { // Validate ELF class
        debug("elf_load: not a 64-bit ELF file");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) { // Validate data as little endian
        debug("elf_load: not little-endian");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }
    
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) {   // Validate ELF version
        debug("elf_load: ELF version error");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    if (ehdr.e_machine != EM_RISCV) {   // Validate machine as RISC-V
        debug("elf_load: not a RISC-V executable");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }    

    if (ehdr.e_type != ET_EXEC) {
        debug("elf_load: not an executable file");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }
    
    if (ehdr.e_entry < PROG_MEM_START || ehdr.e_entry >= PROG_MEM_END) {    // Validate entry point and header
        debug("elf_load: entry point 0x%lx outside range", ehdr.e_entry);
        lock_release(&elf_load_lock);
        return -EINVAL;
    }
    if (ehdr.e_phnum == 0 || ehdr.e_phentsize != sizeof(struct elf64_phdr)) {
        debug("elf_load: invalid program header info");
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    size_t phdr_size = ehdr.e_phnum * sizeof(struct elf64_phdr);
    struct elf64_phdr *phdrs = kmalloc(phdr_size);
    if (phdrs == NULL) {
        debug("elf_load: failed to allocate memory for program headers");
        lock_release(&elf_load_lock);
        return -ENOMEM;
    }

    // Seek to program headers
    unsigned long long phoff = ehdr.e_phoff;
    int ret = uio_cntl(uio, FCNTL_SETPOS, &phoff);
    
    if (ret != 0) {
        debug("elf_load: failed to seek to program headers");
        kfree(phdrs);
        lock_release(&elf_load_lock);
        return ret;
    }

    // Read all program headers
    bytes_read = uio_read(uio, phdrs, phdr_size);
    
    if (bytes_read != (long)phdr_size) {    // Validate read size and panic if not matching
        debug("elf_load: failed to read program headers");
        kfree(phdrs);
        lock_release(&elf_load_lock);
        return -EINVAL;
    }

    // Process each PT_LOAD segment
    for (int i = 0; i < ehdr.e_phnum; i++) {
        struct elf64_phdr *phdr = &phdrs[i];

        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        if (phdr->p_align != 0 && phdr->p_align != 1 && 
            phdr->p_align != PAGE_SIZE && (phdr->p_align & (phdr->p_align - 1)) != 0) {
            debug("elf_load: segment %d has invalid alignment %lu", i, phdr->p_align);
            kfree(phdrs);
            lock_release(&elf_load_lock);
            return -EINVAL;
        }

        if (phdr->p_vaddr < PROG_MEM_START || 
            (phdr->p_vaddr + phdr->p_memsz) > PROG_MEM_END) {   // Validate memory range and barrier
            debug("elf_load: segment %d outside user space", i);
            kfree(phdrs);
            lock_release(&elf_load_lock);
            return -EINVAL;
        }

        if (phdr->p_memsz == 0) {
            continue;  // Skip empty segments
        }

        // Calculate aligned virtual address range
        uintptr_t vaddr_start = ROUND_DOWN(phdr->p_vaddr, PAGE_SIZE);
        uintptr_t vaddr_end = ROUND_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);

        // change ELF segment permissions to page table flags
        int flags = PTE_U;  // umode accessible
        if (phdr->p_flags & PF_R) flags |= PTE_R;
        if (phdr->p_flags & PF_W) flags |= PTE_W;
        if (phdr->p_flags & PF_X) flags |= PTE_X;

        // vma set
        void* phys_addr = alloc_and_map_range(vaddr_start, vaddr_end - vaddr_start, flags);
        if (phys_addr == NULL) {
            debug("elf_load: failed to map segment %d", i);
            kfree(phdrs);
            lock_release(&elf_load_lock);
            return -ENOMEM;
        }

        // Calculate offset of segment start within the allocated region
        uintptr_t segment_offset = phdr->p_vaddr - vaddr_start;
        void* segment_phys = (char*)phys_addr + segment_offset;

        // Clear the memory using physical address
        memset(segment_phys, 0, phdr->p_memsz);


        if (phdr->p_filesz > 0) {
            // Seek to segment data in file
            unsigned long long offset = phdr->p_offset;
            ret = uio_cntl(uio, FCNTL_SETPOS, &offset);
            
            if (ret != 0) {
                debug("elf_load: failed to seek to segment offset");
                kfree(phdrs);
                lock_release(&elf_load_lock);
                return ret;
            }

            // Read segment data into memory using physical address
            bytes_read = uio_read(uio, segment_phys, phdr->p_filesz);
            
            if (bytes_read != (long)phdr->p_filesz) {
                debug("elf_load: failed to read segment data (got %ld, expected %lu)",
                      bytes_read, phdr->p_filesz);
                kfree(phdrs);
                lock_release(&elf_load_lock);
                return -EINVAL;
            }
        }
    }

    kfree(phdrs);

    // Set entry point
    *eptr = (void (*)(void))ehdr.e_entry;
    
    debug("elf_load: successfully loaded ELF, entry point at 0x%lx", ehdr.e_entry);
    lock_release(&elf_load_lock);
    return 0;
}