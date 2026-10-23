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
 * Left as stock Wine, because GE 11.7 returns the same status:
 *   ntsync and every other sync object (no_get_fd)
 *   consoles, d3dkmt and the interface-change socket (no_fd_get_file_info)
 *   a real socket, fifo or device, whose path is not a file or a directory
 */
#include <sys/stat.h>

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

#endif /* PW_WINE_FILE_QUERY_H */
