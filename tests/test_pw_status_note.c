/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The line printed when a syscall returns 0xC0000024. The dispatcher fills the fields. */
#include "../wine/ps5/pw_wine_status.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void expect_substr(const char *what, const char *line, const char *want)
{
    if (line && want && strstr(line, want)) return;
    fprintf(stderr, "%s: line \"%s\" missing \"%s\"\n", what, line ? line : "(null)", want ? want : "(null)");
    exit(1);
}

int main(void)
{
    char line[192];
    char tiny[8];
    int n;

    n = pw_status_mismatch_line(line, sizeof(line), "NtQueryInformationThread",
                                 0x25, 1, 0xdffffa5903ULL);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        fprintf(stderr, "line length %d\n", n);
        return 1;
    }
    if (line[n - 1] != '\n' || line[n] != '\0') {
        fprintf(stderr, "line is not a terminated single line\n");
        return 1;
    }
    expect_substr("prefix", line, "wine-ps5: status 0xc0000024 ");
    expect_substr("syscall", line, "syscall=NtQueryInformationThread");
    expect_substr("id", line, "id=0x25");
    expect_substr("arg2", line, "arg2=0x1");
    expect_substr("caller", line, "caller=0xdffffa5903");

    n = pw_status_mismatch_line(line, sizeof(line), NULL, 0x10, 0, 0);
    if (n <= 0) return 1;
    expect_substr("null name", line, "syscall=?");
    n = pw_status_mismatch_line(line, sizeof(line), "", 0x10, 0, 0);
    if (n <= 0) return 1;
    expect_substr("empty name", line, "syscall=?");

    memset(tiny, 0x5a, sizeof(tiny));
    pw_status_mismatch_line(tiny, sizeof(tiny), "NtClose", 0xf, 0, 0);
    if (tiny[sizeof(tiny) - 1] != '\0') {
        fprintf(stderr, "tiny buffer was not terminated\n");
        return 1;
    }
    if (pw_status_mismatch_line(NULL, 8, "NtClose", 0, 0, 0) != -1) return 1;
    if (pw_status_mismatch_line(tiny, 0, "NtClose", 0, 0, 0) != -1) return 1;
    return 0;
}
