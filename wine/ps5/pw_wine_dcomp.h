/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_DCOMP_H
#define PW_WINE_DCOMP_H
/*
 * DirectComposition and Dxgk syscall stubs.
 *
 * Wine's win32u lists these as `@ stub -syscall`. The unix stub raises
 * EXCEPTION_WINE_STUB, and ntdll prints "unimplemented function" and aborts
 * the process. Windows 10 build 19045 (22H2) already has every one of these
 * names in that spec; nothing is missing from the export list. Upstream Wine
 * and this tree's Proton pin abort the same way.
 *
 * A name that applies returns a status instead. Create, Open, Duplicate,
 * Query, Get, Enum, Check, Is, Capture and Read write a handle or a buffer, so
 * they return STATUS_NOT_IMPLEMENTED (0xC0000002) and the caller does not
 * read that buffer. A commit, wait, notify, set or retire has no compositor
 * waiting on it and returns STATUS_SUCCESS. NtDCompositionCommitSynchronizationObject
 * is the call WowB reached, and it is in that second set.
 *
 * NtUser, NtGdi and ntdll services stay on the aborting stub.
 */
#include <string.h>

#define PW_DCOMP_SUCCESS 0u
#define PW_DCOMP_NOT_IMPLEMENTED 0xC0000002u

static inline int pw_dcomp_stub_applies(const char *name)
{
    if (!name || !name[0]) return 0;
    if (!strncmp(name, "NtDxgk", 6)) return 1;
    if (!strncmp(name, "NtFlipObject", 12)) return 1;
    if (strstr(name, "Composition")) return 1;
    if (!strcmp(name, "NtVisualCaptureBits")) return 1;
    return 0;
}

/* The call fills a handle or a buffer the caller will read. */
static inline int pw_dcomp_writes_output(const char *name)
{
    if (!name) return 0;
    if (strstr(name, "Create")) return 1;
    if (strstr(name, "Open")) return 1;
    if (strstr(name, "Duplicate")) return 1;
    if (strstr(name, "Query")) return 1;
    if (strstr(name, "Get")) return 1;
    if (strstr(name, "Enum")) return 1;
    if (strstr(name, "Check")) return 1;
    if (strstr(name, "Is")) return 1;
    if (strstr(name, "Capture")) return 1;
    if (strstr(name, "Read")) return 1;
    return 0;
}

static inline unsigned pw_dcomp_stub_status(const char *name)
{
    if (!pw_dcomp_stub_applies(name)) return PW_DCOMP_NOT_IMPLEMENTED;
    if (pw_dcomp_writes_output(name)) return PW_DCOMP_NOT_IMPLEMENTED;
    return PW_DCOMP_SUCCESS;
}
#endif
