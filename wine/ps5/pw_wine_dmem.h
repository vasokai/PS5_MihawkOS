/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_DMEM_H
#define PW_WINE_DMEM_H
#include <stddef.h>
#include <stdint.h>

/* Anonymous memory backed by the console's direct memory instead of its
 * flexible memory.
 *
 * A title has about 440 MiB of flexible memory, which every anonymous
 * mapping draws from, and up to 12 GiB of direct memory, which it allocates
 * by physical offset and maps where it chooses (measured on FW 12.02). This
 * module owns address regions, each reserved up front, and backs the pages
 * inside them with direct memory on demand. A page of a region is reserved
 * (nothing behind it: an access faults), backed by a run of direct memory
 * mapped read-write and then protected, or holds a mapping of the caller's
 * (a file). Freed pages return to reserved and their direct memory is
 * released at once.
 *
 * The kernel is reached only through PwWineDmemOps, so the policy is
 * portable and a host test stands in for the console. Every entry point
 * takes the module's own lock and none allocates from a heap: the table of
 * runs is caller storage.
 *
 * Wine never unmaps, maps over or reserves again over a kernel entry it did
 * not make. A run is Wine's own mapping; anything else in a region's bare
 * part (no run) is a reservation of Wine's, a hole, or foreign: an entry the
 * kernel shows with another protection or flags (ops.foreign). The console
 * hung unmapping a bare 28 GiB span in a helper whose GPU had started
 * ("unwire timeout at 0xc80000000 of pid 1098", late-library 2026-10-15, and
 * the freezes of the earlier emulator experiment (removed) on 2026-10-13/14 at the same address). So a commit that
 * would map over a foreign entry is refused, a release unmaps around it and
 * keeps its region, and the entry is recorded as a hole for Wine's views
 * (pw_wine_dmem_take_hole). Only bare parts are asked about, one query per
 * part and per entry in it; a run is torn down without asking, by a
 * reservation laid over it in one call, so no hole opens for a mapping made
 * without an address to land in, and a hole found in a bare part is reserved
 * again before it is trusted (pw_wine_vm_foreign). Limits: the kernel does not
 * list physical shared memory over a reservation, so such a mapping is
 * invisible here; an entry the kernel places inside a reservation between the
 * query and the fixed mapping is not seen. A placed GPU mapping
 * (VK_EXT_map_memory_placed, patch 0951) goes over a range win32u gave back to
 * a bare reservation first (patch 1130: __wine_ps5_placed_range_bare calls
 * __wine_ps5_dmem_file_region, then __wine_ps5_dmem_file_released, under the
 * virtual mutex), and is wired direct memory, never physical shared memory
 * (graphics patch 0014), so the engine reads it as foreign;
 * winevulkan still unmaps it with VK_MEMORY_UNMAP_RESERVE_BIT_EXT before
 * NtFreeVirtualMemory (tests/test_placed_unmap_order.py holds both orders,
 * tests/test_crash_replays.c runs them). */

enum {
    PW_WINE_DMEM_READ = 1,
    PW_WINE_DMEM_WRITE = 2,
    PW_WINE_DMEM_EXEC = 4,
    PW_WINE_DMEM_MAX_REGIONS = 4096,
};

/* What the module asks of the kernel. Addresses and sizes are multiples of
 * `page`. Each returns 0, or nonzero with nothing changed. */
typedef struct PwWineDmemOps {
    void *context;
    size_t page;
    /* Claim [address, +bytes) without memory behind it; refuse to replace
     * anything mapped there. */
    int (*reserve)(void *context, uintptr_t address, size_t bytes);
    /* Take `bytes` of direct memory; *offset is its physical offset. */
    int (*allocate)(void *context, size_t bytes, int64_t *offset);
    /* Map direct memory read-write at exactly address, over a reservation. */
    int (*map)(void *context, uintptr_t address, size_t bytes, int64_t offset);
    /* PW_WINE_DMEM_* protection of a mapped range. */
    int (*protect)(void *context, uintptr_t address, size_t bytes, unsigned protection);
    /* Remove whatever is mapped or reserved there. */
    int (*unmap)(void *context, uintptr_t address, size_t bytes);
    /* Give back direct memory, possibly a part of one allocation. */
    int (*release)(void *context, int64_t offset, size_t bytes);
    /* 1 in a payload: execute reaches protect only for the caller's runs (JIT memory), never for direct or flexible
     * runs, whose execute the kernel drops (exec-kernel-2026-10-14: 0x1/0x3) and which are converted at their first
     * execute instead. 0 (a title): execute passes through. */
    int exec_caller_only;
    /* Optional, NULL: unmap, then reserve. A reservation laid over Wine's own mapping, replacing it in one call (the
     * GPU client releases its direct memory and pw_wine_pages its shared pages this way). */
    int (*lay)(void *context, uintptr_t address, size_t bytes);
    /* Optional, NULL: no guard. The first kernel entry in [address, +bytes) that is not a bare reservation: 1 with its
     * range (it may reach outside) and protection, 0 when there is none, -1 when the kernel could not be asked; 2 for a
     * range the caller keeps foreign by its own record (a placed GPU mapping's, inside a view of the caller's): refused
     * and gone around as 1 is, but not recorded as a hole. */
    int (*foreign)(void *context, uintptr_t address, size_t bytes, uintptr_t *start, uintptr_t *end,
                   unsigned *protection);
} PwWineDmemOps;

/* Virtual pages mapped onto contiguous direct memory, or, with offset
 * PW_WINE_DMEM_CALLER, a mapping of the caller's. */
#define PW_WINE_DMEM_CALLER ((int64_t)-1)
typedef struct PwWineDmemRun {
    uintptr_t address;
    size_t bytes;
    int64_t offset;
} PwWineDmemRun;

/* Which call failed last (PwWineDmemStats.failed): a count alone did not say where a 32-bit game's write to
 * unbacked memory came from (2026-10-07, record removed from the tree; in git history: failures=1, then a fault). */
enum {
    PW_WINE_DMEM_FAILED_RELEASE = 1,  /* giving direct memory back */
    PW_WINE_DMEM_FAILED_ALLOCATE,     /* taking direct memory */
    PW_WINE_DMEM_FAILED_MAP,          /* mapping it */
    PW_WINE_DMEM_FAILED_PROTECT,      /* protecting a range */
    PW_WINE_DMEM_FAILED_UNMAP,        /* clearing a range for a mapping of the caller's */
    PW_WINE_DMEM_FAILED_RESERVE,      /* reserving a range again */
    PW_WINE_DMEM_FAILED_OUTSIDE,      /* a range outside the regions, or the table full */
    PW_WINE_DMEM_FAILED_FOREIGN,      /* a mapping over a foreign entry refused, or the kernel not asked */
};

typedef struct PwWineDmemStats {
    uint64_t backed_bytes, peak_backed_bytes;
    /* Of those, below 4 GiB: the part an i386 guest's address space holds. */
    uint64_t low_backed_bytes, peak_low_backed_bytes;
    uint64_t allocations, releases, maps, protects, failures;
    uint32_t runs, peak_runs;
    /* The last failure: PW_WINE_DMEM_FAILED_*, and the range it was for. */
    uint32_t failed;
    uintptr_t failed_address;
    uint64_t failed_bytes;
    /* Foreign entries recorded, unmaps that went around one, mappings refused (one found, or no answer), and queries
     * the kernel did not answer. */
    uint64_t foreign_found, unmaps_skipped, maps_refused, query_failures;
} PwWineDmemStats;

/* A foreign entry's range inside the regions, on whole pages, clipped to the call that found it. pending until the
 * caller takes it (pw_wine_dmem_take_hole), and again at each later skip or refusal there. */
enum { PW_WINE_DMEM_HOLES = 64 };
typedef struct PwWineDmemHole {
    uintptr_t address;
    size_t bytes;
    uint32_t protection, skips, refusals, pending;
} PwWineDmemHole;

enum { PW_WINE_DMEM_DOING_NONE, PW_WINE_DMEM_DOING_ALLOCATE, PW_WINE_DMEM_DOING_MAP, PW_WINE_DMEM_DOING_PROTECT,
       PW_WINE_DMEM_DOING_RELEASE, PW_WINE_DMEM_DOING_UNMAP, PW_WINE_DMEM_DOING_RESERVE, PW_WINE_DMEM_DOING_LAY,
       PW_WINE_DMEM_DOING_FOREIGN, PW_WINE_DMEM_DOINGS };

typedef struct PwWineDmem {
    PwWineDmemOps ops;
    int lock;
    /* Who holds lock (its kernel thread id, pw_wine_perf_tid; 0 when free) and since when (a TSC tick): diagnostics,
     * read with atomic loads and never the lock (__wine_ps5_dmem_holder). The Unix heap holds it across direct-memory
     * calls, and a holder stuck in one stops every fault and every 4 KiB MMU classify spinning behind it. */
    uint64_t holder, held_since;
    /* What the holder asks the kernel now (PW_WINE_DMEM_DOING_*; kept until the next call or the unlock, 0 after), of
     * which range and protection, and the direct memory's offset (-1 when there is none yet): read like holder. The
     * holder stuck 30 s and more in NtProtectVirtualMemory (2026-10-16; record removed from the tree; in git history) could
     * not be told apart from its allocate, map or protect. */
    uint32_t doing, doing_protection;
    uint64_t doing_address, doing_bytes;
    int64_t doing_offset;
    uintptr_t region_low[PW_WINE_DMEM_MAX_REGIONS], region_high[PW_WINE_DMEM_MAX_REGIONS];
    unsigned regions;
    PwWineDmemRun *runs;       /* sorted by address, never overlapping */
    uint32_t run_count, run_capacity;
    unsigned zero_fill;
    PwWineDmemHole holes[PW_WINE_DMEM_HOLES];   /* a full table only counts: the query is the guard, not the table */
    unsigned hole_count;
    PwWineDmemStats stats;
} PwWineDmem;

/* runs is storage for capacity entries. zero_fill clears newly backed
 * pages, for a kernel that hands out direct memory uncleared. 0, or -1. */
int pw_wine_dmem_init(PwWineDmem *, const PwWineDmemOps *, PwWineDmemRun *runs,
                      uint32_t capacity, unsigned zero_fill);
/* Own [address, +bytes) from now on, reserving it first unless the caller
 * already has. A region touching or overlapping one already owned joins
 * it. 0, or -1. */
int pw_wine_dmem_add_region(PwWineDmem *, uintptr_t address, size_t bytes, int reserved);
/* Release [address, +bytes): unmapped in the kernel and no longer owned (a later replace would otherwise take the bare
 * range for a reservation it no longer is), its runs forgotten and their direct memory released; a foreign entry is
 * left as it is, with its pages still owned, and recorded. 0, or -1 (bad range, a full table, a refused unmap, or a
 * query the kernel did not answer: the rest of the range is left as it was). */
int pw_wine_dmem_unmap(PwWineDmem *, uintptr_t address, size_t bytes);
/* The caller keeps [address, +bytes) reserved instead of unmapping it (Wine's reserved area, patch 1128): its bare
 * parts are asked about and every foreign entry recorded. 0, or -1 when it is not inside one region. */
int pw_wine_dmem_keep(PwWineDmem *, uintptr_t address, size_t bytes);
/* 1 when [address, +bytes) lies inside one region. */
int pw_wine_dmem_owns(PwWineDmem *, uintptr_t address, size_t bytes);
/* Where [address, +bytes) first crosses a region's edge: *piece is the
 * length of its leading part, which is wholly inside a region (1) or wholly
 * outside every region (0). A range touching both is handled piece by
 * piece. */
int pw_wine_dmem_split(PwWineDmem *, uintptr_t address, size_t bytes, size_t *piece);

/* Fresh zeroed memory with this protection, replacing whatever the range
 * held, backed or the caller's; protection 0 leaves it reserved. This is
 * an anonymous MAP_FIXED mapping. 0, or -1 with the range reserved, and
 * errno EEXIST when a foreign entry (or no answer) refused it before any
 * change. Only the runs are torn down: a bare part is left alone, it is
 * reserved already, so protection 0 over no run asks the kernel nothing. */
int pw_wine_dmem_replace(PwWineDmem *, uintptr_t address, size_t bytes, unsigned protection);
/* Before the caller maps something of its own over [address, +bytes) with a fixed mapping: 0, or -1 with errno EEXIST
 * when a foreign entry lies in its bare parts (or the kernel did not answer), recorded. */
int pw_wine_dmem_check(PwWineDmem *, uintptr_t address, size_t bytes);
/* As pw_wine_dmem_check, for guards of the caller's own wherever they lie (a demand-paged view's unmapped pages, adopted
 * as the caller's run or outside the regions): every entry in the range is asked about, runs or not, before the
 * caller's fixed mapping there. 0, or -1 with errno EEXIST, recorded and counted as a refused mapping. */
int pw_wine_dmem_check_guards(PwWineDmem *, uintptr_t address, size_t bytes);
/* The caller has mapped something of its own (a file, or a payload's JIT
 * memory) over the range with a fixed mapping: the direct memory it replaced
 * is released, and the range is protected like the rest but never backed.
 * 0, or -1. */
int pw_wine_dmem_adopt(PwWineDmem *, uintptr_t address, size_t bytes);
/* The caller's range [address, +bytes), adopted and since given back to guards of its own (a demand-paged view):
 * its runs are forgotten, so it is bare again and a release asks the kernel what is there. 0, or -1 (not inside one
 * region, a full table, or a run there that is not the caller's). */
int pw_wine_dmem_disown(PwWineDmem *, uintptr_t address, size_t bytes);
/* The next pending hole, no longer pending: 1, or 0 when there is none. */
int pw_wine_dmem_take_hole(PwWineDmem *, uintptr_t *address, size_t *bytes);
/* 1 when pw_wine_dmem_adopt of the range would succeed now (inside a region, room in the table), so a caller can ask
 * before it replaces the mapping, which cannot be undone. */
int pw_wine_dmem_can_adopt(PwWineDmem *, uintptr_t address, size_t bytes);
/* Commit: the backed pages and the caller's mappings keep their contents
 * and take the protection; reserved pages are backed, zeroed, first,
 * unless protection is 0. 0, or -1 (errno EEXIST: refused before any change,
 * as pw_wine_dmem_replace). */
int pw_wine_dmem_protect(PwWineDmem *, uintptr_t address, size_t bytes, unsigned protection);
/* The direct-memory run holding address, never a mapping of the caller's: 1 with *run, else 0. A run is one mapping
 * of contiguous direct memory ([run.address, +run.bytes) from run.offset; add_run joins neighbours only where both
 * continue), which a second mapping of the same memory (pw_wine_alias.h) may cover. Valid until the engine's next
 * change: the caller holds Wine's virtual mutex, as every release of a Windows view's memory does. */
int pw_wine_dmem_run_at(PwWineDmem *, uintptr_t address, PwWineDmemRun *run);
/* How many bytes of the range are backed by direct memory. */
size_t pw_wine_dmem_backed(PwWineDmem *, uintptr_t address, size_t bytes);
void pw_wine_dmem_stats(PwWineDmem *, PwWineDmemStats *);

/* 1 when a mapping should be given a fresh direct-memory region of its own: private, anonymous, not shared,
 * not placed at a fixed address, and not executable. A fixed commit inside a reservation Wine already enrolled
 * stays on that region. An executable mapping stays on flexible memory: the kernel drops execute from direct
 * memory, and a Unix mmap that asks for execute has to be able to run on return (the first-execute conversion
 * is for Wine's fault path). Each argument is 0 or nonzero. */
static inline int pw_wine_anon_fresh_direct(int anonymous, int private_map, int shared, int fixed, int executable)
{
    return anonymous && private_map && !shared && !fixed && !executable;
}

#endif
