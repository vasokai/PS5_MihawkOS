/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PRX_DATA_H
#define PW_PRX_DATA_H
#include <stddef.h>
#include <stdint.h>
/* Move one non-executable run of a user-space .prx.elf off flexible memory onto a fresh direct-memory mapping.
 *
 * The loader maps the whole image as private anonymous memory, relocates it, then copies executable runs into JIT
 * objects (native/pw_prx_elf_ps5.c). The data runs stay anonymous flexible memory, one copy in every process, and
 * sceKernelGetResidentFmemCount counts them. Direct memory does not. The bytes are relocated, so they are not
 * shareable with another process's copy of the same file; each process gets its own direct pages.
 *
 * The kernel is reached only through PwPrxDataOps. A failure before the flexible mapping is removed leaves that
 * mapping in place (PW_PRX_DATA_KEPT). A failure after it is removed tries to put an anonymous mapping back and
 * copy the bytes from the scratch; if that also fails the range is gone (PW_PRX_DATA_BROKEN) and the caller fails
 * the load, which ends the process. Nothing here writes the kernel image or a disk. */
enum { PW_PRX_DATA_DIRECT = 0, PW_PRX_DATA_KEPT = 1, PW_PRX_DATA_BROKEN = -1 };

typedef struct PwPrxDataOps {
    void *context;
    size_t page;
    /* Direct memory of bytes. 0, or nonzero with nothing taken. */
    int (*allocate)(void *context, size_t bytes, uint64_t *offset);
    /* Map that memory read-write at an address the kernel chooses. 0 with *where, or nonzero. */
    int (*map_new)(void *context, size_t bytes, uint64_t offset, uintptr_t *where);
    /* Map the same memory read-write at exactly address. 0, or nonzero with address unchanged. */
    int (*map_fixed)(void *context, uintptr_t address, size_t bytes, uint64_t offset);
    /* Private anonymous read-write at exactly address, for the restore. 0, or nonzero. */
    int (*map_anon)(void *context, uintptr_t address, size_t bytes);
    int (*unmap)(void *context, uintptr_t address, size_t bytes);
    int (*release)(void *context, uint64_t offset, size_t bytes);
    /* protection is the POSIX bits the caller asked for (1 read, 2 write, 4 exec). 0, or nonzero. */
    int (*protect)(void *context, uintptr_t address, size_t bytes, int protection);
    void (*copy)(void *dst, const void *src, size_t bytes);
} PwPrxDataOps;

#ifdef __cplusplus
extern "C" {
#endif
/*  PW_PRX_DATA_DIRECT, KEPT, or BROKEN. address and bytes are multiples of ops->page, and bytes is nonzero.
 *  An unaligned range or executable protection is KEPT without asking the kernel: execute does not stick on
 *  direct memory, and a direct mapping is placed on a page. */
int pw_prx_data_move(const PwPrxDataOps *ops, uintptr_t address, size_t bytes, int protection);
#ifdef __cplusplus
}
#endif
#endif
