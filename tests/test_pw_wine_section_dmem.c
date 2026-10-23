/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The section data test with the section in direct memory: the title's table (pw_wine_dsection) hands out chunks of a
 * pool and the pager maps them through pw_wine_dmap; every alias reads every write, with no flush to a file. Then the
 * pager's direct path (pw_wine_section_use_direct): whole 2 MiB chunks, mapped once and never evicted, each host page
 * with its own protection, and a title answer that fails for now retried instead of failing the view. */
#define PW_SECTION_DMEM
#define main section_data_main
#include "test_pw_wine_section_data.c"
#undef main

enum { CHUNK = PW_WINE_SECTION_CHUNK, READ = PW_WINE_SECTION_READ, WRITE = PW_WINE_SECTION_WRITE };

static unsigned page_failures, map_failures;
static int page_errno;
static unsigned protects, unmaps, reserves;
static uintptr_t protect_at, readonly_page;
static size_t protect_bytes;
static unsigned protect_prot;
static int section_fd = -1;

static int fake_page(void *c, uint64_t dev, uint64_t ino, uint64_t off, int64_t *dmem, uint64_t *co, uint64_t *cb)
{
    if (page_failures) {
        page_failures--;
        errno = page_errno;
        return -1;
    }
    return dmap_page(c, dev, ino, off, dmem, co, cb);
}
/* A mapping that is not Wine's, placed inside a view's guards after the view was added; no map may reach it. */
static uintptr_t foreign_at;
static unsigned foreign_checks;
static int guard_op(void *c, uintptr_t at, size_t n)
{
    (void)c;
    foreign_checks++;
    return foreign_at && at < foreign_at + PAGE && foreign_at < at + n;
}
/* A refused kernel mapping that took the range's reservation with it. */
static int fake_map(void *c, uintptr_t at, size_t n, int64_t dmem)
{
    assert(!foreign_at || at >= foreign_at + PAGE || foreign_at >= at + n);
    if (map_failures) {
        map_failures--;
        assert(!munmap((void *)at, n));
        return -1;
    }
    return dmap_map(c, at, n, dmem);
}
static int plain_files;   /* the pager's question answers "a file" (this test's identity names one section only) */
static int direct_op(void *c, int fd) { (void)c; return plain_files ? 0 : pw_wine_dmap_direct(fd); }
static unsigned protection_op(void *c, uintptr_t at) { (void)c; return at == readonly_page ? READ : READ | WRITE; }
static int counting_protect(void *c, uintptr_t at, size_t n, unsigned prot)
{
    protects++;
    protect_at = at;
    protect_bytes = n;
    protect_prot = prot;
    return protect_op(c, at, n, prot);
}
static int counting_unmap(void *c, uintptr_t at, size_t n) { unmaps++; return unmap_op(c, at, n); }
static int counting_reserve(void *c, uintptr_t at, size_t n) { reserves++; return reserve_op(c, at, n); }
/* A guard laid over the mapping in one call: a fixed mapping replaces it. */
static unsigned lays;
static int laying;
static int counting_lay(void *c, uintptr_t at, size_t n)
{
    (void)c;
    lays++;
    return mmap((void *)at, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)at ? 0 : -1;
}
static uint64_t asked(void)
{
    uint64_t requests;
    pw_wine_dmap_stats(&requests, NULL, NULL);
    return requests;
}

/* The pager's cache (the included test's `cache`, which its SIGSEGV handler serves) set up for direct views. */
static void direct_setup(void)
{
    static const PwWineDsectionOps table_ops = { NULL, pool_allocate, pool_grant, pool_zero, pool_release, pool_alive,
                                                 0 };
    static const PwWineDmapOps client_ops = { NULL, dmap_identity, fake_page, fake_map };
    static const PwWineSectionOps ops = { NULL, PAGE, retain, closed, map_op, counting_protect, flush_op,
                                          counting_unmap, counting_reserve, NULL, guard_op };
    static const PwWineSectionOps laid_ops = { NULL, PAGE, retain, closed, map_op, counting_protect, flush_op,
                                               counting_unmap, counting_reserve, counting_lay, guard_op };
    static uint32_t buckets[64];
    if (pool_fd >= 0) close(pool_fd);
    pool_fd = memfd_create("pool", 0);
    assert(pool_fd >= 0 && !ftruncate(pool_fd, (off_t)96 << 20));
    pool_next = 0;
    pw_wine_dsection_init(&table_ops);
    pw_wine_dsection_release(7, 42);
    (void)pw_wine_dsection_collect(NULL, NULL);
    assert(!pw_wine_dsection_register(-1, 7, 42, 256u << 20));   /* views reach 224 MiB */
    pw_wine_dmap_init(&client_ops);
    if (section_fd < 0) section_fd = memfd_create("section", 0);
    assert(section_fd >= 0);
    memset(&cache, 0, sizeof(cache));
    assert(!pw_wine_section_init(&cache, laying ? &laid_ops : &ops, pages, CACHE));
    assert(!pw_wine_section_use_index(&cache, buckets, 64));
    assert(pw_wine_section_use_direct(&cache, NULL, protection_op) == -1);
    assert(!pw_wine_section_use_direct(&cache, direct_op, protection_op));
}
static uintptr_t direct_view(PwWineSectionView *view, size_t bytes, uint64_t offset)
{
    void *at = mmap(NULL, bytes + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(at != MAP_FAILED);
    const uintptr_t base = ((uintptr_t)at + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    assert(!pw_wine_section_add(&cache, view, base, bytes, section_fd, offset));
    return base;
}
static int fault(uintptr_t at, unsigned protection, unsigned access)
{
    return pw_wine_section_fault_run(&cache, at, protection, access, 0, &retry, 1);
}

/* 64 MiB written a byte every 16 KiB through SIGSEGV: 32 chunks mapped, nothing through the cache (4,096 page faults
 * before), one question to the title a chunk; an alias reads it all back without asking again. */
static void sequential(void)
{
    const size_t bytes = (size_t)64 << 20;
    direct_setup();
    const uintptr_t base = direct_view(&views[0], bytes, 0);
    for (size_t off = 0; off < bytes; off += PAGE) ((volatile unsigned char *)base)[off] = (unsigned char)(off / PAGE);
    assert(cache.direct_maps == 32 && cache.direct_bytes == bytes && !cache.faults && !cache.evictions && !cache.count);
    assert(asked() == 32);
    const uintptr_t alias = direct_view(&views[1], bytes, 0);
    for (size_t off = 0; off < bytes; off += PAGE)
        assert(((volatile unsigned char *)alias)[off] == (unsigned char)(off / PAGE));
    assert(cache.direct_maps == 64 && asked() == 32);
    unmaps = reserves = 0;
    assert(!pw_wine_section_remove(&cache, &views[0]) && !pw_wine_section_remove(&cache, &views[1]));
    assert(unmaps == 64 && reserves == 64 && !cache.direct_bytes && !cache.failures);
}

/* A host page that is not read-write takes its own protection; a fault the protection forbids maps and asks nothing. */
static void protections(void)
{
    direct_setup();
    const uintptr_t base = direct_view(&views[0], 2 * CHUNK, 128u << 20);
    const uint64_t before = asked();
    retry = (PwWineSectionRetry){ 0 };
    assert(!fault(base + CHUNK, 0, READ) && !fault(base + CHUNK, READ, WRITE));
    assert(asked() == before && !cache.direct_maps);
    readonly_page = base + 5 * PAGE;
    protects = 0;
    assert(fault(base, READ, READ) == 1);
    assert(protects == 1 && protect_at == base + 5 * PAGE && protect_bytes == PAGE && protect_prot == READ);
    /* One question: this chunk (whether the section is direct was asked when the view was added). */
    assert(cache.direct_maps == 1 && cache.direct_bytes == CHUNK && asked() == before + 1);
    /* A write the page's protection forbids is the program's own; after VirtualProtect makes it writable it is not. */
    assert(!fault(base + 5 * PAGE, READ, WRITE));
    readonly_page = 0;
    protects = 0;
    assert(pw_wine_section_protect(&cache, base + 5 * PAGE, PAGE, READ | WRITE) == 1 && protects == 1);
    ((volatile unsigned char *)base)[5 * PAGE] = 0x5a;
    /* The kernel's protection lagging Windows': protected again once, then the same access is a real fault. */
    assert(fault(base + 6 * PAGE, READ | WRITE, WRITE) == 1 && protects == 2);
    assert(!fault(base + 6 * PAGE, READ | WRITE, WRITE) && protects == 2);
    /* Only the mapped chunk of a range is protected. */
    protects = 0;
    assert(pw_wine_section_protect(&cache, base + CHUNK - PAGE, 2 * PAGE, READ) == 1);
    assert(protects == 1 && protect_at == base + CHUNK - PAGE && protect_bytes == PAGE);
    assert(!pw_wine_section_remove(&cache, &views[0]) && !cache.direct_bytes);
}

/* The title's answer failing: for now, retried; three times in a row, -1; for good, -1 at once; none fails the view.
 * A refused kernel mapping does. */
static void answers(void)
{
    direct_setup();
    const uintptr_t base = direct_view(&views[0], 6 * CHUNK, 160u << 20);
    retry = (PwWineSectionRetry){ 0 };
    assert(fault(base, READ | WRITE, READ) == 1);   /* learns the section is direct */
    page_errno = ENOMEM; page_failures = 1;
    assert(fault(base + CHUNK, READ | WRITE, WRITE) == PW_WINE_SECTION_RETRY && !views[0].failed && cache.retries == 1);
    assert(fault(base + CHUNK, READ | WRITE, WRITE) == 1);
    page_errno = ETIMEDOUT; page_failures = 1;
    assert(fault(base + 2 * CHUNK, READ | WRITE, WRITE) == PW_WINE_SECTION_RETRY && cache.retries == 2);
    assert(fault(base + 2 * CHUNK, READ | WRITE, WRITE) == 1);
    page_errno = ENOMEM; page_failures = 100;
    assert(fault(base + 3 * CHUNK, READ | WRITE, WRITE) == PW_WINE_SECTION_RETRY);
    assert(fault(base + 3 * CHUNK, READ | WRITE, WRITE) == PW_WINE_SECTION_RETRY);
    assert(fault(base + 3 * CHUNK, READ | WRITE, WRITE) == -1 && !views[0].failed && cache.failed_stage == 8);
    page_failures = 0;
    assert(fault(base + 3 * CHUNK, READ | WRITE, WRITE) == 1);
    page_errno = EIO; page_failures = 1;
    assert(fault(base + 4 * CHUNK, READ | WRITE, WRITE) == -1 && !views[0].failed);
    assert(fault(base + 4 * CHUNK, READ | WRITE, WRITE) == 1);
    for (unsigned c = 0; c < 5; c++) ((volatile unsigned char *)base)[c * CHUNK + 7] = (unsigned char)c;
    assert(cache.direct_maps == 5);
    map_failures = 1;
    assert(fault(base + 5 * CHUNK, READ | WRITE, WRITE) == -1 && views[0].failed == 1 && !views[0].lost_guard);
    assert(cache.failed_stage == 5 && cache.direct_maps == 5);
    /* Removal gives back the five mapped chunks; the refused one is a guard again. */
    unmaps = reserves = 0;
    assert(!pw_wine_section_remove(&cache, &views[0]));
    assert(unmaps == 5 && reserves == 5 && !cache.direct_bytes);
}

/* Remove with chunks 0 and 2 of 3 mapped: two unmap and reserve pairs. */
static void removal(void)
{
    direct_setup();
    const uintptr_t base = direct_view(&views[0], 3 * CHUNK - 4 * PAGE, 224u << 20);
    retry = (PwWineSectionRetry){ 0 };
    assert(fault(base, READ | WRITE, WRITE) == 1 && fault(base + 2 * CHUNK, READ | WRITE, WRITE) == 1);
    assert(cache.direct_bytes == CHUNK + CHUNK - 4 * PAGE);
    unmaps = reserves = 0;
    assert(!pw_wine_section_remove(&cache, &views[0]));
    assert(unmaps == 2 && reserves == 2 && !cache.direct_bytes && !views[0].chunks);
    /* With a lay op each chunk goes back to a guard in one call, never through a hole. */
    laying = 1;
    direct_setup();
    const uintptr_t again = direct_view(&views[0], 3 * CHUNK - 4 * PAGE, 224u << 20);
    retry = (PwWineSectionRetry){ 0 };
    assert(fault(again, READ | WRITE, WRITE) == 1 && fault(again + 2 * CHUNK, READ | WRITE, WRITE) == 1);
    unmaps = reserves = lays = 0;
    assert(!pw_wine_section_remove(&cache, &views[0]));
    assert(lays == 2 && !unmaps && !reserves && !cache.direct_bytes);
    laying = 0;
}

/* The GPU client's mapping, placed by the kernel inside an untouched chunk after the view was added (late-library
 * 2026-10-15): a touch anywhere in that chunk maps nothing over it; the fault fails (stage 9) and the view stays usable.
 * One check per chunk fault; once the mapping is gone the chunk maps. */
static void foreign_in_chunk(void)
{
    direct_setup();
    const uintptr_t base = direct_view(&views[0], 2 * CHUNK, 32u << 20);
    const int radv = memfd_create("radv", 0);
    assert(radv >= 0 && !ftruncate(radv, PAGE));
    assert(mmap((void *)(base + CHUNK / 2), PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, radv, 0) ==
           (void *)(base + CHUNK / 2));
    memset((void *)(base + CHUNK / 2), 0x5a, PAGE);
    foreign_at = base + CHUNK / 2;
    foreign_checks = 0;
    retry = (PwWineSectionRetry){ 0 };
    const uint64_t failures = cache.failures;
    assert(fault(base, READ | WRITE, WRITE) == -1 && cache.failed_stage == 9 && !views[0].failed);
    assert(foreign_checks == 1 && !cache.direct_maps && cache.failures == failures + 1);
    for (unsigned i = 0; i < PAGE; i++) assert(((unsigned char *)foreign_at)[i] == 0x5a);
    assert(fault(base + CHUNK, READ | WRITE, WRITE) == 1 && cache.direct_maps == 1 && foreign_checks == 2);
    /* The client freed it and the guard is back: the chunk maps. */
    assert(mmap((void *)foreign_at, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
           (void *)foreign_at);
    foreign_at = 0;
    assert(fault(base + 7 * PAGE, READ | WRITE, WRITE) == 1 && cache.direct_maps == 2);
    assert(!pw_wine_section_remove(&cache, &views[0]) && !cache.direct_bytes);
    close(radv);
}

/* A process that mapped a view and has not touched it is admitted: the section's release and a collect leave its
 * chunks, and its first touch maps a chunk instead of finding the section gone (ENOENT, then the file). */
static int alive_pid;
static int alive_op(void *c, int pid) { (void)c; return pid == alive_pid; }
static void admitted_when_mapped(void)
{
    static const PwWineDsectionOps alive_ops = { NULL, pool_allocate, pool_grant, pool_zero, pool_release, alive_op, 0 };
    direct_setup();
    pw_wine_dsection_init(&alive_ops);   /* only the ops change; the registered section stays */
    alive_pid = 1234;                    /* the pager's pid (dmap_page) */
    const uintptr_t base = direct_view(&views[0], CHUNK, 0);
    assert(views[0].direct == 1);
    pw_wine_dsection_release(7, 42);     /* the last handle closed; the view remains */
    uint64_t given = 0, lost = 0;
    assert(pw_wine_dsection_collect(&given, &lost) == 1 && !given);
    retry = (PwWineSectionRetry){ 0 };
    assert(fault(base, READ | WRITE, WRITE) == 1 && cache.direct_maps == 1);
    assert(!pw_wine_section_remove(&cache, &views[0]));
    alive_pid = 0;                       /* the process ended: its chunk goes back */
    assert(!pw_wine_dsection_collect(&given, &lost) && given == 1 && !lost);
}

/* A second mapping of a mapped chunk (pw_wine_alias.h): only a direct view's mapped chunk answers, with that chunk's
 * extent in the view (a view at an offset off the chunk boundary starts mid-chunk) and its direct memory from the
 * table without asking the title again; that memory mapped at another address reads and writes the view's bytes.
 * Nothing answers before the chunk is mapped, for a cache view, or once the view is removed. */
static void backing_for_alias(void)
{
    direct_setup();
    const uint64_t offset = (64u << 20) - 4 * PAGE;
    const uintptr_t base = direct_view(&views[0], 2 * CHUNK, offset);
    int fd = -1;
    uint64_t at_offset = 0, chunk_offset = 0, chunk_bytes = 0;
    uintptr_t low = 0, high = 0;
    int64_t dmem = -1;
    assert(!pw_wine_section_backing(&cache, base + 5 * PAGE, &fd, &at_offset, &low, &high));
    retry = (PwWineSectionRetry){ 0 };
    assert(fault(base + 5 * PAGE, READ | WRITE, WRITE) == 1);   /* the second chunk of the view: [4 pages, +CHUNK) */
    assert(!pw_wine_section_backing(&cache, base, &fd, &at_offset, &low, &high));   /* its first chunk: not mapped */
    assert(pw_wine_section_backing(&cache, base + 5 * PAGE, &fd, &at_offset, &low, &high) == 1);
    assert(fd == views[0].fd && at_offset == offset + 5 * PAGE && low == base + 4 * PAGE && high == low + CHUNK);
    assert(!pw_wine_section_backing(NULL, base + 5 * PAGE, &fd, &at_offset, &low, &high));
    assert(!pw_wine_section_backing(&cache, base + 5 * PAGE, NULL, &at_offset, &low, &high));
    const uint64_t before = asked();
    assert(!pw_wine_dmap_lookup(fd, at_offset, &dmem, &chunk_offset, &chunk_bytes) && asked() == before);
    assert(chunk_offset == 64u << 20 && chunk_bytes == CHUNK && dmem >= 0);
    errno = 0;
    assert(pw_wine_dmap_lookup(fd, at_offset, NULL, &chunk_offset, &chunk_bytes) == -1 && errno == EINVAL);
    /* The chunk's memory at a second address is the view's. */
    void *second = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, (off_t)dmem);
    assert(second != MAP_FAILED);
    ((volatile unsigned char *)second)[PAGE + 9] = 0xa5;
    assert(((volatile unsigned char *)base)[5 * PAGE + 9] == 0xa5);
    ((volatile unsigned char *)base)[5 * PAGE + 10] = 0x5a;
    assert(((volatile unsigned char *)second)[PAGE + 10] == 0x5a);
    assert(!munmap(second, CHUNK));
    assert(!pw_wine_section_remove(&cache, &views[0]));
    assert(!pw_wine_section_backing(&cache, base + 5 * PAGE, &fd, &at_offset, &low, &high));
    /* A file's view is the cache's: no direct memory, no alias. */
    const int file = memfd_create("file", 0);
    assert(file >= 0 && !ftruncate(file, CHUNK));
    void *at = mmap(NULL, CHUNK + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(at != MAP_FAILED);
    const uintptr_t cached = ((uintptr_t)at + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    plain_files = 1;
    assert(!pw_wine_section_add(&cache, &views[1], cached, CHUNK, file, 0) && views[1].direct == -1);
    plain_files = 0;
    retry = (PwWineSectionRetry){ 0 };
    assert(fault(cached, READ | WRITE, WRITE) == 1 && cache.count == 1);
    assert(!pw_wine_section_backing(&cache, cached, &fd, &at_offset, &low, &high));
    assert(!pw_wine_section_remove(&cache, &views[1]));
    close(file);
}

/* The included test's threaded round trip (every word through every alias, 4 threads), on the direct path: one map a
 * chunk each view touches (2 + 2 + 2), nothing evicted. */
static void direct_round_trip(void)
{
    direct_setup();
    memset(model, 0, sizeof(model));
    failures = 0;
    run_pages = 1;
    for (int v = 0; v < 3; v++) bases[v] = direct_view(&views[v], view_pages[v] * PAGE, offsets[v]);
    pthread_t threads[THREADS];
    for (uintptr_t t = 0; t < THREADS; t++) assert(!pthread_create(&threads[t], NULL, worker, (void *)t));
    for (int t = 0; t < THREADS; t++) pthread_join(threads[t], NULL);
    for (int v = 0; v < 3; v++)
        for (size_t w = offsets[v] / 8; w < offsets[v] / 8 + view_pages[v] * PAGE / 8; w++)
            if (*word_at(v, w) != model[w]) failures++;
    printf("direct: maps=%llu evictions=%llu failures=%llu mismatches=%u\n", (unsigned long long)cache.direct_maps,
           (unsigned long long)cache.evictions, (unsigned long long)cache.failures, failures);
    assert(!failures && !cache.failures && !cache.evictions && !cache.faults && cache.direct_maps == 6);
    for (int v = 0; v < 3; v++) assert(!pw_wine_section_remove(&cache, &views[v]));
    assert(!cache.direct_bytes);
}

/* A 4 KiB view of a direct section is asked as the 16 KiB host page that holds it. An aligned span is unchanged.
 * Battle.net's Mojo buffers were mapped at 4 KiB and the kernel refused the direct mapping. */
static uintptr_t recorded_at;
static size_t recorded_bytes;
static int64_t recorded_dmem;
static int record_map(void *c, uintptr_t at, size_t n, int64_t dmem)
{
    (void)c;
    recorded_at = at;
    recorded_bytes = n;
    recorded_dmem = dmem;
    return 0;
}
static void host_page_cover(void)
{
    static const PwWineDmapOps ops = { NULL, dmap_identity, fake_page, record_map };
    direct_setup();
    pw_wine_dmap_init(&ops);
    assert(pw_wine_dmap_direct(section_fd) == 1);
    assert(!pw_wine_dmap_map(section_fd, 0x4e10000, 0x1000, 0));
    assert(recorded_at == 0x4e10000 && recorded_bytes == 16384 && (recorded_dmem & 16383) == 0);
    assert(!pw_wine_dmap_map(section_fd, 0x4e10000, 0x1c000, 0));
    assert(recorded_at == 0x4e10000 && recorded_bytes == 0x1c000);
}

int main(void)
{
    struct sigaction action = { 0 };
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    host_page_cover();
    sequential();
    protections();
    answers();
    removal();
    foreign_in_chunk();
    admitted_when_mapped();
    backing_for_alias();
    direct_round_trip();
    puts("section direct chunks: whole chunks, per-page protection, retried answers, removal, a foreign mapping in a "
         "chunk never mapped over, a mapped chunk's backing for a second mapping, aliases under threads, passed");
    close(section_fd);
    return section_data_main();
}
