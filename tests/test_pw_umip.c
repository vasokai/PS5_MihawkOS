/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_umip.h"
#include <stdio.h>
#include <string.h>

static int failed;

static void expect(const char *what, int cond)
{
    if (cond) return;
    fprintf(stderr, "fail: %s\n", what);
    failed = 1;
}

static void expect_mem(const char *what, const unsigned char *got, const unsigned char *want, unsigned n)
{
    if (n && memcmp(got, want, n) == 0) return;
    fprintf(stderr, "fail: %s\n", what);
    failed = 1;
}

static int decode(const unsigned char *b, unsigned n, int long_mode, struct pw_umip_insn *out)
{
    return pw_umip_decode(b, n, long_mode, out);
}

int main(void)
{
    struct pw_umip_insn insn;
    unsigned long long regs[16];
    static const unsigned char sgdt_rax[] = { 0x0f, 0x01, 0x00 };
    static const unsigned char sgdt_sib[] = { 0x0f, 0x01, 0x44, 0x8b, 0x08 };
    static const unsigned char sidt_rip[] = { 0x0f, 0x01, 0x0d, 0x10, 0x00, 0x00, 0x00 };
    static const unsigned char smsw_eax[] = { 0x0f, 0x01, 0xe0 };
    static const unsigned char smsw_ax[] = { 0x66, 0x0f, 0x01, 0xe0 };
    static const unsigned char smsw_rax[] = { 0x48, 0x0f, 0x01, 0xe0 };
    static const unsigned char smsw_rbx[] = { 0x0f, 0x01, 0x23 };
    static const unsigned char sldt_eax[] = { 0x0f, 0x00, 0xc0 };
    static const unsigned char sldt_r8[] = { 0x41, 0x0f, 0x00, 0xc0 };
    static const unsigned char str_rcx[] = { 0x0f, 0x00, 0x09 };
    static const unsigned char str_rax[] = { 0x48, 0x0f, 0x00, 0xc8 };
    static const unsigned char sgdt_fs[] = { 0x64, 0x0f, 0x01, 0x00 };
    static const unsigned char sgdt_eax[] = { 0x67, 0x0f, 0x01, 0x00 };
    static const unsigned char locked[] = { 0xf0, 0x0f, 0x01, 0x00 };
    static const unsigned char repeated[] = { 0xf3, 0x0f, 0x01, 0x00 };
    static const unsigned char xgetbv[] = { 0x0f, 0x01, 0xd0 };
    static const unsigned char rdtscp[] = { 0x0f, 0x01, 0xf9 };
    static const unsigned char cpuid[] = { 0x0f, 0xa2 };
    static const unsigned char syscall_op[] = { 0x0f, 0x05 };
    static const unsigned char ud2[] = { 0x0f, 0x0b };
    static const unsigned char int3[] = { 0xcc };
    static const unsigned char in_dx[] = { 0xec };
    static const unsigned char rdpid[] = { 0xf3, 0x0f, 0xc7, 0xf8 };
    static const unsigned char sgdt_data[] = { 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const unsigned char sidt_data[] = { 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const unsigned char sgdt32[] = { 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff };
    static const unsigned char smsw32[] = { 0x33, 0x00, 0x05, 0x80 };
    static const unsigned char smsw16[] = { 0x33, 0x00 };
    static const unsigned char str16[] = { 0x40, 0x00 };
    unsigned char none[8];

    memset(regs, 0, sizeof(regs));
    expect("null", decode(NULL, 1, 1, &insn) == 0);
    expect("empty", decode(sgdt_rax, 0, 1, &insn) == 0);
    memset(none, 0x5a, sizeof(none));
    expect("no out", pw_umip_decode(sgdt_rax, sizeof(sgdt_rax), 1, NULL) == 0);

    expect("sgdt", decode(sgdt_rax, sizeof(sgdt_rax), 1, &insn) == 1);
    expect("sgdt op", insn.op == PW_UMIP_SGDT && insn.mem && insn.length == 3 && insn.size == 10);
    expect("sgdt base", insn.base == 0 && insn.index == 255 && !insn.rip_rel && !insn.addr32);
    expect_mem("sgdt data", insn.data, sgdt_data, 10);
    expect("name", strcmp(pw_umip_name(insn.op), "sgdt") == 0);
    regs[0] = 0x1000;
    expect("sgdt addr", pw_umip_address(&insn, regs, 0) == 0x1000);

    expect("sib", decode(sgdt_sib, sizeof(sgdt_sib), 1, &insn) == 1);
    expect("sib fields", insn.base == 3 && insn.index == 1 && insn.scale == 4 && insn.disp == 8 && insn.length == 5);
    regs[3] = 0x2000;
    regs[1] = 3;
    expect("sib addr", pw_umip_address(&insn, regs, 0) == 0x2000 + 12 + 8);

    expect("sidt", decode(sidt_rip, sizeof(sidt_rip), 1, &insn) == 1);
    expect("sidt rel", insn.op == PW_UMIP_SIDT && insn.rip_rel && insn.length == 7 && insn.disp == 0x10);
    expect_mem("sidt data", insn.data, sidt_data, 10);
    expect("sidt addr", pw_umip_address(&insn, regs, 0x1000) == 0x1000 + 7 + 0x10);

    expect("smsw eax", decode(smsw_eax, sizeof(smsw_eax), 1, &insn) == 1);
    expect("smsw eax form", insn.op == PW_UMIP_SMSW && !insn.mem && insn.reg == 0 && insn.size == 4 && insn.zext);
    expect_mem("smsw eax data", insn.data, smsw32, 4);

    expect("smsw ax", decode(smsw_ax, sizeof(smsw_ax), 1, &insn) == 1);
    expect("smsw ax form", insn.size == 2 && !insn.zext && insn.length == 4);
    expect_mem("smsw ax data", insn.data, smsw16, 2);

    expect("smsw rax", decode(smsw_rax, sizeof(smsw_rax), 1, &insn) == 1);
    expect("smsw rax form", insn.size == 8 && insn.reg == 0 && !insn.zext);
    expect_mem("smsw rax low", insn.data, smsw32, 4);
    expect("smsw rax high", insn.data[4] == 0 && insn.data[7] == 0);

    expect("smsw mem", decode(smsw_rbx, sizeof(smsw_rbx), 1, &insn) == 1);
    expect("smsw mem form", insn.mem && insn.size == 2 && insn.base == 3);
    expect_mem("smsw mem data", insn.data, smsw16, 2);

    expect("sldt", decode(sldt_eax, sizeof(sldt_eax), 1, &insn) == 1);
    expect("sldt form", insn.op == PW_UMIP_SLDT && !insn.mem && insn.reg == 0 && insn.zext && insn.data[0] == 0);

    expect("sldt r8", decode(sldt_r8, sizeof(sldt_r8), 1, &insn) == 1);
    expect("sldt r8 reg", insn.reg == 8 && insn.length == 4);

    expect("str mem", decode(str_rcx, sizeof(str_rcx), 1, &insn) == 1);
    expect("str mem form", insn.op == PW_UMIP_STR && insn.mem && insn.size == 2 && insn.base == 1);
    expect_mem("str mem data", insn.data, str16, 2);

    expect("str rax", decode(str_rax, sizeof(str_rax), 1, &insn) == 1);
    expect("str rax form", insn.size == 8 && insn.reg == 0);
    expect_mem("str rax data", insn.data, str16, 2);

    expect("fs", decode(sgdt_fs, sizeof(sgdt_fs), 1, &insn) == 1);
    expect("fs seg", insn.seg == PW_UMIP_SEG_FS && insn.base == 0 && insn.length == 4);

    expect("addr32", decode(sgdt_eax, sizeof(sgdt_eax), 1, &insn) == 1);
    expect("addr32 flag", insn.addr32 && insn.base == 0);
    regs[0] = 0x100000001ULL;
    expect("addr32 mask", pw_umip_address(&insn, regs, 0) == 1);

    expect("compat sgdt", decode(sgdt_rax, sizeof(sgdt_rax), 0, &insn) == 1);
    expect("compat size", insn.size == 6 && insn.addr32);
    expect_mem("compat data", insn.data, sgdt32, 6);

    expect("lock", decode(locked, sizeof(locked), 1, &insn) == 0);
    expect("rep", decode(repeated, sizeof(repeated), 1, &insn) == 0);
    expect("xgetbv", decode(xgetbv, sizeof(xgetbv), 1, &insn) == 0);
    expect("rdtscp", decode(rdtscp, sizeof(rdtscp), 1, &insn) == 0);
    expect("cpuid", decode(cpuid, sizeof(cpuid), 1, &insn) == 0);
    expect("syscall", decode(syscall_op, sizeof(syscall_op), 1, &insn) == 0);
    expect("ud2", decode(ud2, sizeof(ud2), 1, &insn) == 0);
    expect("int3", decode(int3, sizeof(int3), 1, &insn) == 0);
    expect("in", decode(in_dx, sizeof(in_dx), 1, &insn) == 0);
    expect("rdpid", decode(rdpid, sizeof(rdpid), 1, &insn) == 0);
    expect("short", decode(sgdt_rax, 2, 1, &insn) == 0);
    expect("unknown name", strcmp(pw_umip_name(0), "no") == 0);

    if (failed) return 1;
    return 0;
}
