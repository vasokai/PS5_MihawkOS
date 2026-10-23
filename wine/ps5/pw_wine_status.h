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
 * That address is the ntdll syscall thunk, not the RtlTimeToTimeFields fault and not
 * the module that invoked the thunk. guest is the address at the guest stack pointer
 * saved after the entry pop, which is that thunk's caller. handle is the first
 * argument (r10). arg2 is the second argument saved at entry: an event handle for
 * NtWriteFile and NtDeviceIoControlFile, the completion key for NtSetIoCompletion.
 *
 * pw_type_is_iocompletion and pw_completion_post are the server's decision for
 * NtSetIoCompletion. Windows accepts an IoCompletion. A second image of the server
 * has a different ops pointer and the same type name. A file, pipe or device that
 * already has a port bound is posted to that port: the guest's handle is not the
 * port object, and returning 0xC0000024 is the pointer RtlTimeToTimeFields reads.
 * An event, a section, and a file with no bound port stay a mismatch, which is
 * what Windows returns for those.
 */
#include <stdio.h>
#include <string.h>

enum {
    PW_COMPLETION_REJECT = 0,
    PW_COMPLETION_PORT = 1,
    PW_COMPLETION_BOUND = 2
};

/* Type name bytes, not a C string. IoCompletionReserve is a different object. */
static inline int pw_type_is_iocompletion(const void *name, unsigned bytes)
{
    static const unsigned short io[] = {
        'I','o','C','o','m','p','l','e','t','i','o','n'
    };

    if (!name || bytes != sizeof(io)) return 0;
    return memcmp(name, io, sizeof(io)) == 0;
}

/* type_is_port wins. has_bound_port is an fd whose completion pointer is set. */
static inline int pw_completion_post(int type_is_port, int has_bound_port)
{
    if (type_is_port) return PW_COMPLETION_PORT;
    if (has_bound_port) return PW_COMPLETION_BOUND;
    return PW_COMPLETION_REJECT;
}

static inline int pw_status_mismatch_line(char *out, size_t n, const char *syscall,
    unsigned id, unsigned long long arg2, unsigned long long caller,
    unsigned long long handle, unsigned long long guest, const char *type)
{
    if (!out || !n) return -1;
    return snprintf(out, n,
        "wine-ps5: status 0xc0000024 syscall=%s id=%#x arg2=%#llx caller=%#llx handle=%#llx guest=%#llx type=%s\n",
        (syscall && syscall[0]) ? syscall : "?", id, arg2, caller, handle, guest,
        (type && type[0]) ? type : "?");
}
#endif
