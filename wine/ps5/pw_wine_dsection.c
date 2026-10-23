/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_dsection.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Section {
    uint64_t dev, ino, size;
    int64_t *chunks;              /* direct-memory offset of each chunk, -1 before its first page is asked for */
    uint32_t chunk_count;
    int pids[PW_WINE_DSECTION_PIDS];
    unsigned pid_count;
    int used;
    int busy;                     /* a request is asking the memory helper for it, without the lock */
    int released;                 /* the server destroyed it */
    int identity;                 /* the backing file, held until the chunks go back, or -1 */
} Section;

/* The collector copies the taken sections on its 256 KiB stack (pw_wine_process.c). */
_Static_assert(sizeof(Section) * PW_WINE_DSECTION_SECTIONS <= (96u << 10),
               "the section table copy no longer fits the collector stack");

static pthread_mutex_t dsection_lock = PTHREAD_MUTEX_INITIALIZER;
static PwWineDsectionOps dsection_ops;
static Section sections[PW_WINE_DSECTION_SECTIONS];
static uint64_t live_sections, live_chunks, grants, failures;

void pw_wine_dsection_init(const PwWineDsectionOps *ops)
{
    pthread_mutex_lock(&dsection_lock);
    dsection_ops = ops ? *ops : (PwWineDsectionOps){ 0 };
    pthread_mutex_unlock(&dsection_lock);
}

static Section *find(uint64_t dev, uint64_t ino)
{
    for (unsigned i = 0; i < PW_WINE_DSECTION_SECTIONS; i++)
        if (sections[i].used && sections[i].dev == dev && sections[i].ino == ino) return &sections[i];
    return NULL;
}

static uint64_t chunk_bytes_of(const Section *s, uint32_t c)
{
    const uint64_t start = (uint64_t)c * PW_WINE_DSECTION_CHUNK, left = s->size - start;
    return left < PW_WINE_DSECTION_CHUNK ? (left + 16383) & ~(uint64_t)16383 : PW_WINE_DSECTION_CHUNK;
}

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

int pw_wine_dsection_register(int fd, uint64_t dev, uint64_t ino, uint64_t size)
{
    int status = -1, error = ENOSPC;

    pthread_mutex_lock(&dsection_lock);
    if (size < PW_WINE_DSECTION_MIN_BYTES || size == (uint64_t)PW_WINE_DSECTION_SESSION_BYTES ||
        !dsection_ops.allocate) { error = EINVAL; goto done; }
    if (find(dev, ino)) { error = EEXIST; goto done; }
    for (unsigned i = 0; i < PW_WINE_DSECTION_SECTIONS; i++) {
        if (sections[i].used) continue;
        const uint64_t count = (size + PW_WINE_DSECTION_CHUNK - 1) / PW_WINE_DSECTION_CHUNK;
        int64_t *chunks = count <= UINT32_MAX ? malloc(count * sizeof(*chunks)) : NULL;
        if (!chunks) { error = ENOMEM; break; }
        const int identity = fd >= 0 ? dup(fd) : -1;
        if (fd >= 0 && identity < 0) { error = errno; free(chunks); break; }
        for (uint64_t c = 0; c < count; c++) chunks[c] = -1;
        sections[i] = (Section){ .dev = dev, .ino = ino, .size = size, .chunks = chunks, .chunk_count = (uint32_t)count,
                                 .used = 1, .identity = identity };
        live_sections++;
        status = 0;
        break;
    }
done:
    pthread_mutex_unlock(&dsection_lock);
    if (status) errno = error;
    return status;
}

int pw_wine_dsection_known(uint64_t dev, uint64_t ino)
{
    pthread_mutex_lock(&dsection_lock);
    const int known = find(dev, ino) != NULL;
    pthread_mutex_unlock(&dsection_lock);
    return known;
}

static int admitted(const Section *s, int pid)
{
    for (unsigned i = 0; i < s->pid_count; i++) if (s->pids[i] == pid) return 1;
    return 0;
}

/* The section is busy (its chunks and pids change only under the lock, by the busy holder): pid is granted every
 * chunk that exists. 0, or the grant's error. */
static int grant_all(const PwWineDsectionOps *ops, const Section *s, int pid, uint64_t *made)
{
    for (uint32_t c = 0; c < s->chunk_count; c++) {
        if (s->chunks[c] < 0) continue;
        if (ops->grant(ops->context, s->chunks[c], chunk_bytes_of(s, c), pid)) return errno ? errno : EACCES;
        ++*made;
    }
    return 0;
}

/* The section is busy: chunk c is allocated for pid, zeroed, and granted to every other admitted process. One whose
 * grant fails and that has ended (not yet forgotten by the collector) goes into ended[] instead of failing the live
 * requester. 0 with *at, or the error (the chunk then goes back). */
static int make_chunk(const PwWineDsectionOps *ops, const Section *s, uint32_t c, int pid, int64_t *at, int *ended,
                      unsigned *ended_count, uint64_t *made)
{
    const uint64_t bytes = chunk_bytes_of(s, c);
    int error = 0;

    if (ops->allocate(ops->context, bytes, pid, at)) return errno ? errno : ENOMEM;
    if (ops->zero(ops->context, *at, bytes)) error = errno ? errno : EIO;
    for (unsigned i = 0; !error && i < s->pid_count; i++) {
        if (s->pids[i] == pid) continue;
        if (!ops->grant(ops->context, *at, bytes, s->pids[i])) { ++*made; continue; }
        error = errno ? errno : EACCES;
        if (ops->alive && !ops->alive(ops->context, s->pids[i])) { ended[(*ended_count)++] = s->pids[i]; error = 0; }
    }
    if (error) (void)ops->release(ops->context, *at, bytes);
    return error;
}

static void forget(Section *s, const int *ended, unsigned count)
{
    for (unsigned e = 0; e < count; e++)
        for (unsigned i = 0; i < s->pid_count; i++)
            if (s->pids[i] == ended[e]) { s->pids[i] = s->pids[--s->pid_count]; break; }
}

int pw_wine_dsection_page(uint64_t dev, uint64_t ino, uint64_t offset, int pid, int64_t *dmem,
                          uint64_t *chunk_offset, uint64_t *chunk_bytes)
{
    uint64_t deadline = 0;
    int error = 0;

    pthread_mutex_lock(&dsection_lock);
    const PwWineDsectionOps ops = dsection_ops;
    for (;;) {
        Section *s = find(dev, ino);
        if (!s) { error = ENOENT; break; }
        if (offset >= s->size) { error = ERANGE; break; }
        const uint32_t c = (uint32_t)(offset / PW_WINE_DSECTION_CHUNK);
        const int in = admitted(s, pid);
        if (in && s->chunks[c] >= 0) {
            *dmem = s->chunks[c];
            *chunk_offset = (uint64_t)c * PW_WINE_DSECTION_CHUNK;
            *chunk_bytes = chunk_bytes_of(s, c);
            pthread_mutex_unlock(&dsection_lock);
            return 0;
        }
        if (s->busy) {
            const uint64_t now = now_ms();
            if (!deadline) deadline = now + (ops.wait_ms ? ops.wait_ms : PW_WINE_DSECTION_WAIT_MS);
            if (now >= deadline) { error = ETIMEDOUT; failures++; break; }
            /* shortcut: polls each millisecond; a condition variable needs a monotonic timed wait not yet proven on
             * the console. */
            pthread_mutex_unlock(&dsection_lock);
            usleep(1000);
            pthread_mutex_lock(&dsection_lock);
            continue;
        }
        if (!in && s->pid_count == PW_WINE_DSECTION_PIDS) { error = EMFILE; failures++; break; }
        s->busy = 1;
        pthread_mutex_unlock(&dsection_lock);
        int ended[PW_WINE_DSECTION_PIDS];
        unsigned ended_count = 0;
        uint64_t made = 0;
        int64_t at = -1;
        error = in ? 0 : grant_all(&ops, s, pid, &made);
        if (!error && s->chunks[c] < 0) error = make_chunk(&ops, s, c, pid, &at, ended, &ended_count, &made);
        pthread_mutex_lock(&dsection_lock);
        grants += made;
        forget(s, ended, ended_count);
        if (!error && !in) s->pids[s->pid_count++] = pid;
        if (!error && at >= 0) { s->chunks[c] = at; live_chunks++; }
        s->busy = 0;
        if (error) { failures++; break; }
    }
    pthread_mutex_unlock(&dsection_lock);
    errno = error;
    return -1;
}

void pw_wine_dsection_release(uint64_t dev, uint64_t ino)
{
    pthread_mutex_lock(&dsection_lock);
    Section *s = find(dev, ino);
    if (s) s->released = 1;
    pthread_mutex_unlock(&dsection_lock);
}

unsigned pw_wine_dsection_collect(uint64_t *given, uint64_t *lost)
{
    Section taken[PW_WINE_DSECTION_SECTIONS];
    unsigned count = 0, waiting = 0;
    uint64_t back = 0, kept = 0;

    pthread_mutex_lock(&dsection_lock);
    const PwWineDsectionOps ops = dsection_ops;
    for (unsigned i = 0; i < PW_WINE_DSECTION_SECTIONS; i++) {
        Section *s = &sections[i];
        if (!s->used) continue;
        if (s->busy) { waiting += s->released; continue; }
        for (unsigned p = 0; p < s->pid_count;)
            if (ops.alive && !ops.alive(ops.context, s->pids[p])) s->pids[p] = s->pids[--s->pid_count];
            else p++;
        if (!s->released) continue;
        if (s->pid_count) { waiting++; continue; }
        taken[count++] = *s;
        for (uint32_t c = 0; c < s->chunk_count; c++) live_chunks -= s->chunks[c] >= 0;
        live_sections--;
        memset(s, 0, sizeof(*s));
    }
    pthread_mutex_unlock(&dsection_lock);
    for (unsigned i = 0; i < count; i++) {
        for (uint32_t c = 0; c < taken[i].chunk_count; c++) {
            if (taken[i].chunks[c] < 0) continue;
            if (ops.release(ops.context, taken[i].chunks[c], chunk_bytes_of(&taken[i], c))) kept++;
            else back++;
        }
        free(taken[i].chunks);
        if (taken[i].identity >= 0) close(taken[i].identity);
    }
    if (given) *given = back;
    if (lost) *lost = kept;
    return waiting;
}

void pw_wine_dsection_stats(uint64_t *s, uint64_t *c, uint64_t *g, uint64_t *f)
{
    pthread_mutex_lock(&dsection_lock);
    if (s) *s = live_sections;
    if (c) *c = live_chunks;
    if (g) *g = grants;
    if (f) *f = failures;
    pthread_mutex_unlock(&dsection_lock);
}
