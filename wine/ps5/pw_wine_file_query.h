/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_FILE_QUERY_H
#define PW_WINE_FILE_QUERY_H
/*
 * Which object a file query should answer as.
 *
 * On the PS5 an idle file keeps its descriptor number, held by a socket, and
 * is reopened on the next use (pw_wine_fdv.h). fstat of that placeholder is
 * not a regular file or a directory. wineserver's open_fd then stores no
 * inode and no unix name, and file_get_fd_type returns FD_TYPE_CHAR.
 *
 * Those objects make IO_STATUS_BLOCK writers return STATUS_OBJECT_TYPE_MISMATCH
 * (0xC0000024) where stock Wine, which statted the real file, returns the
 * file's data or success:
 *
 *   get_handle_unix_name, and WineFileUnixNameInformation through it
 *   set_fd_name and set_fd_disposition (NtSetInformationFile)
 *   file_open_file (a second open of that file)
 *   file_id_to_unix_file_name when the root was stored as FD_TYPE_CHAR
 *     (NtCreateFile with FILE_OPEN_BY_FILE_ID)
 *
 * NtQueryInformationFile does not itself return 0xC0000024 for a placeholder
 * that still has a unix fd. FileBasicInformation and FileStatInformation
 * return STATUS_INVALID_INFO_CLASS (0xC0000003). FileStandardInformation,
 * FileAllInformation and FileNetworkOpenInformation fill from the socket.
 * FileNameInformation uses the NT name. Statting the path that was opened
 * makes FileBasic, FileStandard, FileName and FileAll the file or directory
 * stock Wine would have returned.
 *
 * Three cases stay wrong if the path stat is the only fallback. realpath can
 * fail after stat succeeded, and then unix_name is NULL: get_handle_unix_name,
 * WineFileUnixNameInformation, set_fd_name, file_open_file and the reparse
 * FSCTLs return 0xC0000024. A handle from create_file_for_fd has no unix name,
 * so the path fallback never runs and FileBasic stays 0xC0000003 while
 * FileStandard describes the socket. get_view_file stores file->mode from the
 * live descriptor, so a parked module is FD_TYPE_CHAR: NtQueryDirectoryFile
 * returns STATUS_INVALID_PARAMETER and a relative open returns
 * STATUS_BAD_DEVICE_TYPE. Those paths pin the descriptor (pw_wine_fdv enter)
 * before they fstat, and a missing realpath keeps the normalised path.
 *
 * Left as stock Wine, because GE 11.7 returns the same status:
 *   ntsync and every other sync object (no_get_fd)
 *   consoles, d3dkmt and the interface-change socket (no_fd_get_file_info)
 *   a real socket, fifo or device, whose path is not a file or a directory
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum pw_file_query_kind {
    PW_FILE_QUERY_FILE = 1,
    PW_FILE_QUERY_DIRECTORY = 2,
    PW_FILE_QUERY_SPECIAL = 3
};

/* STATUS_INVALID_INFO_CLASS. Stock Wine's FileBasicInformation answer for a socket, fifo or device. */
#define PW_FILE_QUERY_INVALID_INFO_CLASS 0xC0000003u

/* live_mode is fstat of the descriptor Wine holds. path_mode is stat of the path it was opened as, or 0 when that
 * path was not statted. The path wins when it is a regular file or a directory: a park placeholder must not hide
 * the file. A real socket or fifo, whose path is not a file or a directory, stays special. */
static inline int pw_file_query_kind(mode_t live_mode, mode_t path_mode)
{
    if (S_ISREG(path_mode)) return PW_FILE_QUERY_FILE;
    if (S_ISDIR(path_mode)) return PW_FILE_QUERY_DIRECTORY;
    if (S_ISREG(live_mode)) return PW_FILE_QUERY_FILE;
    if (S_ISDIR(live_mode)) return PW_FILE_QUERY_DIRECTORY;
    return PW_FILE_QUERY_SPECIAL;
}

/* FileBasicInformation / FileStatInformation. Success means fill from the file or directory. Never 0xC0000024. */
static inline unsigned pw_file_basic_status(mode_t live_mode, mode_t path_mode)
{
    if (pw_file_query_kind(live_mode, path_mode) == PW_FILE_QUERY_SPECIAL)
        return PW_FILE_QUERY_INVALID_INFO_CLASS;
    return 0;
}

/* STATUS_OBJECT_TYPE_MISMATCH. What a NULL unix name returns from the file-handle queries above. */
#define PW_FILE_QUERY_TYPE_MISMATCH 0xC0000024u

/* file_get_fd_type's answer, the server_fd_type values. A module file is FD_TYPE_FILE, a directory is
 * FD_TYPE_DIR, and a socket, fifo or device is FD_TYPE_CHAR. */
enum {
    PW_FILE_QUERY_FD_INVALID = 0,
    PW_FILE_QUERY_FD_FILE = 1,
    PW_FILE_QUERY_FD_DIR = 2,
    PW_FILE_QUERY_FD_SOCKET = 3,
    PW_FILE_QUERY_FD_SERIAL = 4,
    PW_FILE_QUERY_FD_CHAR = 5,
    PW_FILE_QUERY_FD_DEVICE = 6
};

static inline int pw_file_query_fd_type(mode_t mode)
{
    if (S_ISREG(mode) || S_ISBLK(mode)) return PW_FILE_QUERY_FD_FILE;
    if (S_ISDIR(mode)) return PW_FILE_QUERY_FD_DIR;
    return PW_FILE_QUERY_FD_CHAR;
}

/* The mode create_file_for_fd and create_file_for_fd_obj store. path_mode is 0 when the handle has no
 * path (alloc_file_handle). A file or directory path wins over a placeholder; otherwise the live mode,
 * which is the real file once the descriptor has been pinned. */
static inline mode_t pw_file_query_stored_mode(mode_t live_mode, mode_t path_mode)
{
    if (S_ISREG(path_mode) || S_ISDIR(path_mode)) return path_mode;
    return live_mode;
}

/* 0 when unix_name can be returned. NULL is STATUS_OBJECT_TYPE_MISMATCH. */
static inline unsigned pw_file_query_name_status(const char *unix_name)
{
    return unix_name && unix_name[0] ? 0u : PW_FILE_QUERY_TYPE_MISMATCH;
}

/* Absolute path with ".", ".." and repeated slashes collapsed. Relative paths start from the working
 * directory. This is the unix name when realpath fails after stat succeeded. */
static inline char *pw_file_query_normalize(const char *path)
{
    char prefix[PATH_MAX];
    char *joined, *out;
    size_t prefix_len = 0, path_len, cap, n;
    const char *src;

    if (!path || !path[0]) return NULL;
    path_len = strlen(path);
    if (path[0] != '/')
    {
        if (!getcwd(prefix, sizeof(prefix))) return NULL;
        prefix_len = strlen(prefix);
        if (!prefix_len || prefix_len + 1 >= sizeof(prefix)) return NULL;
        if (prefix[prefix_len - 1] != '/') prefix[prefix_len++] = '/';
    }
    cap = prefix_len + path_len + 1;
    joined = malloc(cap);
    if (!joined) return NULL;
    if (prefix_len) memcpy(joined, prefix, prefix_len);
    memcpy(joined + prefix_len, path, path_len + 1);

    out = malloc(cap + 1);
    if (!out)
    {
        free(joined);
        return NULL;
    }
    out[0] = '/';
    n = 1;
    src = joined;
    if (*src == '/') src++;
    while (*src)
    {
        const char *slash = strchr(src, '/');
        size_t clen = slash ? (size_t)(slash - src) : strlen(src);

        if (clen == 1 && src[0] == '.')
            ;
        else if (clen == 2 && src[0] == '.' && src[1] == '.')
        {
            while (n > 1 && out[n - 1] != '/') n--;
            if (n > 1) n--;
        }
        else if (clen)
        {
            if (n > 1) out[n++] = '/';
            if (n + clen > cap)
            {
                free(joined);
                free(out);
                return NULL;
            }
            memcpy(out + n, src, clen);
            n += clen;
        }
        if (!slash) break;
        src = slash + 1;
    }
    out[n] = 0;
    free(joined);
    return out;
}

/* realpath when it works, so an existing file keeps stock Wine's name. The normalised path when realpath
 * fails, so a stat that succeeded still has a unix name. */
static inline char *pw_file_query_unix_name(const char *path)
{
    char resolved[PATH_MAX];

    if (!path || !path[0]) return NULL;
    if (realpath(path, resolved)) return strdup(resolved);
    return pw_file_query_normalize(path);
}

/* Pin a parked descriptor so the next fstat sees the file. enter/leave are pw_wine_fdv's, or NULL when
 * that table is not installed yet. A real socket is not tracked: enter returns 0 and *pin stays 0, and
 * fstat is unchanged. */
static inline int pw_file_query_pin(int fd, int (*enter)(int fd, uint32_t *pin), uint32_t *pin)
{
    if (pin) *pin = 0;
    if (fd < 0 || !enter || !pin) return 0;
    return enter(fd, pin);
}

static inline void pw_file_query_unpin(uint32_t pin, void (*leave)(uint32_t pin))
{
    if (pin && leave) leave(pin);
}

/* One mismatch line, at most PW_FILE_QUERY_MISMATCH_LIMIT per pid in this module. The caller writes the
 * line; this only decides and formats. type -1 is "unknown" (ntdll has no fd object in hand). */
#define PW_FILE_QUERY_MISMATCH_SLOTS 64u
#define PW_FILE_QUERY_MISMATCH_LIMIT 20u

static inline const char *pw_file_query_type_name(int type)
{
    static const char *const names[] = {
        "invalid", "file", "dir", "socket", "serial", "char", "device"
    };

    if (type < 0 || (unsigned)type >= sizeof(names) / sizeof(names[0])) return "unknown";
    return names[type];
}

static inline int pw_file_query_mismatch_line(char *out, size_t out_len, const char *syscall,
                                              unsigned class, unsigned options, int type)
{
    if (!out || !out_len) return -1;
    return snprintf(out, out_len, "wine-ps5: file mismatch syscall=%s class=%u options=%#x type=%s\n",
                    syscall ? syscall : "?", class, options, pw_file_query_type_name(type));
}

static inline int pw_file_query_mismatch_allow(unsigned long *pids, unsigned *counts, unsigned slots,
                                               unsigned long pid, unsigned limit)
{
    unsigned i;
    unsigned long key = pid + 1; /* 0 stays an empty slot */

    if (!pids || !counts || !slots || !limit) return 0;
    for (i = 0; i < slots; i++)
    {
        unsigned long got = __atomic_load_n(&pids[i], __ATOMIC_RELAXED);
        unsigned n;

        if (got == key)
        {
            n = __atomic_fetch_add(&counts[i], 1, __ATOMIC_RELAXED);
            return n < limit;
        }
        if (got != 0) continue;
        {
            unsigned long expected = 0;

            if (__atomic_compare_exchange_n(&pids[i], &expected, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            {
                __atomic_store_n(&counts[i], 1, __ATOMIC_RELAXED);
                return 1;
            }
            if (expected == key)
            {
                n = __atomic_fetch_add(&counts[i], 1, __ATOMIC_RELAXED);
                return n < limit;
            }
        }
    }
    return 0;
}

static inline int pw_file_query_note_mismatch(unsigned long pid, const char *syscall, unsigned class,
                                              unsigned options, int type, char *line, size_t line_len)
{
    static unsigned long pids[PW_FILE_QUERY_MISMATCH_SLOTS];
    static unsigned counts[PW_FILE_QUERY_MISMATCH_SLOTS];

    if (!pw_file_query_mismatch_allow(pids, counts, PW_FILE_QUERY_MISMATCH_SLOTS, pid,
                                      PW_FILE_QUERY_MISMATCH_LIMIT))
        return 0;
    return pw_file_query_mismatch_line(line, line_len, syscall, class, options, type) >= 0;
}

#endif /* PW_WINE_FILE_QUERY_H */
