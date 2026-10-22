/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_jit_share.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

/* The title's table: one fd for one run of identical bytes, a second size is another object, and a full table
 * refuses. Sharing stays off until a caller arms it, so lookup and published do nothing on their own. */
static int asked, published, armed_fd;

static int ask(const void *bytes, size_t size, int *fd)
{
    uint64_t hash = pw_wine_jit_share_hash(bytes, size);
    asked++;
    return pw_wine_jit_share_find(hash, (uint32_t)size, fd);
}

static void publish(const void *bytes, size_t size, int fd)
{
    int held = -1;
    published++;
    assert(!pw_wine_jit_share_add(pw_wine_jit_share_hash(bytes, size), (uint32_t)size, fd, &held));
    armed_fd = held;
}

int main(void)
{
    const char code[] = { 0x5a, 0x11, 0x22, 0x33 };
    char other[sizeof code];
    int held = -1, found = -1;
    uint64_t hash, again;

    memcpy(other, code, sizeof code);
    hash = pw_wine_jit_share_hash(code, sizeof code);
    again = pw_wine_jit_share_hash(code, sizeof code);
    assert(hash && hash == again);
    other[3] ^= 1;
    assert(pw_wine_jit_share_hash(other, sizeof other) != hash);
    assert(!pw_wine_jit_share_hash(code, 0) && !pw_wine_jit_share_hash(NULL, sizeof code));

    assert(!pw_wine_jit_share_add(hash, sizeof code, 7, &held) && held == 7);
    assert(pw_wine_jit_share_add(hash, sizeof code, 9, &held) == 1 && held == 7);
    assert(pw_wine_jit_share_find(hash, sizeof code, &found) && found == 7);
    assert(!pw_wine_jit_share_find(hash, sizeof code + 1, &found));
    assert(!pw_wine_jit_share_add(hash, 64, 11, &held) && held == 11);
    assert(pw_wine_jit_share_find(hash, 64, &found) && found == 11);

    pw_wine_jit_share_reset();
    assert(!pw_wine_jit_share_find(hash, sizeof code, &found));
    for (unsigned i = 0; i < PW_WINE_JIT_SHARE_SLOTS; i++)
        assert(!pw_wine_jit_share_add((uint64_t)i + 1, 16, (int)i, &held));
    errno = 0;
    assert(pw_wine_jit_share_add(0xabc, 16, 3, &held) == -1 && errno == ENOSPC);
    assert(pw_wine_jit_share_add(0, 16, 3, &held) == -1 && errno == EINVAL);

    /* Off: a publisher and an asker are ignored. On: the second publish of the same bytes keeps the first fd. */
    pw_wine_jit_share_reset();
    pw_wine_jit_share_set_ask(ask);
    pw_wine_jit_share_set_publish(publish);
    assert(!pw_wine_jit_share_lookup(code, sizeof code, &found) && !asked);
    pw_wine_jit_share_published(code, sizeof code, 4);
    assert(!published);
    pw_wine_jit_share_enable(1);
    assert(!pw_wine_jit_share_lookup(code, sizeof code, &found) && asked == 1);
    pw_wine_jit_share_published(code, sizeof code, 4);
    assert(published == 1 && armed_fd == 4);
    assert(pw_wine_jit_share_lookup(code, sizeof code, &found) == 1 && found == 4 && asked == 2);
    pw_wine_jit_share_enable(0);
    assert(!pw_wine_jit_share_lookup(code, sizeof code, &found) && asked == 2);

    return 0;
}
