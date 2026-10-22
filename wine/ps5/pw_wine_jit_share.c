/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_jit_share.h"
#include <errno.h>
#include <string.h>

struct Slot {
    uint64_t hash;
    uint32_t bytes;
    int fd;
    int used;
};

static struct Slot slots[PW_WINE_JIT_SHARE_SLOTS];
static unsigned used_slots;
static int share_on;
static int (*share_ask)(const void *, size_t, int *);
static void (*share_publish)(const void *, size_t, int);

uint64_t pw_wine_jit_share_hash(const void *bytes, size_t size)
{
    const unsigned char *p = bytes;
    uint64_t hash = 14695981039346656037ull;
    if (!bytes || !size) return 0;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

static unsigned index_of(uint64_t hash)
{
    return (unsigned)(hash % PW_WINE_JIT_SHARE_SLOTS);
}

int pw_wine_jit_share_find(uint64_t hash, uint32_t bytes, int *fd)
{
    if (!hash || !bytes || !fd) return 0;
    for (unsigned n = 0, i = index_of(hash); n < PW_WINE_JIT_SHARE_SLOTS; n++, i = (i + 1) % PW_WINE_JIT_SHARE_SLOTS) {
        if (!slots[i].used) return 0;
        if (slots[i].hash == hash && slots[i].bytes == bytes) {
            *fd = slots[i].fd;
            return 1;
        }
    }
    return 0;
}

int pw_wine_jit_share_add(uint64_t hash, uint32_t bytes, int fd, int *held)
{
    int existing = -1;
    if (!hash || !bytes || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (pw_wine_jit_share_find(hash, bytes, &existing)) {
        if (held) *held = existing;
        return 1;
    }
    if (used_slots >= PW_WINE_JIT_SHARE_SLOTS) {
        errno = ENOSPC;
        return -1;
    }
    for (unsigned n = 0, i = index_of(hash); n < PW_WINE_JIT_SHARE_SLOTS; n++, i = (i + 1) % PW_WINE_JIT_SHARE_SLOTS) {
        if (slots[i].used) continue;
        slots[i] = (struct Slot){ hash, bytes, fd, 1 };
        used_slots++;
        if (held) *held = fd;
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

void pw_wine_jit_share_reset(void)
{
    memset(slots, 0, sizeof(slots));
    used_slots = 0;
}

void pw_wine_jit_share_enable(int on)
{
    __atomic_store_n(&share_on, on ? 1 : 0, __ATOMIC_RELEASE);
}

void pw_wine_jit_share_set_ask(int (*ask)(const void *, size_t, int *))
{
    share_ask = ask;
}

void pw_wine_jit_share_set_publish(void (*publish)(const void *, size_t, int))
{
    share_publish = publish;
}

int pw_wine_jit_share_lookup(const void *bytes, size_t size, int *fd)
{
    if (!__atomic_load_n(&share_on, __ATOMIC_ACQUIRE) || !share_ask || !bytes || !size || !fd) return 0;
    return share_ask(bytes, size, fd) == 1 ? 1 : 0;
}

void pw_wine_jit_share_published(const void *bytes, size_t size, int fd)
{
    if (!__atomic_load_n(&share_on, __ATOMIC_ACQUIRE) || !share_publish || !bytes || !size || fd < 0) return;
    share_publish(bytes, size, fd);
}
