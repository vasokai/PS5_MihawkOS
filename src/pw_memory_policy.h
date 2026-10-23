/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_MEMORY_POLICY_H
#define PW_MEMORY_POLICY_H
/* What the title's memory guard watches, and where a session sets that.
 *
 * The guard (src/pw_memory_guard.h) used to be fed the raw resident census: sceKernelGetResidentFmemCount summed
 * over every process but the title. That figure counts a shared mapping in every process (K14, docs/PLAN.md) and,
 * on a firmware 12.20 session whose helpers committed several hundred MiB, it rose in step with the application
 * direct-memory pool while sceKernel's flexible-free figure stayed flat (about 946 MiB free the whole time; direct
 * memory still had about 10 GiB free). Direct-memory pages do not come out of the flexible pool. The pool is what
 * swaps the console out from under a title (the Pro's stall past about 989 MiB of flexible-resident memory; the
 * firmware 12 default of 1600 MiB is that generation's measured census, src/pw_memory_guard.h).
 *
 * So the title feeds the guard the part of the census that direct-memory growth since the start does not explain,
 * and it still ends a program when the flexible pool itself is nearly empty or the raw census passes a backstop.
 * PROTON_SYSTEM_MEMORY_RAW=1 feeds the raw census again. Nothing here has been run on a console: the subtraction is
 * the reading of that one session, and an earlier build that stopped counting one shared object was reverted because
 * the kernel's own RSS followed the raw census (docs/PLAN.md, 2026-10-20).
 *
 * A session sets the limit in /data/ps5proton/memory.conf, or with the same variables in the title's environment.
 * The environment wins, so a gate run's PROTON_SYSTEM_MEMORY_LIMIT still lowers the mark. A game's own environment
 * does not: the guard runs in the title, and a desktop session does not re-read the program's game.json. An explicit
 * limit may be higher than the firmware default. 0 turns the guard off, including the flexible-pool floor and the
 * raw-census backstop. An explicit end mark above the firmware's is still held to the firmware's when the limit was
 * not raised: that is what keeps the steep-climb rule on, and what leaves the gate's end mark out of reach.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_SESSION_MEMORY_PATH "/data/ps5proton/memory.conf"
/* Flexible free at or below this ends a program, whatever the attributed census says. */
#define PW_MEMORY_FLEXIBLE_RESERVE_MIB 96u
/* Raw resident census at or above this ends a program even when direct-memory growth explains it. A limit the
 * session sets above this becomes the backstop, so opting into a higher mark is not undone by it. */
#define PW_MEMORY_CENSUS_BACKSTOP_MIB 6144u

enum pw_memory_limit_source {
    PW_MEMORY_LIMIT_FIRMWARE = 0,
    PW_MEMORY_LIMIT_FILE,
    PW_MEMORY_LIMIT_ENVIRONMENT
};

/* One memory.conf. Missing keys stay unset. The last occurrence of a key wins. */
typedef struct PwSessionMemory {
    int limit_set;
    unsigned limit_mib; /* 0 turns the guard off, and only when limit_set */
    int end_set;
    unsigned end_mib;
    int raw_set;
    int raw; /* 1: the guard is fed the raw census */
    /* The raw-census governor and the display watchdog (src/pw_system_pool.h). */
    int pool_set;
    int pool_on; /* PROTON_SYSTEM_POOL: 0 off, nonzero on */
    int pool_warn_set;
    unsigned pool_warn_mib;
    int pool_kill_set;
    unsigned pool_kill_mib;
    int pool_hold_set;
    unsigned pool_hold_s; /* PROTON_SYSTEM_POOL_HOLD, seconds */
    int pool_release_set;
    unsigned pool_release_mib;
    int pool_spike_set;
    unsigned pool_spike_mib; /* PROTON_SYSTEM_POOL_SPIKE: a rise of this many MiB pauses a helper */
    int hang_display_set;
    unsigned hang_display_ms; /* PROTON_HANG_DISPLAY_MS; 0 disables the watchdog */
    int hang_display_warn_set;
    unsigned hang_display_warn_ms; /* PROTON_HANG_DISPLAY_WARN_MS */
    unsigned ignored; /* non-empty lines that were not a comment or a known key */
} PwSessionMemory;

/* text is the whole file. 0. *out is replaced. */
int pw_session_memory_parse(const char *text, struct PwSessionMemory *out);
/* 0, or the errno. ENOENT leaves *out zeroed (nothing set). */
int pw_session_memory_load(struct PwSessionMemory *out, const char *path);

typedef struct PwMemoryLimitChoice {
    unsigned limit_mib;
    unsigned end_mib; /* concrete, including the margin when no end was set */
    int off; /* the guard does not act */
    int above_default; /* an explicit limit above the firmware default */
    int raw; /* feed the guard the raw census */
    int source; /* pw_memory_limit_source: who set the limit */
} PwMemoryLimitChoice;

/* firmware_default_mib is pw_memory_guard_default_limit_mib. file NULL is no file. env_limit, env_end and env_raw
 * are the title's environment, NULL when unset. A value that is not a decimal integer is ignored, not taken as 0. */
void pw_memory_limit_choose(unsigned firmware_default_mib, const struct PwSessionMemory *file,
                            const char *env_limit, const char *env_end, const char *env_raw,
                            struct PwMemoryLimitChoice *out);
const char *pw_memory_limit_source_name(int source);

/* The running subtraction. Zero it once at the start of a session. */
typedef struct PwMemoryPressure {
    int have_census;
    uint64_t census_base;
    int have_direct;
    uint64_t direct_base;
    uint64_t direct_last; /* the last reading, reused when a later sample has none */
} PwMemoryPressure;

typedef struct PwMemoryPressureIn {
    uint64_t census; /* raw resident sum, bytes */
    uint64_t direct_total; /* 0: this sample has no direct-memory reading */
    uint64_t direct_free;
    uint64_t flexible_free; /* UINT64_MAX: no reading */
    uint64_t backstop; /* raw census at or above this is census_backstop; 0 disables it */
    uint64_t flexible_reserve; /* flexible free at or below this is flexible_low; 0 disables it */
    int attribute_direct; /* 0: guarded is the raw census (PROTON_SYSTEM_MEMORY_RAW) */
} PwMemoryPressureIn;

typedef struct PwMemoryPressureOut {
    uint64_t guarded; /* bytes to give pw_memory_guard_sample */
    uint64_t direct_used;
    uint64_t direct_growth; /* since the first sample that had a reading */
    uint64_t flexible_free;
    int direct_known;
    int flexible_known;
    int flexible_low;
    int census_backstop;
} PwMemoryPressureOut;

void pw_memory_pressure_sample(struct PwMemoryPressure *pressure, const struct PwMemoryPressureIn *in,
                               struct PwMemoryPressureOut *out);

#ifdef __cplusplus
}
#endif

#endif
