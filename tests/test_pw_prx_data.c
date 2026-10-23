/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_prx_data.h"
#include <assert.h>
#include <stdalign.h>
#include <string.h>

enum { PAGE = 4096, BYTES = 8192 };

typedef struct World {
    unsigned char flex[BYTES];
    unsigned char direct[BYTES];
    unsigned char scratch[BYTES];
    int flex_mapped, direct_mapped, scratch_mapped;
    int fail_alloc, fail_map_new, fail_unmap, fail_map_fixed, fail_anon;
    int allocs, releases, anon_maps;
} World;

static int allocate(void *context, size_t bytes, uint64_t *offset)
{
    World *w = context;
    if (w->fail_alloc || bytes != BYTES) return -1;
    w->allocs++;
    *offset = 0x1000;
    return 0;
}
static int map_new(void *context, size_t bytes, uint64_t offset, uintptr_t *where)
{
    World *w = context;
    (void)offset;
    if (w->fail_map_new || bytes != BYTES) return -1;
    w->scratch_mapped = 1;
    *where = (uintptr_t)w->scratch;
    return 0;
}
static int map_fixed(void *context, uintptr_t address, size_t bytes, uint64_t offset)
{
    World *w = context;
    (void)offset;
    if (w->fail_map_fixed || address != (uintptr_t)w->flex || bytes != BYTES) return -1;
    /* The fixed map is the same direct pages as the scratch, so the bytes copied there show up here. */
    memcpy(w->flex, w->scratch, BYTES);
    w->direct_mapped = 1;
    w->flex_mapped = 0;
    return 0;
}
static int map_anon(void *context, uintptr_t address, size_t bytes)
{
    World *w = context;
    if (w->fail_anon || address != (uintptr_t)w->flex || bytes != BYTES) return -1;
    w->anon_maps++;
    w->flex_mapped = 1;
    return 0;
}
static int unmap(void *context, uintptr_t address, size_t bytes)
{
    World *w = context;
    (void)bytes;
    if (w->fail_unmap && address == (uintptr_t)w->flex) return -1;
    if (address == (uintptr_t)w->flex) w->flex_mapped = 0;
    if (address == (uintptr_t)w->scratch) w->scratch_mapped = 0;
    return 0;
}
static int release(void *context, uint64_t offset, size_t bytes)
{
    World *w = context;
    (void)offset;
    (void)bytes;
    w->releases++;
    w->direct_mapped = 0;
    return 0;
}
static int protect(void *context, uintptr_t address, size_t bytes, int protection)
{
    (void)context; (void)address; (void)bytes; (void)protection;
    return 0;
}
static void copy(void *dst, const void *src, size_t bytes)
{
    memcpy(dst, src, bytes);
}

static PwPrxDataOps ops_of(World *w)
{
    PwPrxDataOps ops = {w, PAGE, allocate, map_new, map_fixed, map_anon, unmap, release, protect, copy};
    return ops;
}

int main(void)
{
    alignas(4096) World w;
    PwPrxDataOps ops;
    memset(&w, 0, sizeof(w));
    w.flex_mapped = 1;
    memset(w.flex, 0xab, BYTES);
    ops = ops_of(&w);
    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex, BYTES, 1 | 4) == PW_PRX_DATA_KEPT);
    assert(w.allocs == 0 && w.flex[0] == 0xab);
    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex + 1, BYTES, 1) == PW_PRX_DATA_KEPT);
    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex, BYTES - 1, 1) == PW_PRX_DATA_KEPT);
    assert(w.allocs == 0);

    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex, BYTES, 1) == PW_PRX_DATA_DIRECT);
    assert(w.flex[0] == 0xab && w.flex[BYTES - 1] == 0xab);
    assert(!w.scratch_mapped && w.direct_mapped && !w.flex_mapped);
    assert(w.releases == 0);

    memset(&w, 0, sizeof(w));
    w.flex_mapped = 1;
    memset(w.flex, 0xcd, BYTES);
    w.fail_alloc = 1;
    ops = ops_of(&w);
    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex, BYTES, 1 | 2) == PW_PRX_DATA_KEPT);
    assert(w.flex_mapped && w.flex[0] == 0xcd && w.releases == 0);

    memset(&w, 0, sizeof(w));
    w.flex_mapped = 1;
    memset(w.flex, 0x11, BYTES);
    w.fail_map_fixed = 1;
    ops = ops_of(&w);
    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex, BYTES, 1) == PW_PRX_DATA_KEPT);
    assert(w.flex_mapped && w.flex[0] == 0x11 && w.anon_maps == 1 && w.releases == 1 && !w.scratch_mapped);

    memset(&w, 0, sizeof(w));
    w.flex_mapped = 1;
    w.fail_map_fixed = 1;
    w.fail_anon = 1;
    ops = ops_of(&w);
    assert(pw_prx_data_move(&ops, (uintptr_t)w.flex, BYTES, 1) == PW_PRX_DATA_BROKEN);
    assert(!w.flex_mapped && w.releases == 1 && !w.scratch_mapped);
    return 0;
}
