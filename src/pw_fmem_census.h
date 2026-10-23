/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_FMEM_CENSUS_H
#define PW_FMEM_CENSUS_H
#include "pw_vm_census.h"
/* What a helper's resident system memory is made of, from one walk (src/pw_vm_census.h).
 *
 * sceKernelGetResidentFmemCount is one number per process. The walk names the mappings. This sorts each group into
 * the class that decides whether it can leave the small pool:
 *
 *   jit     executable flexible memory, or a mapping whose name says jit. The kernel charges a shared JIT object in
 *           every process that maps it (K14, K18), so identical code reported here is not a saving from sharing.
 *   data    flexible, not executable, not a stack, not a file name. Heaps and anonymous buffers. Direct memory does
 *           not count in the resident census, so this is what a move onto the application pool can take off.
 *   file    the same, when the name is a path or a dll/so/elf/prx. A private dirty copy can move; a clean file page
 *           may already be one physical page charged once per mapping, which this walk cannot see.
 *   stack   the kernel's stack flag. Left where the kernel put it.
 *   direct  the application pool. Already outside the census.
 *   other   reservations and anything the flags do not name.
 *
 * Across processes, duplicate_code is (copies - 1) times the smallest resident JIT total of that name. It is an upper
 * bound on bytes that are the same code, and it is not a saving: sharing does not lower the census. movable is data
 * plus file. */
#include <stdint.h>

enum {
    PW_FMEM_JIT = 0, PW_FMEM_DATA, PW_FMEM_FILE, PW_FMEM_STACK, PW_FMEM_DIRECT, PW_FMEM_OTHER,
    PW_FMEM_CLASSES
};

enum { PW_FMEM_NAMES = 64, PW_FMEM_SAMPLES = 256 };

typedef struct PwFmemSample {
    char name[PW_VM_NAME + 1];
    uint8_t klass;
    int32_t pid;
    uint64_t resident;
} PwFmemSample;

typedef struct PwFmemRollup {
    uint64_t class_bytes[PW_FMEM_CLASSES];
    uint64_t duplicate_code; /* upper bound on identical JIT bytes; sharing does not reclaim them */
    uint64_t movable;        /* data + file */
    unsigned processes;
    int truncated;           /* a sample did not fit in the name table */
} PwFmemRollup;

#ifdef __cplusplus
extern "C" {
#endif
/* The class of one mapping. */
uint8_t pw_fmem_class(uint8_t flags, int32_t protection, const char *name);
/* "jit=N data=N file=N stack=N direct=N other=N KiB" plus up to 8 "name=class:KiB" of the largest groups. */
size_t pw_fmem_format(const PwVmCensus *census, char *out, size_t size);
/* Sum samples. Names that do not fit still count in class_bytes and set truncated; they are left out of
 * duplicate_code. */
void pw_fmem_rollup(const PwFmemSample *samples, unsigned count, PwFmemRollup *out);
#ifdef __cplusplus
}
#endif
#endif
