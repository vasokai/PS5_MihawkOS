/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PROTON_FLIGHT_RECORDER_H
#define PROTON_FLIGHT_RECORDER_H
#include <stdint.h>
/* The title's flight recorder: what is left on /data when the console hangs. Once a second a thread of its own, at the
 * title loop's priority, appends one block to the session's flight-recorder.log (src/pw_log_sessions.h: <root>/0, kept
 * through the next two launches) and fsyncs it. It reads only what never waits on a lock another thread may hold, and
 * decides one thing, the broker wedge:
 *
 *   fr t=<s> verdict=<fine|stalled|-> present=<ms> census=<ms> flips=+<n> mem_lock=<free|busy:n|?> broker=<..> display=<..> fences=<issued>/<done>[ phase=<name>:<ms> tid=<tid>|none]
 *   wedge broker_busy_s=<n>: <ending the game's helpers|reporting only>
 *   p pid=<pid> role=<title|memory|game> state=<p_stat> wait=<wmesg> pri=<class>/<level> cpu=<last> threads=<n> run_ms=+<n> name=<comm>
 *   busy pid=<pid> for=<ms>
 *
 * verdict, present and census are the hang breaker's last sample, as the title's loop published it (proton_flight_publish;
 * '-' before the first). busy:n is a lock found held at n samples in a row (a trylock released at once: the memory
 * helper's lock, the GPU broker's client and display locks, graphics patch 0012); fences are the broker's GPU submissions
 * and the last one completed. While the client lock is held, phase is the call its holder waits in, for how long, and
 * on which thread (graphics patch 0017; none: in no call named there). The once-per-session wedge line comes when the
 * client lock was held PW_HANG_WEDGE_S samples in a row spanning PW_HANG_WEDGE_S - 1 s (src/pw_hang_breaker.h; a
 * recorder that fell behind takes the samples it missed back to back): the title log says so as well, and
 * end_helpers, when given, ends the game. The p rows come from sysctl kern.proc, read by a worker that may block without
 * stopping the recorder (src/pw_flight_probe.h; "busy" then says where). The first round reads only the title's own
 * process: a title's sysctl names are approved one by one by the kernel (klog's "[SYSCTL] Error : ... is not approved"),
 * so if that read is refused one line says so, here and in the title log, and kern.proc is never asked again. */
struct pw_memory_census;
struct ProtonFlightSources {
    unsigned (*helpers)(int *pids, unsigned capacity, int *memory_pid);   /* pw_ps5_helpers, asked once, or null */
    int (*memory_lock_busy)();                                            /* pw_ps5_memory_lock_busy, or null */
    int (*foreground)();                                                  /* the server's foreground process, or null */
    int (*census)(struct pw_memory_census *);                             /* the memory helper's census, or null */
    bool (*end_helpers)(const char *why);   /* the title's SIGKILL ending (native/proton_wine.cpp), or null: report only */
    unsigned display_stall_ms;              /* no new frame this long ends the session when the title is in vmwait or the raw census is at pressure_mib; 0 off */
    unsigned display_warn_ms;               /* the same condition, a notice only, before the stall; 0 off */
    unsigned pressure_mib;                  /* a raw resident census at or above this MiB counts as pressure; 0 never */
    void (*notice)(const char *text);       /* on-screen notice when the session is ended, or null */
    void (*warn_notice)(const char *text);  /* on-screen notice when the picture pauses, or null */
};
/* The hang breaker's last sample and verdict (src/pw_hang_breaker.h), from the title's loop. */
void proton_flight_publish(uint64_t present_age_ms, uint64_t census_age_ms, bool census_known, bool stalled);
/* Opens the log and starts the thread: false (and said) when either fails. Once per process. */
bool proton_flight_recorder_start(const ProtonFlightSources &sources);
#endif
