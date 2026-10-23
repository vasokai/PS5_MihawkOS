/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Which win32u stubs return a status instead of aborting. */
#include "../wine/ps5/pw_wine_dcomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_int(const char *what, int got, int want)
{
    if (got == want) return;
    fprintf(stderr, "%s: got %#x want %#x\n", what, got, want);
    exit(1);
}

static void expect_status(const char *name, int applies, unsigned status)
{
    char what[128];

    snprintf(what, sizeof(what), "%s applies", name);
    expect_int(what, pw_dcomp_stub_applies(name), applies);
    if (!applies) return;
    snprintf(what, sizeof(what), "%s status", name);
    expect_int(what, (int)pw_dcomp_stub_status(name), (int)status);
}

int main(void)
{
    expect_int("null", pw_dcomp_stub_applies(NULL), 0);
    expect_int("empty", pw_dcomp_stub_applies(""), 0);
    expect_status("NtDCompositionCommitSynchronizationObject", 1, PW_DCOMP_SUCCESS);
    expect_status("NtDCompositionCommitChannel", 1, PW_DCOMP_SUCCESS);
    expect_status("NtDCompositionWaitForChannel", 1, PW_DCOMP_SUCCESS);
    expect_status("NtDCompositionSynchronize", 1, PW_DCOMP_SUCCESS);
    expect_status("NtDCompositionCreateChannel", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtDCompositionCreateSynchronizationObject", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtDCompositionGetFrameStatistics", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtDxgkSubmitPresentToHwQueue", 1, PW_DCOMP_SUCCESS);
    expect_status("NtDxgkGetProperties", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtDxgkIsFeatureEnabled", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtDxgkEnumAdapters3", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtFlipObjectPresentCancel", 1, PW_DCOMP_SUCCESS);
    expect_status("NtFlipObjectCreate", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtFlipObjectReadNextMessageToProducer", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtUserSetWindowCompositionAttribute", 1, PW_DCOMP_SUCCESS);
    expect_status("NtUserGetWindowCompositionAttribute", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtUserCreateDCompositionHwndTarget", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtVisualCaptureBits", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_status("NtBindCompositionSurface", 1, PW_DCOMP_SUCCESS);
    expect_status("NtQueryCompositionSurfaceStatistics", 1, PW_DCOMP_NOT_IMPLEMENTED);
    expect_int("NtSetIoCompletion", pw_dcomp_stub_applies("NtSetIoCompletion"), 0);
    expect_int("NtUserGetDC", pw_dcomp_stub_applies("NtUserGetDC"), 0);
    expect_int("NtUserGetMessage", pw_dcomp_stub_applies("NtUserGetMessage"), 0);
    expect_int("NtGdiBitBlt", pw_dcomp_stub_applies("NtGdiBitBlt"), 0);
    printf("dcomp-stub commit=%08x create=%08x\n",
           pw_dcomp_stub_status("NtDCompositionCommitSynchronizationObject"),
           pw_dcomp_stub_status("NtDCompositionCreateChannel"));
    return 0;
}
