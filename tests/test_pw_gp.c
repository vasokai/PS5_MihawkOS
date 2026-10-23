/* SPDX-License-Identifier: GPL-3.0-or-later */
/* #GP(0) memory operand: WoW's or byte [rbx], al at ntdll+0x5834b. */
#include "../wine/ps5/pw_wine_gp.h"
#include <stdio.h>
#include <string.h>

static int failed;

static void expect(const char *what, int cond)
{
    if (cond) return;
    fprintf(stderr, "fail: %s\n", what);
    failed = 1;
}

static void zero(unsigned long long regs[16])
{
    memset(regs, 0, 16 * sizeof(regs[0]));
}

static void check(const char *what, const unsigned char *bytes, unsigned len,
                  unsigned long long rip, const unsigned long long regs[16],
                  int want_ok, int want_write, unsigned long long want_addr, unsigned want_seg)
{
    struct pw_gp_mem op;
    int ok;

    memset(&op, 0x5a, sizeof(op));
    ok = pw_gp_memory(bytes, len, 1, regs, rip, &op);
    if (!want_ok) {
        expect(what, ok == 0);
        return;
    }
    if (!ok) {
        fprintf(stderr, "fail: %s did not decode\n", what);
        failed = 1;
        return;
    }
    if (op.write != (unsigned char)want_write || op.address != want_addr || op.seg != want_seg) {
        fprintf(stderr, "fail: %s write=%u addr=%#llx seg=%u want write=%d addr=%#llx seg=%u\n",
                what, op.write, op.address, op.seg, want_write, want_addr, want_seg);
        failed = 1;
    }
}

int main(void)
{
    unsigned long long regs[16];
    /* The 11-byte store then the leftover displacement of test [0x7ffe0308], 1. */
    static const unsigned char probe[] = {
        0x5c, 0x24, 0x30, 0xba, 0x00, 0x04, 0x00, 0x00, 0x48, 0x89, 0xd9,
        0x08, 0x03, 0xfe, 0x7f, 0x01, 0x75, 0x03, 0x0f, 0x05, 0xc3, 0xcd, 0x2e
    };
    static const unsigned char test_abs[] = { 0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe, 0x7f, 0x01 };
    static const unsigned char mov_rip[] = { 0x89, 0x05, 0x10, 0x00, 0x00, 0x00 };
    static const unsigned char cmp_rip[] = {
        0x81, 0x3d, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    static const unsigned char sib[] = { 0x88, 0x04, 0x8b };
    static const unsigned char disp8[] = { 0x88, 0x43, 0x08 };
    static const unsigned char rex[] = { 0x41, 0x08, 0x03 };
    static const unsigned char regform[] = { 0x08, 0xc3 };
    static const unsigned char int2e[] = { 0xcd, 0x2e };
    static const unsigned char syscall_op[] = { 0x0f, 0x05 };
    static const unsigned char hlt[] = { 0xf4 };
    static const unsigned char locked[] = { 0xf0, 0x08, 0x03 };
    static const unsigned char fs_or[] = { 0x64, 0x08, 0x03 };
    static const unsigned char moff[] = {
        0xa2, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00
    };

    zero(regs);
    regs[3] = 0xffffffffffffffffULL;
    check("probe at +11", probe + 11, sizeof(probe) - 11, 0xdfffd6834bULL, regs,
          1, 1, 0xffffffffffffffffULL, PW_GP_SEG_NONE);
    regs[3] = 0;
    check("probe null rbx", probe + 11, 2, 0xdfffd6834bULL, regs, 1, 1, 0, PW_GP_SEG_NONE);

    zero(regs);
    check("test absolute is a read", test_abs, sizeof(test_abs), 0x1000, regs,
          1, 0, 0x7ffe0308ULL, PW_GP_SEG_NONE);
    check("mov [rip+0x10]", mov_rip, sizeof(mov_rip), 0x2000, regs,
          1, 1, 0x2000 + 6 + 0x10, PW_GP_SEG_NONE);
    check("cmp [rip+0x10]", cmp_rip, sizeof(cmp_rip), 0x3000, regs,
          1, 0, 0x3000 + 10 + 0x10, PW_GP_SEG_NONE);

    zero(regs);
    regs[3] = 0x100;
    regs[1] = 0x10;
    check("sib [rbx+rcx*4]", sib, sizeof(sib), 0, regs, 1, 1, 0x140, PW_GP_SEG_NONE);
    check("disp8 [rbx+8]", disp8, sizeof(disp8), 0, regs, 1, 1, 0x108, PW_GP_SEG_NONE);
    regs[11] = 0x55;
    check("rex [r11]", rex, sizeof(rex), 0, regs, 1, 1, 0x55, PW_GP_SEG_NONE);

    check("register form", regform, sizeof(regform), 0, regs, 0, 0, 0, 0);
    check("int 2e", int2e, sizeof(int2e), 0, regs, 0, 0, 0, 0);
    check("syscall", syscall_op, sizeof(syscall_op), 0, regs, 0, 0, 0, 0);
    check("hlt", hlt, sizeof(hlt), 0, regs, 0, 0, 0, 0);
    check("lock or [rbx]", locked, sizeof(locked), 0, regs, 1, 1, regs[3], PW_GP_SEG_NONE);
    check("fs or [rbx]", fs_or, sizeof(fs_or), 0, regs, 1, 1, regs[3], PW_GP_SEG_FS);
    check("mov moffs", moff, sizeof(moff), 0, regs, 1, 1, 0x12345678ULL, PW_GP_SEG_NONE);
    check("empty", probe, 0, 0, regs, 0, 0, 0, 0);

    if (failed) return 1;
    return 0;
}
