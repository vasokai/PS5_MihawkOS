/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_system_pool.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MIB(n) ((uint64_t)(n) << 20)

static int act(PwSystemPool *pool, const PwSystemPoolConfig *config, unsigned system_mib, unsigned rise_mib,
               uint64_t now_ms, int helper, int title, int shell, int *target)
{
    *target = -1;
    return pw_system_pool_sample(pool, config, MIB(system_mib), MIB(rise_mib), now_ms, helper, title, shell, target);
}

int main(void)
{
    PwSystemPoolConfig config;
    PwSystemPool pool;
    PwSessionMemory file;
    int target = 0;

    pw_system_pool_choose(1600, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(!config.off && config.warn_mib == 1600 && config.kill_mib == 1670);
    assert(config.hold_ms == 8000 && config.release_mib == 64 && config.spike_mib == 32);
    assert(config.spike_hold_ms == 250 && config.display_ms == 5000 && config.display_warn_ms == 2000);

    pw_system_pool_choose(870, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(!config.off && config.warn_mib == 870 && config.kill_mib == 940);

    pw_system_pool_choose(1600, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(config.off);
    pw_system_pool_choose(1600, 1, NULL, "1", NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(!config.off && config.warn_mib == 1600 && config.kill_mib == 1670);
    pw_system_pool_choose(1600, 0, NULL, "0", NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(config.off);

    assert(!pw_session_memory_parse(
        "PROTON_SYSTEM_POOL=1\nPROTON_SYSTEM_POOL_WARN=1000\nPROTON_SYSTEM_POOL_KILL=1100\n"
        "PROTON_SYSTEM_POOL_HOLD=2\nPROTON_SYSTEM_POOL_RELEASE=10\nPROTON_SYSTEM_POOL_SPIKE=16\n"
        "PROTON_HANG_DISPLAY_MS=5000\nPROTON_HANG_DISPLAY_WARN_MS=1500\n"
        "not a key\n", &file));
    assert(file.pool_set && file.pool_on && file.pool_warn_mib == 1000 && file.pool_kill_mib == 1100);
    assert(file.pool_hold_s == 2 && file.pool_release_mib == 10 && file.pool_spike_mib == 16);
    assert(file.hang_display_ms == 5000 && file.hang_display_warn_ms == 1500);
    assert(file.ignored == 1);
    pw_system_pool_choose(1600, 0, &file, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(!config.off && config.warn_mib == 1000 && config.kill_mib == 1100);
    assert(config.hold_ms == 2000 && config.release_mib == 10 && config.spike_mib == 16);
    assert(config.display_ms == 5000 && config.display_warn_ms == 1500);
    pw_system_pool_choose(1600, 0, &file, NULL, "1300", "1500", "0", "0", "0", NULL, NULL, &config);
    assert(!config.off && config.warn_mib == 1300 && config.kill_mib == 1500);
    assert(config.hold_ms == 0 && config.release_mib == 0 && config.display_ms == 0 && !config.display_warn_ms);

    /* A warn above the kill mark is clamped. */
    pw_system_pool_choose(1600, 0, NULL, "1", "2000", "1500", NULL, NULL, NULL, NULL, NULL, &config);
    assert(config.warn_mib == 1500 && config.kill_mib == 1500);

    /* Idle desktop (~1460) and a launcher up to the warn line, with a large rise, stay running. */
    pw_system_pool_choose(1600, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1460, 200, 0, 400, 269, 0, &target) == PW_POOL_FINE && target == 0);
    assert(act(&pool, &config, 1600, 400, 30000, 400, 269, 0, &target) == PW_POOL_FINE);
    assert(act(&pool, &config, 1630, 40, 31000, 400, 269, 0, &target) == PW_POOL_FINE);

    /* Above the warn line and within one spike of the hard end, the climb pauses the grower.
     * At the hard end a flat figure pauses too, and one hold later the paused helper is ended. */
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1638, 32, 1000, 400, 269, 0, &target) == PW_POOL_SUSPEND && target == 400);
    assert(act(&pool, &config, 1670, 0, 1249, 400, 269, 0, &target) == PW_POOL_FINE);
    assert(act(&pool, &config, 1670, 0, 1250, 400, 269, 0, &target) == PW_POOL_KILL && target == 400);

    /* Already at the hard end: pause on this sample, SIGKILL one hold later. The title and the
     * desktop root are warned and left running. A clock step backwards does not advance the hold. */
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1670, 0, 100000, 309, 269, 0, &target) == PW_POOL_SUSPEND && target == 309);
    assert(act(&pool, &config, 1704, 0, 100249, 309, 269, 0, &target) == PW_POOL_FINE);
    assert(act(&pool, &config, 1704, 0, 100250, 309, 269, 0, &target) == PW_POOL_KILL && target == 309);
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1704, 99, 0, 269, 269, 0, &target) == PW_POOL_WARN && target == 0);
    assert(act(&pool, &config, 1704, 99, 20000, 269, 269, 0, &target) == PW_POOL_FINE);
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1704, 0, 0, 1, 269, 0, &target) == PW_POOL_WARN && target == 0);
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1704, 80, 0, 500, 269, 500, &target) == PW_POOL_WARN && target == 0);
    assert(act(&pool, &config, 1704, 80, 5000, 500, 269, 500, &target) == PW_POOL_FINE);
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1670, 0, 5000, 310, 269, 0, &target) == PW_POOL_SUSPEND && target == 310);
    assert(act(&pool, &config, 1704, 0, 4000, 310, 269, 0, &target) == PW_POOL_FINE);

    /* A rise under one spike, still short of the hard end, is left alone. */
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1660, 31, 0, 400, 269, 0, &target) == PW_POOL_FINE && target == 0);

    /* Under the release line (warn - 64) a paused helper is continued. Ordinary use under the
     * warn line does not pause the next helper. */
    memset(&pool, 0, sizeof(pool));
    assert(act(&pool, &config, 1670, 0, 0, 400, 269, 0, &target) == PW_POOL_SUSPEND && target == 400);
    assert(act(&pool, &config, 1536, 0, 3000, 400, 269, 0, &target) == PW_POOL_RESUME && target == 400);
    assert(act(&pool, &config, 1500, 40, 4000, 401, 269, 0, &target) == PW_POOL_FINE && target == 0);
    assert(act(&pool, &config, 1600, 40, 5000, 401, 269, 0, &target) == PW_POOL_FINE);

    /* A file's own warn and kill still use the same rule: above warn, within one spike of kill. */
    pw_system_pool_choose(1600, 0, &file, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    memset(&pool, 0, sizeof(pool));
    assert(config.warn_mib == 1000 && config.kill_mib == 1100 && config.spike_mib == 16);
    assert(act(&pool, &config, 1083, 16, 0, 400, 269, 0, &target) == PW_POOL_FINE);
    assert(act(&pool, &config, 1084, 16, 1000, 400, 269, 0, &target) == PW_POOL_SUSPEND && target == 400);

    pw_system_pool_choose(1600, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &config);
    assert(pw_system_pool_sample(NULL, &config, 0, 0, 0, 0, 0, 0, &target) == EINVAL);

    /* Per-process flexible charge. Equal copies: stop the newest. One hog: stop that one.
     * The title and the memory helper are never the target. */
    {
        PwPoolCharge rows[4];
        memset(rows, 0, sizeof(rows));
        rows[0].pid = 200;
        rows[0].bytes = MIB(51);
        rows[1].pid = 309;
        rows[1].bytes = MIB(178);
        rows[2].pid = 400;
        rows[2].bytes = MIB(51);
        rows[3].pid = 269;
        rows[3].bytes = MIB(900);
        assert(pw_system_pool_victim(rows, 4, 309, MIB(178), 400, MIB(51), 269, 10, 0) == 309);
        rows[1].bytes = MIB(51);
        assert(pw_system_pool_victim(rows, 4, 309, MIB(51), 400, MIB(51), 269, 10, 0) == 400);
        rows[2].pid = 10; /* the newest row is the memory helper */
        rows[2].bytes = MIB(51);
        assert(pw_system_pool_victim(rows, 4, 309, MIB(51), 10, MIB(51), 269, 10, 0) == 309);
        assert(pw_system_pool_victim(NULL, 0, 309, MIB(178), 400, 0, 269, 10, 0) == 309);
        assert(pw_system_pool_victim(NULL, 0, 309, MIB(51), 400, 0, 269, 10, 0) == 400);
        assert(pw_system_pool_victim(NULL, 0, 269, MIB(400), 269, 0, 269, 10, 0) == 0);
        assert(pw_system_pool_victim(NULL, 0, 1, MIB(400), 0, 0, 269, 10, 0) == 0);
        /* The desktop root is never the one stopped, even when it is the newest and the largest. */
        assert(pw_system_pool_victim(rows, 4, 400, MIB(51), 400, MIB(51), 269, 10, 400) == 309);
        assert(pw_system_pool_victim(NULL, 0, 500, MIB(400), 500, 0, 269, 10, 500) == 0);
    }
    puts("pw_system_pool: ok");
    return 0;
}
