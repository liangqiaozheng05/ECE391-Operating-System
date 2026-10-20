# ECE391 Operating System

A RISC-V operating system course project developed for **UIUC ECE 391:
Computer Systems Engineering, Fall 2025**. Built on the course's Illinix 391
framework, the project connects device drivers, a filesystem, virtual memory,
processes, system calls, and a user-space shell.

This is a collaborative educational project inspired by Unix V6. It is a
source snapshot for explaining systems programming work, not a production OS.

## At a glance

- **Languages:** C and RISC-V assembly.
- **Platform:** 64-bit RISC-V, QEMU `virt`, supervisor and user modes.
- **Memory:** Sv39 three-level page tables, 4 KiB pages, best-fit physical-page
  allocation, demand allocation, and address-space copying.
- **Storage:** VirtIO block driver, KTFS filesystem, and a 64-block LRU
  write-back cache with 512-byte blocks.
- **Processes and IPC:** `fork`, `exec`, `wait`, file descriptor management,
  and one-way pipes backed by 4 KiB circular buffers.
- **User space:** a shell plus `date`, `echo`, `cat`, `ls`, `wc`, `touch`, `rm`,
  and `xargs`.

These describe implementation and configuration in the source. They are not
benchmark results or claims that every integration path has passed testing.

## Architecture

```text
User shell and utilities
        |
   ecall / system calls
        |
Process management ----- Sv39 virtual memory
        |
Uniform I/O: files, devices, listings, and pipes
        |
       KTFS
        |
64-block LRU write-back cache
        |
VirtIO block driver ----- interrupt and thread synchronization
        |
QEMU virtual disk
```

The kernel also includes UART, RTC, VirtIO RNG, PLIC, and a read-only RAM-disk
backend. Drivers and threading build on work from the preceding course project.

## Implementation highlights

### Storage and filesystem

The VirtIO block driver configures MMIO registers and virtqueues, builds
request descriptor chains, and waits for completion using interrupts and
condition variables. Requests are serialized by a driver lock.

The cache tracks access order, dirty state, and active users. Cached reads
avoid repeated backing-device requests. Dirty blocks are written back on
eviction or an explicit flush.

KTFS uses a superblock, inode and block bitmaps, and direct, indirect, and
double-indirect block indexing. The implementation includes file reads and
writes, creation, deletion, size changes, seeking, and directory listings.
The filesystem has a single root directory; it is not a general nested-directory
filesystem.

### Program loading and virtual memory

The ELF loader validates 64-bit RISC-V executables, processes `PT_LOAD`
segments, maps memory with segment permissions, copies program data, and
clears the uninitialized region.

The memory manager allocates physical pages from a best-fit free-chunk list
and manages Sv39 mappings and access flags. It includes user-pointer checks
and page-fault handling that allocates pages in the user address range.
Address-space cloning copies user pages; copy-on-write is not implemented.

### Processes and system calls

Processes associate a kernel thread with an address space and an I/O table.
`exec` loads a program and builds its initial user stack; `fork` copies the
address space and inherits I/O references. Scheduling code switches the
selected process's address space along with its thread context.

The trap and syscall paths use RISC-V CSRs, saved trap frames, `ecall`, and
`sret` to cross the user/kernel boundary. Services include process control,
file and device I/O, file creation/deletion, sleep, descriptor duplication,
and pipe creation.

### Pipes, shell, and utilities

Pipe endpoints share a circular buffer and use condition variables for empty
and full buffer waits. The implementation handles end-of-file and broken-pipe
conditions.

The shell parses arguments, adds the `c/` program prefix when appropriate,
launches programs through `fork`/`exec`, configures standard streams, and
includes input/output redirection and a two-command pipeline.

## Course milestones

1. **Storage and loading:** VirtIO block I/O, RAM disk, cache, read-only KTFS,
   and ELF loading.
2. **User-mode execution:** virtual memory, physical-page allocation, page
   faults, process abstraction, system calls, and writable KTFS operations.
3. **Multi-process interaction:** address-space cloning, `fork`, scheduling
   extensions, pipes, a shell, directory listings, and command-line utilities.

## Repository layout

```text
sys/                 Kernel, memory, processes, filesystem, traps, and syscalls
sys/dev/             UART, RTC, VirtIO, and RAM-disk device drivers
sys/tests/           Kernel test harness and the original placeholder test
usr/                 User runtime, syscall wrappers, and linker scripts
usr/progs/           Shell, hello, and eight Unix-style utilities
LICENSES/            Reference license texts for identified upstream components
NOTICE.md            Attribution and distribution notes
```

## Build and run

Use a Linux/Unix environment (or an appropriately configured WSL environment)
with GNU Make, `riscv64-unknown-elf-gcc` and binutils, and
`qemu-system-riscv64`.

Compile the user programs:

```sh
make -C usr
```

Create `sys/ktfs.raw` with the course's `mkfs_ktfs` tool. That precompiled tool
is not distributed here and must be obtained separately from an authorized
course distribution. If it is placed at `util/mkfs_ktfs`, an example image
creation command from the repository root is:

```sh
./util/mkfs_ktfs sys/ktfs.raw 8M 64 \
  usr/bin/hello usr/bin/shell usr/bin/cat usr/bin/date usr/bin/echo \
  usr/bin/ls usr/bin/rm usr/bin/touch usr/bin/wc usr/bin/xargs
```

Build and launch the kernel with the filesystem image:

```sh
make -C sys
make -C sys run
```

`sys/main.c` is configured to mount KTFS at `c`, mount device I/O at `dev`,
and load `shell` as the initial program. `make -C sys debug` starts QEMU with
`-S -s` for GDB attachment. These commands document the existing build
configuration; a fresh build and runtime demonstration are not certified by
this repository import.

## Validation and known limitations

- The checked-in kernel test suite currently contains a placeholder test
  that returns success. It is not a comprehensive test suite.
- User-mode timer/preemption code exists, but the current assembly interrupt
  path routes interrupts to the supervisor-mode handler. The routing and
  timer initialization need review before claiming working preemptive
  multitasking.
- Filesystem edge cases, resource cleanup, user-memory protection, and shell
  redirection need further integration testing. No reliability or performance
  guarantee is implied.
- The course game binaries, their data, and the precompiled image builder
  are omitted. This source snapshot requires the separately obtained image
  builder to prepare a bootable disk image.

## Attribution and provenance

This repository is maintained by [liangqiaozheng05](https://github.com/liangqiaozheng05)
as a portfolio copy of a collaborative UIUC course project. It includes
University of Illinois course framework code and student implementations;
it is not a claim of sole authorship.

The original snapshot's team README identifies:

- `xuzhehe2` (`xuzhe23`)
- `xinyi39` (`ZetaXiUpsilon`)
- `yuyue2` (`markfsde`)

Original copyright headers and in-file attribution are retained. The import
starts from a local source snapshot without `.git` metadata; the school's
commit history and branch records are not present. No school repository is
modified by this portfolio copy.

See [NOTICE.md](NOTICE.md) for third-party notices and licensing scope.

## Academic integrity

This is historical project material for portfolio review. Do not submit it
as coursework or use it in ways that conflict with your course rules.
Publication does not establish instructor approval or override course
policies, contributor rights, or the licenses of included components.
