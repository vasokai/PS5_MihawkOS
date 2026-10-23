/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cc -std=gnu11 tests/test_pw_wine_dsection.c wine/ps5/pw_wine_dsection.c -lpthread -o /tmp/ds && /tmp/ds */
#include "../wine/ps5/pw_wine_dsection.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* A fake direct-memory pool: which ranges are allocated, zeroed, and which pids may map them. */
enum { MAX = 64 };
/* A dead pid's grant stays until the chunk is released, so a full table plus the process that replaces it needs a spare. */
static struct { int64_t at; uint64_t bytes; int zeroed; int pids[PW_WINE_DSECTION_PIDS + 4]; unsigned npids; int live; } ranges[MAX];
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t next_offset = 0x100000000;
static int refuse_allocate, refuse_grant, refuse_release, slow_allocate;
static unsigned allocations, releases, dead_grants;
static int dead[64];
static unsigned dead_count;
/* A memory helper that never answers: allocate for block_pid waits for a byte on block_pipe ('y' answers, anything
 * else fails as a timed-out request). */
static int block_pid, block_pipe[2], blocked;

static int index_of(int64_t at)
{
    for (int i = 0; i < MAX; i++) if (ranges[i].live && ranges[i].at == at) return i;
    return -1;
}
static int is_dead(int pid)
{
    for (unsigned i = 0; i < dead_count; i++) if (dead[i] == pid) return 1;
    return 0;
}
static void mark_dead(int pid)
{
    pthread_mutex_lock(&fake_lock);
    dead[dead_count++] = pid;
    pthread_mutex_unlock(&fake_lock);
}
static int fake_allocate(void *c, uint64_t bytes, int pid, int64_t *offset)
{
    (void)c;
    if (pid == block_pid) {
        char answer = 0;
        __atomic_store_n(&blocked, 1, __ATOMIC_RELEASE);
        assert(read(block_pipe[0], &answer, 1) == 1);
        if (answer != 'y') { errno = ETIMEDOUT; return -1; }
    }
    if (slow_allocate) usleep(20000);
    pthread_mutex_lock(&fake_lock);
    int status = -1;
    for (int i = 0; !refuse_allocate && i < MAX; i++) if (!ranges[i].live) {
        ranges[i] = (typeof(ranges[0])){ next_offset, bytes, 0, { pid }, 1, 1 };
        *offset = next_offset;
        next_offset += bytes;
        allocations++;
        status = 0;
        break;
    }
    pthread_mutex_unlock(&fake_lock);
    if (status) errno = ENOMEM;
    return status;
}
static int fake_grant(void *c, int64_t at, uint64_t bytes, int pid)
{
    (void)c;
    int status = 0;
    pthread_mutex_lock(&fake_lock);
    const int i = index_of(at);
    assert(i >= 0 && ranges[i].bytes == bytes);
    if (refuse_grant) { errno = EACCES; status = -1; }
    else if (is_dead(pid)) { dead_grants++; errno = ESRCH; status = -1; }
    else { assert(ranges[i].npids < PW_WINE_DSECTION_PIDS + 4); ranges[i].pids[ranges[i].npids++] = pid; }
    pthread_mutex_unlock(&fake_lock);
    return status;
}
static int fake_zero(void *c, int64_t at, uint64_t bytes)
{
    (void)c;
    pthread_mutex_lock(&fake_lock);
    const int i = index_of(at);
    assert(i >= 0 && ranges[i].bytes == bytes);
    ranges[i].zeroed = 1;
    pthread_mutex_unlock(&fake_lock);
    return 0;
}
static int fake_release(void *c, int64_t at, uint64_t bytes)
{
    (void)c;
    pthread_mutex_lock(&fake_lock);
    const int i = index_of(at);
    assert(i >= 0 && ranges[i].bytes == bytes);
    const int refused = refuse_release;
    if (!refused) { ranges[i].live = 0; releases++; }
    pthread_mutex_unlock(&fake_lock);
    if (refused) errno = ETIMEDOUT;
    return refused ? -1 : 0;
}
static int fake_alive(void *c, int pid)
{
    (void)c;
    pthread_mutex_lock(&fake_lock);
    const int alive = !is_dead(pid);
    pthread_mutex_unlock(&fake_lock);
    return alive;
}
static int granted(int64_t at, int pid)
{
    const int i = index_of(at);
    if (i < 0) return 0;
    for (unsigned p = 0; p < ranges[i].npids; p++) if (ranges[i].pids[p] == pid) return 1;
    return 0;
}
static uint64_t ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static unsigned open_descriptors(void)
{
    DIR *dir = opendir("/proc/self/fd");
    unsigned count = 0;
    assert(dir);
    while (readdir(dir)) count++;
    closedir(dir);
    return count;
}
struct pager { uint64_t dev, ino, offset; int pid, status, error; int64_t dmem; };
static void *page_thread(void *opaque)
{
    struct pager *p = opaque;
    uint64_t off, bytes;
    p->status = pw_wine_dsection_page(p->dev, p->ino, p->offset, p->pid, &p->dmem, &off, &bytes);
    p->error = errno;
    return NULL;
}

int main(void)
{
    const PwWineDsectionOps ops = { NULL, fake_allocate, fake_grant, fake_zero, fake_release, fake_alive, 200 };
    const uint64_t chunk = PW_WINE_DSECTION_CHUNK, size = (256u << 20) + 3 * 16384;   /* the last chunk is short */
    int64_t dmem, dmem2;
    uint64_t off, bytes, given, lost, sections, chunks, grants, failures;

    alarm(60);   /* a request that blocks for good fails the test instead of hanging it */
    assert(!pipe(block_pipe));
    pw_wine_dsection_init(&ops);
    /* One host page (user shared data) and the session mapping stay files: the wineserver writes those fds itself. */
    assert(pw_wine_dsection_register(-1, 1, 9, 16u << 10) == -1 && errno == EINVAL);
    assert(pw_wine_dsection_register(-1, 1, 9, PW_WINE_DSECTION_SESSION_BYTES) == -1 && errno == EINVAL);
    assert(!pw_wine_dsection_register(-1, 1, 10, 64u << 20));   /* a client section under the old 256 MiB floor */
    pw_wine_dsection_release(1, 10);
    assert(!pw_wine_dsection_collect(NULL, NULL) && !pw_wine_dsection_known(1, 10));
    assert(!pw_wine_dsection_register(-1, 1, 11, size));
    assert(pw_wine_dsection_register(-1, 1, 11, size) == -1 && errno == EEXIST);       /* once */
    assert(pw_wine_dsection_known(1, 11) && !pw_wine_dsection_known(1, 12));
    assert(pw_wine_dsection_page(1, 12, 0, 100, &dmem, &off, &bytes) == -1 && errno == ENOENT);
    assert(pw_wine_dsection_page(1, 11, size, 100, &dmem, &off, &bytes) == -1 && errno == ERANGE);

    /* First touch allocates one zeroed chunk for pid 100; the same chunk again comes back without a new allocation. */
    assert(!pw_wine_dsection_page(1, 11, chunk + 5, 100, &dmem, &off, &bytes));
    assert(off == chunk && bytes == chunk && allocations == 1 && ranges[index_of(dmem)].zeroed && granted(dmem, 100));
    assert(!pw_wine_dsection_page(1, 11, chunk + chunk - 1, 100, &dmem2, &off, &bytes) && dmem2 == dmem && allocations == 1);

    /* A second process (an alias in another process) is granted the existing chunk, and gets the same memory. */
    assert(!pw_wine_dsection_page(1, 11, chunk, 200, &dmem2, &off, &bytes) && dmem2 == dmem && granted(dmem, 200));
    /* A chunk made later for pid 200 is granted to 100 as well. */
    assert(!pw_wine_dsection_page(1, 11, 0, 200, &dmem2, &off, &bytes) && off == 0 && granted(dmem2, 100) && granted(dmem2, 200));

    /* The short last chunk is rounded to whole pages. */
    assert(!pw_wine_dsection_page(1, 11, size - 1, 100, &dmem2, &off, &bytes) && off == 128 * chunk && bytes == 3 * 16384);

    /* A refused allocation or grant gives an error and leaves nothing behind. */
    const unsigned before = allocations;
    refuse_allocate = 1;
    assert(pw_wine_dsection_page(1, 11, 5 * chunk, 100, &dmem2, &off, &bytes) == -1 && errno == ENOMEM);
    refuse_allocate = 0;
    refuse_grant = 1;
    assert(pw_wine_dsection_page(1, 11, 6 * chunk, 100, &dmem2, &off, &bytes) == -1 && errno == EACCES);
    refuse_grant = 0;
    assert(allocations == before + 1 && releases == 1);                /* the chunk whose grant failed went back */
    pw_wine_dsection_stats(&sections, &chunks, &grants, &failures);
    assert(sections == 1 && chunks == 3 && failures == 2);

    /* The server's release only marks the section: a process that still maps a view gets the same memory, and nothing
     * goes back while one admitted process lives. */
    pw_wine_dsection_release(1, 11);
    assert(pw_wine_dsection_known(1, 11));
    assert(!pw_wine_dsection_page(1, 11, chunk, 100, &dmem2, &off, &bytes) && dmem2 == dmem);
    assert(pw_wine_dsection_collect(&given, &lost) == 1 && !given && !lost && releases == 1);
    mark_dead(100);
    mark_dead(200);
    assert(pw_wine_dsection_collect(&given, &lost) == 0 && given == 3 && !lost);
    pw_wine_dsection_stats(&sections, &chunks, NULL, NULL);
    assert(!sections && !chunks && releases == 4 && !pw_wine_dsection_known(1, 11));
    for (int i = 0; i < MAX; i++) assert(!ranges[i].live);

    /* A memory helper that never answers holds up only its own section: the table, other sections and the collector
     * go on, a second request for the busy section fails after wait_ms, and the late failure publishes nothing. */
    block_pid = 300;
    assert(!pw_wine_dsection_register(-1, 1, 20, size));
    struct pager stuck = { 1, 20, 0, 300, 0, 0, -1 };
    pthread_t stuck_thread;
    assert(!pthread_create(&stuck_thread, NULL, page_thread, &stuck));
    while (!__atomic_load_n(&blocked, __ATOMIC_ACQUIRE)) usleep(1000);
    uint64_t started = ms();
    assert(pw_wine_dsection_known(1, 20));
    pw_wine_dsection_stats(&sections, NULL, NULL, NULL);
    assert(!pw_wine_dsection_register(-1, 1, 21, size));
    assert(!pw_wine_dsection_page(1, 21, 0, 301, &dmem, &off, &bytes));
    pw_wine_dsection_release(1, 20);
    const unsigned releases_before = releases;
    assert(pw_wine_dsection_collect(NULL, NULL) == 1 && releases == releases_before);   /* the busy section is left */
    assert(ms() - started < 1000);
    started = ms();
    assert(pw_wine_dsection_page(1, 20, chunk, 302, &dmem, &off, &bytes) == -1 && errno == ETIMEDOUT);
    const uint64_t waited = ms() - started;
    assert(waited >= 190 && waited < 1000);
    const unsigned allocated = allocations;
    assert(write(block_pipe[1], "n", 1) == 1);
    pthread_join(stuck_thread, NULL);
    assert(stuck.status == -1 && stuck.error == ETIMEDOUT && allocations == allocated);
    block_pid = 0;
    assert(!pw_wine_dsection_page(1, 20, 0, 300, &dmem, &off, &bytes) && allocations == allocated + 1);
    mark_dead(300);
    mark_dead(301);
    assert(pw_wine_dsection_collect(&given, &lost) == 0 && given == 1 && !pw_wine_dsection_known(1, 20));

    /* Eight requests for one new chunk from two processes: one allocation, the same memory for all, both granted. */
    slow_allocate = 1;
    assert(!pw_wine_dsection_register(-1, 1, 30, size));
    struct pager many[8];
    pthread_t threads[8];
    const unsigned made = allocations;
    for (int i = 0; i < 8; i++) {
        many[i] = (struct pager){ 1, 30, 3 * chunk + (uint64_t)i, 400 + (i & 1), 0, 0, -1 };
        assert(!pthread_create(&threads[i], NULL, page_thread, &many[i]));
    }
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
    for (int i = 0; i < 8; i++) assert(!many[i].status && many[i].dmem == many[0].dmem);
    assert(allocations == made + 1 && granted(many[0].dmem, 400) && granted(many[0].dmem, 401));
    slow_allocate = 0;
    mark_dead(400);
    mark_dead(401);
    pw_wine_dsection_release(1, 30);
    assert(pw_wine_dsection_collect(&given, &lost) == 0 && given == 1);

    /* The backing descriptor is held from register until the memory goes back, so (dev, ino) names nothing else. */
    const int backing = open("/dev/null", O_RDONLY);
    assert(backing >= 0);
    const unsigned open_before = open_descriptors();
    assert(!pw_wine_dsection_register(backing, 1, 40, size) && open_descriptors() == open_before + 1);
    assert(!pw_wine_dsection_page(1, 40, 0, 500, &dmem, &off, &bytes));
    pw_wine_dsection_release(1, 40);
    assert(pw_wine_dsection_register(backing, 1, 40, size) == -1 && errno == EEXIST);
    assert(pw_wine_dsection_collect(&given, &lost) == 1 && !given && open_descriptors() == open_before + 1);
    mark_dead(500);
    assert(pw_wine_dsection_collect(&given, &lost) == 0 && given == 1 && open_descriptors() == open_before);
    assert(!pw_wine_dsection_known(1, 40));
    close(backing);

    /* A full set of processes takes a new one once an admitted process has ended and the collector forgot it. */
    assert(!pw_wine_dsection_register(-1, 1, 50, size));
    for (int p = 0; p < PW_WINE_DSECTION_PIDS; p++) assert(!pw_wine_dsection_page(1, 50, 0, 600 + p, &dmem, &off, &bytes));
    assert(pw_wine_dsection_page(1, 50, 0, 600 + PW_WINE_DSECTION_PIDS, &dmem, &off, &bytes) == -1 && errno == EMFILE);
    mark_dead(600);
    pw_wine_dsection_collect(NULL, NULL);
    assert(!pw_wine_dsection_page(1, 50, 0, 600 + PW_WINE_DSECTION_PIDS, &dmem, &off, &bytes));

    /* A process that ended and is not yet forgotten does not fail a live one's new chunk: it is dropped instead. */
    assert(!pw_wine_dsection_register(-1, 1, 60, size));
    assert(!pw_wine_dsection_page(1, 60, 0, 700, &dmem, &off, &bytes) && !pw_wine_dsection_page(1, 60, 0, 701, &dmem, &off, &bytes));
    mark_dead(701);
    const unsigned made_before = allocations, refused = dead_grants;
    assert(!pw_wine_dsection_page(1, 60, chunk, 700, &dmem, &off, &bytes) && allocations == made_before + 1);
    assert(dead_grants == refused + 1);
    assert(!pw_wine_dsection_page(1, 60, 2 * chunk, 700, &dmem, &off, &bytes) && dead_grants == refused + 1);

    /* Chunks the memory helper does not take back are counted lost and never asked for again. */
    mark_dead(700);
    pw_wine_dsection_release(1, 60);
    refuse_release = 1;
    assert(pw_wine_dsection_collect(&given, &lost) == 0 && !given && lost == 3 && !pw_wine_dsection_known(1, 60));
    refuse_release = 0;
    assert(pw_wine_dsection_collect(&given, &lost) == 0 && !given && !lost);
    puts("direct-memory sections: zeroed chunks on first touch, every process granted every chunk, refusals leave "
         "nothing, no lock held across the memory helper, memory back only after its processes end, passed");
    return 0;
}
