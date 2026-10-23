/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_UMIP_H
#define PW_WINE_UMIP_H
/*
 * SGDT, SIDT, SLDT, SMSW and STR. On Linux, CR4.UMIP makes these #GP in user
 * mode and the kernel's umip.c stores dummy values before Wine sees a signal.
 * This tree's ntdll unix signal handler has no emulate_* for them. A PS5 Zen 2
 * with UMIP and no kernel emulation delivers the fault instead. The values
 * match that Linux path: GDT base 0xfffffffffffe0000, IDT base
 * 0xffffffffffff0000, both limits 0, SMSW 0x80050033 (PE|MP|ET|NE|WP|AM|PG),
 * SLDT 0, STR 0x40. A 64-bit SGDT/SIDT stores 10 bytes. A memory SMSW, SLDT
 * or STR stores 2. A register form follows the operand size, and a 32-bit
 * register write is marked to zero-extend. Lock and rep prefixes are not
 * these instructions. XGETBV, RDTSCP, CPUID, RDPID and UD2 are not either.
 */
#include <string.h>

enum {
    PW_UMIP_NONE = 0,
    PW_UMIP_SGDT = 1,
    PW_UMIP_SIDT = 2,
    PW_UMIP_SMSW = 3,
    PW_UMIP_SLDT = 4,
    PW_UMIP_STR = 5
};

enum {
    PW_UMIP_SEG_NONE = 0,
    PW_UMIP_SEG_FS = 1,
    PW_UMIP_SEG_GS = 2
};

struct pw_umip_insn {
    unsigned char op;
    unsigned char length;
    unsigned char mem;
    unsigned char reg;
    unsigned char size;
    unsigned char zext;
    unsigned char addr32;
    unsigned char rip_rel;
    unsigned char base;
    unsigned char index;
    unsigned char scale;
    unsigned char seg;
    int disp;
    unsigned char data[10];
};

static inline const char *pw_umip_name(unsigned op)
{
    switch (op) {
    case PW_UMIP_SGDT: return "sgdt";
    case PW_UMIP_SIDT: return "sidt";
    case PW_UMIP_SMSW: return "smsw";
    case PW_UMIP_SLDT: return "sldt";
    case PW_UMIP_STR: return "str";
    default: return "no";
    }
}

static inline void pw_umip_put(struct pw_umip_insn *out, unsigned long long value, unsigned size)
{
    unsigned i;

    out->size = (unsigned char)size;
    memset(out->data, 0, sizeof(out->data));
    for (i = 0; i < size && i < sizeof(out->data); i++)
        out->data[i] = (unsigned char)(value >> (8 * i));
}

/* 1 when bytes are one of the five instructions. long_mode is the code segment. */
static inline int pw_umip_decode(const unsigned char *bytes, unsigned len, int long_mode,
                                struct pw_umip_insn *out)
{
    unsigned i, prefixes, opcode, modrm_at;
    unsigned char rex, has_66, has_67, seg;
    unsigned char modrm, mod, reg, rm, sib;
    int has_sib, disp, rip_rel, base, index, scale, addr32;

    if (out) memset(out, 0, sizeof(*out));
    if (!bytes || !out || !len) return 0;

    rex = 0;
    has_66 = 0;
    has_67 = 0;
    seg = PW_UMIP_SEG_NONE;
    prefixes = 0;
    i = 0;
    while (i < len && prefixes < 14) {
        unsigned char b = bytes[i];
        if (b == 0xf0 || b == 0xf2 || b == 0xf3) return 0;
        if (b == 0x66) has_66 = 1;
        else if (b == 0x67) has_67 = 1;
        else if (b == 0x64) seg = PW_UMIP_SEG_FS;
        else if (b == 0x65) seg = PW_UMIP_SEG_GS;
        else if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e) seg = PW_UMIP_SEG_NONE;
        else if (b >= 0x40 && b <= 0x4f) rex = b;
        else break;
        prefixes++;
        i++;
    }
    if (i + 2 >= len) return 0;
    if (bytes[i] != 0x0f) return 0;
    opcode = bytes[i + 1];
    if (opcode != 0x00 && opcode != 0x01) return 0;
    modrm_at = i + 2;
    modrm = bytes[modrm_at];
    mod = modrm >> 6;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;
    i = modrm_at + 1;

    if (opcode == 0x01) {
        if (reg == 0 || reg == 1) {
            if (mod == 3) return 0;
            out->op = (reg == 0) ? PW_UMIP_SGDT : PW_UMIP_SIDT;
        } else if (reg == 4) {
            out->op = PW_UMIP_SMSW;
        } else {
            return 0;
        }
    } else {
        if (reg == 0) out->op = PW_UMIP_SLDT;
        else if (reg == 1) out->op = PW_UMIP_STR;
        else return 0;
    }

    has_sib = 0;
    disp = 0;
    rip_rel = 0;
    base = 255;
    index = 255;
    scale = 1;
    addr32 = has_67 || !long_mode;
    sib = 0;

    if (mod == 3) {
        out->mem = 0;
        out->reg = (unsigned char)(rm | ((rex & 1) ? 8 : 0));
    } else {
        out->mem = 1;
        out->reg = 0;
        if (rm == 4) {
            if (i >= len) return 0;
            sib = bytes[i++];
            has_sib = 1;
        }
        if (mod == 1) {
            if (i >= len) return 0;
            disp = (signed char)bytes[i++];
        } else if (mod == 2 || (mod == 0 && ((has_sib ? (sib & 7) : rm) == 5))) {
            if (i + 4 > len) return 0;
            disp = (int)(bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) | (bytes[i + 3] << 24));
            i += 4;
        }
        if (mod == 0 && !has_sib && rm == 5) {
            if (long_mode && !has_67) rip_rel = 1;
        } else if (has_sib) {
            if (!(mod == 0 && (sib & 7) == 5))
                base = (sib & 7) | ((rex & 1) ? 8 : 0);
            if ((sib & 0x38) != 0x20 || (rex & 2))
                index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
            scale = 1 << (sib >> 6);
        } else {
            base = rm | ((rex & 1) ? 8 : 0);
        }
    }

    if (i > 15) return 0;
    out->length = (unsigned char)i;
    out->addr32 = (unsigned char)addr32;
    out->rip_rel = (unsigned char)rip_rel;
    out->base = (unsigned char)base;
    out->index = (unsigned char)index;
    out->scale = (unsigned char)scale;
    out->seg = seg;
    out->disp = disp;

    if (out->op == PW_UMIP_SGDT || out->op == PW_UMIP_SIDT) {
        unsigned long long base_addr = (out->op == PW_UMIP_SGDT) ? 0xfffffffffffe0000ULL
                                                                   : 0xffffffffffff0000ULL;
        unsigned size = long_mode ? 10 : 6;
        unsigned base_size = long_mode ? 8 : 4;

        out->size = (unsigned char)size;
        memset(out->data, 0, sizeof(out->data));
        pw_umip_put(out, base_addr, base_size);
        memmove(out->data + 2, out->data, base_size);
        out->data[0] = 0;
        out->data[1] = 0;
        out->size = (unsigned char)size;
    } else {
        unsigned long long value = 0;
        unsigned size;

        if (out->op == PW_UMIP_SMSW) value = 0x80050033ULL;
        else if (out->op == PW_UMIP_STR) value = 0x40ULL;
        if (out->mem) size = 2;
        else if (rex & 8) size = 8;
        else if (has_66) size = 2;
        else {
            size = 4;
            out->zext = 1;
        }
        pw_umip_put(out, value, size);
    }
    return 1;
}

static inline unsigned long long pw_umip_address(const struct pw_umip_insn *insn,
                                                 const unsigned long long regs[16],
                                                 unsigned long long rip)
{
    unsigned long long addr = 0;

    if (!insn) return 0;
    if (insn->rip_rel) {
        addr = rip + insn->length + (unsigned long long)(long long)insn->disp;
    } else {
        if (insn->base != 255 && insn->base < 16) addr += regs[insn->base];
        if (insn->index != 255 && insn->index < 16)
            addr += regs[insn->index] * (unsigned)insn->scale;
        addr += (unsigned long long)(long long)insn->disp;
    }
    if (insn->addr32) addr &= 0xffffffffULL;
    return addr;
}

#endif
