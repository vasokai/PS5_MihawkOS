/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_fmem_census.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void encode(uint8_t raw[PW_VM_RECORD], uint64_t start, uint64_t end, int32_t prot, uint8_t flags, const char *name)
{
    memset(raw, 0, PW_VM_RECORD);
    memcpy(raw, &start, 8);
    memcpy(raw + 8, &end, 8);
    memcpy(raw + 24, &prot, 4);
    raw[32] = flags;
    memcpy(raw + 33, name, strlen(name) < PW_VM_NAME ? strlen(name) : PW_VM_NAME);
}

typedef struct Map { uint64_t start, end; int32_t prot; uint8_t flags; const char *name; uint64_t resident; } Map;
static const Map *maps;
static unsigned map_count;

static int query(void *context, uintptr_t address, uint8_t raw[PW_VM_RECORD])
{
    unsigned i;
    (void)context;
    for (i = 0; i < map_count; i++)
        if (maps[i].end > address) {
            encode(raw, maps[i].start, maps[i].end, maps[i].prot, maps[i].flags, maps[i].name);
            return 0;
        }
    return 1;
}

static uint64_t resident(void *context, uintptr_t start, uintptr_t end)
{
    unsigned i;
    (void)context;
    for (i = 0; i < map_count; i++)
        if (maps[i].start == start && maps[i].end == end) return maps[i].resident;
    return 0;
}

int main(void)
{
    /* Starts a gibibyte apart so the ranges do not overlap: the walk takes the next mapping whose end is past
     * the address it has reached. */
    const Map table[] = {
        {0x100000000ull, 0x100000000ull + (62u << 20), 5, PW_VM_FLEXIBLE, "libcef.dll", 62u << 20},
        {0x200000000ull, 0x200000000ull + (40u << 20), 3, PW_VM_FLEXIBLE, "libc heap", 40u << 20},
        {0x300000000ull, 0x300000000ull + (8u << 20), 3, PW_VM_FLEXIBLE | PW_VM_STACK, "stack", 1u << 20},
        {0x400000000ull, 0x400000000ull + (31u << 20), 1, PW_VM_FLEXIBLE, "/data/ps5proton/ntdll.prx.elf", 4u << 20},
        {0x500000000ull, 0x500000000ull + (600u << 20), 3, PW_VM_DIRECT, "wine", 600u << 20},
        {0x600000000ull, 0x600000000ull + (16u << 10), 0, PW_VM_FLEXIBLE, "reserve", 0},
    };
    PwVmCensus census;
    char line[1024];
    PwFmemSample samples[6];
    PwFmemRollup rollup;
    maps = table;
    map_count = 6;
    assert(pw_fmem_class(PW_VM_FLEXIBLE, 5, "libcef.dll") == PW_FMEM_JIT);
    assert(pw_fmem_class(PW_VM_FLEXIBLE, 3, "libc heap") == PW_FMEM_DATA);
    assert(pw_fmem_class(PW_VM_FLEXIBLE | PW_VM_STACK, 3, "stack") == PW_FMEM_STACK);
    assert(pw_fmem_class(PW_VM_FLEXIBLE, 1, "/data/ntdll.prx.elf") == PW_FMEM_FILE);
    assert(pw_fmem_class(PW_VM_DIRECT, 3, "wine") == PW_FMEM_DIRECT);
    assert(pw_fmem_class(PW_VM_FLEXIBLE, 0, "reserve") == PW_FMEM_OTHER);

    pw_vm_census_walk(&census, query, resident, NULL);
    pw_fmem_format(&census, line, sizeof(line));
    assert(strstr(line, "jit=63488"));
    assert(strstr(line, "data=40960"));
    assert(strstr(line, "file=4096"));
    assert(strstr(line, "stack=1024"));
    assert(strstr(line, "direct=614400"));
    assert(strstr(line, "libcef.dll=jit:63488"));
    assert(strstr(line, "libc heap=data:40960"));

    /* Two processes hold the same 62 MiB of libcef JIT and 40 / 10 MiB of heap. Sharing the JIT would not
     * lower the census; duplicate_code is only the upper bound on the repeated bytes. */
    memset(samples, 0, sizeof(samples));
    memcpy(samples[0].name, "libcef.dll", 11);
    samples[0].klass = PW_FMEM_JIT; samples[0].pid = 10; samples[0].resident = 62u << 20;
    memcpy(samples[1].name, "libc heap", 10);
    samples[1].klass = PW_FMEM_DATA; samples[1].pid = 10; samples[1].resident = 40u << 20;
    memcpy(samples[2].name, "libcef.dll", 11);
    samples[2].klass = PW_FMEM_JIT; samples[2].pid = 11; samples[2].resident = 34u << 20;
    memcpy(samples[3].name, "libc heap", 10);
    samples[3].klass = PW_FMEM_DATA; samples[3].pid = 11; samples[3].resident = 10u << 20;
    memcpy(samples[4].name, "libcef.dll", 11);
    samples[4].klass = PW_FMEM_JIT; samples[4].pid = 11; samples[4].resident = 2u << 20;
    memcpy(samples[5].name, "/data/ntdll.prx.elf", 20);
    samples[5].klass = PW_FMEM_FILE; samples[5].pid = 10; samples[5].resident = 4u << 20;
    pw_fmem_rollup(samples, 6, &rollup);
    assert(rollup.processes == 2);
    assert(rollup.duplicate_code == (36u << 20));
    assert(rollup.movable == (40u << 20) + (10u << 20) + (4u << 20));
    assert(rollup.class_bytes[PW_FMEM_JIT] == (62u << 20) + (36u << 20));
    assert(!rollup.truncated);
    return 0;
}
