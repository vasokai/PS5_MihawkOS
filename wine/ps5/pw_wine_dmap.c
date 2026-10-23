/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_dmap.h"
#include <errno.h>
#include <string.h>

enum { UNKNOWN, FILE_BACKED, DIRECT };
_Static_assert(PW_WINE_DMAP_FILES < 65535, "a file slot packs into the chunk key's low 16 bits");
static PwWineDmapOps dmap_ops;
static struct { uint64_t dev, ino; int state; } files[PW_WINE_DMAP_FILES];
static unsigned file_count;
/* chunk index + 1 and file slot + 1 packed in key (0 = empty), open addressing */
static struct { uint64_t key; int64_t dmem; uint64_t chunk_offset, chunk_bytes; } chunks[PW_WINE_DMAP_CHUNKS];
static uint64_t requests, remembered, failures;

void pw_wine_dmap_init(const PwWineDmapOps *ops)
{
    dmap_ops = ops ? *ops : (PwWineDmapOps){ 0 };
    memset(files, 0, sizeof(files));
    memset(chunks, 0, sizeof(chunks));
    file_count = 0;
    requests = remembered = failures = 0;
}

static int slot_of(int fd)
{
    uint64_t dev, ino;
    if (!dmap_ops.identity || dmap_ops.identity(dmap_ops.context, fd, &dev, &ino)) return -1;
    for (unsigned i = 0; i < file_count; i++) if (files[i].dev == dev && files[i].ino == ino) return (int)i;
    if (file_count == PW_WINE_DMAP_FILES) { errno = ENOSPC; return -2; }
    files[file_count] = (typeof(files[0])){ dev, ino, UNKNOWN };
    return (int)file_count++;
}

/* The chunk holding offset of file slot: remembered, or asked of the title. */
static int chunk_of(int slot, uint64_t offset, int64_t *dmem, uint64_t *chunk_offset, uint64_t *chunk_bytes)
{
    /* Chunks are 2 MiB (pw_wine_dsection.h); the key is the 2 MiB index in the high bits and the file slot in the low
     * 16, and the title's answer says the real extent. */
    const uint64_t index = offset >> 21, key = ((index + 1) << 16) | (uint64_t)(slot + 1);
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 49) % PW_WINE_DMAP_CHUNKS;
    for (uint32_t probe = 0; probe < PW_WINE_DMAP_CHUNKS; probe++, h = (h + 1) % PW_WINE_DMAP_CHUNKS) {
        if (chunks[h].key == key) {
            *dmem = chunks[h].dmem; *chunk_offset = chunks[h].chunk_offset; *chunk_bytes = chunks[h].chunk_bytes;
            return 0;
        }
        if (!chunks[h].key) break;
    }
    requests++;
    if (dmap_ops.page(dmap_ops.context, files[slot].dev, files[slot].ino, offset, dmem, chunk_offset, chunk_bytes))
        return -1;
    if (!chunks[h].key && remembered < PW_WINE_DMAP_CHUNKS - 1) {   /* a full table only costs a request per fault */
        chunks[h].key = key; chunks[h].dmem = *dmem;
        chunks[h].chunk_offset = *chunk_offset; chunks[h].chunk_bytes = *chunk_bytes;
        remembered++;
    }
    return 0;
}

int pw_wine_dmap_direct(int fd)
{
    const int slot = slot_of(fd);
    if (slot == -1) return 0;   /* not a file this process can name: the caller maps it */
    if (slot < 0) return -1;    /* the file table is full; mapping the file would be a second copy */
    if (files[slot].state == UNKNOWN) {
        int64_t dmem; uint64_t chunk_offset, chunk_bytes;
        if (!chunk_of(slot, 0, &dmem, &chunk_offset, &chunk_bytes)) files[slot].state = DIRECT;
        else if (errno == ENOENT) files[slot].state = FILE_BACKED;
        else { failures++; return -1; }
    }
    return files[slot].state == DIRECT;
}

/* The chunk holding offset of file slot, checked to hold it. 0, or -1 (counted). */
static int lookup(int slot, uint64_t offset, int64_t *dmem, uint64_t *chunk_offset, uint64_t *chunk_bytes)
{
    if (chunk_of(slot, offset, dmem, chunk_offset, chunk_bytes)) { failures++; return -1; }
    if (offset < *chunk_offset || offset >= *chunk_offset + *chunk_bytes) { failures++; errno = EIO; return -1; }
    return 0;
}

int pw_wine_dmap_lookup(int fd, uint64_t offset, int64_t *dmem, uint64_t *chunk_offset, uint64_t *chunk_bytes)
{
    const int slot = slot_of(fd);
    if (slot < 0 || files[slot].state != DIRECT || !dmem || !chunk_offset || !chunk_bytes) {
        errno = EINVAL;
        return -1;
    }
    return lookup(slot, offset, dmem, chunk_offset, chunk_bytes);
}

int pw_wine_dmap_map(int fd, uintptr_t address, size_t bytes, uint64_t offset)
{
    const int slot = slot_of(fd);
    if (slot < 0 || files[slot].state != DIRECT) { errno = EINVAL; return -1; }
    while (bytes) {
        int64_t dmem; uint64_t chunk_offset, chunk_bytes;
        if (lookup(slot, offset, &dmem, &chunk_offset, &chunk_bytes)) return -1;
        const uint64_t inside = offset - chunk_offset, left = chunk_bytes - inside;
        const size_t piece = bytes < left ? bytes : (size_t)left;
        /* The kernel maps direct memory in 16 KiB pages. A WoW64 view is a multiple of 4 KiB
         * (Battle.net's Mojo shared buffers, mapped at 0x4e10000 for 0x1000). Cover the host
         * pages the piece touches, still inside this chunk, so the bytes asked for stay the
         * section's. An aligned piece is unchanged. */
        const uint64_t host = 16384;
        const uintptr_t map_at = address & ~(uintptr_t)(host - 1);
        const uint64_t lead = (uint64_t)(address - map_at);
        if (lead > inside) { failures++; errno = EINVAL; return -1; }
        const uint64_t map_inside = inside - lead;
        const uintptr_t chunk_end = address + (uintptr_t)left;
        uintptr_t map_end = (address + piece + (uintptr_t)(host - 1)) & ~(uintptr_t)(host - 1);
        if (map_end < address + piece) { failures++; errno = EINVAL; return -1; }
        if (map_end > chunk_end) map_end = chunk_end;
        const size_t map_bytes = (size_t)(map_end - map_at);
        const int64_t map_dmem = dmem + (int64_t)map_inside;
        if (!map_bytes || map_end < address + piece || (map_at & (host - 1)) || (map_bytes & (host - 1)) ||
            ((uint64_t)map_dmem & (host - 1))) {
            failures++;
            errno = EINVAL;
            return -1;
        }
        if (dmap_ops.map(dmap_ops.context, map_at, map_bytes, map_dmem)) {
            failures++;
            errno = ENXIO;
            return -1;
        }
        address += piece; offset += piece; bytes -= piece;
    }
    return 0;
}

void pw_wine_dmap_stats(uint64_t *r, uint64_t *c, uint64_t *f)
{
    if (r) *r = requests;
    if (c) *c = remembered;
    if (f) *f = failures;
}
