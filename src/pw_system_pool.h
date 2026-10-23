/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SYSTEM_POOL_H
#define PW_SYSTEM_POOL_H
/* The guarded resident figure, governed beside the flexible-pressure guard
 * (src/pw_memory_policy.h). The title feeds this the same figure that guard watches: the
 * resident census minus direct-memory growth since the start. On firmware 12 a launcher's
 * raw census climbs past 1600 MiB, and past 1800, while that guarded figure stays near the
 * idle desktop and flexible memory stays free. Watching the raw census paused the desktop
 * during boot (the idle raw figure is already about 1460 MiB, and the first helper charge
 * counted as a rise) and would pause a launcher that is drawing normally.
 *
 * The warn line is the firmware limit (1600 MiB from 12.00, 870 before). At or below it,
 * nothing is paused. The kill line is the flexible guard's hard end, PW_MEMORY_GUARD_END_MARGIN_MIB
 * past that limit (1670 or 940). A rise of spike_mib (32 by default) while the guarded figure
 * is already above the warn line and within one spike of that end, or the figure reaching the
 * end, pauses the helper that grew (SIGSTOP from the title). If the figure is still at the end
 * spike_hold_ms later (250 by default), that helper is SIGKILLed so the pages are given back.
 * The title, the memory helper, and the desktop session's root (explorer) are never signalled.
 * The title's loop performs the signal; this file only decides.
 *
 * Standing down is release_mib (64) under the warn line: a paused helper is continued once the
 * guarded figure is there. PROTON_SYSTEM_POOL=0 forces the governor off. With the flexible
 * guard off (LIMIT=0) and PROTON_SYSTEM_POOL unset, the governor stays off.
 * PROTON_SYSTEM_POOL=1 forces it on. The environment wins over /data/ps5proton/memory.conf.
 * PROTON_HANG_DISPLAY_MS (5000) is when the flight recorder ends a session whose picture has
 * stopped; PROTON_HANG_DISPLAY_WARN_MS (2000) is the notice before that. 0 disables the
 * watchdog. The watchdog's pressure mark stays the firmware limit, which the caller passes
 * separately: this file's kill line is the hard end, not that mark. Nothing here allocates
 * or aborts.
 */
#include "pw_memory_policy.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PW_POOL_FINE = 0,
    PW_POOL_WARN,
    PW_POOL_TRIM,
    PW_POOL_KILL,
    PW_POOL_CLEAR,
    PW_POOL_SUSPEND,
    PW_POOL_RESUME
};

typedef struct PwSystemPoolConfig {
    int off;
    unsigned warn_mib;
    unsigned kill_mib;
    unsigned hold_ms;
    unsigned release_mib;
    unsigned spike_mib;       /* a rise of this much, above warn and within one spike of the end, pauses */
    unsigned spike_hold_ms;   /* still at the kill line this long after the pause: SIGKILL */
    unsigned display_ms;      /* 0: the display watchdog does not act */
    unsigned display_warn_ms; /* 0: no warning before it acts */
} PwSystemPoolConfig;

/* phase: 0 idle, 1 warned, 4 paused. stopped_pid is the helper last paused. Zero it once per session. */
typedef struct PwSystemPool {
    int phase;
    int episode;
    int stopped_pid;
    uint64_t phase_ms;
} PwSystemPool;

/* firmware_limit_mib is pw_memory_guard_default_limit_mib. guard_off is the flexible guard's off
 * flag. file NULL is no file. Environment strings NULL are unset; a non-number is ignored. */
void pw_system_pool_choose(unsigned firmware_limit_mib, int guard_off, const struct PwSessionMemory *file,
                           const char *env_pool, const char *env_warn, const char *env_kill,
                           const char *env_hold, const char *env_release, const char *env_display,
                           const char *env_spike, const char *env_display_warn,
                           struct PwSystemPoolConfig *out);

/* One helper's resident flexible memory (sceKernelGetResidentFmemCount). On a helper that runs
 * Windows code that figure is mostly the executable copy the helper made for itself. */
typedef struct PwPoolCharge {
    int pid;
    uint64_t bytes;
} PwPoolCharge;

/* Who to stop. Never the title, the memory helper, the desktop root (shell_pid), or a pid of 1
 * or less. shell_pid 0 or 1 means there is no desktop root to spare. Rows may be NULL.
 * When the charges are the same shape, the newest helper is the one stopped: each new helper
 * adds its own flexible copy, and shedding the latest one leaves the session up. A helper whose
 * charge is at least 64 MiB above the newest and at least twice it, and at least 96 MiB, is the
 * one stopped instead. With no row for the newest, a largest charge of 96 MiB or more is stopped;
 * under that, the newest pid is. 0 when nothing is eligible. */
int pw_system_pool_victim(const struct PwPoolCharge *rows, unsigned count, int largest_pid,
                          uint64_t largest_bytes, int newest_pid, uint64_t newest_bytes, int title_pid,
                          int memory_pid, int shell_pid);

/* system_bytes is the guarded figure (the resident census minus direct-memory growth). rise_bytes
 * is how far that figure climbed within the last half second. now_ms is the title's clock, so a
 * sample every 250 ms or sooner advances the pause. helper_pid is the helper that grew, or the one
 * pw_system_pool_victim chose. A pid of 1 or less, the title's own, or the desktop root is never
 * chosen: the sample returns WARN and the caller does not signal. At or below warn_mib nothing is
 * paused. Above it, a rise of spike_mib within one spike of kill_mib pauses, and reaching kill_mib
 * pauses even with no rise. Dropping to warn_mib - release_mib or below resumes a paused helper
 * (RESUME) and ends the episode. now_ms going backwards does not advance the pause. EINVAL for a
 * NULL argument. */
int pw_system_pool_sample(struct PwSystemPool *pool, const struct PwSystemPoolConfig *config,
                          uint64_t system_bytes, uint64_t rise_bytes, uint64_t now_ms, int helper_pid,
                          int title_pid, int shell_pid, int *target);

#ifdef __cplusplus
}
#endif

#endif
