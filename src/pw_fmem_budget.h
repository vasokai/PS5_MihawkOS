/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_FMEM_BUDGET_H
#define PW_FMEM_BUDGET_H
#include <stddef.h>
/* How many Chromium renderer processes a session keeps, and the switches the browser process is given so it does
 * not start the ones the cap would refuse.
 *
 * Each helper copies the code it runs into its own JIT memory, and the kernel charges that copy in full in every
 * process that maps it (K14, K18 in docs/PLAN.md). Sharing the copy does not lower sceKernelGetResidentFmemCount.
 * The copy is not made when the process is not started. MihawkBrowser already asks for one renderer
 * (src/pw_browser_policy.h). A Chromium launcher does not, and a launcher session's 15 or 16 helpers were the census
 * that stood on the firmware line.
 *
 * The cap is pw_ps5_spawn's, in the title's ntdll: a third --type=renderer from a Chromium host fails CreateProcess
 * with EAGAIN. The switches are the same request, written into the browser process's command line before Wine records
 * it, so Chromium does not try the spawn the cap would refuse. WINE_PS5_CHROMIUM_BUDGET=0 turns both off.
 * WINE_PS5_RENDERER_LIMIT is the count (unset: 2, 0: no cap and no switches). WoW's command line is not a Chromium
 * host and is left as it is. */
#ifdef __cplusplus
extern "C" {
#endif
/* 1 when image or its command line names a Chromium host (a launcher, Chrome, Chromium, MihawkBrowser, libcef). */
int pw_fmem_budget_is_chromium(const char *image, const char *command);
/* 1 when that host is being started as --type=renderer. */
int pw_fmem_budget_is_renderer(const char *image, const char *command);
/* The renderer cap. text NULL: enforced, *limit 2. "0": not enforced. A positive decimal: that limit.
 * Anything else: enforced, *limit 2. *limit is set only when enforced. */
int pw_fmem_budget_limit(const char *text, int *limit);
/* Append the browser switches image does not already carry. 1 when out differs from command, 0 when it is left
 * equal to command, -1 when cap is too small (out unchanged). A child (--type=) and a non-Chromium image are
 * unchanged. command NULL or empty is unchanged: the executable path is not invented. renderer_limit is the
 * --renderer-process-limit value, at least 1. */
int pw_fmem_budget_rewrite(const char *image, const char *command, int renderer_limit, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
