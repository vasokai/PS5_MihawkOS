/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_GP_H
#define PW_WINE_GP_H
/*
 * #GP(0) has no fault address. Wine's x86_64 signal path reports a read of
 * 0xffffffffffffffff. WoW Classic's loader stores 11 bytes over the
 * NtSetIoCompletion thunk and falls into the test instruction's displacement,
 * 08 03, which is or byte [rbx], al. rbx is non-canonical, so the CPU raises
 * #GP(0). The handler matches a write of that rbx. Register forms, int 2e,
 * syscall and privileged opcodes are not memory operands and stay on the
 * existing path.
 *
 * regs[] is rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15. rip is the
 * address of the first byte. The address does not include an fs/gs base;
 * seg names which one the caller adds.
 */
enum {
    PW_GP_SEG_NONE = 0,
    PW_GP_SEG_FS = 1,
    PW_GP_SEG_GS = 2
};

struct pw_gp_mem {
    unsigned char write;
    unsigned char seg;
    unsigned long long address;
};

static inline int pw_gp_classify( unsigned op, int two, unsigned reg, int has_66, int rex_w,
                                  int *write, unsigned *imm )
{
    *write = 0;
    *imm = 0;
    if (!two) {
        if (op <= 0x3f && (op & 7) <= 3 && (op & 4) == 0) {
            *write = (op & 2) == 0;
            return 1;
        }
        if (op == 0x63 || op == 0x69 || op == 0x6b) {
            if (op == 0x69) *imm = (has_66 && !rex_w) ? 2 : 4;
            if (op == 0x6b) *imm = 1;
            return 1;
        }
        if (op == 0x80 || op == 0x82 || op == 0x83) {
            *write = reg != 7;
            *imm = 1;
            return 1;
        }
        if (op == 0x81) {
            *write = reg != 7;
            *imm = (has_66 && !rex_w) ? 2 : 4;
            return 1;
        }
        if (op == 0x86 || op == 0x87 || op == 0x88 || op == 0x89 || op == 0x8c) {
            *write = 1;
            return 1;
        }
        if (op == 0x8a || op == 0x8b || op == 0x8e) return 1;
        if (op == 0x8f) {
            if (reg != 0) return 0;
            *write = 1;
            return 1;
        }
        if (op == 0xc0 || op == 0xc1) {
            *write = 1;
            *imm = 1;
            return 1;
        }
        if (op == 0xc6) {
            if (reg != 0) return 0;
            *write = 1;
            *imm = 1;
            return 1;
        }
        if (op == 0xc7) {
            if (reg != 0) return 0;
            *write = 1;
            *imm = (has_66 && !rex_w) ? 2 : 4;
            return 1;
        }
        if (op >= 0xd0 && op <= 0xd3) {
            *write = 1;
            return 1;
        }
        if (op == 0xf6 || op == 0xf7) {
            if (reg == 1) return 0;
            *write = (reg == 2 || reg == 3);
            if (reg == 0) {
                if (op == 0xf6) *imm = 1;
                else *imm = (has_66 && !rex_w) ? 2 : 4;
            }
            return 1;
        }
        if (op == 0xfe) {
            if (reg > 1) return 0;
            *write = 1;
            return 1;
        }
        if (op == 0xff) {
            if (reg > 6) return 0;
            *write = reg <= 1;
            return 1;
        }
        return 0;
    }
    if (op == 0xa3) return 1;
    if (op == 0xab || op == 0xb3 || op == 0xbb) {
        *write = 1;
        return 1;
    }
    if (op == 0xb6 || op == 0xb7 || op == 0xbe || op == 0xbf) return 1;
    return 0;
}

/* 1 when bytes begin with one memory operand. long_mode is the code segment. */
static inline int pw_gp_memory( const unsigned char *bytes, unsigned len, int long_mode,
                                const unsigned long long regs[16], unsigned long long rip,
                                struct pw_gp_mem *out )
{
    unsigned i, prefixes, opcode, second, imm, end;
    unsigned char rex, has_66, has_67, seg, modrm, mod, reg, rm, sib;
    int has_sib, two, write, rex_w, addr32, rip_rel, base, index, scale, disp;
    unsigned long long addr;

    if (!bytes || !regs || !out || !len) return 0;
    rex = 0;
    has_66 = 0;
    has_67 = 0;
    seg = PW_GP_SEG_NONE;
    prefixes = 0;
    i = 0;
    while (i < len && prefixes < 14) {
        unsigned char b = bytes[i];
        if (b == 0xf0) {
            /* lock does not change the address */
        } else if (b == 0xf2 || b == 0xf3) {
            return 0;
        } else if (b == 0x66) {
            has_66 = 1;
        } else if (b == 0x67) {
            has_67 = 1;
        } else if (b == 0x64) {
            seg = PW_GP_SEG_FS;
        } else if (b == 0x65) {
            seg = PW_GP_SEG_GS;
        } else if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e) {
            seg = PW_GP_SEG_NONE;
        } else if (b >= 0x40 && b <= 0x4f) {
            rex = b;
        } else {
            break;
        }
        prefixes++;
        i++;
    }
    if (i >= len || i > 15) return 0;
    opcode = bytes[i++];
    two = 0;
    second = 0;
    if (opcode == 0x0f) {
        if (i >= len) return 0;
        second = bytes[i++];
        two = 1;
        opcode = second;
    }
    addr32 = has_67 || !long_mode;
    if (!two && opcode >= 0xa0 && opcode <= 0xa3) {
        unsigned n = addr32 ? 4 : 8;
        unsigned long long moff = 0;
        unsigned k;

        if (i + n > len || i + n > 15) return 0;
        for (k = 0; k < n; k++) moff |= (unsigned long long)bytes[i + k] << (8 * k);
        out->write = opcode >= 0xa2;
        out->seg = seg;
        out->address = moff;
        return 1;
    }
    if (i >= len) return 0;
    modrm = bytes[i++];
    mod = modrm >> 6;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;
    rex_w = (rex & 8) != 0;
    if (!pw_gp_classify( opcode, two, reg, has_66, rex_w, &write, &imm )) return 0;
    if (mod == 3) return 0;

    has_sib = 0;
    disp = 0;
    rip_rel = 0;
    base = -1;
    index = -1;
    scale = 1;
    sib = 0;
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
        disp = (int)(bytes[i] | ((unsigned)bytes[i + 1] << 8) |
                     ((unsigned)bytes[i + 2] << 16) | ((unsigned)bytes[i + 3] << 24));
        i += 4;
    }
    if (mod == 0 && !has_sib && rm == 5) {
        if (long_mode && !has_67) rip_rel = 1;
        else base = -1;
    } else if (has_sib) {
        if (!(mod == 0 && (sib & 7) == 5))
            base = (sib & 7) | ((rex & 1) ? 8 : 0);
        if ((sib & 0x38) != 0x20 || (rex & 2))
            index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
        scale = 1 << (sib >> 6);
    } else {
        base = rm | ((rex & 1) ? 8 : 0);
    }
    if (i + imm > len || i + imm > 15) return 0;
    end = i + imm;

    addr = 0;
    if (rip_rel) {
        addr = rip + end + (unsigned long long)(long long)disp;
    } else {
        if (base >= 0 && base < 16) addr += regs[base];
        if (index >= 0 && index < 16) addr += regs[index] * (unsigned)scale;
        addr += (unsigned long long)(long long)disp;
    }
    if (addr32 && !rip_rel) addr &= 0xffffffffULL;
    out->write = write ? 1 : 0;
    out->seg = seg;
    out->address = addr;
    return 1;
}

#endif
