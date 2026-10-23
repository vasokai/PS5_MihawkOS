/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A parked descriptor is a socket. File queries must answer as the path, which is what stock Wine returns. */
#include "../wine/ps5/pw_wine_file_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static void expect_int(const char *what, int got, int want)
{
    if (got == want) return;
    fprintf(stderr, "%s: got %d, want %d\n", what, got, want);
    exit(1);
}

static void expect_status(const char *what, unsigned got, unsigned want)
{
    if (got == want) return;
    fprintf(stderr, "%s: got %#x, want %#x\n", what, got, want);
    exit(1);
}

int main(void)
{
    const mode_t file = S_IFREG | 0644;
    const mode_t dir = S_IFDIR | 0755;
    const mode_t sock = S_IFSOCK | 0666;
    const mode_t fifo = S_IFIFO | 0644;
    const mode_t chr = S_IFCHR | 0666;

    expect_int("regular file", pw_file_query_kind(file, 0), PW_FILE_QUERY_FILE);
    expect_int("directory", pw_file_query_kind(dir, 0), PW_FILE_QUERY_DIRECTORY);
    expect_int("socket", pw_file_query_kind(sock, 0), PW_FILE_QUERY_SPECIAL);
    expect_int("fifo", pw_file_query_kind(fifo, 0), PW_FILE_QUERY_SPECIAL);
    expect_int("char device", pw_file_query_kind(chr, 0), PW_FILE_QUERY_SPECIAL);

    /* The placeholder is a socket (or a fifo, or a char device) and the path is the exe or its directory. */
    expect_int("socket over file", pw_file_query_kind(sock, file), PW_FILE_QUERY_FILE);
    expect_int("socket over directory", pw_file_query_kind(sock, dir), PW_FILE_QUERY_DIRECTORY);
    expect_int("fifo over directory", pw_file_query_kind(fifo, dir), PW_FILE_QUERY_DIRECTORY);
    expect_int("char over file", pw_file_query_kind(chr, file), PW_FILE_QUERY_FILE);

    /* A real socket or fifo stays special, including when the live mode and the path agree. */
    expect_int("socket path", pw_file_query_kind(sock, sock), PW_FILE_QUERY_SPECIAL);
    expect_int("fifo path", pw_file_query_kind(fifo, fifo), PW_FILE_QUERY_SPECIAL);
    expect_int("file fd, socket path", pw_file_query_kind(file, sock), PW_FILE_QUERY_FILE);

    expect_status("FileBasic of a file", pw_file_basic_status(file, 0), 0);
    expect_status("FileBasic of a directory", pw_file_basic_status(dir, 0), 0);
    expect_status("FileBasic of a parked exe", pw_file_basic_status(sock, file), 0);
    expect_status("FileBasic of a parked module directory", pw_file_basic_status(sock, dir), 0);
    expect_status("FileBasic of a socket", pw_file_basic_status(sock, sock), PW_FILE_QUERY_INVALID_INFO_CLASS);
    expect_status("FileBasic of a fifo", pw_file_basic_status(fifo, 0), PW_FILE_QUERY_INVALID_INFO_CLASS);

    /* 0xC0000024 is STATUS_OBJECT_TYPE_MISMATCH. The loader dereferences it. These classes must not return it. */
    if (pw_file_basic_status(sock, file) == 0xC0000024u ||
        pw_file_basic_status(sock, dir) == 0xC0000024u ||
        pw_file_basic_status(file, 0) == 0xC0000024u ||
        pw_file_basic_status(sock, sock) == 0xC0000024u) {
        fprintf(stderr, "FileBasic returned STATUS_OBJECT_TYPE_MISMATCH\n");
        return 1;
    }
    return 0;
}
