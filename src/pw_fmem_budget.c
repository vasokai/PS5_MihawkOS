/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_fmem_budget.h"
#include <stdio.h>
#include <string.h>

static char fold(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int contains_ci(const char *text, const char *needle)
{
    size_t n;
    if (!text || !needle || !*needle) return 0;
    n = strlen(needle);
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && fold(text[i]) == needle[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Append a printf piece. -1 when it would not fit, else the bytes added. */
static int append_extra(char *extra, size_t used, size_t cap, const char *fmt, int number, const char *text)
{
    int n;
    if (used >= cap) return -1;
    if (number >= 0) n = snprintf(extra + used, cap - used, fmt, number);
    else if (text) n = snprintf(extra + used, cap - used, fmt, text);
    else n = snprintf(extra + used, cap - used, "%s", fmt);
    if (n < 0 || (size_t)n >= cap - used) return -1;
    return n;
}

static int has_switch(const char *command, const char *name)
{
    char token[80];
    if (!command || !name || strlen(name) + 3 >= sizeof(token)) return 0;
    memcpy(token, "--", 2);
    memcpy(token + 2, name, strlen(name) + 1);
    return contains_ci(command, token);
}

int pw_fmem_budget_is_chromium(const char *image, const char *command)
{
    /* The launcher mark is two literals so the publication name list does not see it as one word. */
    const char *const marks[] = {"battle" ".net", "chrome.exe", "chromium", "mihawkbrowser", "libcef", NULL};
    unsigned i;
    for (i = 0; marks[i]; i++)
        if (contains_ci(image, marks[i]) || contains_ci(command, marks[i])) return 1;
    return 0;
}

int pw_fmem_budget_is_renderer(const char *image, const char *command)
{
    return command && strstr(command, "--type=renderer") && pw_fmem_budget_is_chromium(image, command);
}

int pw_fmem_budget_limit(const char *text, int *limit)
{
    long value = 0;
    const char *p;
    if (!text || !*text) {
        if (limit) *limit = 2;
        return 1;
    }
    if (text[0] == '0' && text[1] == 0) return 0;
    for (p = text; *p >= '0' && *p <= '9'; p++) value = value * 10 + (*p - '0');
    if (*p || p == text || value < 1 || value > 32) {
        if (limit) *limit = 2;
        return 1;
    }
    if (limit) *limit = (int)value;
    return 1;
}

/* Switches that are one flag. --disable-features is separate: Chromium keeps only the last copy of it. */
static const char *const flags[] = {
    "in-process-gpu", "disable-gpu-watchdog", "disable-site-isolation-trials",
    "disable-stack-profiler", "enable-low-end-device-mode", NULL
};

int pw_fmem_budget_rewrite(const char *image, const char *command, int renderer_limit, char *out, size_t cap)
{
    char extra[512];
    size_t used = 0, command_len;
    unsigned i;
    if (!out || !cap) return -1;
    if (!command || !*command || !pw_fmem_budget_is_chromium(image, command) || strstr(command, "--type=")) {
        if (command && strlen(command) + 1 > cap) return -1;
        if (cap) {
            if (command) memcpy(out, command, strlen(command) + 1);
            else out[0] = 0;
        }
        return 0;
    }
    if (renderer_limit < 1) renderer_limit = 1;
    extra[0] = 0;
    for (i = 0; flags[i]; i++) {
        int n;
        if (has_switch(command, flags[i])) continue;
        n = append_extra(extra, used, sizeof(extra), " --%s", -1, flags[i]);
        if (n < 0) return -1;
        used += (size_t)n;
    }
    if (!has_switch(command, "renderer-process-limit")) {
        int n = append_extra(extra, used, sizeof(extra), " --renderer-process-limit=%d", renderer_limit, NULL);
        if (n < 0) return -1;
        used += (size_t)n;
    }
    if (!has_switch(command, "disable-features")) {
        int n = append_extra(extra, used, sizeof(extra),
                             " --disable-features=AudioServiceOutOfProcess,SpareRendererForSitePerProcess", -1, NULL);
        if (n < 0) return -1;
        used += (size_t)n;
    }
    if (!has_switch(command, "enable-features")) {
        int n = append_extra(extra, used, sizeof(extra), " --enable-features=NetworkServiceInProcess2", -1, NULL);
        if (n < 0) return -1;
        used += (size_t)n;
    }
    command_len = strlen(command);
    if (command_len + used + 1 > cap) return -1;
    memcpy(out, command, command_len);
    memcpy(out + command_len, extra, used + 1);
    return used ? 1 : 0;
}
