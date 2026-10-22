/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console side of pw_wine_dmem, linked into ntdll.prx: libkernel's
 * direct memory behind the mmap, munmap and mprotect calls of ntdll's
 * virtual memory manager (patch 0600 routes them here), for the address
 * ranges it reserves up front. A private anonymous mapping with no address,
 * once the application allocator is on, is given a direct-memory region of
 * its own (pw_wine_anon_fresh_direct). A fixed address outside a region, and
 * every executable mapping, still passes straight through.
 *
 * The recipe is the one ps5-xash3d's heap runs on (FW 12.02): main direct
 * memory, mapped read-write at a fixed address over a reservation, then
 * protected with sceKernelMprotect; execute permission is refused at map
 * time but granted by mprotect. A self-check maps, writes, protects and
 * frees one page before the first region is taken; if any step fails, the
 * regions stay ordinary reservations and every call passes through. A
 * payload's kernel keeps execute only on JIT memory: its executable pages are
 * converted at their first execute (__wine_ps5_exec_convert). */
#include "pw_wine_alias.h"
#include "pw_wine_dmem.h"
#include "pw_wine_heap.h"
#include "pw_wine_pages.h"
#include "pw_wine_perf.h"
#include "pw_wine_vm.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <ps5platform/memory.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

extern int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment);
extern int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags,
                                    int64_t offset, size_t alignment);
extern int sceKernelMprotect(const void *address, size_t bytes, int protection);
extern int sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
extern int sceKernelMunmap(void *address, size_t bytes);
extern int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes);
extern int sceKernelDebugOutText(int channel, const char *text);
/* A payload's executable memory (__wine_ps5_exec_convert). libkernel's mmap, never the payload SDK's. */
extern int sceKernelJitCreateSharedMemory(const char *name, size_t bytes, int max_protection, int *fd);
extern int sceKernelMmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset, void **out);

/* A span of 1 GiB or more that Wine unmaps, lays a reservation over or keeps goes to klog first, with the kernel's
 * entries in it (how many, to 64, and the first 8, protection 0 included): a helper's own log is lost when the
 * console hangs, and the console froze unmapping such a span twice ("unwire timeout at 0xc80000000",
 * 2026-10-14 (record removed from the tree; in git history), late-library 2026-10-15) with nothing to say what was at that address. Called
 * under the engine's lock: no heap. */
enum { DUMP_BYTES = (size_t)1 << 30, DUMP_COUNT = 64, DUMP_SHOWN = 8 };
static void dump_entries(const char *what, uintptr_t address, size_t bytes)
{
    const uintptr_t high = address + bytes;
    unsigned count = 0;
    PwWineVmEntry entry;
    char line[200];

    if (bytes < DUMP_BYTES) return;
    snprintf(line, sizeof(line), "[wine-ps5] %s %p +%#zx\n", what, (void *)address, bytes);
    sceKernelDebugOutText(0, line);
    for (uintptr_t cursor = address; cursor < high && count < DUMP_COUNT; cursor = entry.end) {
        if (pw_wine_vm_entry((const void *)cursor, &entry) || entry.start >= high) break;
        if (++count > DUMP_SHOWN) continue;
        snprintf(line, sizeof(line), "[wine-ps5]   entry %#lx-%#lx protection %#x type %d flags %#x %s\n",
                 (unsigned long)entry.start, (unsigned long)entry.end, entry.protection, entry.type, entry.flags,
                 entry.name);
        sceKernelDebugOutText(0, line);
    }
    snprintf(line, sizeof(line), "[wine-ps5]   entries %u%s\n", count, count == DUMP_COUNT ? " or more" : "");
    sceKernelDebugOutText(0, line);
}

static int kernel_munmap(void *address, size_t bytes)
{
    dump_entries("unmap", (uintptr_t)address, bytes);
    return sceKernelMunmap(address, bytes);
}

/* Only a payload supplies this private system-library API. Titles retain
 * their ordinary MainDirectMemory allocator and never import libkernel_sys. */
static int (*payload_allocate_direct)(size_t, size_t, int, int64_t *);
static int payload_protect(void *address, size_t bytes, int protection);

enum {
    PAGE = 0x4000,
    KERNEL_FIXED = 0x10,
    KERNEL_NO_OVERWRITE = 0x80,
    MEMORY_TYPE = 0x0c,       /* as ps5-xash3d's heap */
    CPU_READ_WRITE = 0x03,
    CPU_GPU_READ_WRITE = 0xf2,  /* what ps5-xash3d's heap maps with */
    RUNS = 32768,
};

/* WINE_PS5_WAIT_WATCHDOG: a direct-memory call slower than 50 ms is
 * logged (prospero-win#250). */
static uint64_t slow_start(void)
{
    static int enabled = -1;
    struct timespec ts;

    if (enabled == -1) enabled = getenv("WINE_PS5_WAIT_WATCHDOG") != NULL;
    if (!enabled) return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static void slow_end(uint64_t start, const char *what, size_t bytes)
{
    struct timespec ts;
    uint64_t now;

    if (!start) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    if (now - start > 50000)
        fprintf(stderr, "wine-ps5: slow dmem %s bytes=%zu ms=%llu\n", what, bytes,
                (unsigned long long)((now - start) / 1000u));
}

/* The first refusals, with the kernel's answer: a process that ran into the
 * ceiling of what it can map says which call was refused and with what. */
/* Whether a payload's refused protection change asked for execute ("execute") or not ("mprotect"), and the kernel's
 * errno. Games failed at this point in Mihawk OS (library-mihawk-2026-10-10-01: a .NET game's runtime, a page RWX;
 * a Java game's LWJGL, its .text RX) and the record did not say what was refused. Not thread-local (a PRX has no static
 * TLS block, pw_wine_dl.c): Wine makes these changes under its virtual-memory lock, which also covers the report. */
static const char *protect_step = "";
static int protect_errno;

/* Which virtual range a refused commit was for. */
static void refused_range(const char *what, const void *address, size_t bytes, int protection)
{
    static int reported;

    if (reported >= 12) return;
    reported++;
    fprintf(stderr, "wine-ps5: %s of %p, %zu bytes, protection %d failed (%s errno %d)\n", what, address, bytes,
            protection, *protect_step ? protect_step : "call", protect_errno);
}

static void refused(const char *what, int rc, size_t bytes)
{
    static int reported;
    struct ps5_memory_stats memory;

    if (reported >= 12) return;
    reported++;
    fprintf(stderr, "wine-ps5: dmem %s refused: %#x for %zu bytes\n", what, (unsigned)rc, bytes);
    if (ps5_memory_query(&memory))
        fprintf(stderr, "wine-ps5: direct memory at that moment: %llu MiB of %llu free in %u ranges, the largest %llu MiB; "
                "flexible %llu MiB free\n", (unsigned long long)(memory.free_bytes >> 20),
                (unsigned long long)(memory.total_bytes >> 20), memory.free_ranges,
                (unsigned long long)(memory.largest_bytes >> 20), (unsigned long long)(memory.flexible_bytes >> 20));
}

/* CPU read-write, unless the kernel wants the GPU bits too. */
static int map_protection = CPU_READ_WRITE;

static int reserve(void *context, uintptr_t address, size_t bytes)
{
    void *at = (void *)address;
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelReserveVirtualRange(&at, bytes, KERNEL_FIXED | KERNEL_NO_OVERWRITE, PAGE) ||
         at != (void *)address;
    slow_end(t0, "reserve", bytes);
    return rc;
}
static int allocate(void *context, size_t bytes, int64_t *offset)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = payload_allocate_direct ? payload_allocate_direct(bytes, PAGE, MEMORY_TYPE, offset)
                                 : sceKernelAllocateMainDirectMemory(bytes, PAGE, MEMORY_TYPE, offset);
    slow_end(t0, "allocate", bytes);
    if (rc) refused("allocate", rc, bytes);
    return rc != 0;
}
static int map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    void *at = (void *)address;
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMapDirectMemory(&at, bytes, map_protection, KERNEL_FIXED, offset, PAGE);
    slow_end(t0, "map", bytes);
    if (rc) refused("map", rc, bytes);
    if (rc && map_protection == CPU_READ_WRITE && !payload_allocate_direct) {
        /* Refused CPU-only in a title: the GPU bits, and keep them if they work. Never in a payload: every later run
         * would be wired GPU memory, which each release, lay and conversion tears down ("unwire timeout"). */
        at = (void *)address;
        rc = sceKernelMapDirectMemory(&at, bytes, CPU_GPU_READ_WRITE, KERNEL_FIXED, offset, PAGE);
        if (!rc) map_protection = CPU_GPU_READ_WRITE;
    }
    return rc || at != (void *)address;
}
static int protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    if (payload_allocate_direct) return payload_protect((void *)address, bytes, (int)protection) != 0;
    rc = sceKernelMprotect((const void *)address, bytes, (int)protection);
    slow_end(t0, "protect", bytes);
    if (rc) refused("protect", rc, bytes);
    return rc != 0;
}
static int unmap(void *context, uintptr_t address, size_t bytes)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = kernel_munmap((void *)address, bytes) != 0;
    slow_end(t0, "unmap", bytes);
    return rc;
}
/* Every release of direct memory, by every path of the engine's (docs/MMU4K.md section 8): no second mapping of it
 * (pw_wine_alias.h, I4) outlives it, so each alias over any of it is given back first. */
static int release(void *context, int64_t offset, size_t bytes)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    pw_wine_alias_forget(offset, bytes);
    rc = sceKernelReleaseDirectMemory(offset, bytes) != 0;
    slow_end(t0, "release", bytes);
    return rc;
}

/* A reservation laid over Wine's own mapping replaces it in one call, as the GPU client releases its direct memory and
 * pw_wine_pages its shared pages: the range never becomes a hole another thread's mapping without an address lands
 * in. Where the kernel refuses, the engine unmaps and reserves instead. */
static int lay_off;
static int lay(void *context, uintptr_t address, size_t bytes)
{
    void *at = (void *)address;
    (void)context;
    if (lay_off) return -1;
    dump_entries("lay", address, bytes);
    return sceKernelReserveVirtualRange(&at, bytes, KERNEL_FIXED, PAGE) || at != (void *)address;
}

/* What a bare reservation of Wine's looks like to sceKernelVirtualQuery, measured on the first region before its
 * self-check (measure_signature): 0 until then, 1 measured, -1 not measurable, and then no entry is called foreign
 * (the guard is off, said once; the engine runs as before). Any entry inside the regions' bare parts with another
 * protection or flags is foreign. */
static int signature_state;
static PwWineVmEntry signature;

static void say(const char *line)
{
    (void)!write(2, line, strlen(line));
    sceKernelDebugOutText(0, line);
}

/* A hole in a bare part (a mapping the kernel placed there and since freed) is reserved again before it is trusted, so
 * a mapping made without an address cannot land in it between the query and Wine's fixed mapping. */
static int refill(uintptr_t address, size_t bytes)
{
    static unsigned reported;
    char line[128];

    if (reserve(NULL, address, bytes)) return -1;
    if (reported++ < 4) {
        snprintf(line, sizeof(line), "[wine-ps5] hole %#lx +%#zx in a reservation of Wine's reserved again\n",
                 (unsigned long)address, bytes);
        say(line);
    }
    return 0;
}

/* Ranges win32u lent to a placed GPU mapping (patch 1130), from __wine_ps5_placed_range_bare until Wine releases them
 * (__wine_ps5_dmem_keep, __wine_ps5_munmap): foreign by record. The kernel shows wired direct memory there, but the
 * owner's shared memory reads as the reservation under it, so the engine would have mapped over it
 * (host-dmem-fallback64, v2-backend-gate-2026-10-15). Its own lock: the pager asks outside the virtual mutex.
 * Limit: the record ends at any release touching it, which is right in win32u's order (the client's unmap with
 * VK_MEMORY_UNMAP_RESERVE_BIT_EXT, then NtFreeVirtualMemory). A program that frees win32u's view itself first (which
 * Windows refuses for a mapped pointer) leaves shared memory unguarded; wired direct memory the kernel still shows. */
enum { LENT_MAX = 1024 };
static struct { uintptr_t start, end; } lent[LENT_MAX];
static unsigned lent_count;
static int lent_lock;

static void lent_take(void)
{
    while (__atomic_exchange_n(&lent_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&lent_lock, __ATOMIC_RELAXED)) __builtin_ia32_pause();
}
static void lent_give(void) { __atomic_store_n(&lent_lock, 0, __ATOMIC_RELEASE); }

/* The lowest lent range touching [address, +bytes): 1 with its range, else 0. */
static int lent_find(uintptr_t address, size_t bytes, uintptr_t *start, uintptr_t *end)
{
    int found = 0;

    lent_take();
    for (unsigned i = 0; i < lent_count; i++)
        if (lent[i].start < address + bytes && address < lent[i].end && (!found || lent[i].start < *start)) {
            *start = lent[i].start;
            *end = lent[i].end;
            found = 1;
        }
    lent_give();
    return found;
}

/* Every lent range touching [at, end) is Wine's again: it is releasing them. */
static void lent_drop(uintptr_t at, uintptr_t end)
{
    lent_take();
    for (unsigned i = 0; i < lent_count;)
        if (lent[i].start < end && at < lent[i].end) lent[i] = lent[--lent_count];
        else i++;
    lent_give();
}

static int foreign_op(void *context, uintptr_t address, size_t bytes, uintptr_t *start, uintptr_t *end,
                      unsigned *protection)
{
    static unsigned reported, failures;
    PwWineVmEntry found;
    char line[224];
    uintptr_t lent_start, lent_end;
    int status, lent_here;
    (void)context;

    /* A lent range is foreign by record (2: refused or gone around, not a hole, it lies in a view); the kernel is
     * asked only about the part below it, so an entry there is still the first one found. */
    lent_here = lent_find(address, bytes, &lent_start, &lent_end);
    if (lent_here && lent_start > address) bytes = lent_start - address;
    if (lent_here && lent_start <= address) status = 0;
    else if (__atomic_load_n(&signature_state, __ATOMIC_ACQUIRE) <= 0) status = 0;
    else status = pw_wine_vm_foreign((const void *)address, bytes, &signature, &found, refill);
    if (!status && lent_here) {
        *start = lent_start;
        *end = lent_end;
        *protection = PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE;
        return 2;
    }
    if (status > 0) {
        *start = found.start;
        *end = found.end;
        *protection = found.protection;
        if (reported++ < 8) {
            snprintf(line, sizeof(line), "[wine-ps5] foreign entry %#lx-%#lx protection %#x type %d flags %#x %s "
                     "inside %#lx +%#zx\n", (unsigned long)found.start, (unsigned long)found.end, found.protection,
                     found.type, found.flags, found.name, (unsigned long)address, bytes);
            say(line);
        }
    } else if (status < 0 && failures++ < 4) {
        snprintf(line, sizeof(line), "[wine-ps5] no answer about %#lx +%#zx: refused\n", (unsigned long)address, bytes);
        say(line);
    }
    return status;
}

/* The signature is the guard's own step. A query that fails, or a first page that is not a bare reservation
 * (protection 0), leaves the guard off and never refuses the engine: a refused self-check would leave every Windows
 * allocation unguarded on flexible memory. */
static void measure_signature(uintptr_t address)
{
    PwWineVmEntry entry;
    char line[224];

    if (signature_state) return;
    if (pw_wine_vm_entry((const void *)address, &entry) || entry.start > address || entry.protection) {
        signature_state = -1;
        say("[wine-ps5] no bare reservation to measure: foreign mappings are not guarded against\n");
        return;
    }
    signature = entry;
    __atomic_store_n(&signature_state, 1, __ATOMIC_RELEASE);
    snprintf(line, sizeof(line), "wine-ps5: a bare reservation reads protection %#x type %d flags %#x %s\n",
             entry.protection, entry.type, entry.flags, entry.name);
    say(line);
}

/* The self-checks' last step lays a reservation over their page (direct memory in a title, JIT memory in a payload,
 * flexible memory then JIT memory with WINE_PS5_PAYLOAD_DMEM=0), so laying is measured at every start: if the kernel
 * still shows a mapping there, it is off from then on and the engine unmaps and reserves instead. */
static void check_laid(uintptr_t address)
{
    PwWineVmEntry entry;
    void *at = (void *)address;

    if (!pw_wine_vm_entry((const void *)address, &entry) && entry.start <= address &&
        (signature_state > 0 ? entry.protection == signature.protection && entry.flags == signature.flags
                             : !entry.protection))
        return;
    lay_off = 1;
    say("[wine-ps5] a reservation laid over a mapping did not replace it: unmapped and reserved instead\n");
    (void)sceKernelMunmap(at, PAGE);
    (void)sceKernelReserveVirtualRange(&at, PAGE, KERNEL_FIXED | KERNEL_NO_OVERWRITE, PAGE);
}

static const PwWineDmemOps ops = { NULL, PAGE, reserve, allocate, map, protect, unmap, release, 0, lay, foreign_op };
/* A payload's direct memory never asks for execute; its executable pages become JIT memory (caller's runs). */
static const PwWineDmemOps payload_ops = { NULL, PAGE, reserve, allocate, map, protect, unmap, release, 1, lay,
                                           foreign_op };
static PwWineDmem dmem;
static PwWineDmemRun runs[RUNS];
/* 0 before the self-check, 1 in use, -1 refused: every call passes. */
static int state;
/* shortcut: an adopt that failed after the caller's own mapping replaced runs leaves runs naming memory no longer mapped
 * there; no alias after that (stores step). Upgrade: hold the engine lock across can_adopt and adopt. */
static int backing_stale;

static uintptr_t page_down(const void *address) { return (uintptr_t)address & ~(uintptr_t)(PAGE - 1); }
static size_t page_span(const void *address, size_t bytes)
{
    return (((uintptr_t)address + bytes + PAGE - 1) & ~(uintptr_t)(PAGE - 1)) - page_down(address);
}

/* Whether the kernel holds CPU execute on the page now: a grant it answered with success but did not make would
 * otherwise first show as an execute fault in the first Windows code, before anything says why. The page is not run
 * here; no Wine handler is installed yet. */
static int exec_set(uintptr_t address)
{
    void *start = NULL, *end = NULL;
    uint32_t protection = 0;
    const int query = sceKernelQueryMemoryProtection((void *)address, &start, &end, &protection);

    if (!query && (protection & 4)) return 1;
    fprintf(stderr, "wine-ps5: execute granted but not set (query %#x)\n", query ? (unsigned)query : protection);
    return 0;
}

/* One page through every step, inside the first region, before any use. */
static int self_check(uintptr_t address)
{
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    int ok = !pw_wine_dmem_replace(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE);

    if (ok) {
        bytes[0] = 0x5a;
        bytes[PAGE - 1] = 0xa5;
        ok = !pw_wine_dmem_protect(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_EXEC) && exec_set(address) &&
             bytes[0] == 0x5a && bytes[PAGE - 1] == 0xa5 &&
             !pw_wine_dmem_protect(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE);
    }
    ok = !pw_wine_dmem_replace(&dmem, address, PAGE, 0) && ok;
    check_laid(address);
    return ok;
}

/* The heap's own area (pw_wine_heap): spans and large blocks come from direct
 * memory too, in a range reserved for it alone, so the malloc family that
 * Mesa, the JVM's natives and Wine's Unix side share no longer runs out with
 * the ~440 MiB of flexible memory. Pages are handed out first-fit from a
 * bitmap and given back to the reservation, with their direct memory
 * released, when a large block is freed. Called under the heap's lock. */
void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset);
enum { HEAP_AREA_BYTES = (size_t)6 << 30, HEAP_PAGES = HEAP_AREA_BYTES / PAGE };
static uintptr_t heap_area;
static uint64_t heap_bitmap[HEAP_PAGES / 64];
static size_t heap_hint;   /* the page where the next search begins */

static int heap_page_used(size_t page) { return (heap_bitmap[page / 64] >> (page % 64)) & 1; }
static void heap_pages_set(size_t first, size_t count, int used)
{
    for (size_t page = first; page < first + count; page++) {
        if (used) heap_bitmap[page / 64] |= UINT64_C(1) << (page % 64);
        else heap_bitmap[page / 64] &= ~(UINT64_C(1) << (page % 64));
    }
}

/* __wine_ps5_mmap over the heap's area holds the engine's spin lock across direct-memory kernel calls (and a release
 * takes the alias lock under it): a thread suspended (SIGUSR1) or ended (SIGQUIT, SIGINT) in there would leave every
 * thread that reaches the engine spinning, faults under the virtual mutex among them. Blocked as pages_enter does. */
static void *heap_mmap(void *address, size_t bytes, int protection)
{
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGQUIT);
    sigaddset(&block, SIGINT);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    void *const at = __wine_ps5_mmap(address, bytes, protection, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
    const int error = errno;   /* heap_map reads it */
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = error;
    return at;
}

static void *heap_map(size_t bytes)
{
    size_t pages = bytes / PAGE, first = (size_t)-1;

    if (!heap_area || !pages || bytes % PAGE) return NULL;
again:
    for (int pass = 0; pass < 2 && first == (size_t)-1; pass++) {
        size_t from = pass ? 0 : heap_hint, to = pass ? (heap_hint + pages < HEAP_PAGES ? heap_hint + pages : HEAP_PAGES) : HEAP_PAGES;
        size_t run = 0, start = 0;

        for (size_t page = from; page < to; page++) {
            if (heap_page_used(page)) { run = 0; continue; }
            if (!run) start = page;
            if (++run == pages) { first = start; break; }
        }
    }
    if (first == (size_t)-1) return NULL;
    heap_pages_set(first, pages, 1);
    if (heap_mmap((void *)(heap_area + first * PAGE), bytes, PROT_READ | PROT_WRITE) == MAP_FAILED) {
        /* A foreign entry there (EEXIST): those pages stay taken, or first-fit would offer them on every call, and the
         * search goes on (the heap has no other memory). */
        if (errno == EEXIST) {
            first = (size_t)-1;
            goto again;
        }
        heap_pages_set(first, pages, 0);
        return NULL;
    }
    heap_hint = first + pages;
    return (void *)(heap_area + first * PAGE);
}

static void heap_unmap(void *address, size_t bytes)
{
    size_t first = ((uintptr_t)address - heap_area) / PAGE, pages = bytes / PAGE;

    /* Protection 0 over a region page: its direct memory goes back, the
     * address stays reserved for the next block. */
    (void)heap_mmap(address, bytes, PROT_NONE);
    heap_pages_set(first, pages, 0);
    if (first < heap_hint) heap_hint = first;
}

/* Where the kernel puts a reservation it is given no address for is the
 * first free range above 8 GiB, which is where RADV's 4 GiB address window
 * lives (0x200000000, mapped without replacing at its startup): a heap area
 * taken there left Vulkan with no adapter. The area asks for addresses well
 * above every fixed range instead, exactly there and never over anything (a
 * placed one could land in a hole of another range), and not 0x3000000000,
 * the GPU client's high host range (PW_GPU_HOST_HIGH_BASE). */
static const uintptr_t heap_area_hints[] = { 0x2000000000u, 0x5000000000u };

static void heap_area_init(void)
{
    void *at = NULL;

    if (heap_area) return;
    for (unsigned i = 0; i < sizeof(heap_area_hints) / sizeof(heap_area_hints[0]); i++) {
        at = (void *)heap_area_hints[i];
        if (!sceKernelReserveVirtualRange(&at, HEAP_AREA_BYTES, KERNEL_FIXED | KERNEL_NO_OVERWRITE, PAGE) &&
            at == (void *)heap_area_hints[i])
            break;
        if (at && at != (void *)heap_area_hints[i]) (void)sceKernelMunmap(at, HEAP_AREA_BYTES);
        at = NULL;
    }
    if (!at) {
        fprintf(stderr, "wine-ps5: no address range for the heap's direct memory; it stays on flexible memory\n");
        return;
    }
    if (pw_wine_dmem_add_region(&dmem, page_down(at), HEAP_AREA_BYTES, 1)) {
        (void)sceKernelMunmap(at, HEAP_AREA_BYTES);
        fprintf(stderr, "wine-ps5: cannot track the heap's area; it stays on flexible memory\n");
        return;
    }
    heap_area = (uintptr_t)at;
    pw_wine_heap_set_backing(heap_map, heap_unmap);
    fprintf(stderr, "wine-ps5: the heap takes its memory from direct memory (area %p, %zu MiB)\n", at,
            (size_t)(HEAP_AREA_BYTES >> 20));
}

/* The payload supplies its SDK's mmap and mprotect, which refuse execute (native/proton_wine_child.c), and
 * optionally its ForApp allocator. Without the latter, keep the diagnostic flexible-memory path, including lazy
 * backing for large reservations. payload_mprotect is never called: it only says this is a payload. */
static void *(*payload_mmap)(void *,size_t,int,int,int,off_t);
static int (*payload_mprotect)(const void *,size_t,int);

int __wine_ps5_set_payload_memory(void *(*map_fn)(void *,size_t,int,int,int,off_t),
                                  int (*protect_fn)(const void *,size_t,int))
{
    if(!map_fn || !protect_fn || state) { errno=EINVAL;return -1; }
    payload_mmap=map_fn;payload_mprotect=protect_fn;
    return 0;
}

/* The carrier image a payload loader gave back (native/proton_payload_child.c carrier_release): libkernel still names
 * its execute-only text there, and a first execute fault inside it is the kernel's SYSTEM_XO_VIOLATION debug stop, not
 * the SIGSEGV Wine converts (fixed-base32-2026-10-19-01; carrier-xo32-2026-10-19-01 ran 32-bit code at 0x400000 from a
 * JIT page, so the range serves code once it is JIT memory). A protection change that makes a range there executable
 * converts it now, in __wine_ps5_mprotect, before the fault can happen. 0 when set; -1 and EINVAL for a range a
 * 32-bit image could not use. */
static uintptr_t low_image_base, low_image_end;

int __wine_ps5_set_payload_low_image(uintptr_t base, uintptr_t end)
{
    if(!payload_mmap || base >= end || end > (uintptr_t)0x100000000ull || (base | end) & (PAGE-1)) {
        errno=EINVAL;return -1;
    }
    low_image_base=base;low_image_end=end;
    return 0;
}



/* A payload's protection change is one sceKernelMprotect: the kernel's mprotect (system call 74), which clips the map
 * entries and changes them under the map's lock. Never the payload SDK's mprotect or mmap with execute: they find the
 * entry by reading the vm_map tree without its lock and write its protection byte (kernel_set_vmem_protection),
 * racing every other thread's map change (cause 2 of the 2026-10-14 freeze review). The kernel keeps execute only on
 * JIT memory; on direct, flexible, file and granted memory it answers success and drops it (exec-kernel-2026-10-14),
 * so a range becomes JIT memory at its first execute (__wine_ps5_exec_convert). A refusal is the kernel's answer:
 * counted, logged and returned, never retried or granted another way. */
static uint64_t protect_refusals, conversions, converted_kib, convert_failures, exec_refusals, grant_retries;

static int payload_protect(void *address, size_t bytes, int protection)
{
    const int rc = sceKernelMprotect(address, bytes, protection);

    if (rc) {
        static int reported;
        const uint64_t refusals = __atomic_add_fetch(&protect_refusals, 1, __ATOMIC_RELAXED);
        protect_step = protection & PROT_EXEC ? "execute" : "mprotect";
        /* An SCE code is 0x80020000 plus the errno. */
        protect_errno = errno = (int)((unsigned)rc & 0xffff);
        if (reported++ < 12)
            fprintf(stderr, "wine-ps5: the kernel refused protection %d for %p, %zu bytes (errno %d; %llu refused)\n",
                    protection, address, bytes, protect_errno, (unsigned long long)refusals);
        return -1;
    }
    return 0;
}

/* Lock-free (atomic loads only), so a caller may read it whatever Wine's threads hold or wherever they stopped:
 * protection changes the kernel refused, JIT objects made and their KiB, conversions that failed, execute faults
 * Wine allows that nothing could serve, faults the kernel already allowed when the handler looked (a repeat at the
 * same address is counted too, then takes the upstream path), and 0 (reserved).
 * Returns how many counters there are. */
unsigned int __wine_ps5_exec_stats(uint64_t *out, unsigned int count)
{
    const uint64_t values[7] = { __atomic_load_n(&protect_refusals, __ATOMIC_RELAXED),
                                 __atomic_load_n(&conversions, __ATOMIC_RELAXED),
                                 __atomic_load_n(&converted_kib, __ATOMIC_RELAXED),
                                 __atomic_load_n(&convert_failures, __ATOMIC_RELAXED),
                                 __atomic_load_n(&exec_refusals, __ATOMIC_RELAXED),
                                 __atomic_load_n(&grant_retries, __ATOMIC_RELAXED), 0 };
    for (unsigned int i = 0; i < count && i < 7; i++) out[i] = values[i];
    return 7;
}

/* In a payload Wine's own memory is CPU-only (its direct memory is mapped 0x03, never with the GPU bits), so an entry
 * with a GPU bit in [at, +bytes) is the GPU client's wired memory. Outside the regions, where the engine's guard does not
 * look (it cannot tell Wine's own mappings there from another's), a fixed mapping, an unmap or a conversion never goes
 * over one: tearing it down hung the console ("unwire timeout at 0xc80000000"). One query per entry; 1 also when the
 * kernel did not answer. */
static int gpu_inside(uintptr_t at, size_t bytes)
{
    PwWineVmEntry entry;
    int status;

    if (!payload_mmap) return 0;
    for (uintptr_t cursor = page_down((const void *)at); cursor < at + bytes; cursor = entry.end) {
        if ((status = pw_wine_vm_entry((const void *)cursor, &entry))) return status < 0;
        if (entry.start >= at + bytes) return 0;
        if (entry.protection & 0x30) return 1;
    }
    return 0;
}
static int refused_gpu(uintptr_t at, size_t bytes)
{
    static unsigned reported;
    char line[128];

    if (!gpu_inside(at, bytes)) return 0;
    if (reported++ < 8) {
        snprintf(line, sizeof(line), "[wine-ps5] GPU memory inside %#lx +%#zx: not mapped over or unmapped\n",
                 (unsigned long)at, bytes);
        say(line);
    }
    errno = EEXIST;
    return 1;
}

/* A payload's mapping outside its large reservations, as it is asked for but without execute, which the kernel drops
 * at map time anyway (K8): the range becomes JIT memory at its first execute. */
static void *payload_map(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    if ((flags & MAP_FIXED) && refused_gpu((uintptr_t)address, bytes)) return MAP_FAILED;
    return mmap(address, bytes, protection & ~PROT_EXEC, flags, fd, offset);
}
/* An unmap outside the regions (a title's, or a payload's with the guard above). */
static int outside_munmap(void *address, size_t bytes)
{
    return refused_gpu((uintptr_t)address, bytes) ? -1 : munmap(address, bytes);
}

/* A payload's large reservations stay bare address space and are backed page by page as they are committed, by
 * the same engine as a title's direct memory, with anonymous memory in its place (the kernel hands it out
 * zeroed). A smaller reservation is backed whole by an anonymous PROT_NONE mapping at once, which a later
 * protection change commits; that charges its full size to the process, and the console refused it for the
 * 32 GiB - 64 KiB Chromium's allocator reserves to place a 16 GiB pool (ENOMEM): a launcher's browser stopped in
 * chrome_elf.dll before its first window, 25 times in 240 s (2026-10-06; record removed from the tree; in git history). .NET's region GC, V8's
 * pointer cage and engines with large heaps reserve the same way, and commit a little at a time. */
enum { PAYLOAD_LAZY_BYTES = (size_t)256 << 20 };
static PwWineDmem flex;
static PwWineDmemRun flex_runs[RUNS];
static int flex_state;   /* 0 before the first large reservation, 1 in use, -1 refused: the old way for all */
static int64_t flex_next_offset = 1;

int __wine_ps5_set_payload_direct_memory(int (*allocate_fn)(size_t,size_t,int,int64_t *))
{
    if (!allocate_fn || !payload_mmap || state || flex_state) { errno = EINVAL; return -1; }
    payload_allocate_direct = allocate_fn;
    return 0;
}

static int flex_allocate(void *context, size_t bytes, int64_t *offset)
{
    (void)context;
    /* No physical memory to take; the offset only tells the engine's runs apart. */
    *offset = flex_next_offset;
    flex_next_offset += (int64_t)bytes;
    return 0;
}
static int flex_map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    void *mapped = payload_mmap((void *)address, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                                -1, 0);
    (void)context;
    (void)offset;
    if (mapped == MAP_FAILED) refused("map", errno, bytes);
    return mapped != (void *)address;
}
static int flex_protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    (void)context;
    return payload_protect((void *)address, bytes, (int)protection) != 0;
}
static int flex_unmap(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    return kernel_munmap((void *)address, bytes) != 0;
}
static int flex_release(void *context, int64_t offset, size_t bytes)
{
    (void)context;
    (void)offset;
    (void)bytes;
    return 0;
}
static const PwWineDmemOps flex_ops = { NULL, PAGE, reserve, flex_allocate, flex_map, flex_protect, flex_unmap,
                                        flex_release, 1, lay, foreign_op };

/* Whether [at, end) touches a large reservation: then it goes piece by piece. */
static int flex_touches(uintptr_t at, uintptr_t end)
{
    size_t piece;
    return flex_state > 0 && (pw_wine_dmem_split(&flex, at, end - at, &piece) || piece != end - at);
}

/* 1 when the kernel holds every bit of access (PROT_*, the kernel's own bits) on each page of [at, +bytes). */
_Static_assert(PROT_READ == 1 && PROT_WRITE == 2 && PROT_EXEC == 4, "PROT_* are the kernel's protection bits");
static int kernel_allows(uintptr_t at, size_t bytes, int access)
{
    const uint32_t need = (uint32_t)access & 7;

    for (uintptr_t page = page_down((const void *)at); page < at + bytes; page += PAGE) {
        void *start = NULL, *end = NULL;
        uint32_t protection = 0;
        if (sceKernelQueryMemoryProtection((void *)page, &start, &end, &protection) || (protection & need) != need)
            return 0;
    }
    return 1;
}

/* [at, +bytes) becomes one JIT object of exactly that size (a larger one does not map, K4), mapped read-write over
 * the live mapping in one call (K3) with the same contents. The range is read-only first: a writer faults and waits
 * on virtual_mutex, which the caller holds, so no write is lost. *step names what failed. */
static int jit_over(uintptr_t at, size_t bytes, const char **step)
{
    void *scratch, *over = NULL;
    int fd = -1, rc, error;

    *step = "read-only";
    if ((rc = sceKernelMprotect((void *)at, bytes, PROT_READ))) goto refused;
    *step = "create";
    if ((rc = sceKernelJitCreateSharedMemory("pw-jit", bytes, PROT_READ | PROT_WRITE | PROT_EXEC, &fd))) goto refused;
    /* pwrite on a JIT object fails (ESPIPE, K5): the copy goes through a temporary mapping in the helper's shared-page
     * range, never where the kernel would place it (pw_wine_pages.h), which pw_wine_pages_map does without a range. */
    *step = "scratch";
    if (!pw_wine_pages_base(NULL)) {
        errno = ENOMEM;
        goto failed;
    }
    if ((scratch = pw_wine_pages_map(bytes, PROT_READ | PROT_WRITE, fd)) == MAP_FAILED) goto failed;
    memcpy(scratch, (const void *)at, bytes);
    pw_wine_pages_unmap(scratch, bytes);
    *step = "map";
    if ((rc = sceKernelMmap((void *)at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0, &over)))
        goto refused;
    close(fd);   /* the mapping keeps the object (K6) */
    return 0;
refused:
    errno = (int)((unsigned)rc & 0xffff);
failed:
    error = errno;
    if (fd >= 0) close(fd);
    errno = error;
    return -1;
}

static int convert_failed(const void *address, size_t bytes, int unix_prot, const char *step)
{
    const int error = errno;
    const uint64_t failures = __atomic_add_fetch(&convert_failures, 1, __ATOMIC_RELAXED);

    if (failures <= 12 || !(failures % 1024))
        fprintf(stderr, "wine-ps5: executable memory at %p, %zu bytes, protection %d not made: %s failed (errno %d; "
                "%llu failed)\n", address, bytes, unix_prot, step, error, (unsigned long long)failures);
    errno = error;
    return -1;
}

/* Wine's execute fault on [address, +bytes), one view's pages of one host protection, under virtual_mutex: the
 * direct or flexible memory there becomes JIT memory with the same contents, its direct memory is released, and the
 * range takes unix_prot, execute included, checked by query. Pages already JIT memory stay as they are, so running
 * code never goes read-only again: the engine's caller runs inside the regions, the pages the kernel lets run outside
 * them (only JIT memory keeps execute, K8). On failure the kernel holds again what it held before: unix_prot without
 * execute on direct and flexible memory, unix_prot on JIT memory. 0, or -1 with errno; a title has no JIT memory
 * (0x80020001): ENOTSUP. */
int __wine_ps5_exec_convert(void *address, size_t bytes, int unix_prot)
{
    const uintptr_t at = (uintptr_t)address, end = at + bytes;
    PwWineDmem *engine = payload_allocate_direct ? &dmem : &flex;
    const int active = payload_allocate_direct ? state : flex_state;   /* 0 during the self-check */
    const char *step = "range";
    size_t piece = bytes;
    int owned = 0, error;

    if (!payload_mmap) {
        errno = ENOTSUP;
        return -1;
    }
    if (!bytes || at % PAGE || bytes % PAGE || end < at || !(unix_prot & PROT_EXEC)) {
        errno = EINVAL;
        return convert_failed(address, bytes, unix_prot, step);
    }
    owned = active >= 0 && pw_wine_dmem_split(engine, at, bytes, &piece);
    if (piece != bytes) {   /* across a region's edge: not one view */
        errno = EINVAL;
        return convert_failed(address, bytes, unix_prot, step);
    }
    /* Outside the regions nothing tells Wine's mappings from another's but a GPU bit: refused before any change. */
    step = "gpu";
    if (!owned && refused_gpu(at, bytes)) return convert_failed(address, bytes, unix_prot, step);
    for (uintptr_t from = at, to; from < end; from = to) {
        for (to = from;
             to < end && (owned ? pw_wine_dmem_backed(engine, to, PAGE) : !kernel_allows(to, PAGE, PROT_EXEC));
             to += PAGE) {}
        if (to == from) {
            to += PAGE;
            continue;
        }
        step = "table";
        if (owned && !pw_wine_dmem_can_adopt(engine, from, to - from)) {
            errno = ENOMEM;
            goto failed;
        }
        if (jit_over(from, to - from, &step)) goto failed;
        if (owned && pw_wine_dmem_adopt(engine, from, to - from))   /* releases the direct memory */
            __atomic_store_n(&backing_stale, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&conversions, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&converted_kib, (to - from) >> 10, __ATOMIC_RELAXED);
    }
    step = "protect";
    if (owned ? pw_wine_dmem_protect(engine, at, bytes, (unsigned)unix_prot & 7)
              : payload_protect(address, bytes, unix_prot))
        goto failed;
    step = "query";
    if (kernel_allows(at, bytes, unix_prot)) return 0;
    errno = EACCES;
failed:
    error = errno;
    /* Execute stays on JIT memory and is dropped from direct and flexible memory (by the engine inside the regions, by
     * the kernel outside them). */
    (void)(owned ? pw_wine_dmem_protect(engine, at, bytes, (unsigned)unix_prot & 7)
                 : payload_protect(address, bytes, unix_prot));
    errno = error;
    return convert_failed(address, bytes, unix_prot, step);
}

/* Patch 1127's rule under virtual_mutex: 1 when the kernel already allows access (PROT_*) at address, so the
 * faulting instruction runs again (another thread's change finished while this one waited). Called only by the fault
 * handler for an access Wine allows, so each 1 is counted, including the repeat patch 1127 sends upstream. */
int __wine_ps5_host_allows(const void *address, int access)
{
    if (!kernel_allows((uintptr_t)address, 1, access)) return 0;
    __atomic_add_fetch(&grant_retries, 1, __ATOMIC_RELAXED);
    return 1;
}

/* An execute fault Wine allows that nothing can serve (a section or file view, or a failed conversion): counted, and
 * the first ones and then one in 1024 logged. */
void __wine_ps5_exec_refused(const void *address)
{
    const uint64_t refusals = __atomic_add_fetch(&exec_refusals, 1, __ATOMIC_RELAXED);

    if (refusals <= 12 || !(refusals % 1024))
        fprintf(stderr, "wine-ps5: execute at %p is allowed but has no executable memory (%llu refused)\n", address,
                (unsigned long long)refusals);
}

/* The kernel's protection of the page at address is want; otherwise says which step gave what. */
static int expect(uintptr_t address, unsigned want, const char *step)
{
    void *start = NULL, *end = NULL;
    uint32_t protection = 0;
    const int query = sceKernelQueryMemoryProtection((void *)address, &start, &end, &protection);

    if (!query && (protection & 7) == want) return 1;
    fprintf(stderr, "wine-ps5: execute self-check: %s gave %#x, not %#x\n", step,
            query ? (unsigned)query : protection & 7, want);
    return 0;
}

/* A payload's executable memory, one page through every step inside the first region before any use: direct and
 * flexible memory drop execute (0x1), and JIT memory made from them keeps the contents and takes RX 0x5, RWX 0x7
 * and RW 0x3 (K1, K2, K3: exec-kernel-2026-10-14-0{1,2,3}). Nothing is run; no Wine handler is installed yet. */
static int exec_self_check(PwWineDmem *engine, uintptr_t address)
{
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    int ok = !pw_wine_dmem_replace(engine, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE) && !bytes[0] &&
             !bytes[PAGE - 1];

    if (ok) {
        bytes[0] = 0x5a;
        bytes[PAGE - 1] = 0xa5;
        ok = !pw_wine_dmem_protect(engine, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_EXEC) &&
             expect(address, 1, "RX before conversion") &&
             !__wine_ps5_exec_convert((void *)address, PAGE, PROT_READ | PROT_EXEC) && expect(address, 5, "RX") &&
             bytes[0] == 0x5a && bytes[PAGE - 1] == 0xa5 &&
             !pw_wine_dmem_protect(engine, address, PAGE, 7) && expect(address, 7, "RWX") &&
             !pw_wine_dmem_protect(engine, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE) &&
             expect(address, 3, "RW");
    }
    ok = !pw_wine_dmem_replace(engine, address, PAGE, 0) && ok;
    check_laid(address);
    return ok;
}

/* A payload whose direct memory failed its self-check would run with no backing in its reserved areas and no guard
 * against mappings that are not Wine's: it ends instead, and the title hears why (PW_CHILD_EXIT, STATUS_NO_MEMORY).
 * pw_wine_process.c in ntdll; weak, so a host test may leave it out. */
extern void pw_ps5_child_exit(unsigned int status) __attribute__((weak, noreturn));
static void refused_helper(void)
{
    say("[wine-ps5] direct memory refused by the self-check: this helper ends\n");
    if (pw_ps5_child_exit && getenv("WINE_PS5_CHILD")) pw_ps5_child_exit(0xc0000017u);
}

/* ntdll's fresh reserved area [base, +size): enroll title direct-memory
 * backing, or establish a payload anonymous mapping for later commits. */
int __wine_ps5_dmem_region(void *base, size_t size)
{
    if(payload_mmap && !payload_allocate_direct) {
        uintptr_t low = page_down(base);
        size_t span = page_span(base, size);
        void *mapped;

        /* Already a large reservation's (one that was released and is taken again): the engine backs it. */
        if (flex_state > 0 && pw_wine_dmem_owns(&flex, low, span)) return 0;
        if (span >= PAYLOAD_LAZY_BYTES && flex_state >= 0) {
            if (!flex_state && pw_wine_dmem_init(&flex, &flex_ops, flex_runs, RUNS, 0)) flex_state = -1;
            if (flex_state >= 0 && !pw_wine_dmem_add_region(&flex, low, span, 1)) {
                if (!flex_state) {
                    measure_signature(low);
                    flex_state = exec_self_check(&flex, low) ? 1 : -1;
                    fprintf(stderr, "wine-ps5: large reservations %s backed as they are committed (first %p, %zu MiB)\n",
                            flex_state > 0 ? "are" : "cannot be", base, span >> 20);
                }
                if (flex_state > 0) return 0;
            }
        }
        /* Sony's bare virtual reservation is not an anonymous mapping:
         * mprotect can report success without giving a later commit backing.
         * Establish the normal anonymous PROT_NONE mapping now so subsequent
         * protection changes can fault in flexible pages. This range was just
         * reserved; never use this hook on a range containing live mappings. */
        mapped=payload_mmap(base,size,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);
        if(mapped==MAP_FAILED || mapped!=base) {
            fprintf(stderr,"wine-ps5: payload anonymous reservation failed bytes=%zu errno=%d\n",size,errno);
            return -1;
        }
        return 0;
    }
    if (!state && pw_wine_dmem_init(&dmem, payload_mmap ? &payload_ops : &ops, runs, RUNS, 1)) state = -1;
    if (state < 0) return -1;
    /* Kernel reservations cover whole 16 KiB pages, including PE sections
     * whose Windows size is only a multiple of 4 KiB. */
    if (pw_wine_dmem_add_region(&dmem, page_down(base), page_span(base, size), 1)) {
        fprintf(stderr, "wine-ps5: cannot track reservation %p size=%zu regions=%u\n",
                base, size, dmem.regions);
        return -1;
    }
    if (!state) {
        measure_signature(page_down(base));
        state = (payload_mmap ? exec_self_check(&dmem, page_down(base)) : self_check((uintptr_t)base)) ? 1 : -1;
        fprintf(stderr, "wine-ps5: direct memory %s for anonymous memory (region %p, %zu MiB)\n",
                state > 0 ? "in use" : "refused", base, size >> 20);
        if (state > 0) heap_area_init();
        else if (payload_mmap) refused_helper();
    }
    return state > 0 ? 0 : -1;
}

/* Before a shared file mapping is laid over [at, end): each region piece is checked, so it never replaces a foreign
 * entry. 1 refused (errno EEXIST). */
static int refuse_foreign(PwWineDmem *engine, uintptr_t at, uintptr_t end)
{
    size_t piece;

    for (; at < end; at += piece)
        if (pw_wine_dmem_split(engine, at, end - at, &piece) && pw_wine_dmem_check(engine, at, piece)) return 1;
    return 0;
}

/* A private anonymous mapping the caller did not place: the kernel chooses the address, the range is enrolled,
 * and direct memory backs it when the protection asks for access. PROT_NONE stays a reservation. A failure leaves
 * nothing enrolled and the caller takes the flexible path. */
static void *anon_direct_place(size_t bytes, int protection)
{
    void *at = NULL;
    static unsigned reported, fallen;

    if (!bytes || sceKernelReserveVirtualRange(&at, bytes, 0, PAGE) || !at) return MAP_FAILED;
    if (pw_wine_dmem_add_region(&dmem, (uintptr_t)at, bytes, 1)) {
        (void)sceKernelMunmap(at, bytes);
        if (fallen++ < 4)
            fprintf(stderr, "wine-ps5: anonymous mapping of %zu bytes stays on flexible memory (regions %u)\n",
                    bytes, dmem.regions);
        return MAP_FAILED;
    }
    if ((protection & 7) && pw_wine_dmem_replace(&dmem, (uintptr_t)at, bytes, (unsigned)protection & 7)) {
        (void)pw_wine_dmem_unmap(&dmem, (uintptr_t)at, bytes);
        return MAP_FAILED;
    }
    if (reported++ < 4)
        fprintf(stderr, "wine-ps5: private anonymous mapping with no address takes direct memory (%zu bytes at %p)\n",
                bytes, at);
    return at;
}

void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if(payload_mmap && !payload_allocate_direct) {
        if (!(flags & MAP_FIXED) || (uintptr_t)address != at || !flex_touches(at, end))
            return payload_map(address, bytes, protection, flags, fd, offset);
        if (!(flags & MAP_ANON)) {
            void *mapped;

            /* As over a title's direct memory: a private file view is read in (ENODEV makes ntdll pread it), a
             * shared one maps over the reservation, at exactly the address. */
            if (!(flags & MAP_SHARED)) {
                errno = ENODEV;
                return MAP_FAILED;
            }
            if (refuse_foreign(&flex, at, end)) return MAP_FAILED;
            mapped = mmap(address, bytes, protection, flags, fd, offset);
            if (mapped == MAP_FAILED) return MAP_FAILED;
            if (mapped != address) {
                munmap(mapped, bytes);
                errno = ENOMEM;
                return MAP_FAILED;
            }
            for (; at < end; at += piece)
                if (pw_wine_dmem_split(&flex, at, end - at, &piece)) (void)pw_wine_dmem_adopt(&flex, at, piece);
            return address;
        }
        errno = 0;
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&flex, at, end - at, &piece))
                status |= pw_wine_dmem_replace(&flex, at, piece, (unsigned)protection & 7);
            else if (payload_map((void *)at, piece, protection, flags, -1, 0) == MAP_FAILED)
                status = -1;
        if (!status) return address;
        refused_range("mmap", address, bytes, protection);
        if (errno != EEXIST) errno = ENOMEM;
        return MAP_FAILED;
    }
    /* Helpers' Windows commits land in a region Wine reserved. A private anonymous mapping with no address
     * (the Unix side, and an allocator that maps a pool and commits it later) used to be flexible memory.
     * With the application allocator it takes a direct-memory region. Executable and fixed mappings do not. */
    if (payload_allocate_direct && state > 0 &&
        pw_wine_anon_fresh_direct(!!(flags & MAP_ANON), !!(flags & MAP_PRIVATE), !!(flags & MAP_SHARED),
                                  !!(flags & MAP_FIXED), !!(protection & PROT_EXEC))) {
        void *placed = anon_direct_place(page_span(address, bytes), protection);
        if (placed != MAP_FAILED) return placed;
    }
    if (state <= 0 || !(flags & MAP_FIXED) || (uintptr_t)address != at ||
        (!pw_wine_dmem_split(&dmem, at, end - at, &piece) && piece == end - at))
        return payload_mmap ? payload_map(address, bytes, protection, flags, fd, offset)
                            : mmap(address, bytes, protection, flags, fd, offset);
    if (!(flags & MAP_ANON)) {
        void *mapped;

        /* A private file view (an image section) is read into direct
         * memory instead: ntdll falls back to pread when mmap reports
         * ENODEV. On the console a fixed file mapping over a reservation
         * does not give the requested pages. */
        if (!(flags & MAP_SHARED)) {
            errno = ENODEV;
            return MAP_FAILED;
        }
        /* Shared memory stays a file mapping, over what is there, as
         * before direct memory; only the exact address is accepted. */
        if (refuse_foreign(&dmem, at, end)) return MAP_FAILED;
        mapped = mmap(address, bytes, protection, flags, fd, offset);
        if (mapped == MAP_FAILED) return MAP_FAILED;
        if (mapped != address) {
            munmap(mapped, bytes);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&dmem, at, end - at, &piece) && pw_wine_dmem_adopt(&dmem, at, piece))
                __atomic_store_n(&backing_stale, 1, __ATOMIC_RELAXED);
        return address;
    }
    errno = 0;
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, (unsigned)protection & 7);
        else if ((payload_mmap ? payload_map((void *)at, piece, protection, flags, -1, 0)
                               : mmap((void *)at, piece, protection, flags, -1, 0)) == MAP_FAILED)
            status = -1;
    if (!status) return address;
    refused_range("mmap", address, bytes, protection);
    /* EEXIST: a foreign entry refused it; map_fixed_area answers STATUS_CONFLICTING_ADDRESSES. */
    if (errno != EEXIST) errno = ENOMEM;
    return MAP_FAILED;
}

/* Owned pages give their direct memory back, then their address space. */
int __wine_ps5_munmap(void *address, size_t bytes)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    lent_drop(at, end);
    if (payload_mmap && !payload_allocate_direct) {
        if (!flex_touches(at, end)) return outside_munmap(address, bytes);
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&flex, at, end - at, &piece))
                status |= pw_wine_dmem_unmap(&flex, at, piece);
            else
                status |= outside_munmap((void *)at, piece);
        if (status) errno = EINVAL;
        return status ? -1 : 0;
    }
    /* Ordinary unmapping keeps Wine's established page rounding. */
    if (state <= 0) return outside_munmap(address, bytes);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            /* Around any foreign entry, which keeps its pages owned; the rest leaves the regions. */
            status |= pw_wine_dmem_unmap(&dmem, at, piece);
        else
            status |= outside_munmap((void *)at, piece);
    if (status) errno = EINVAL;
    return status ? -1 : 0;
}

int __wine_ps5_mprotect(void *address, size_t bytes, int protection)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    /* A range the loader gave back that is about to run: convert it to JIT memory here, before anything executes it,
     * so the first execute fault at the carrier's old address never happens (the kernel answers that fault with
     * SYSTEM_XO_VIOLATION; fixed-base32-2026-10-19-01, carrier-xo32-2026-10-19-01). The conversion applies the
     * protection itself and checks it; a refusal falls through to the ordinary path, which changes nothing. A range
     * that only crosses the released image has its inside part converted here and the rest taken by that path. */
    if (payload_mmap && (protection & PROT_EXEC) && low_image_base) {
        const uintptr_t low_at = at > low_image_base ? at : low_image_base;
        const uintptr_t low_end = end < low_image_end ? end : low_image_end;
        if (low_at < low_end && !__wine_ps5_exec_convert((void *)low_at, low_end - low_at, protection) &&
            low_at == at && low_end == end)
            return 0;
    }

    if(payload_mprotect && !payload_allocate_direct) {
        if (!flex_touches(at, end)) return payload_protect(address, bytes, protection);
        errno = 0;
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&flex, at, end - at, &piece))
                status |= pw_wine_dmem_protect(&flex, at, piece, (unsigned)protection & 7);
            else
                status |= payload_protect((void *)at, piece, protection);
        if (status) {
            refused_range("mprotect", address, bytes, protection);
            if (errno != EEXIST) errno = ENOMEM;
        }
        return status ? -1 : 0;
    }
    if (state <= 0) return payload_mprotect ? payload_protect(address, bytes, protection)
                                          : mprotect(address, bytes, protection);
    errno = 0;
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_protect(&dmem, at, piece, (unsigned)protection & 7);
        else
            status |= payload_mprotect ? payload_protect((void *)at, piece, protection)
                                       : mprotect((void *)at, piece, protection);
    if (status) {
        refused_range("mprotect", address, bytes, protection);
        /* EEXIST: a commit refused over a foreign entry (patch 1128 then leaves the pages uncommitted). */
        if (errno != EEXIST) errno = ENOMEM;
    }
    return status ? -1 : 0;
}

/* For the title's log, as __wine_virtual_stats: direct memory backed now and
 * at most, its runs and refused calls, the heap's mapped and peak mapped
 * bytes, the direct memory backed below 4 GiB now and at most, then the
 * heap's live bytes, allocations, frees and contended calls, then the last
 * refused call (PW_WINE_DMEM_FAILED_*), its address and bytes. Returns how
 * many counters there are. */
unsigned int __wine_ps5_memory_stats(uint64_t *out, unsigned int count)
{
    PwWineDmemStats dmem_stats = { 0 };
    PwWineHeapStats heap_stats;
    uint64_t values[15];

    if (state > 0) pw_wine_dmem_stats(&dmem, &dmem_stats);
    pw_wine_heap_stats(&heap_stats);
    values[0] = dmem_stats.backed_bytes;
    values[1] = dmem_stats.peak_backed_bytes;
    values[2] = dmem_stats.runs;
    values[3] = dmem_stats.failures;
    values[4] = heap_stats.mapped_bytes;
    values[5] = heap_stats.peak_mapped_bytes;
    values[6] = dmem_stats.low_backed_bytes;
    values[7] = dmem_stats.peak_low_backed_bytes;
    values[8] = heap_stats.live_bytes;
    values[9] = heap_stats.allocations;
    values[10] = heap_stats.frees;
    values[11] = heap_stats.contended;
    values[12] = dmem_stats.failed;
    values[13] = dmem_stats.failed_address;
    values[14] = dmem_stats.failed_bytes;
    for (unsigned int i = 0; i < count && i < 15; i++) out[i] = values[i];
    return 15;
}

/* Who holds an engine's lock and for how long (PwWineDmem.holder): out[0] its kernel thread id, 0 when neither
 * engine is held, out[1] microseconds. The longer hold of the two engines. Atomic loads only: the helper's stall
 * watch reads it while the holder may be stuck (native/proton_wine_child.c). */
unsigned int __wine_ps5_dmem_holder(uint64_t *out, unsigned int count)
{
    const PwWineDmem *engines[2] = { &dmem, &flex };
    uint64_t values[7] = { 0, 0, 0, 0, 0, 0, (uint64_t)-1 };
    for (unsigned int i = 0; i < 2; i++) {
        const PwWineDmem *d = engines[i];
        const uint64_t holder = __atomic_load_n(&d->holder, __ATOMIC_ACQUIRE);
        const uint64_t held = holder ? pw_wine_perf_age_us(__atomic_load_n(&d->held_since, __ATOMIC_RELAXED)) : 0;
        if (holder && (!values[0] || held > values[1])) {
            values[0] = holder; values[1] = held;
            values[2] = __atomic_load_n(&d->doing, __ATOMIC_ACQUIRE);
            values[3] = __atomic_load_n(&d->doing_address, __ATOMIC_RELAXED);
            values[4] = __atomic_load_n(&d->doing_bytes, __ATOMIC_RELAXED);
            values[5] = __atomic_load_n(&d->doing_protection, __ATOMIC_RELAXED);
            values[6] = (uint64_t)__atomic_load_n(&d->doing_offset, __ATOMIC_RELAXED);
        }
    }
    for (unsigned int i = 0; i < count && i < 7; i++) out[i] = values[i];
    return 7;
}

/* Shared-file paging owns its real file pages, not anonymous direct memory.
 * Mark existing reservations caller-owned once; cache faults and eviction do
 * not consume a direct-memory run per page. The virtual mutex serializes this. */
int __wine_ps5_payload_memory_ready(void) { return payload_mmap != NULL; }
int __wine_ps5_dmem_file_region(void *address, size_t bytes)
{
    /* Whole host pages: a view of 4 KiB pages (win32u's placed range, patch 1130) is mapped by the host page. */
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    PwWineDmem *engine = payload_mmap && !payload_allocate_direct ? &flex : &dmem;
    int active = payload_mmap && !payload_allocate_direct ? flex_state : state;
    if ((uintptr_t)address + bytes < (uintptr_t)address) return -1;
    if (active <= 0) return 0;
    /* The pager maps file pages into the range on demand over its reservation: anything the engine had backed there
     * goes back to a bare reservation first. Adopting alone released the direct memory and left the pages unmapped,
     * and a mapping made without an address landed inside the view (2026-10-14; record removed from the tree; in git history). */
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(engine, at, end - at, &piece) &&
            (pw_wine_dmem_check(engine, at, piece) || pw_wine_dmem_replace(engine, at, piece, 0) ||
             pw_wine_dmem_adopt(engine, at, piece)))
            return -1;
    return 0;
}

/* A payload's direct memory under address, for a second mapping of it (pw_wine_alias.h): the direct memory at address
 * and the extent its run backs contiguously, [*low, *high). 1, or 0: a title (no JIT memory, so no engine to store
 * through an alias), flexible memory (nothing to map twice), a page of the caller's (JIT memory, a file) or a bare
 * one, or any after an adopt failed (backing_stale). Under Wine's virtual mutex: every release of a Windows view's memory
 * holds it too. */
int __wine_ps5_dmem_backing(uintptr_t address, int64_t *offset, uintptr_t *low, uintptr_t *high)
{
    PwWineDmemRun run;

    if (__atomic_load_n(&backing_stale, __ATOMIC_RELAXED)) return 0;
    if (!payload_allocate_direct || state <= 0 || !pw_wine_dmem_run_at(&dmem, address, &run)) return 0;
    *offset = run.offset + (int64_t)(address - run.address);
    *low = run.address;
    *high = run.address + run.bytes;
    return 1;
}

/* The engine in use, or NULL before its self-check and after a refused one. */
static PwWineDmem *active(void)
{
    if (payload_mmap && !payload_allocate_direct) return flex_state > 0 ? &flex : NULL;
    return state > 0 ? &dmem : NULL;
}

/* The pager has put every page of [address, +bytes) back to a guard of its own (__wine_ps5_section_remove), or never
 * took it: the engine forgets the range it adopted for it, so the release that follows asks the kernel what is there
 * instead of trusting a run that is not a mapping of Wine's. 0, or -1 when a piece stayed the caller's run (a full
 * table): a placed GPU mapping must not go there, as the engine would protect it and lay over it at a release. */
int __wine_ps5_dmem_file_released(void *address, size_t bytes)
{
    PwWineDmem *engine = active();
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (!engine || (uintptr_t)address + bytes < (uintptr_t)address) return 0;
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(engine, at, end - at, &piece)) status |= pw_wine_dmem_disown(engine, at, piece);
    return status ? -1 : 0;
}

/* win32u's placed range, made bare (__wine_ps5_dmem_file_region, then __wine_ps5_dmem_file_released), is lent to the
 * GPU client's mapping until Wine releases it. 0, or -1 when the table is full (the placed map is refused). */
int __wine_ps5_dmem_lend(void *address, size_t bytes)
{
    const uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    int status = -1;

    lent_take();
    if (lent_count < LENT_MAX) {
        lent[lent_count].start = at;
        lent[lent_count++].end = end;
        status = 0;
    }
    lent_give();
    if (status) say("[wine-ps5] placed GPU range not lent: the table is full\n");
    return status;
}

/* Patch 1128's release: a range wholly inside one region is kept reserved (Wine makes it its reserved area) instead of
 * unmapped, so no large munmap of a bare span is made any more; its bare parts are asked about first and every foreign
 * entry recorded. 0 kept, -1 not the engine's (the caller unmaps as before). */
int __wine_ps5_dmem_keep(void *address, size_t bytes)
{
    PwWineDmem *engine = active();
    const uintptr_t at = page_down(address);
    const size_t span = page_span(address, bytes);

    lent_drop(at, at + span);
    if (!engine || !pw_wine_dmem_owns(engine, at, span)) return -1;
    dump_entries("keep", at, span);
    return pw_wine_dmem_keep(engine, at, span);
}

/* Before the pager maps into [address, +bytes) of a view's guards with a fixed mapping (pw_wine_section_ps5.c): the
 * view is the caller's run or lies outside the regions (the section arena), so the engine never asks about it
 * otherwise. 0, or -1 with errno EEXIST when a mapping that is not Wine's lies there (or the kernel did not answer),
 * recorded and counted. */
int __wine_ps5_foreign_in(void *address, size_t bytes)
{
    PwWineDmem *engine = active();
    return engine ? pw_wine_dmem_check_guards(engine, page_down(address), page_span(address, bytes)) : 0;
}

/* Before Wine reserves [address, +bytes) inside a reserved area it kept (patch 1128): a replace with protection 0 asks
 * the kernel nothing, so the bare parts are asked about here, as a commit's are. 0, or -1 with errno EEXIST. */
int __wine_ps5_dmem_check(void *address, size_t bytes)
{
    PwWineDmem *engine = active();
    const uintptr_t at = page_down(address);
    return engine && refuse_foreign(engine, at, at + page_span(address, bytes)) ? -1 : 0;
}

/* VirtualQuery's walk between Windows views (patch 0959) with the guard's signature: a mapping that is not Wine's
 * inside its reserved areas reads as occupied, even before the engine has run into it. */
int __wine_ps5_vm_gap(const void *address, const void *limit, pw_wine_vm_owned_query owned, void **start, void **end)
{
    static unsigned reported;
    const int guarded = __atomic_load_n(&signature_state, __ATOMIC_ACQUIRE) > 0;
    const int status = pw_wine_vm_gap(address, limit, owned, guarded ? &signature : NULL, start, end);
    PwWineVmEntry entry;
    char line[224];

    /* Which entries read as occupied for the signature alone: virtual-query32 found nothing free below 16 MiB once
     * the signature applied here (v2-backend-gate-2026-10-15), where it had found 16 blocks. */
    if (status == 1 && guarded && reported < 8 && !pw_wine_vm_entry(*start, &entry) &&
        (entry.protection != signature.protection || entry.flags != signature.flags)) {
        reported++;
        snprintf(line, sizeof(line), "[wine-ps5] gap %#lx-%#lx reads occupied: protection %#x type %d flags %#x %s\n",
                 (unsigned long)entry.start, (unsigned long)entry.end, entry.protection, entry.type, entry.flags,
                 entry.name);
        say(line);
    }
    return status;
}

/* The pager lays its guards as the engine does, and not at all once the self-check found that a laid reservation does
 * not read as a bare one (check_laid): the guard would take every guard laid since for a foreign mapping. */
int __wine_ps5_lay(void *address, size_t bytes)
{
    return lay(NULL, (uintptr_t)address, bytes);
}

/* The next foreign entry found inside the regions and not yet given to Wine, which makes it a view of its own (patch
 * 1128): 1 with its range, 0 when there is none. Under virtual_mutex. */
int __wine_ps5_native_hole(void **address, size_t *bytes)
{
    PwWineDmem *engine = active();
    uintptr_t at;

    if (!engine || !pw_wine_dmem_take_hole(engine, &at, bytes)) return 0;
    *address = (void *)at;
    return 1;
}

/* For the helper's census and its terminal line, lock-free (plain loads, wherever Wine's threads stopped): foreign
 * entries found, unmaps that went around one, mappings refused, queries not answered, holes recorded, then for the
 * first four holes their address, bytes, protection, skips and refusals. Returns how many counters there are. */
unsigned int __wine_ps5_foreign_stats(uint64_t *out, unsigned int count)
{
    PwWineDmem *engine = payload_mmap && !payload_allocate_direct ? &flex : &dmem;
    const unsigned holes = __atomic_load_n(&engine->hole_count, __ATOMIC_RELAXED);
    uint64_t values[25] = { __atomic_load_n(&engine->stats.foreign_found, __ATOMIC_RELAXED),
                            __atomic_load_n(&engine->stats.unmaps_skipped, __ATOMIC_RELAXED),
                            __atomic_load_n(&engine->stats.maps_refused, __ATOMIC_RELAXED),
                            __atomic_load_n(&engine->stats.query_failures, __ATOMIC_RELAXED), holes };

    for (unsigned i = 0; i < 4 && i < holes; i++) {
        const PwWineDmemHole *hole = &engine->holes[i];
        values[5 + i * 5] = __atomic_load_n(&hole->address, __ATOMIC_RELAXED);
        values[6 + i * 5] = __atomic_load_n(&hole->bytes, __ATOMIC_RELAXED);
        values[7 + i * 5] = __atomic_load_n(&hole->protection, __ATOMIC_RELAXED);
        values[8 + i * 5] = __atomic_load_n(&hole->skips, __ATOMIC_RELAXED);
        values[9 + i * 5] = __atomic_load_n(&hole->refusals, __ATOMIC_RELAXED);
    }
    for (unsigned int i = 0; i < count && i < 25; i++) out[i] = values[i];
    return 25;
}
