/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The demand-mapped section cache with real memory: every byte written through a view must read back through every
 * alias of the same file offset, across evictions, clustered faults and threads. The cache's native calls are real
 * Linux calls on a memory file, and pages are brought in by a SIGSEGV handler, as the console's fault path does.
 * A Unity game under an emulator read other data in place of its own (2026-10-14, record removed from the tree: a free-list link
 * holding "mscorlib"); this is the check that the cache itself does not mix pages up. */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_section.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { PAGE = 16384, FILE_PAGES = 256, CACHE = 24, THREADS = 4, OPS = 60000 };
enum { WORDS = FILE_PAGES * PAGE / 8 };

static PwWineSection cache;
static PwWineSectionPage pages[CACHE];
static PwWineSectionView views[3];
static uintptr_t bases[3];
static uint64_t offsets[3] = { 0, 0, 64 * PAGE };   /* view 2 aliases the file from page 64 */
static size_t view_pages[3] = { FILE_PAGES, FILE_PAGES, FILE_PAGES - 64 };
static uint64_t model[WORDS];
static pthread_mutex_t pager = PTHREAD_MUTEX_INITIALIZER;   /* Wine's virtual mutex */
static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t run_pages = 1;
static __thread PwWineSectionRetry retry;

static int retain(void *c, int fd) { (void)c; return dup(fd); }
static int closed(void *c, int fd) { (void)c; return close(fd); }
static int prot_of(unsigned p) { return (p & 1 ? PROT_READ : 0) | (p & 2 ? PROT_WRITE : 0); }
/* PW_SECTION_INCOHERENT: model the console's /data (bfs) as measured for sparse files (docs/PLAN.md, patch 1007:
 * mapped writes to blocks ftruncate created are not seen by other readers until msync): each mapping is its own copy,
 * read from the file when mapped and written back by msync. Without it, mappings share the page cache (Linux). */
#ifdef PW_SECTION_DMEM
/* PW_SECTION_DMEM: the section in direct memory (wine/ps5/pw_wine_dsection.h, pw_wine_dmap.h): the title's table hands
 * out 2 MiB chunks of a pool (a memory file standing in for the console's direct memory) and the pager maps them. */
#include "../wine/ps5/pw_wine_dmap.h"
#include "../wine/ps5/pw_wine_dsection.h"
static int pool_fd = -1;
static int64_t pool_next;
static int pool_allocate(void *c, uint64_t bytes, int pid, int64_t *offset) { (void)c; (void)pid; *offset = pool_next; pool_next += bytes; return 0; }
static int pool_grant(void *c, int64_t o, uint64_t b, int pid) { (void)c; (void)o; (void)b; (void)pid; return 0; }
static int pool_zero(void *c, int64_t o, uint64_t b) { (void)c; (void)o; (void)b; return 0; }   /* a fresh memory file reads zero */
static int pool_release(void *c, int64_t o, uint64_t b) { (void)c; (void)o; (void)b; return 0; }
static int pool_alive(void *c, int pid) { (void)c; (void)pid; return 0; }   /* each round's pager is gone by the next */
static int dmap_identity(void *c, int fd, uint64_t *dev, uint64_t *ino) { (void)c; (void)fd; *dev = 7; *ino = 42; return 0; }
static int dmap_page(void *c, uint64_t dev, uint64_t ino, uint64_t off, int64_t *dmem, uint64_t *co, uint64_t *cb)
{
    (void)c;
    return pw_wine_dsection_page(dev, ino, off, 1234, dmem, co, cb);
}
static int dmap_map(void *c, uintptr_t at, size_t n, int64_t dmem)
{
    (void)c;
    return mmap((void *)at, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, pool_fd, (off_t)dmem) == (void *)at ? 0 : -1;
}
static int map_op(void *c, uintptr_t at, size_t n, unsigned prot, int fd, uint64_t off)
{
    (void)c;
    if (pw_wine_dmap_direct(fd) != 1 || pw_wine_dmap_map(fd, at, n, off)) return -1;
    return mprotect((void *)at, n, prot_of(prot));
}
static int flush_op(void *c, uintptr_t at, size_t n) { (void)c; (void)at; (void)n; return 0; }   /* nothing to write back */
static int unmap_op(void *c, uintptr_t at, size_t n) { (void)c; return munmap((void *)at, n); }
#elif defined(PW_SECTION_INCOHERENT)
#include "../wine/ps5/pw_wine_blocks.h"
static int backing_fd = -1;
static unsigned char allocated[FILE_PAGES * PAGE / PW_WINE_BLOCKS_CHUNK + 1];   /* chunks with real blocks */
static struct { uintptr_t at; uint64_t off; size_t n; } copies[4096];
static int chunks_allocated(uint64_t off, size_t n)
{
    for (uint64_t c = off / PW_WINE_BLOCKS_CHUNK; c <= (off + n - 1) / PW_WINE_BLOCKS_CHUNK; c++) if (!allocated[c]) return 0;
    return 1;
}
static void allocate_chunks(uint64_t off, size_t n)
{
    for (uint64_t c = off / PW_WINE_BLOCKS_CHUNK; c <= (off + n - 1) / PW_WINE_BLOCKS_CHUNK; c++) allocated[c] = 1;
}
static int map_op(void *c, uintptr_t at, size_t n, unsigned prot, int fd, uint64_t off)
{
    (void)c;
#ifdef PW_SECTION_BLOCKS
    (void)pw_wine_blocks_ensure(fd, off, n);
#endif
    /* Blocks allocated: one page cache, every mapping sees every write. A hole: this mapping's own copy. */
    if (chunks_allocated(off, n))
        return mmap((void *)at, n, prot_of(prot), MAP_SHARED | MAP_FIXED, fd, (off_t)off) == (void *)at ? 0 : -1;
    if (mmap((void *)at, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)at) return -1;
    if (pread(backing_fd, (void *)at, n, (off_t)off) != (ssize_t)n) return -1;
    for (unsigned i = 0; i < 4096; i++) if (!copies[i].n) { copies[i].at = at; copies[i].off = off; copies[i].n = n; break; }
    return mprotect((void *)at, n, prot_of(prot));
}
static int flush_op(void *c, uintptr_t at, size_t n)
{
    (void)c;
    for (unsigned i = 0; i < 4096; i++)
        if (copies[i].n && at >= copies[i].at && at < copies[i].at + copies[i].n) {
            const uint64_t off = copies[i].off + (at - copies[i].at);
            allocate_chunks(off, n);   /* a real write allocates */
            return pwrite(backing_fd, (void *)at, n, (off_t)off) == (ssize_t)n ? 0 : -1;
        }
    return msync((void *)at, n, MS_SYNC);
}
static int unmap_op(void *c, uintptr_t at, size_t n)
{
    (void)c;
    for (unsigned i = 0; i < 4096; i++) if (copies[i].n && copies[i].at == at) copies[i].n = 0;
    return munmap((void *)at, n);
}
static int block_identity(void *c, int fd, uint64_t *id) { (void)c; (void)fd; *id = 1; return 0; }
static ssize_t block_read(void *c, int fd, void *b, size_t n, uint64_t off) { (void)c; return pread(fd, b, n, (off_t)off); }
static ssize_t block_write(void *c, int fd, const void *b, size_t n, uint64_t off)
{
    (void)c;
    allocate_chunks(off, n);
    return pwrite(fd, b, n, (off_t)off);
}
#else
static int map_op(void *c, uintptr_t at, size_t n, unsigned prot, int fd, uint64_t off)
{
    (void)c;
    return mmap((void *)at, n, prot_of(prot), MAP_SHARED | MAP_FIXED, fd, (off_t)off) == (void *)at ? 0 : -1;
}
static int flush_op(void *c, uintptr_t at, size_t n) { (void)c; return msync((void *)at, n, MS_SYNC); }
static int unmap_op(void *c, uintptr_t at, size_t n) { (void)c; return munmap((void *)at, n); }
#endif
static int protect_op(void *c, uintptr_t at, size_t n, unsigned prot) { (void)c; return mprotect((void *)at, n, prot_of(prot)); }
static int reserve_op(void *c, uintptr_t at, size_t n)
{
    (void)c;
    return mmap((void *)at, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)at ? 0 : -1;
}

static void on_fault(int sig, siginfo_t *info, void *context)
{
    const uintptr_t at = (uintptr_t)info->si_addr;
    int status;
    (void)sig; (void)context;
    pthread_mutex_lock(&pager);
#ifdef PW_SECTION_V1
    status = pw_wine_section_fault(&cache, at, 3, 1, 0, &retry);
#else
    status = pw_wine_section_fault_run(&cache, at, 3, 1, 0, &retry, run_pages);
#endif
    pthread_mutex_unlock(&pager);
    if (status != 1) {
        static const char message[] = "section data test: fault not resolved\n";
        (void)!write(2, message, sizeof(message) - 1);
        _exit(2);
    }
}

static volatile uint64_t *word_at(int view, size_t word)
{
    return (volatile uint64_t *)(bases[view] + word * 8 - offsets[view]);
}

static unsigned failures;
static void *worker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    for (unsigned op = 0; op < OPS; op++) {
        seed = seed * 1103515245u + 12345u;
        const int view = (seed >> 8) % 3;
        const size_t first = offsets[view] / 8, last = first + view_pages[view] * PAGE / 8;
        /* each thread owns the words whose index is its own modulo THREADS: no data race, only page races */
        size_t word = first + ((size_t)(seed >> 3) * 7919u) % (last - first);
        word -= word % THREADS;
        word += (size_t)(uintptr_t)arg;
        if (word >= last) continue;
        if ((seed >> 4) & 1) {
            const uint64_t value = ((uint64_t)(uintptr_t)arg << 56) | ((uint64_t)op << 24) | word;
            *word_at(view, word) = value;
            pthread_mutex_lock(&model_lock);
            model[word] = value;
            pthread_mutex_unlock(&model_lock);
        } else {
            const uint64_t got = *word_at(view, word);
            pthread_mutex_lock(&model_lock);
            const uint64_t want = model[word];
            pthread_mutex_unlock(&model_lock);
            if (got != want && __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED) < 5)
                fprintf(stderr, "word %zu via view %d: %#llx, wrote %#llx\n", word, view,
                        (unsigned long long)got, (unsigned long long)want);
        }
    }
    return NULL;
}

static void round_trip(size_t run)
{
    const int fd = memfd_create("section", 0);
    assert(fd >= 0 && !ftruncate(fd, (off_t)FILE_PAGES * PAGE));
#ifdef PW_SECTION_DMEM
    {
        static const PwWineDsectionOps table_ops = { NULL, pool_allocate, pool_grant, pool_zero, pool_release, pool_alive, 0 };
        static const PwWineDmapOps client_ops = { NULL, dmap_identity, dmap_page, dmap_map };
        pool_fd = memfd_create("pool", 0);
        assert(pool_fd >= 0 && !ftruncate(pool_fd, (off_t)FILE_PAGES * PAGE + (4 << 20)));
        pool_next = 0;
        pw_wine_dsection_init(&table_ops);
        /* The previous round's section goes back once its process has ended, as the title's collector does it. */
        pw_wine_dsection_release(7, 42);
        (void)pw_wine_dsection_collect(NULL, NULL);
        assert(!pw_wine_dsection_register(-1, 7, 42, 256u << 20));   /* the views stay inside one section */
        pw_wine_dmap_init(&client_ops);
    }
#endif
#ifdef PW_SECTION_INCOHERENT
    backing_fd = fd;
    memset(allocated, 0, sizeof(allocated));
    static const PwWineBlocksOps block_ops = { NULL, block_identity, block_read, block_write };
    pw_wine_blocks_init(&block_ops);
#endif
    PwWineSectionOps ops = { NULL, PAGE, retain, closed, map_op, protect_op, flush_op, unmap_op, reserve_op, NULL, NULL };
    memset(&cache, 0, sizeof(cache));
    memset(model, 0, sizeof(model));
    failures = 0;
    run_pages = run;
    assert(!pw_wine_section_init(&cache, &ops, pages, CACHE));
#ifndef PW_SECTION_V1
    static uint32_t buckets[64];
    assert(!pw_wine_section_use_index(&cache, buckets, 64));
#endif
    for (int v = 0; v < 3; v++) {
        void *at = mmap(NULL, view_pages[v] * PAGE + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(at != MAP_FAILED);
        bases[v] = ((uintptr_t)at + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
        assert(!pw_wine_section_add(&cache, &views[v], bases[v], view_pages[v] * PAGE, fd, offsets[v]));
    }
    pthread_t threads[THREADS];
    for (uintptr_t t = 0; t < THREADS; t++) assert(!pthread_create(&threads[t], NULL, worker, (void *)t));
    for (int t = 0; t < THREADS; t++) pthread_join(threads[t], NULL);
    /* every word, through every view that holds it */
    for (int v = 0; v < 3; v++)
        for (size_t w = offsets[v] / 8; w < offsets[v] / 8 + view_pages[v] * PAGE / 8; w++)
            if (*word_at(v, w) != model[w] && failures++ < 10)
                fprintf(stderr, "final word %zu via view %d: %#llx, wrote %#llx\n", w, v,
                        (unsigned long long)*word_at(v, w), (unsigned long long)model[w]);
    printf("run %zu: faults=%llu evictions=%llu failures=%llu mismatches=%u\n", run, (unsigned long long)cache.faults,
           (unsigned long long)cache.evictions, (unsigned long long)cache.failures, failures);
    assert(!failures && !cache.failures && cache.evictions > 1000);
    for (int v = 0; v < 3; v++) assert(!pw_wine_section_remove(&cache, &views[v]));
    close(fd);
}

int main(void)
{
    struct sigaction action = { 0 };
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    round_trip(1);
#ifndef PW_SECTION_V1
    round_trip(8);
#endif
    puts("section cache data: every word reads back through every alias, across evictions, clustered faults and "
         "threads, passed");
    return 0;
}
