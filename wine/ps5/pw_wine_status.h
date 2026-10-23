/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_STATUS_H
#define PW_WINE_STATUS_H
/*
 * One stderr line when a syscall returns STATUS_OBJECT_TYPE_MISMATCH (0xC0000024).
 *
 * The dispatcher is the choke point: every Nt* return reaches it, including a status
 * wineserver stored with set_error. The line names that syscall. It does not change
 * the status. caller is the guest instruction the syscall returns to (the frame's
 * saved rip is 0xb earlier; the caller of this formatter passes the adjusted address).
 * That address is not the RtlTimeToTimeFields fault. arg2 is the second argument
 * saved at syscall entry (an info class for NtQueryInformationThread, an
 * IO_STATUS_BLOCK for NtQueryInformationFile).
 */
#include <stdio.h>

static inline int pw_status_mismatch_line(char *out, size_t n, const char *syscall,
    unsigned id, unsigned long long arg2, unsigned long long caller)
{
    if (!out || !n) return -1;
    return snprintf(out, n,
        "wine-ps5: status 0xc0000024 syscall=%s id=%#x arg2=%#llx caller=%#llx\n",
        (syscall && syscall[0]) ? syscall : "?", id, arg2, caller);
}
#endif
