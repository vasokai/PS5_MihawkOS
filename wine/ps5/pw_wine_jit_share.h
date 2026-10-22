/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_JIT_SHARE_H
#define PW_WINE_JIT_SHARE_H
#include <stddef.h>
#include <stdint.h>

/* One JIT object for identical code, so a second helper can map the first helper's copy instead of creating another.
 *
 * The kernel does not charge that object once. K14 (exec-share-2026-10-18-01) mapped one 16 MiB JIT object into a
 * second payload: sceKernelGetResidentFmemCount rose by the full 16 MiB in the taker and stayed at 16 MiB in the
 * maker, and two mappings in one process counted 32 MiB. K18 (2026-10-21, launcher, builds ae34b5fe vs 09acfc9f,
 * records off the tree) shared 28,054 clusters, 448,864 KiB of mappings: helper census 1532 to 1568 MiB, SceShellCore
 * RSS 1524 to 1560, kernel 431 to 464 MiB, and 4 memory-guard ends against 0 without sharing. Page tables fell.
 * Sharing stays off unless a caller enables it. Turning it on does not lower the system-pool figure that vmwait
 * follows, and it is not enabled here.
 *
 * The process itself already takes the application pool. A payload is budget 0, so sceKernelAllocateMainDirectMemory
 * would be the system partition; proton_payload_child.c adopts sceKernelAllocateDirectMemoryForApp before ntdll
 * starts, unless WINE_PS5_PAYLOAD_DMEM=0. Execute is dropped on that memory (exec-kernel-2026-10-14), so the JIT
 * object cannot move with it. Heap, VirtualAlloc, file views and thread stacks are not this module.
 *
 * The table is the title's. A helper never allocates from it: it asks (pw_wine_jit_share_lookup) and, on a miss,
 * publishes the private object it just made (pw_wine_jit_share_published). Both do nothing until
 * pw_wine_jit_share_enable(1) and the callbacks are set. No console run has enabled them. */

enum { PW_WINE_JIT_SHARE_SLOTS = 1024 };

/* FNV-1a of [bytes, +size). 0 for an empty range. */
uint64_t pw_wine_jit_share_hash(const void *bytes, size_t size);

/* Remember fd for this code. The caller keeps fd; the table stores the number and does not close it.
 * 0 stored, 1 already stored (*held is that fd; fd is not stored again), -1 (errno EINVAL or ENOSPC). */
int pw_wine_jit_share_add(uint64_t hash, uint32_t bytes, int fd, int *held);
/* 1 with *fd, or 0. */
int pw_wine_jit_share_find(uint64_t hash, uint32_t bytes, int *fd);
void pw_wine_jit_share_reset(void);

/* Off at start. enable(1) arms lookup and published; the callbacks may be NULL, and then both stay misses. */
void pw_wine_jit_share_enable(int on);
void pw_wine_jit_share_set_ask(int (*ask)(const void *bytes, size_t size, int *fd));
void pw_wine_jit_share_set_publish(void (*publish)(const void *bytes, size_t size, int fd));

/* 1 and *fd set: map this object over the code (the caller does not close fd). 0: make a private object. */
int pw_wine_jit_share_lookup(const void *bytes, size_t size, int *fd);
/* The private object now holds these bytes. No-op while sharing is off or no publisher is set. */
void pw_wine_jit_share_published(const void *bytes, size_t size, int fd);

#endif
