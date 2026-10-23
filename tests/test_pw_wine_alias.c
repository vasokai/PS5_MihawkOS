/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cc -std=gnu11 tests/test_pw_wine_alias.c wine/ps5/pw_wine_alias.c -o /tmp/alias && /tmp/alias
 *
 * The alias module (wine/ps5/pw_wine_alias.h) on fake ops with a call log, over real host memory: "direct memory" is a
 * memfd, the program's views and every alias are shared mappings of it, so a store through an alias is read back at
 * the program's address. Cases (docs/MMU4K.md section 11): window clipping, reuse by offset, eviction, forget, a
 * refused map, a full range, no backing, before init. */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_alias.h"
#include "../include/prospero_win.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { PAGE = PW_WINE_ALIAS_HOST_PAGE, SLOTS = 20 };
#define MIB ((size_t)1 << 20)
#define WINDOW ((size_t)PW_WINE_ALIAS_WINDOW)

static int memory = -1;            /* the direct memory */
static uintptr_t program, range;   /* the program's 64 MiB, 2 MiB aligned; the helper's range of SLOTS windows */

/* The program's views: [address, +bytes) mapped from dmem. backing answers from them. */
static struct { uintptr_t address; size_t bytes; int64_t dmem; } runs[32];
static unsigned run_count;
static void view(uintptr_t address, size_t bytes, int64_t dmem)
{
    assert(mmap((void *)address, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memory, dmem) ==
           (void *)address);
    runs[run_count].address = address;
    runs[run_count].bytes = bytes;
    runs[run_count++].dmem = dmem;
}

/* The call log: what was asked, in order. */
typedef struct { char op; uintptr_t address; size_t bytes; int64_t dmem; } Call;
static Call calls[256];
static unsigned call_count;
static void logged(char op, uintptr_t address, size_t bytes, int64_t dmem)
{
    if (call_count < 256) calls[call_count] = (Call){ op, address, bytes, dmem };
    call_count++;
}
static unsigned count_of(char op)
{
    unsigned n = 0;
    for (unsigned i = 0; i < call_count && i < 256; i++) n += calls[i].op == op;
    return n;
}
static const Call *last(char op)
{
    for (unsigned i = call_count < 256 ? call_count : 256; i; i--)
        if (calls[i - 1].op == op) return &calls[i - 1];
    return NULL;
}

static int bad_answer, slots_free = SLOTS, refuse_map, take_fails;
static int fake_backing(void *c, uintptr_t at, int64_t *dmem, uintptr_t *low, uintptr_t *high)
{
    (void)c;
    logged('b', at, 0, 0);
    for (unsigned i = 0; i < run_count; i++)
        if (at >= runs[i].address && at < runs[i].address + runs[i].bytes) {
            *dmem = runs[i].dmem + (int64_t)(at - runs[i].address);
            *low = runs[i].address;
            *high = runs[i].address + runs[i].bytes;
            if (bad_answer == 1) *low = at + PAGE;         /* does not hold the page */
            if (bad_answer == 2) *dmem += 4096;           /* not a host page of direct memory */
            return 1;
        }
    return 0;
}
/* The helper's range: SLOTS slots of one window each, first-fit; slots_free of them may be taken at once. */
static int slot_used[SLOTS];
static uintptr_t fake_take(void *c, size_t bytes)
{
    (void)c;
    assert(bytes && bytes <= WINDOW && !(bytes % PAGE));
    logged('t', 0, bytes, 0);
    if (take_fails) return 0;
    for (unsigned i = 0; i < SLOTS && slots_free > 0; i++)
        if (!slot_used[i]) {
            slot_used[i] = 1;
            slots_free--;
            return range + i * WINDOW;
        }
    return 0;
}
static void fake_give(void *c, uintptr_t at, size_t bytes)
{
    const unsigned i = (unsigned)((at - range) / WINDOW);
    (void)c;
    assert(at >= range && i < SLOTS && at == range + i * WINDOW && slot_used[i]);
    logged('g', at, bytes, 0);
    /* The reservation laid back over the alias. */
    assert(mmap((void *)at, WINDOW, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)at);
    slot_used[i] = 0;
    slots_free++;
}
static int fake_map(void *c, uintptr_t at, size_t bytes, int64_t dmem)
{
    (void)c;
    logged('m', at, bytes, dmem);
    if (refuse_map) return -1;
    return mmap((void *)at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memory, dmem) == (void *)at ? 0 : -1;
}
static const PwWineAliasOps ops = { NULL, fake_backing, fake_take, fake_give, fake_map };

static void stats_are(uint64_t maps, uint64_t map_failures, uint64_t forgets, uint64_t evictions, uint64_t noalias)
{
    PwWineAliasStats s;
    pw_wine_alias_stats(&s);
    if (s.maps != maps || s.map_failures != map_failures || s.forgets != forgets || s.evictions != evictions ||
        s.noalias != noalias)
        fprintf(stderr, "stats maps=%llu failures=%llu forgets=%llu evictions=%llu noalias=%llu\n",
                (unsigned long long)s.maps, (unsigned long long)s.map_failures, (unsigned long long)s.forgets,
                (unsigned long long)s.evictions, (unsigned long long)s.noalias);
    assert(s.maps == maps && s.map_failures == map_failures && s.forgets == forgets && s.evictions == evictions &&
           s.noalias == noalias);
}
/* A store through the alias lands in the program's page, and the reverse. */
static void coherent(uintptr_t host_page, uintptr_t alias)
{
    static unsigned char value = 1;
    assert(alias && alias != host_page);
    ((volatile unsigned char *)alias)[PAGE - 1] = value;
    assert(((volatile unsigned char *)host_page)[PAGE - 1] == value);
    ((volatile unsigned char *)host_page)[0] = (unsigned char)(value + 1);
    assert(((volatile unsigned char *)alias)[0] == (unsigned char)(value + 1));
    value += 2;
}
static uintptr_t reserve(size_t bytes, size_t align)
{
    void *at = mmap(NULL, bytes + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(at != MAP_FAILED);
    return ((uintptr_t)at + align - 1) & ~(uintptr_t)(align - 1);
}
static void setup(void)
{
    pw_wine_alias_reset();
    call_count = run_count = 0;
    bad_answer = refuse_map = take_fails = 0;
    slots_free = SLOTS;
    for (unsigned i = 0; i < SLOTS; i++)
        if (slot_used[i]) {
            assert(mmap((void *)(range + i * WINDOW), WINDOW, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
                        0) == (void *)(range + i * WINDOW));
            slot_used[i] = 0;
        }
    assert(mmap((void *)program, 64 * MIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
           (void *)program);
    assert(pw_wine_alias_init(&ops) == PW_OK);
}

/* Before init nothing is asked and nothing is answered; init checks its ops and runs once. */
static void before_init(void)
{
    PwWineAliasOps missing = ops;
    pw_wine_alias_reset();
    call_count = 0;
    assert(!pw_wine_alias_get(program) && !pw_wine_alias_find(program) && !call_count);
    pw_wine_alias_forget(0, 64 * MIB);
    assert(!call_count && !pw_wine_alias_generation());
    missing.map = NULL;
    assert(pw_wine_alias_init(NULL) == PW_ERR_PRECONDITION);
    assert(pw_wine_alias_init(&missing) == PW_ERR_PRECONDITION);
    assert(pw_wine_alias_init(&ops) == PW_OK && pw_wine_alias_init(&ops) == PW_ERR_STATE);
    stats_are(0, 0, 0, 0, 0);
}

/* The window is the 2 MiB block around the page, clipped to what the source backs contiguously: a run shorter than
 * 2 MiB, and a run that crosses a 2 MiB boundary at an offset off the chunk's (a section viewed at an odd offset). */
static void clipping(void)
{
    setup();
    /* 512 KiB at 2 MiB + 64 KiB: the window is the run. */
    const uintptr_t a = program + 2 * MIB + 64 * 1024;
    view(a, 512 * 1024, 16 * MIB + 32 * 1024);
    const uintptr_t h = a + 3 * PAGE, alias = pw_wine_alias_get(h);
    const Call *map = last('m');
    assert(map && map->bytes == 512 * 1024 && map->dmem == 16 * MIB + 32 * 1024);
    assert(alias == map->address + 3 * PAGE);
    coherent(h, alias);
    /* 3 MiB + 256 KiB from 4 MiB - 256 KiB: a page past 4 MiB gets [4 MiB, 6 MiB), one before it the 256 KiB below. */
    const uintptr_t b = program + 4 * MIB - 256 * 1024;
    view(b, 3 * MIB + 256 * 1024, 40 * MIB + 16 * 1024);
    const uintptr_t above = program + 4 * MIB + PAGE, below = program + 4 * MIB - 4 * PAGE;
    const uintptr_t alias_above = pw_wine_alias_get(above);
    map = last('m');
    assert(map->bytes == WINDOW && map->dmem == 40 * MIB + 16 * 1024 + 256 * 1024);
    assert(alias_above == map->address + PAGE);
    coherent(above, alias_above);
    const uintptr_t alias_below = pw_wine_alias_get(below);
    map = last('m');
    assert(map->bytes == 256 * 1024 && map->dmem == 40 * MIB + 16 * 1024 && alias_below == map->address + 12 * PAGE);
    coherent(below, alias_below);
    /* The run's last page, at 7 MiB - 16 KiB: [6 MiB, 7 MiB). */
    const uintptr_t end = b + 3 * MIB + 256 * 1024 - PAGE;
    coherent(end, pw_wine_alias_get(end));
    assert(last('m')->bytes == MIB && last('m')->dmem == 40 * MIB + 16 * 1024 + 2 * MIB + 256 * 1024);
    stats_are(4, 0, 0, 0, 0);
}

/* Windows go by direct-memory offset: the same page again, its neighbour, and another view of the same memory at
 * another address all reuse one window. */
static void reuse(void)
{
    setup();
    view(program, 2 * MIB, 8 * MIB);
    view(program + 8 * MIB, 2 * MIB, 8 * MIB);   /* a second view of the same memory */
    const uintptr_t first = pw_wine_alias_get(program + 5 * PAGE);
    assert(count_of('m') == 1 && count_of('t') == 1);
    assert(pw_wine_alias_get(program + 5 * PAGE) == first && pw_wine_alias_get(program + 6 * PAGE) == first + PAGE);
    assert(pw_wine_alias_get(program + 8 * MIB + 5 * PAGE) == first);
    coherent(program + 8 * MIB + 5 * PAGE, first);
    assert(((volatile unsigned char *)program)[5 * PAGE + PAGE - 1] == ((volatile unsigned char *)first)[PAGE - 1]);
    assert(count_of('m') == 1 && count_of('t') == 1 && !pw_wine_alias_generation());
    stats_are(1, 0, 0, 0, 0);
}

/* A seventeenth window gives the least recently used one back first; the generation moves. */
static void eviction(void)
{
    setup();
    for (unsigned i = 0; i < 16; i++) view(program + i * 2 * MIB, 2 * MIB, (int64_t)(i * 2 * MIB));
    uintptr_t aliases[16];
    for (unsigned i = 0; i < 16; i++) aliases[i] = pw_wine_alias_get(runs[i].address);
    for (unsigned i = 0; i < 16; i++) assert(aliases[i]);
    /* Touch window 0 again: window 1 is now the least recently used. */
    assert(pw_wine_alias_get(runs[0].address) == aliases[0] && !pw_wine_alias_generation());
    view(program + 32 * MIB, PAGE, 60 * MIB);   /* a 17th memory */
    const uintptr_t alias = pw_wine_alias_get(program + 32 * MIB);
    assert(alias && count_of('g') == 1 && last('g')->address == aliases[1] && pw_wine_alias_generation() == 1);
    coherent(program + 32 * MIB, alias);
    /* The others stay. */
    assert(pw_wine_alias_get(runs[0].address) == aliases[0] && pw_wine_alias_get(runs[15].address) == aliases[15]);
    stats_are(17, 0, 0, 1, 0);
}

/* Eviction off (WINE_PS5_ALIAS_EVICT=0): a full table answers 0, counted as noalias, and gives nothing back. */
static void no_eviction(void)
{
    setup();
    pw_wine_alias_set_eviction(0);
    for (unsigned i = 0; i < 16; i++) view(program + i * 2 * MIB, 2 * MIB, (int64_t)(i * 2 * MIB));
    for (unsigned i = 0; i < 16; i++) assert(pw_wine_alias_get(runs[i].address));
    view(program + 32 * MIB, PAGE, 60 * MIB);   /* a 17th memory */
    assert(!pw_wine_alias_get(program + 32 * MIB) && !count_of('g') && !pw_wine_alias_generation());
    stats_are(16, 0, 0, 0, 1);
    pw_wine_alias_set_eviction(1);
    /* A smaller limit holds as many windows and no more. */
    setup();
    pw_wine_alias_set_eviction(0);
    pw_wine_alias_set_windows(4);
    for (unsigned i = 0; i < 5; i++) view(program + i * 2 * MIB, 2 * MIB, (int64_t)(i * 2 * MIB));
    for (unsigned i = 0; i < 4; i++) assert(pw_wine_alias_get(runs[i].address));
    assert(!pw_wine_alias_get(runs[4].address) && !count_of('g'));
    stats_are(4, 0, 0, 0, 1);
    pw_wine_alias_set_eviction(1);
    pw_wine_alias_set_windows(PW_WINE_ALIAS_WINDOWS);
}

/* A release forgets every window over any of its memory, and only those, before it returns; the next get maps anew. */
static void forget(void)
{
    setup();
    view(program, 2 * MIB, 0);
    view(program + 2 * MIB, 2 * MIB, 2 * MIB);
    view(program + 4 * MIB, 2 * MIB, 4 * MIB);
    const uintptr_t a = pw_wine_alias_get(program), b = pw_wine_alias_get(program + 2 * MIB),
                    c = pw_wine_alias_get(program + 4 * MIB);
    assert(a && b && c);
    /* Memory no window maps: nothing given back, the generation stays. */
    pw_wine_alias_forget(6 * MIB, 2 * MIB);
    assert(!count_of('g') && !pw_wine_alias_generation());
    /* One page at the end of the second window's memory and one at the start of the third's. */
    pw_wine_alias_forget(4 * MIB - PAGE, 2 * PAGE);
    assert(count_of('g') == 2 && pw_wine_alias_generation() == 2);
    assert((calls[call_count - 2].address == b && calls[call_count - 1].address == c) ||
           (calls[call_count - 2].address == c && calls[call_count - 1].address == b));
    /* The first is still there and still the same alias; the second is made again. */
    assert(pw_wine_alias_get(program + PAGE) == a + PAGE && count_of('m') == 3);
    const uintptr_t again = pw_wine_alias_get(program + 2 * MIB);
    assert(again && count_of('m') == 4);
    coherent(program + 2 * MIB, again);
    stats_are(4, 0, 2, 0, 0);
}

/* A refused map takes nothing: the run goes back and nothing is recorded; a full range evicts once, then answers 0. */
static void refusals(void)
{
    setup();
    view(program, 2 * MIB, 0);
    view(program + 2 * MIB, 2 * MIB, 2 * MIB);
    refuse_map = 1;
    assert(!pw_wine_alias_get(program));
    assert(count_of('t') == 1 && count_of('m') == 1 && count_of('g') == 1 && last('g')->address == last('m')->address);
    assert(slots_free == SLOTS && !pw_wine_alias_generation());
    refuse_map = 0;
    const uintptr_t a = pw_wine_alias_get(program);
    assert(a);
    /* The range full: the window is given back, the take asked once more, and still full: 0. */
    take_fails = 1;
    call_count = 0;
    assert(!pw_wine_alias_get(program + 2 * MIB));
    assert(count_of('t') == 2 && count_of('g') == 1 && last('g')->address == a && !count_of('m'));
    assert(pw_wine_alias_generation() == 1);
    /* Nothing left to evict: 0 at once. */
    call_count = 0;
    assert(!pw_wine_alias_get(program + 2 * MIB) && count_of('t') == 1 && !count_of('g'));
    stats_are(1, 1, 0, 1, 2);
}

/* find answers a window that exists and does not make one, move the eviction order, or count a miss. */
static void find_existing(void)
{
    setup();
    view(program, 2 * MIB, 0);
    assert(!pw_wine_alias_find(program) && count_of('b') == 1 && !count_of('t') && !count_of('m'));
    stats_are(0, 0, 0, 0, 0);
    const uintptr_t alias = pw_wine_alias_get(program);
    const unsigned maps = count_of('m');
    call_count = 0;
    assert(pw_wine_alias_find(program) == alias && pw_wine_alias_find(program + PAGE) == alias + PAGE);
    assert(count_of('b') == 2 && !count_of('t') && !count_of('m'));
    assert(!pw_wine_alias_find(program + 4096) && !pw_wine_alias_find(program + 4 * MIB));
    stats_are(1, 0, 0, 0, 0);
    assert(maps == 1 && pw_wine_alias_get(program) == alias);
}

/* No backing, or an answer that does not hold the page: 0, counted, and no window asked for. A misaligned page: 0. */
static void no_backing(void)
{
    setup();
    assert(!pw_wine_alias_get(program) && count_of('b') == 1 && !count_of('t'));
    view(program, 2 * MIB, 0);
    bad_answer = 1;
    assert(!pw_wine_alias_get(program));
    bad_answer = 2;
    assert(!pw_wine_alias_get(program));
    bad_answer = 0;
    assert(!pw_wine_alias_get(program + 4096) && count_of('b') == 3 && !count_of('t'));
    stats_are(0, 0, 0, 0, 3);
}

int main(void)
{
    memory = memfd_create("direct-memory", 0);
    assert(memory >= 0 && !ftruncate(memory, 64 * MIB));
    program = reserve(64 * MIB, 2 * MIB);
    range = reserve(SLOTS * WINDOW, PAGE);
    before_init();
    clipping();
    reuse();
    eviction();
    no_eviction();
    forget();
    refusals();
    find_existing();
    no_backing();
    puts("aliases: nothing before init, windows clipped to the 2 MiB block and the backed run, reused by direct-memory "
         "offset across views, the least recently used evicted (or, with eviction off, 0), forgotten before release (only those over it), a "
         "refused map given back, a full range evicted once then 0, no backing 0, find answers an existing window and creates none, every store coherent, passed");
    return 0;
}
