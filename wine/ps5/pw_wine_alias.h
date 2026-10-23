/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_ALIAS_H
#define PW_WINE_ALIAS_H
#include <stddef.h>
#include <stdint.h>

/* A second, writable mapping (an alias) of the direct memory behind a host page of a Windows view, for the 4 KiB MMU
 * (docs/MMU4K.md, sections 5 and 8): when the kernel holds a 16 KiB host page read-only because one of its four
 * Windows pages is (patch 1122's rule), a store to one of its plain pages goes through the alias instead of opening
 * the page and stepping the store, which costs two signals, 86 us a store (vmp-tracker-2026-10-14-03), and held a Unity game
 * in the earlier emulator experiment (removed) at 2.0-2.4 fps (2026-10-15; record removed from the tree; in git history).
 *
 * Sources (ops.backing): a payload's direct-memory run (__wine_ps5_dmem_backing over pw_wine_dmem_run_at) and a direct
 * section's chunk mapped in its view (__wine_ps5_section_backing over pw_wine_section_backing and pw_wine_dmap_lookup).
 * Flexible memory, file pages (the section cache) and JIT memory have no direct memory to map twice: no alias.
 *
 * A window maps up to PW_WINE_ALIAS_WINDOW of direct memory around the host page, inside the 2 MiB-aligned block that
 * holds it and the range the source backs contiguously, CPU read-write, at a run of the helper's own reserved range
 * (ops.take, ops.give: pw_wine_pages_take, pw_wine_pages_give), never where the kernel chooses (pw_wine_pages.h).
 * Windows are keyed by direct-memory offset, not by address: a range of the program's that changes backing never
 * reaches an old window, and two views of one memory share one. At most PW_WINE_ALIAS_WINDOWS live (32 MiB of the
 * helper's 128 MiB range); a new one past that, or past a full range, evicts the least recently used (given back)
 * first.
 *
 * Invariant I4: no alias maps direct memory after it is released. pw_wine_alias_forget, called by every release of
 * direct memory (pw_wine_dmem_ps5.c's release(), before sceKernelReleaseDirectMemory), gives back every window over any
 * of it before it returns. Which release paths reach it, and why the others hold no alias: docs/MMU4K.md section 8;
 * tests/test_crash_replays.c ("alias outlives its memory") drives each one against the fake kernel, which records any
 * release of direct memory that is still mapped.
 *
 * Generation: bumped by every eviction and every forget that gave a window back. A holder of an alias address (an
 * engine run) compares it with the value it saw when it got the address and asks again when it changed.
 *
 * Locks: get runs under Wine's virtual mutex and calls backing before it takes this module's lock (backing takes the
 * direct-memory engine's lock); forget runs on any thread inside the engine's lock. Order: the virtual mutex, the
 * engine, this module, the shared pages. No heap. Before pw_wine_alias_init (a title, a host build, the engine off) get
 * answers 0 without asking anything and forget finds nothing. */

enum {
    PW_WINE_ALIAS_HOST_PAGE = 16384,
    PW_WINE_ALIAS_WINDOW = 2u << 20,
    PW_WINE_ALIAS_WINDOWS = 16,        /* the default limit (pw_wine_alias_set_windows) */
    PW_WINE_ALIAS_WINDOWS_MAX = 64,
};

typedef struct PwWineAliasOps {
    void *context;
    /* The direct memory behind host page address: 1 with *dmem (its offset) and the virtual range [*low, *high) around
     * it backed contiguously by the same memory, or 0 (nothing to alias). */
    int (*backing)(void *context, uintptr_t address, int64_t *dmem, uintptr_t *low, uintptr_t *high);
    /* A run of bytes of address space reserved for the caller's own fixed mapping: its address, or 0 (full). */
    uintptr_t (*take)(void *context, size_t bytes);
    /* That run back, its mapping replaced by the reservation. */
    void (*give)(void *context, uintptr_t address, size_t bytes);
    /* Map bytes of direct memory from dmem, CPU read-write, at exactly address over its reservation. 0, or nonzero. */
    int (*map)(void *context, uintptr_t address, size_t bytes, int64_t dmem);
} PwWineAliasOps;

/* maps: windows mapped; map_failures: maps the kernel refused; forgets: windows given back by a release; evictions:
 * windows given back to make room; noalias: gets answered 0 for anything else (no backing, an answer that does not
 * hold the page, no room after one eviction). */
typedef struct PwWineAliasStats {
    uint64_t maps, map_failures, forgets, evictions, noalias;
} PwWineAliasStats;

/* PW_OK; PW_ERR_PRECONDITION (an op missing), PW_ERR_STATE (initialized already) (include/prospero_win.h). */
int pw_wine_alias_init(const PwWineAliasOps *ops);
/* The alias of host page host_page (16 KiB aligned): an address that maps the same 16 KiB of direct memory read-write,
 * made (with its window) when no window holds it; 0 when there is none (not initialized, misaligned, or counted).
 * Valid until the generation changes. Under Wine's virtual mutex. */
uintptr_t pw_wine_alias_get(uintptr_t host_page);
/* The alias of host_page when a window already maps that direct memory, or 0. Does not create a window, does not
 * change which window is oldest, and does not count a miss. Same lock order as get (backing, then this module). */
uintptr_t pw_wine_alias_find(uintptr_t host_page);
/* Direct memory [dmem, +bytes) is about to be released: every window over any of it is given back before this
 * returns. */
void pw_wine_alias_forget(int64_t dmem, uint64_t bytes);
uint64_t pw_wine_alias_generation(void);
/* Whether a full table evicts its least recently used window (the default) or answers 0, counted as noalias, so the
 * store steps (patch 1122). An A/B switch (WINE_PS5_ALIAS_EVICT=0): in three runs of a Unity game on the 4 KiB MMU a
 * thread stuck 15-30 s in NtProtectVirtualMemory's kernel mprotect of direct memory, 31-37 s in, each shortly after
 * the table filled and windows began to be given back (2026-10-16, record removed from the tree; in git history: evictions 0 at 20 s, 14 at 30 s). */
void pw_wine_alias_set_eviction(int allowed);
/* How many windows may live at once, 1 to PW_WINE_ALIAS_WINDOWS_MAX (WINE_PS5_ALIAS_WINDOWS), before a get evicts or,
 * with eviction off, answers 0: with eviction off the game's 16 filled within its first minute and the rest of its
 * stores stepped at 2.3 fps (2026-10-16, record removed from the tree; in git history: 76,000 noalias answers per 10 s). Before any window is made. */
void pw_wine_alias_set_windows(unsigned int limit);
/* Lock-free (relaxed loads), so a census may read it wherever Wine's threads stopped. */
void pw_wine_alias_stats(PwWineAliasStats *stats);
/* Back to not initialized, with no window and zero counters, giving nothing back (tests). */
void pw_wine_alias_reset(void);

#endif
