/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The line printed when a syscall returns 0xC0000024, and which object
 * NtSetIoCompletion may post to. The dispatcher fills the line. */
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

static void expect_int(const char *what, int got, int want)
{
    if (got == want) return;
    fprintf(stderr, "%s: got %d want %d\n", what, got, want);
    exit(1);
}

int main(void)
{
    static const unsigned short io_completion[] = {
        'I','o','C','o','m','p','l','e','t','i','o','n'
    };
    static const unsigned short io_reserve[] = {
        'I','o','C','o','m','p','l','e','t','i','o','n','R','e','s','e','r','v','e'
    };
    static const unsigned short file[] = { 'F','i','l','e' };
    char line[256];
    char tiny[8];
    int n;

    n = pw_status_mismatch_line(line, sizeof(line), "NtSetIoCompletion",
                                0xe9, 0x100020dbb8ULL, 0xdffff9727fULL,
                                0x1c4ULL, 0xdff9334ab0ULL, "File");
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        fprintf(stderr, "line length %d\n", n);
        return 1;
    }
    if (line[n - 1] != '\n' || line[n] != '\0') {
        fprintf(stderr, "line is not a terminated single line\n");
        return 1;
    }
    expect_substr("prefix", line, "wine-ps5: status 0xc0000024 ");
    expect_substr("syscall", line, "syscall=NtSetIoCompletion");
    expect_substr("id", line, "id=0xe9");
    expect_substr("arg2", line, "arg2=0x100020dbb8");
    expect_substr("caller", line, "caller=0xdffff9727f");
    expect_substr("handle", line, "handle=0x1c4");
    expect_substr("guest", line, "guest=0xdff9334ab0");
    expect_substr("type", line, "type=File");

    n = pw_status_mismatch_line(line, sizeof(line), "NtWriteFile",
                                0x8, 0, 0xdffff9565fULL, 0x30, 0, NULL);
    if (n <= 0) return 1;
    expect_substr("write caller", line, "caller=0xdffff9565f");
    expect_substr("write arg2", line, "arg2=0 ");
    expect_substr("null type", line, "type=?");

    n = pw_status_mismatch_line(line, sizeof(line), NULL, 0x10, 0, 0, 0, 0, "");
    if (n <= 0) return 1;
    expect_substr("null name", line, "syscall=?");
    expect_substr("empty type", line, "type=?");
    n = pw_status_mismatch_line(line, sizeof(line), "", 0x10, 0, 0, 0, 0, "Event");
    if (n <= 0) return 1;
    expect_substr("empty name", line, "syscall=?");
    expect_substr("event type", line, "type=Event");

    memset(tiny, 0x5a, sizeof(tiny));
    pw_status_mismatch_line(tiny, sizeof(tiny), "NtClose", 0xf, 0, 0, 0, 0, "File");
    if (tiny[sizeof(tiny) - 1] != '\0') {
        fprintf(stderr, "tiny buffer was not terminated\n");
        return 1;
    }
    if (pw_status_mismatch_line(NULL, 8, "NtClose", 0, 0, 0, 0, 0, "File") != -1) return 1;
    if (pw_status_mismatch_line(tiny, 0, "NtClose", 0, 0, 0, 0, 0, "File") != -1) return 1;

    expect_int("completion name", pw_type_is_iocompletion(io_completion, sizeof(io_completion)), 1);
    expect_int("reserve is not a port", pw_type_is_iocompletion(io_reserve, sizeof(io_reserve)), 0);
    expect_int("file name", pw_type_is_iocompletion(file, sizeof(file)), 0);
    expect_int("null name bytes", pw_type_is_iocompletion(NULL, sizeof(io_completion)), 0);
    expect_int("short name", pw_type_is_iocompletion(io_completion, 2), 0);

    expect_int("port wins over a bound fd", pw_completion_post(1, 1), PW_COMPLETION_PORT);
    expect_int("port alone", pw_completion_post(1, 0), PW_COMPLETION_PORT);
    expect_int("bound file", pw_completion_post(0, 1), PW_COMPLETION_BOUND);
    expect_int("event or unbound file", pw_completion_post(0, 0), PW_COMPLETION_REJECT);
    return 0;
}
