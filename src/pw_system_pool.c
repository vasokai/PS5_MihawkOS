/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_system_pool.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *skip(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    return text;
}

/* 1 with *out a decimal integer, 0 when text is not only a number. */
static int parse_u(const char *text, unsigned *out)
{
    char *end = NULL;
    unsigned long value;

    text = skip(text);
    if (!*text) return 0;
    value = strtoul(text, &end, 10);
    end = (char *)skip(end);
    if (end == text || *end || value > 1000000ul) return 0;
    *out = (unsigned)value;
    return 1;
}

static int pool_eligible(int pid, int title_pid, int memory_pid)
{
    return pid > 1 && pid != title_pid && (memory_pid <= 1 || pid != memory_pid);
}

int pw_system_pool_victim(const struct PwPoolCharge *rows, unsigned count, int largest_pid,
                          uint64_t largest_bytes, int newest_pid, uint64_t newest_bytes, int title_pid,
                          int memory_pid)
{
    int best_pid = 0, new_pid = 0;
    uint64_t best_bytes = 0, new_bytes = 0;
    unsigned i;

    if (rows) {
        for (i = 0; i < count; i++) {
            if (!pool_eligible(rows[i].pid, title_pid, memory_pid)) continue;
            if (!best_pid || rows[i].bytes > best_bytes) {
                best_pid = rows[i].pid;
                best_bytes = rows[i].bytes;
            }
            if (rows[i].pid > new_pid) {
                new_pid = rows[i].pid;
                new_bytes = rows[i].bytes;
            }
        }
    }
    if (pool_eligible(largest_pid, title_pid, memory_pid) && (!best_pid || largest_bytes > best_bytes)) {
        best_pid = largest_pid;
        best_bytes = largest_bytes;
    }
    if (!new_pid && pool_eligible(newest_pid, title_pid, memory_pid)) {
        new_pid = newest_pid;
        new_bytes = newest_bytes;
    }
    if (!best_pid) return new_pid;
    if (!new_pid || best_pid == new_pid) return best_pid;
    /* One helper holds the flexible charge. The rest are the ordinary per-process copies. */
    if (best_bytes >= new_bytes + (64ull << 20) && best_bytes >= new_bytes * 2 && best_bytes >= (96ull << 20))
        return best_pid;
    return new_pid;
}

void pw_system_pool_choose(unsigned firmware_limit_mib, int guard_off, const struct PwSessionMemory *file,
                           const char *env_pool, const char *env_warn, const char *env_kill,
                           const char *env_hold, const char *env_release, const char *env_display,
                           const char *env_spike, const char *env_display_warn,
                           struct PwSystemPoolConfig *out)
{
    unsigned parsed = 0;
    int pool_set = 0, pool_on = 0;
    unsigned warn, kill, hold_s, release, display, spike, display_warn;

    memset(out, 0, sizeof(*out));
    if (file && file->pool_set) {
        pool_set = 1;
        pool_on = file->pool_on;
    }
    if (env_pool && parse_u(env_pool, &parsed)) {
        pool_set = 1;
        pool_on = parsed != 0;
    }
    if ((pool_set && !pool_on) || (!pool_set && guard_off)) out->off = 1;

    warn = firmware_limit_mib > 150u ? firmware_limit_mib - 150u : firmware_limit_mib * 3u / 4u;
    kill = firmware_limit_mib;
    hold_s = 8u;
    release = 64u;
    spike = 32u;
    display = 5000u;
    display_warn = 2000u;
    if (file && file->pool_warn_set) warn = file->pool_warn_mib;
    if (file && file->pool_kill_set) kill = file->pool_kill_mib;
    if (file && file->pool_hold_set) hold_s = file->pool_hold_s;
    if (file && file->pool_release_set) release = file->pool_release_mib;
    if (file && file->pool_spike_set) spike = file->pool_spike_mib;
    if (file && file->hang_display_set) display = file->hang_display_ms;
    if (file && file->hang_display_warn_set) display_warn = file->hang_display_warn_ms;
    if (env_warn && parse_u(env_warn, &parsed)) warn = parsed;
    if (env_kill && parse_u(env_kill, &parsed)) kill = parsed;
    if (env_hold && parse_u(env_hold, &parsed)) hold_s = parsed;
    if (env_release && parse_u(env_release, &parsed)) release = parsed;
    if (env_spike && parse_u(env_spike, &parsed)) spike = parsed;
    if (env_display && parse_u(env_display, &parsed)) display = parsed;
    if (env_display_warn && parse_u(env_display_warn, &parsed)) display_warn = parsed;
    if (warn > kill) warn = kill;
    if (display_warn > display) display_warn = display;
    out->warn_mib = warn;
    out->kill_mib = kill;
    out->hold_ms = hold_s * 1000u;
    out->release_mib = release;
    out->spike_mib = spike;
    out->spike_hold_ms = 250u;
    out->display_ms = display;
    out->display_warn_ms = display ? display_warn : 0;
}

int pw_system_pool_sample(struct PwSystemPool *pool, const struct PwSystemPoolConfig *config,
                          uint64_t system_bytes, uint64_t rise_bytes, uint64_t now_ms, int helper_pid,
                          int title_pid, int *target)
{
    uint64_t warn, kill_at, spike, release, clear_at, near;
    int eligible, hot, close, over;

    if (!pool || !config || !target) return EINVAL;
    *target = 0;
    if (config->off) return PW_POOL_FINE;

    warn = (uint64_t)config->warn_mib << 20;
    kill_at = (uint64_t)config->kill_mib << 20;
    spike = (uint64_t)config->spike_mib << 20;
    release = (uint64_t)config->release_mib << 20;
    clear_at = release >= warn ? 0 : warn - release;
    near = kill_at > spike ? kill_at - spike : 0;
    if (pool->episode && system_bytes <= clear_at) {
        const int stopped = pool->phase == 4 ? pool->stopped_pid : 0;
        pool->phase = 0;
        pool->episode = 0;
        pool->phase_ms = 0;
        pool->stopped_pid = 0;
        if (stopped > 1) {
            *target = stopped;
            return PW_POOL_RESUME;
        }
        return PW_POOL_CLEAR;
    }

    eligible = helper_pid > 1 && helper_pid != title_pid;
    hot = rise_bytes >= spike && system_bytes >= warn;
    close = rise_bytes > 0 && system_bytes >= near;
    over = system_bytes >= kill_at;
    if (!hot && !close && !over) return PW_POOL_FINE;

    pool->episode = 1;
    if (pool->phase && now_ms < pool->phase_ms) return PW_POOL_FINE;
    if (!eligible) {
        if (!pool->phase) {
            pool->phase = 1;
            pool->phase_ms = now_ms;
            return PW_POOL_WARN;
        }
        return PW_POOL_FINE;
    }
    /* Already paused. Still at the limit after the hold: the pause did not give the pages back. */
    if (pool->phase == 4) {
        if (over && now_ms >= pool->phase_ms && now_ms - pool->phase_ms >= (uint64_t)config->spike_hold_ms &&
            pool->stopped_pid > 1) {
            pool->phase = 3;
            pool->phase_ms = now_ms;
            *target = pool->stopped_pid;
            return PW_POOL_KILL;
        }
        if (helper_pid != pool->stopped_pid) {
            pool->stopped_pid = helper_pid;
            pool->phase_ms = now_ms;
            *target = helper_pid;
            return PW_POOL_SUSPEND;
        }
        return PW_POOL_FINE;
    }
    pool->phase = 4;
    pool->phase_ms = now_ms;
    pool->stopped_pid = helper_pid;
    *target = helper_pid;
    return PW_POOL_SUSPEND;
}
