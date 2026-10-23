// SPDX-License-Identifier: GPL-3.0-or-later
// The title's flight recorder (native/proton_flight_recorder.h).
#include "proton_flight_recorder.h"
#ifdef PS5_GPU_BROKER
#include "proton_gpu_server.h"
#endif
#include "../ps5/src/platform.h"
extern "C" {
#include "../src/pw_flight_probe.h"
#include "../src/pw_hang_breaker.h"
#include "../src/pw_log_sessions.h"
#include "../src/pw_memory_helper.h"
int scePthreadSetprio(pthread_t thread, int priority);
}
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

namespace {
constexpr uint64_t kRotateBytes = 32ull << 20;   // then flight-recorder.1.log takes the older half
const char kPath[] = PW_LOG_SESSIONS_ROOT "/0/flight-recorder.log";
const char kOlder[] = PW_LOG_SESSIONS_ROOT "/0/flight-recorder.1.log";

ProtonFlightSources sources;
PwFlightProbe probe;
bool probe_started = false;
int memory_pid = 0;   // the memory helper's, which lives as long as the session
int log_file = -1;
std::atomic<int> verdict{-1};   // -1 before the title's loop published one, else 1 for stalled
std::atomic<uint64_t> present_ms{0}, census_ms{0};
std::atomic<bool> census_known{false};

uint64_t monotonic_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1000000;
}

// One process's record from the kernel. The console's records are 1,096 bytes, not the SDK's 1,088
// (tools/console-processes.c): the fields read here come first, so a record is copied into the SDK's layout.
int read_process(void *, int pid, PwFlightRow *row)
{
    static unsigned char buffer[16 << 10];   // the worker's only
    static bool placed;
    if (!placed) placed = !scePthreadSetprio(pthread_self(), 690);   // beside the recorder, above every Windows thread
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t bytes = sizeof(buffer);
    if (sysctl(mib, 4, buffer, &bytes, nullptr, 0)) return errno ? errno : EIO;
    int size = 0;
    if (bytes >= sizeof(size)) memcpy(&size, buffer, sizeof(size));
    if (bytes < sizeof(size)) return ESRCH;
    if (size <= 0 || size_t(size) > bytes) return EPROTO;
    kinfo_proc record{};
    memcpy(&record, buffer, std::min(sizeof(record), size_t(size)));
    row->state = record.ki_stat;
    row->cpu = record.ki_lastcpu;
    row->pri_class = record.ki_pri.pri_class;
    row->pri_level = record.ki_pri.pri_level;
    row->pri_native = record.ki_pri.pri_native;
    row->pri_user = record.ki_pri.pri_user;
    row->threads = uint32_t(record.ki_numthreads);
    row->runtime_us = record.ki_runtime;
    snprintf(row->name, sizeof(row->name), "%s", record.ki_comm);
    snprintf(row->wait, sizeof(row->wait), "%s", record.ki_wmesg);
    return 0;
}

struct Block {
    char text[4096];
    int used = 0;
    __attribute__((format(printf, 2, 3))) void add(const char *format, ...)
    {
        if (used < 0 || used >= int(sizeof(text))) return;
        va_list args;
        va_start(args, format);
        const int length = vsnprintf(text + used, sizeof(text) - used, format, args);
        va_end(args);
        used = length < 0 ? used : std::min<int>(used + length, sizeof(text) - 1);
    }
};

// A lock found held at this many samples in a row; -1 when it cannot be read.
void lock_field(Block &block, const char *name, int busy, unsigned &streak)
{
    streak = busy > 0 ? streak + 1 : 0;
    if (busy < 0) block.add(" %s=?", name);
    else if (busy) block.add(" %s=busy:%u", name, streak);
    else block.add(" %s=free", name);
}

void *recorder_main(void *)
{
    scePthreadSetprio(pthread_self(), 690);   // the title loop's: above every Windows thread (patch 0896)
    const int self = getpid();
    const uint64_t start = monotonic_ms();
    uint64_t written = 0;
    unsigned memory_streak = 0, broker_streak = 0, display_streak = 0;
    uint64_t broker_since = 0;   // when the broker's streak's first busy sample was taken
    bool rows_on = probe_started, rows_proven = false, wedge_seen = false;
    bool display_presented = false, display_latched = false, display_warned = false, title_vmwait = false;
    uint64_t display_flat_since = 0;
    struct { int pid; uint64_t runtime_us; } seen[PW_FLIGHT_PIDS] = {};
#ifdef PS5_GPU_BROKER
    unsigned long long flips_before = proton_gpu_broker_flips();
#endif
    for (uint64_t second = 1;; ++second) {
        const uint64_t due = start + second * 1000, now = monotonic_ms();
        if (due > now) usleep(useconds_t((due - now) * 1000));
        Block block;
        const int decided = verdict.load(std::memory_order_relaxed);
        const uint64_t sampled = monotonic_ms();
        int display_stalled = 0, display_warning = 0, pressure = 0;
        uint64_t flat_ms = 0;
#ifdef PS5_GPU_BROKER
        const unsigned long long flips = proton_gpu_broker_flips();
        const unsigned long long flip_delta = flips - flips_before;
        if (sources.display_stall_ms) {
            if (flip_delta > 0) {
                display_presented = true;
                display_flat_since = 0;
                display_latched = false;
                display_warned = false;
            } else if (display_presented) {
                if (!display_flat_since) display_flat_since = sampled;
                flat_ms = sampled - display_flat_since;
                if (sources.pressure_mib && sources.census) {
                    pw_memory_census census{};
                    if (!sources.census(&census) && (census.system >> 20) >= sources.pressure_mib) pressure = 1;
                }
                PwHangDisplay picture = {flat_ms, sources.display_stall_ms, 1, title_vmwait ? 1 : 0, pressure};
                PwHangVerdict picture_verdict = PW_HANG_FINE;
                if (!pw_hang_display_decide(&picture, &picture_verdict) && picture_verdict == PW_HANG_STALLED)
                    display_stalled = 1;
                else if (sources.display_warn_ms) {
                    int warning = 0;
                    if (!pw_hang_display_warn(&picture, sources.display_warn_ms, &warning) && warning)
                        display_warning = 1;
                }
            }
        }
#endif
        if (display_stalled)
            block.add("fr t=%.1f verdict=stalled present=%llu", (sampled - start) / 1000.0, (unsigned long long)flat_ms);
        else
            block.add("fr t=%.1f verdict=%s", (sampled - start) / 1000.0,
                      decided < 0 ? "-" : decided ? "stalled" : "fine");
        if (!display_stalled && decided >= 0)
            block.add(" present=%llu", (unsigned long long)present_ms.load(std::memory_order_relaxed));
        if (display_stalled || decided >= 0) {
            if (census_known.load(std::memory_order_relaxed))
                block.add(" census=%llu", (unsigned long long)census_ms.load(std::memory_order_relaxed));
            else
                block.add(" census=-");
        }
#ifdef PS5_GPU_BROKER
        block.add(" flips=+%llu", flip_delta);
        flips_before = flips;
#endif
        lock_field(block, "mem_lock", sources.memory_lock_busy ? sources.memory_lock_busy() : -1, memory_streak);
        bool wedge = false;
#ifdef PS5_GPU_BROKER
        ProtonGpuHealth health{};
        proton_gpu_broker_health(&health);
        lock_field(block, "broker", int(health.lock_busy), broker_streak);
        lock_field(block, "display", int(health.display_busy), display_streak);
        block.add(" fences=%u/%u", health.fences_issued, health.fences_completed);
        if (health.lock_busy && health.phase) block.add(" phase=%s:%llu tid=%d", health.phase, health.phase_ms, health.holder_tid);
        else if (health.lock_busy) block.add(" phase=none");
        if (broker_streak == 1) broker_since = sampled;
        // The span too: after a slow write or fsync this loop takes the samples it missed back to back.
        wedge = !wedge_seen && pw_hang_breaker_wedged(broker_streak, sampled - broker_since);
#else
        (void)broker_streak;(void)display_streak;(void)broker_since;
#endif
        block.add("\n");
        if (wedge)
            block.add("wedge broker_busy_s=%u: %s\n", broker_streak,
                      sources.end_helpers ? "ending the game's helpers" : "reporting only");
        if (display_stalled)
            block.add("display flat_ms=%llu vmwait=%d pressure=%d: %s\n", (unsigned long long)flat_ms,
                      title_vmwait ? 1 : 0, pressure,
                      sources.end_helpers ? "ending the Windows session" : "reporting only");
        if (rows_on) {
            PwFlightRow rows[PW_FLIGHT_PIDS];
            int busy_pid = 0;
            uint64_t busy_ms = 0;
            const unsigned taken = pw_flight_probe_take(&probe, rows, PW_FLIGHT_PIDS, &busy_pid, &busy_ms);
            if (busy_pid) block.add("busy pid=%d for=%llu ms\n", busy_pid, (unsigned long long)busy_ms);
            for (unsigned i = 0; i < taken; ++i) {
                const PwFlightRow &row = rows[i];
                const char *role = row.pid == self ? "title" : row.pid == memory_pid ? "memory" : "game";
                if (!rows_proven && row.pid == self) {
                    if (row.error) {
                        say("flight recorder: process rows unavailable (kern.proc errno %d); not asked again", row.error);
                        block.add("process rows unavailable (kern.proc errno %d); not asked again\n", row.error);
                        rows_on = false;
                        break;
                    }
                    rows_proven = true;
                    say("flight recorder: process rows from kern.proc (the title's own: state=%d pri=%u/%u cpu=%d threads=%u)",
                        row.state, row.pri_class, row.pri_level, row.cpu, row.threads);
                }
                if (row.error) {
                    block.add("p pid=%d role=%s error=%d\n", row.pid, role, row.error);
                    continue;
                }
                if (row.pid == self) title_vmwait = row.wait[0] && !strcmp(row.wait, "vmwait");
                uint64_t before = row.runtime_us;
                unsigned slot = 0;
                while (slot < PW_FLIGHT_PIDS - 1 && seen[slot].pid && seen[slot].pid != row.pid) ++slot;
                if (seen[slot].pid == row.pid) before = seen[slot].runtime_us;
                seen[slot] = {row.pid, row.runtime_us};
                block.add("p pid=%d role=%s state=%d wait=%s pri=%u/%u cpu=%d threads=%u run_ms=+%llu name=%s\n", row.pid,
                          role, row.state, row.wait[0] ? row.wait : "-", row.pri_class, row.pri_level, row.cpu, row.threads,
                          (unsigned long long)(row.runtime_us > before ? (row.runtime_us - before) / 1000 : 0), row.name);
            }
            if (rows_on && !busy_pid) {
                // The title's own process alone until kern.proc has answered once (one refusal, one klog report).
                int pids[3], count = 0;
                pids[count++] = self;
                if (rows_proven) {
                    if (memory_pid > 0) pids[count++] = memory_pid;
                    pw_memory_census census{};
                    int game = sources.foreground ? sources.foreground() : 0;
#ifdef PS5_GPU_BROKER
                    if (game <= 0) game = proton_gpu_broker_game_mode();
#endif
                    if (game <= 0 && sources.census && !sources.census(&census)) game = census.largest_pid;
                    if (game > 0 && game != self && game != memory_pid) pids[count++] = game;
                }
                pw_flight_probe_ask(&probe, pids, unsigned(count));
            }
        }
        if (write(log_file, block.text, size_t(block.used)) > 0) written += uint64_t(block.used);
        fsync(log_file);
        if (wedge) {
            // On disk before the SIGKILL, which is what this run is for if the console then stops.
            wedge_seen = true;
            say("flight recorder: wedge broker_busy_s=%u: the GPU broker's client lock is held%s", broker_streak,
                sources.end_helpers ? "; ending the game's helpers" : "; reporting only");
            char why[96];
            snprintf(why, sizeof(why), "the GPU broker's client lock held %u s", broker_streak);
            if (sources.end_helpers && !sources.end_helpers(why)) say("flight recorder: the game is already being ended");
        }
        if (display_warning && !display_warned) {
            display_warned = true;
            const char *because = pressure && title_vmwait ? "the title waits in vmwait and system memory is at the limit" :
                                   pressure ? "system memory is at the limit" :
                                   title_vmwait ? "the title waits in vmwait" : "the picture is flat";
            say("Wine: display watchdog: no new frame for %llu ms while %s: warning",
                (unsigned long long)flat_ms, because);
            if (sources.warn_notice)
                sources.warn_notice("The picture paused. If it does not resume, the Windows session will be ended.");
        }
        if (display_stalled && !display_latched) {
            display_latched = true;
            const char *because = pressure && title_vmwait ? "the title waits in vmwait and system memory is at the limit" :
                                   pressure ? "system memory is at the limit" : "the title waits in vmwait";
            say("Wine: display watchdog: no new frame for %llu ms while %s: %s",
                (unsigned long long)flat_ms, because,
                sources.end_helpers ? "ending the Windows session" : "reporting only");
            if (sources.notice && sources.end_helpers)
                sources.notice("The picture stopped. The Windows session was ended so the PS5 would keep running.");
            if (sources.end_helpers) {
                char why[160];
                snprintf(why, sizeof(why), "no new frame for %llu ms while %s",
                         (unsigned long long)flat_ms, because);
                if (!sources.end_helpers(why)) say("flight recorder: the game is already being ended");
            }
        }
        if (written >= kRotateBytes) {
            close(log_file);
            rename(kPath, kOlder);
            log_file = open(kPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
            written = 0;
            if (log_file < 0) {
                say("flight recorder: %s could not be opened again (errno %d); it stops", kPath, errno);
                return nullptr;
            }
        }
    }
}
} // namespace

void proton_flight_publish(uint64_t present_age_ms, uint64_t census_age_ms, bool known, bool stalled)
{
    present_ms.store(present_age_ms, std::memory_order_relaxed);
    census_ms.store(census_age_ms, std::memory_order_relaxed);
    census_known.store(known, std::memory_order_relaxed);
    verdict.store(stalled ? 1 : 0, std::memory_order_relaxed);
}

bool proton_flight_recorder_start(const ProtonFlightSources &given)
{
    sources = given;
    int helpers[PW_FLIGHT_PIDS];   // only the memory helper's pid is kept: the list's lock is taken again only at a wedge
    if (sources.helpers) sources.helpers(helpers, PW_FLIGHT_PIDS, &memory_pid);
    log_file = open(kPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_file < 0) {
        say("flight recorder: %s could not be opened (errno %d); no recorder", kPath, errno);
        return false;
    }
    const int probe_status = pw_flight_probe_start(&probe, read_process, nullptr, monotonic_ms);
    probe_started = !probe_status;
    pthread_attr_t attributes;
    pthread_t thread;
    int status = pthread_attr_init(&attributes);
    if (!status) {
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attributes, 256u << 10);
        status = pthread_create(&thread, &attributes, recorder_main, nullptr);
        pthread_attr_destroy(&attributes);
    }
    say("flight recorder: %s every second, process reader %d, thread %d", kPath, probe_status, status);
    if (status) {
        close(log_file);
        log_file = -1;
    }
    return !status;
}
