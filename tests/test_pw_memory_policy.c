/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_memory_policy.h"
#include "../wine/ps5/pw_wine_dmem.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB(n) ((uint64_t)(n) << 20)

static PwMemoryPressureOut sample(PwMemoryPressure *pressure, uint64_t census, uint64_t total, uint64_t free_bytes,
                                  uint64_t flexible, int attribute)
{
    PwMemoryPressureIn in = {0};
    PwMemoryPressureOut out;
    in.census = census;
    in.direct_total = total;
    in.direct_free = free_bytes;
    in.flexible_free = flexible;
    in.backstop = MIB(PW_MEMORY_CENSUS_BACKSTOP_MIB);
    in.flexible_reserve = MIB(PW_MEMORY_FLEXIBLE_RESERVE_MIB);
    in.attribute_direct = attribute;
    pw_memory_pressure_sample(pressure, &in, &out);
    return out;
}

int main(void)
{
    struct PwSessionMemory file;
    struct PwMemoryLimitChoice choice;
    struct PwMemoryPressure pressure;
    PwMemoryPressureOut out;
    char path[] = "/tmp/pw-memory-conf-XXXXXX";
    int fd;

    /* The firmware default, with the end mark held to it. An end above that, and a limit below it, is the gate's
     * memory-guard case: the end stays out of reach and the steep-climb rule stays off. */
    pw_memory_limit_choose(1600, NULL, NULL, NULL, NULL, &choice);
    assert(!choice.off && choice.limit_mib == 1600 && choice.end_mib == 1670 && !choice.above_default && !choice.raw);
    assert(choice.source == PW_MEMORY_LIMIT_FIRMWARE);
    pw_memory_limit_choose(1600, NULL, "450", "2000", NULL, &choice);
    assert(!choice.off && choice.limit_mib == 450 && choice.end_mib == 1670);
    assert(choice.source == PW_MEMORY_LIMIT_ENVIRONMENT && !choice.above_default);
    assert(choice.end_mib > choice.limit_mib + 70);

    /* 0 turns the guard off. A non-number does not. A limit above the firmware default is kept, and its end follows. */
    pw_memory_limit_choose(1600, NULL, "0", "9000", NULL, &choice);
    assert(choice.off && !choice.limit_mib && !choice.end_mib);
    pw_memory_limit_choose(1600, NULL, "nope", NULL, NULL, &choice);
    assert(!choice.off && choice.limit_mib == 1600 && choice.end_mib == 1670);
    pw_memory_limit_choose(1600, NULL, "4096", NULL, "1", &choice);
    assert(!choice.off && choice.limit_mib == 4096 && choice.end_mib == 4166 && choice.above_default && choice.raw);
    assert(!strcmp(pw_memory_limit_source_name(choice.source), "the environment"));

    /* The session file, then the environment over it. */
    assert(!pw_session_memory_parse(
        "# session\n\nPROTON_SYSTEM_MEMORY_LIMIT=0\nPROTON_SYSTEM_MEMORY_END=100\n"
        "PROTON_SYSTEM_MEMORY_RAW=1\nnot a key\n", &file));
    assert(file.limit_set && !file.limit_mib && file.end_set && file.end_mib == 100 && file.raw && file.ignored == 1);
    pw_memory_limit_choose(870, &file, NULL, NULL, NULL, &choice);
    assert(choice.off && choice.raw && choice.source == PW_MEMORY_LIMIT_FILE);
    pw_memory_limit_choose(1600, &file, "3200", NULL, "0", &choice);
    assert(!choice.off && choice.limit_mib == 3200 && choice.end_mib == 3270 && !choice.raw && choice.above_default);
    assert(choice.source == PW_MEMORY_LIMIT_ENVIRONMENT);

    /* The last key wins, and a missing file is nothing set. */
    assert(!pw_session_memory_parse("PROTON_SYSTEM_MEMORY_LIMIT=100\nPROTON_SYSTEM_MEMORY_LIMIT=250\n", &file));
    assert(file.limit_mib == 250 && !file.ignored);
    fd = mkstemp(path);
    assert(fd >= 0);
    {
        const char row[] = "PROTON_SYSTEM_MEMORY_LIMIT=8\n";
        assert(write(fd, row, sizeof(row) - 1) == (ssize_t)(sizeof(row) - 1));
    }
    assert(!close(fd));
    assert(!pw_session_memory_load(&file, path));
    assert(file.limit_set && file.limit_mib == 8);
    assert(pw_session_memory_load(&file, "/tmp/pw-memory-conf-missing") == ENOENT && !file.limit_set);
    assert(!unlink(path));

    /* A firmware 12 session whose helpers committed direct memory: the raw census rose 944 to 1601 MiB while direct
     * memory free fell by 653 MiB and flexible free stayed near 946. The guarded figure stays near the start. */
    memset(&pressure, 0, sizeof(pressure));
    out = sample(&pressure, MIB(944), MIB(11712), MIB(11174), MIB(948), 1);
    assert(out.guarded == MIB(944) && !out.direct_growth && out.direct_used == MIB(538));
    assert(!out.flexible_low && !out.census_backstop);
    out = sample(&pressure, MIB(1601), MIB(11712), MIB(10521), MIB(946), 1);
    assert(out.direct_growth == MIB(653));
    assert(out.guarded == MIB(1601) - MIB(653));
    assert(out.guarded < MIB(1600) && !out.flexible_low && !out.census_backstop);
    /* A sample with no direct-memory reading keeps the last one, so one failed query does not feed the raw census. */
    out = sample(&pressure, MIB(1601), 0, 0, UINT64_MAX, 1);
    assert(out.direct_known && out.direct_growth == MIB(653) && out.guarded == MIB(1601) - MIB(653));
    assert(!out.flexible_known && !out.flexible_low);

    /* Flexible growth is not explained by direct memory, so the guard still sees it. A raw session subtracts nothing. */
    out = sample(&pressure, MIB(2001), MIB(11712), MIB(10521), MIB(400), 1);
    assert(out.guarded == MIB(2001) - MIB(653));
    memset(&pressure, 0, sizeof(pressure));
    (void)sample(&pressure, MIB(944), MIB(11712), MIB(11174), MIB(948), 0);
    out = sample(&pressure, MIB(1601), MIB(11712), MIB(10521), MIB(946), 0);
    assert(out.guarded == MIB(1601) && out.direct_growth == MIB(653));

    /* The flexible pool nearly empty, and the raw census at the backstop, are separate from the attributed figure. */
    memset(&pressure, 0, sizeof(pressure));
    out = sample(&pressure, MIB(900), MIB(11712), MIB(11000), MIB(96), 1);
    assert(out.flexible_low && out.guarded == MIB(900));
    out = sample(&pressure, MIB(6144), MIB(11712), MIB(11000), MIB(900), 1);
    assert(out.census_backstop);
    memset(&pressure, 0, sizeof(pressure));
    out = sample(&pressure, MIB(700), 0, 0, UINT64_MAX, 1);
    assert(!out.direct_known && !out.flexible_known && !out.flexible_low && out.guarded == MIB(700));

    /* Direct memory that grew by more than the census does not pull the guarded figure under the baseline. */
    memset(&pressure, 0, sizeof(pressure));
    (void)sample(&pressure, MIB(1000), MIB(11712), MIB(11000), MIB(800), 1);
    out = sample(&pressure, MIB(1100), MIB(11712), MIB(10000), MIB(800), 1);
    assert(out.direct_growth == MIB(1000) && out.guarded == MIB(1000));

    assert(pw_wine_anon_fresh_direct(1, 1, 0, 0, 0));
    assert(!pw_wine_anon_fresh_direct(1, 1, 0, 1, 0));
    assert(!pw_wine_anon_fresh_direct(1, 1, 0, 0, 1));
    assert(!pw_wine_anon_fresh_direct(0, 1, 0, 0, 0));
    assert(!pw_wine_anon_fresh_direct(1, 1, 1, 0, 0));
    assert(!pw_wine_anon_fresh_direct(1, 0, 0, 0, 0));

    puts("pw_memory_policy: ok");
    return 0;
}
