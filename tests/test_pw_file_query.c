/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A parked descriptor is a socket. File queries must answer as the path, which is what stock Wine returns.
 * realpath can fail after that stat; the unix name is then the normalised path. A handle with no unix name
 * is pinned before fstat, and the mode stored for a module is the real file's. */
#include "../wine/ps5/pw_wine_file_query.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

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

static int kinds(void)
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

static void expect_str(const char *what, const char *got, const char *want)
{
    if (got && want && !strcmp(got, want)) return;
    fprintf(stderr, "%s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want ? want : "(null)");
    exit(1);
}

/* Gap 1: realpath failure after a successful stat still yields a unix name, so the mismatch status is not returned. */
static void test_unix_name(void)
{
    char pattern[] = "/tmp/pw-file-query-XXXXXX";
    char path[PATH_MAX], linkpath[PATH_MAX], *dir, *got, *want, saved[PATH_MAX];
    int fd;

    if (!getcwd(saved, sizeof(saved))) {
        fprintf(stderr, "getcwd: %s\n", strerror(errno));
        exit(1);
    }
    dir = mkdtemp(pattern);
    if (!dir) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        exit(1);
    }
    snprintf(path, sizeof(path), "%s/file", dir);
    fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "open: %s\n", strerror(errno));
        exit(1);
    }
    close(fd);

    want = realpath(path, NULL);
    got = pw_file_query_unix_name(path);
    expect_str("existing file matches realpath", got, want);
    expect_status("existing file has a unix name", pw_file_query_name_status(got), 0);
    free(got);
    free(want);

    snprintf(linkpath, sizeof(linkpath), "%s/link", dir);
    if (symlink(path, linkpath)) {
        fprintf(stderr, "symlink: %s\n", strerror(errno));
        exit(1);
    }
    want = realpath(linkpath, NULL);
    got = pw_file_query_unix_name(linkpath);
    expect_str("symlink matches realpath", got, want);
    free(got);
    free(want);

    got = pw_file_query_normalize("/a/./b/../c//d/");
    expect_str("collapsed absolute path", got, "/a/c/d");
    free(got);

    got = pw_file_query_unix_name("/tmp/pw-file-query-no-such/./a/../b//c");
    expect_str("realpath failure keeps the normalised path", got, "/tmp/pw-file-query-no-such/b/c");
    expect_status("missing path is not STATUS_OBJECT_TYPE_MISMATCH", pw_file_query_name_status(got), 0);
    free(got);
    expect_status("NULL unix name", pw_file_query_name_status(NULL), PW_FILE_QUERY_TYPE_MISMATCH);

    if (chdir(dir)) {
        fprintf(stderr, "chdir: %s\n", strerror(errno));
        exit(1);
    }
    if (!getcwd(path, sizeof(path))) {
        fprintf(stderr, "getcwd in temp: %s\n", strerror(errno));
        exit(1);
    }
    got = pw_file_query_unix_name("missing/../zz");
    {
        char want_rel[PATH_MAX];
        snprintf(want_rel, sizeof(want_rel), "%s/zz", path);
        expect_str("relative realpath failure", got, want_rel);
    }
    free(got);
    if (chdir(saved)) {
        fprintf(stderr, "chdir back: %s\n", strerror(errno));
        exit(1);
    }
    unlink(linkpath);
    unlink(path); /* path was rewritten to dir/zz, which was never created; the file is dir/file */
    snprintf(path, sizeof(path), "%s/file", dir);
    unlink(path);
    rmdir(dir);
}

struct park_model {
    int tracked;
    int pinned;
    mode_t real_mode;
    int enters;
};

static struct park_model *active_model;

static int model_enter(int fd, uint32_t *pin)
{
    (void)fd;
    active_model->enters++;
    if (!active_model->tracked) {
        *pin = 0;
        return 0;
    }
    *pin = 1;
    active_model->pinned = 1;
    return 0;
}

static void model_leave(uint32_t pin)
{
    (void)pin;
    if (active_model) active_model->pinned = 0;
}

/* What fstat returns: a tracked file that is not pinned is the placeholder socket. */
static mode_t model_fstat(struct park_model *model)
{
    if (model->tracked && !model->pinned) return S_IFSOCK | 0666;
    return model->real_mode;
}

/* Gaps 2 and 3: pin before fstat. A nameless handle then answers as the file. A module stores FD_TYPE_FILE
 * and a directory stores FD_TYPE_DIR. A real socket is not tracked and stays a socket. */
static void test_pin_before_stat(void)
{
    const mode_t file = S_IFREG | 0644;
    const mode_t dir = S_IFDIR | 0755;
    const mode_t sock = S_IFSOCK | 0666;
    struct park_model module = { 1, 0, file, 0 };
    struct park_model folder = { 1, 0, dir, 0 };
    struct park_model socket = { 0, 0, sock, 0 };
    uint32_t pin = 0;
    mode_t live, stored;

    /* The placeholder, if fstat runs first. */
    expect_int("parked module before pin", (int)(model_fstat(&module) & S_IFMT), S_IFSOCK);
    expect_status("FileBasic of the placeholder", pw_file_basic_status(sock, 0), PW_FILE_QUERY_INVALID_INFO_CLASS);
    expect_int("placeholder type", pw_file_query_fd_type(sock), PW_FILE_QUERY_FD_CHAR);

    active_model = &module;
    if (pw_file_query_pin(3, model_enter, &pin)) {
        fprintf(stderr, "pin of a module failed\n");
        exit(1);
    }
    live = model_fstat(&module);
    pw_file_query_unpin(pin, model_leave);
    stored = pw_file_query_stored_mode(live, 0); /* alloc_file_handle has no path */
    expect_int("module was pinned", module.enters, 1);
    expect_int("module mode", (int)(stored & S_IFMT), S_IFREG);
    expect_int("module FD_TYPE_FILE", pw_file_query_fd_type(stored), PW_FILE_QUERY_FD_FILE);
    expect_status("FileBasic of a nameless module", pw_file_basic_status(stored, 0), 0);
    expect_int("FileBasic is not the mismatch status",
               pw_file_basic_status(stored, 0) == PW_FILE_QUERY_TYPE_MISMATCH, 0);
    expect_int("FileStandard does not describe the socket", S_ISSOCK(stored), 0);

    pin = 0;
    active_model = &folder;
    if (pw_file_query_pin(4, model_enter, &pin)) exit(1);
    live = model_fstat(&folder);
    pw_file_query_unpin(pin, model_leave);
    stored = pw_file_query_stored_mode(live, 0);
    expect_int("directory FD_TYPE_DIR", pw_file_query_fd_type(stored), PW_FILE_QUERY_FD_DIR);
    /* NtQueryDirectoryFile returns STATUS_INVALID_PARAMETER for any type but DIR.
     * A relative open returns STATUS_BAD_DEVICE_TYPE for any type but DIR. */
    expect_int("directory is not CHAR", pw_file_query_fd_type(stored) == PW_FILE_QUERY_FD_CHAR, 0);

    pin = 99;
    active_model = &socket;
    if (pw_file_query_pin(5, model_enter, &pin)) exit(1);
    expect_int("real socket is not pinned", (int)pin, 0);
    live = model_fstat(&socket);
    pw_file_query_unpin(pin, model_leave);
    stored = pw_file_query_stored_mode(live, sock);
    expect_int("real socket stays a socket", (int)(stored & S_IFMT), S_IFSOCK);
    expect_int("real socket FD_TYPE_CHAR", pw_file_query_fd_type(stored), PW_FILE_QUERY_FD_CHAR);
    expect_status("FileBasic of a real socket", pw_file_basic_status(stored, sock),
                  PW_FILE_QUERY_INVALID_INFO_CLASS);

    /* A path that is a file still wins when the live descriptor could not be reopened. */
    stored = pw_file_query_stored_mode(sock, file);
    expect_int("path file over a leftover placeholder", pw_file_query_fd_type(stored), PW_FILE_QUERY_FD_FILE);
    stored = pw_file_query_stored_mode(sock, dir);
    expect_int("path directory over a leftover placeholder", pw_file_query_fd_type(stored), PW_FILE_QUERY_FD_DIR);
}

static void test_mismatch_log(void)
{
    unsigned long pids[PW_FILE_QUERY_MISMATCH_SLOTS];
    unsigned counts[PW_FILE_QUERY_MISMATCH_SLOTS];
    char line[192];
    unsigned i;
    const char *want = "wine-ps5: file mismatch syscall=NtCreateFile class=0 options=0x2000 type=unknown\n";

    memset(pids, 0, sizeof(pids));
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < PW_FILE_QUERY_MISMATCH_LIMIT; i++) {
        if (!pw_file_query_mismatch_allow(pids, counts, PW_FILE_QUERY_MISMATCH_SLOTS, 42,
                                          PW_FILE_QUERY_MISMATCH_LIMIT)) {
            fprintf(stderr, "mismatch log stopped at %u\n", i);
            exit(1);
        }
    }
    if (pw_file_query_mismatch_allow(pids, counts, PW_FILE_QUERY_MISMATCH_SLOTS, 42,
                                     PW_FILE_QUERY_MISMATCH_LIMIT)) {
        fprintf(stderr, "mismatch log exceeded %u\n", PW_FILE_QUERY_MISMATCH_LIMIT);
        exit(1);
    }
    if (!pw_file_query_mismatch_allow(pids, counts, PW_FILE_QUERY_MISMATCH_SLOTS, 43,
                                      PW_FILE_QUERY_MISMATCH_LIMIT)) {
        fprintf(stderr, "another pid was rate limited\n");
        exit(1);
    }
    memset(pids, 0, sizeof(pids));
    memset(counts, 0, sizeof(counts));
    for (i = 1; i <= PW_FILE_QUERY_MISMATCH_SLOTS; i++) {
        if (!pw_file_query_mismatch_allow(pids, counts, PW_FILE_QUERY_MISMATCH_SLOTS, 1000 + i,
                                          PW_FILE_QUERY_MISMATCH_LIMIT)) {
            fprintf(stderr, "slot table filled early at %u\n", i);
            exit(1);
        }
    }
    if (pw_file_query_mismatch_allow(pids, counts, PW_FILE_QUERY_MISMATCH_SLOTS, 99999,
                                     PW_FILE_QUERY_MISMATCH_LIMIT)) {
        fprintf(stderr, "a full slot table still logged\n");
        exit(1);
    }

    if (!pw_file_query_note_mismatch(7, "NtCreateFile", 0, 0x2000, -1, line, sizeof(line))) {
        fprintf(stderr, "first mismatch note was dropped\n");
        exit(1);
    }
    expect_str("mismatch line", line, want);
    expect_str("file type", pw_file_query_type_name(PW_FILE_QUERY_FD_FILE), "file");
    expect_str("dir type", pw_file_query_type_name(PW_FILE_QUERY_FD_DIR), "dir");
    expect_str("char type", pw_file_query_type_name(PW_FILE_QUERY_FD_CHAR), "char");
    for (i = 1; i < PW_FILE_QUERY_MISMATCH_LIMIT; i++)
        pw_file_query_note_mismatch(7, "NtQueryInformationFile", 4, 0, -1, line, sizeof(line));
    if (pw_file_query_note_mismatch(7, "NtQueryInformationFile", 4, 0, -1, line, sizeof(line))) {
        fprintf(stderr, "note_mismatch exceeded the per-pid limit\n");
        exit(1);
    }
}

int main(void)
{
    if (kinds()) return 1;
    test_unix_name();
    test_pin_before_stat();
    test_mismatch_log();
    return 0;
}
