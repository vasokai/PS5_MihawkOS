/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_DMAP_H
#define PW_WINE_DMAP_H
#include <stddef.h>
#include <stdint.h>

/* A helper's side of large anonymous sections in direct memory (wine/ps5/pw_wine_dsection.h): the section pager
 * (wine/ps5/pw_wine_section.c) maps a view's pages from the direct memory the title gives for each 2 MiB chunk,
 * instead of from the section's file. Which files back such a section and where each chunk is are remembered, so the
 * title is asked once per file and once per chunk, not once per fault.
 *
 * Called by the pager under Wine's virtual mutex (faults included), so there is no lock here. The title and the
 * kernel are reached only through PwWineDmapOps (host-tested: tests/test_pw_wine_section_data.c, PW_SECTION_DMEM). */

/* A full file table fails the mapping (pw_wine_dmap_direct returns -1) rather than falling back to the file: another
 * process may already have mapped that section's direct memory. */
enum { PW_WINE_DMAP_FILES = 1024, PW_WINE_DMAP_CHUNKS = 32768 };

typedef struct PwWineDmapOps {
    void *context;
    /* The file behind fd: 0, or -1 when it is not a regular file. */
    int (*identity)(void *context, int fd, uint64_t *dev, uint64_t *ino);
    /* The title's answer for a page (pw_ps5_dsection_page): 0, or -1 with errno ENOENT when the file is not such a
     * section. */
    int (*page)(void *context, uint64_t dev, uint64_t ino, uint64_t offset, int64_t *dmem, uint64_t *chunk_offset,
                uint64_t *chunk_bytes);
    /* Map bytes of direct memory at exactly address, readable and writable. 0, or -1. */
    int (*map)(void *context, uintptr_t address, size_t bytes, int64_t dmem);
} PwWineDmapOps;

void pw_wine_dmap_init(const PwWineDmapOps *ops);
/* 1 when fd's section is in direct memory, 0 when it is a file, -1 when that could not be learnt. */
int pw_wine_dmap_direct(int fd);
/* Map [address, +bytes) of the view at section offset offset from direct memory, a chunk at a time. Only for a file
 * pw_wine_dmap_direct said 1 for. 0, or -1 with errno: the title's (the range not yet touched by this call is as it
 * was), EIO when its answer does not hold the offset, or ENXIO when the kernel refused the mapping (that range may
 * have lost its reservation). Pieces mapped before the failure stay mapped. */
int pw_wine_dmap_map(int fd, uintptr_t address, size_t bytes, uint64_t offset);
/* The chunk holding offset of fd's section (pw_wine_dmap_direct said 1): remembered, else asked of the title as a fault
 * does. 0 with its direct memory, its offset in the section and its size; -1 with errno: EINVAL (not such a section),
 * the title's, or EIO (its answer does not hold offset). For a second mapping of a mapped chunk (pw_wine_alias.h). */
int pw_wine_dmap_lookup(int fd, uint64_t offset, int64_t *dmem, uint64_t *chunk_offset, uint64_t *chunk_bytes);
/* Requests to the title, chunks remembered, failures. */
void pw_wine_dmap_stats(uint64_t *requests, uint64_t *chunks, uint64_t *failures);

#endif
