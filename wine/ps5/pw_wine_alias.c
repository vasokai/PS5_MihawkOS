/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_alias.h"
#include "pw_wine_spin.h"
#include "../../include/prospero_win.h"
#include <string.h>

typedef struct Window {
    uintptr_t address;   /* 0: free */
    size_t bytes;
    int64_t dmem;
    uint64_t used;       /* tick of the last get that answered from it */
} Window;

static PwWineAliasOps alias_ops;
static int alias_ready, alias_lock, evict_allowed = 1;
static Window windows[PW_WINE_ALIAS_WINDOWS_MAX];
static unsigned window_limit = PW_WINE_ALIAS_WINDOWS;
static uint64_t tick, generation;
static PwWineAliasStats counters;

/* A spin lock, as the direct-memory engine's (pw_wine_spin.h): forget runs inside that one. */
static void lock(void) { pw_wine_spin_lock(&alias_lock); }
static void unlock(void) { __atomic_store_n(&alias_lock, 0, __ATOMIC_RELEASE); }
static void count(uint64_t *counter) { __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED); }

/* Window i given back and out of the table. Under the lock. */
static void drop(unsigned i, uint64_t *counter)
{
    alias_ops.give(alias_ops.context, windows[i].address, windows[i].bytes);
    windows[i] = (Window){ 0 };
    count(counter);
    __atomic_add_fetch(&generation, 1, __ATOMIC_RELEASE);
}

/* The least recently used live window, or -1. Under the lock. */
static int oldest(void)
{
    int found = -1;
    for (unsigned i = 0; i < window_limit; i++)
        if (windows[i].address && (found < 0 || windows[i].used < windows[found].used)) found = (int)i;
    return found;
}

/* A window of bytes of direct memory from dmem: a free slot (else the oldest given back), a run taken (none: the
 * oldest given back, and asked once more), mapped. Its address, or 0 (counted). Under the lock. */
static uintptr_t make(int64_t dmem, size_t bytes)
{
    int slot = -1, victim;
    uintptr_t at;

    for (unsigned i = 0; i < window_limit && slot < 0; i++)
        if (!windows[i].address) slot = (int)i;
    if (slot < 0) {
        if (!__atomic_load_n(&evict_allowed, __ATOMIC_RELAXED)) {
            count(&counters.noalias);
            return 0;
        }
        drop((unsigned)(slot = oldest()), &counters.evictions);
    }
    if (!(at = alias_ops.take(alias_ops.context, bytes)) && __atomic_load_n(&evict_allowed, __ATOMIC_RELAXED) &&
        (victim = oldest()) >= 0) {
        drop((unsigned)victim, &counters.evictions);
        at = alias_ops.take(alias_ops.context, bytes);
    }
    if (!at) {
        count(&counters.noalias);
        return 0;
    }
    if (alias_ops.map(alias_ops.context, at, bytes, dmem)) {
        alias_ops.give(alias_ops.context, at, bytes);
        count(&counters.map_failures);
        return 0;
    }
    windows[slot] = (Window){ at, bytes, dmem, ++tick };
    count(&counters.maps);
    return at;
}

int pw_wine_alias_init(const PwWineAliasOps *ops)
{
    int status = PW_OK;

    if (!ops || !ops->backing || !ops->take || !ops->give || !ops->map) return PW_ERR_PRECONDITION;
    lock();
    if (alias_ready) {
        status = PW_ERR_STATE;
    } else {
        alias_ops = *ops;
        memset(windows, 0, sizeof(windows));
        __atomic_store_n(&alias_ready, 1, __ATOMIC_RELEASE);
    }
    unlock();
    return status;
}

uintptr_t pw_wine_alias_get(uintptr_t host_page)
{
    const uintptr_t page = PW_WINE_ALIAS_HOST_PAGE, mask = page - 1;
    uintptr_t low = 0, high = 0, from, to, alias = 0;
    int64_t dmem = -1;

    if (!__atomic_load_n(&alias_ready, __ATOMIC_ACQUIRE) || (host_page & mask)) return 0;
    /* The source answers before this module's lock is taken (it takes the engine's). */
    if (!alias_ops.backing(alias_ops.context, host_page, &dmem, &low, &high) || dmem < 0 ||
        ((low | high | (uintptr_t)dmem) & mask) || low > host_page || high < host_page + page) {
        count(&counters.noalias);
        return 0;
    }
    from = host_page & ~(uintptr_t)(PW_WINE_ALIAS_WINDOW - 1);
    to = from + PW_WINE_ALIAS_WINDOW;
    if (from < low) from = low;
    if (to > high) to = high;
    lock();
    for (unsigned i = 0; i < window_limit && !alias; i++)
        if (windows[i].address && windows[i].dmem <= dmem &&
            dmem + (int64_t)page <= windows[i].dmem + (int64_t)windows[i].bytes) {
            windows[i].used = ++tick;
            alias = windows[i].address + (uintptr_t)(dmem - windows[i].dmem);
        }
    if (!alias && (alias = make(dmem - (int64_t)(host_page - from), to - from))) alias += host_page - from;
    unlock();
    return alias;
}

uintptr_t pw_wine_alias_find(uintptr_t host_page)
{
    const uintptr_t page = PW_WINE_ALIAS_HOST_PAGE, mask = page - 1;
    uintptr_t low = 0, high = 0, alias = 0;
    int64_t dmem = -1;

    if (!__atomic_load_n(&alias_ready, __ATOMIC_ACQUIRE) || (host_page & mask) || !alias_ops.backing) return 0;
    if (!alias_ops.backing(alias_ops.context, host_page, &dmem, &low, &high) || dmem < 0) return 0;
    lock();
    for (unsigned i = 0; i < window_limit && !alias; i++)
        if (windows[i].address && windows[i].dmem <= dmem &&
            dmem + (int64_t)page <= windows[i].dmem + (int64_t)windows[i].bytes)
            alias = windows[i].address + (uintptr_t)(dmem - windows[i].dmem);
    unlock();
    return alias;
}

void pw_wine_alias_forget(int64_t dmem, uint64_t bytes)
{
    if (!__atomic_load_n(&alias_ready, __ATOMIC_ACQUIRE) || !bytes) return;
    lock();
    for (unsigned i = 0; i < window_limit; i++)
        if (windows[i].address && windows[i].dmem < dmem + (int64_t)bytes &&
            dmem < windows[i].dmem + (int64_t)windows[i].bytes)
            drop(i, &counters.forgets);
    unlock();
}

uint64_t pw_wine_alias_generation(void) { return __atomic_load_n(&generation, __ATOMIC_ACQUIRE); }

void pw_wine_alias_set_eviction(int allowed) { __atomic_store_n(&evict_allowed, allowed != 0, __ATOMIC_RELAXED); }

void pw_wine_alias_set_windows(unsigned int limit)
{
    lock();
    window_limit = limit < 1 ? 1 : limit > PW_WINE_ALIAS_WINDOWS_MAX ? PW_WINE_ALIAS_WINDOWS_MAX : limit;
    unlock();
}

void pw_wine_alias_stats(PwWineAliasStats *stats)
{
    if (!stats) return;
    stats->maps = __atomic_load_n(&counters.maps, __ATOMIC_RELAXED);
    stats->map_failures = __atomic_load_n(&counters.map_failures, __ATOMIC_RELAXED);
    stats->forgets = __atomic_load_n(&counters.forgets, __ATOMIC_RELAXED);
    stats->evictions = __atomic_load_n(&counters.evictions, __ATOMIC_RELAXED);
    stats->noalias = __atomic_load_n(&counters.noalias, __ATOMIC_RELAXED);
}

void pw_wine_alias_reset(void)
{
    lock();
    __atomic_store_n(&alias_ready, 0, __ATOMIC_RELEASE);
    memset(&alias_ops, 0, sizeof(alias_ops));
    memset(windows, 0, sizeof(windows));
    memset(&counters, 0, sizeof(counters));
    tick = 0;
    evict_allowed = 1;
    window_limit = PW_WINE_ALIAS_WINDOWS;
    __atomic_store_n(&generation, 0, __ATOMIC_RELEASE);
    unlock();
}
