#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a built ntdll (and, if given, win32u) against Windows 10 build 19045 syscall numbers.

Wine on this tree reports 10.0.19045 (Windows 10 22H2). On x64 that build, and on 21H2 / 19044,
syscall 0xe9 is NtFlushInstructionCache and NtSetIoCompletion is 0x1a3. The generated x64 table
matches that, with two pins: NtSetIoCompletion stays 0xe9, Forever's hole-filled id, because the
WoW loader enters its pop-rsp probe when the export immediate is 0x1a3. NtFlushInstructionCache
moves to 0x1da, past the Windows table, and 0x1a3 is left empty. Every other implemented name
keeps its 19045 number. A guest that issues the Windows number 0xe9 enters NtSetIoCompletion;
a guest that issues 0x1a3 gets an empty slot.

The Windows tables are j00ru's, fetched 2026-10-23, stored beside this script. Nothing is
downloaded at check time.

  tools/check-syscall-numbers.py --self-test
  tools/check-syscall-numbers.py --ntsyscalls dlls/ntdll/ntsyscalls.h
  tools/check-syscall-numbers.py --pe ntdll.dll --unix ntdll.prx.elf

--pe is the PE ntdll (lib/wine/x86_64-windows/ntdll.dll). --unix is the unstripped Unix
ntdll (ntdll.so or ntdll.prx.elf) whose KeServiceDescriptorTable the dispatcher indexes.
Both are required for a pass: the stub's mov eax and the dispatch slot have to name the
same function, and that function has to be the Windows one. A stripped Unix image has no
symbol names, so the dispatcher half cannot be checked and the run fails.

Exit 0 is a pass. Exit 1 is a mismatch. Exit 2 is a usage or parse error.
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLES = Path(__file__).resolve().parent / 'syscall-numbers'

# ELF64
ET_DYN = 3
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_DYNSYM = 11
SHN_UNDEF = 0
STT_FUNC = 2
R_X86_64_64 = 1
R_X86_64_GLOB_DAT = 6
R_X86_64_RELATIVE = 8

# PE
IMAGE_DOS_SIGNATURE = 0x5A4D
IMAGE_NT_SIGNATURE = 0x00004550
IMAGE_FILE_MACHINE_I386 = 0x14C
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_NT_OPTIONAL_HDR32_MAGIC = 0x10B
IMAGE_NT_OPTIONAL_HDR64_MAGIC = 0x20B


def load_table(name):
    path = TABLES / name
    data = json.loads(path.read_text())
    syscalls = {key: int(value) for key, value in data['syscalls'].items()}
    by_id = {}
    for func, number in syscalls.items():
        if number in by_id:
            raise SystemExit(f'{path.name}: id {number:#x} is both {by_id[number]} and {func}')
        by_id[number] = func
    data['syscalls'] = syscalls
    data['by_id'] = by_id
    return data


def windows_table_for(kind, machine):
    """kind is 'ntdll' or 'win32u'. machine is a PE machine constant, or 'header'."""
    if kind == 'win32u':
        if machine == IMAGE_FILE_MACHINE_I386:
            raise SystemExit('this tree has no Windows x86 win32k table; the crash is the x64 ntdll')
        return load_table('win10-19045-win32k-x64.json')
    if machine == IMAGE_FILE_MACHINE_I386:
        return load_table('win10-19045-x86.json')
    return load_table('win10-19045-x64.json')


def parse_ntsyscalls(text):
    """64-bit SYSCALL_ENTRY list. The _WIN64 half is the one the Unix dispatcher compiles."""
    match = re.search(r'#ifdef _WIN64\b(.*?)#else\b', text, re.S)
    body = match.group(1) if match else text
    found = []
    for entry in re.finditer(r'SYSCALL_ENTRY\w*\(\s*(0x[0-9a-fA-F]+)\s*,\s*(\w+)\s*,', body):
        found.append((int(entry.group(1), 16), entry.group(2)))
    if not found:
        raise ValueError('no SYSCALL_ENTRY lines')
    return found


# WoW Classic's loader enters the pop-rsp probe when the NtSetIoCompletion export
# immediate is the 19045 number 0x1a3. Forever's hole-fill leaves that export at
# 0xe9. NtFlushInstructionCache, which 19045 puts at 0xe9, moves past the table.
NTDLL_PIN = {
    'NtSetIoCompletion': 0xe9,
    'NtFlushInstructionCache': 0x1da,
}
NTDLL_PIN_EMPTY = 0x1a3


def ntdll_pins(table):
    if table.get('file') == 'nt64.csv':
        return NTDLL_PIN
    return {}


def check_entries(entries, table):
    """entries is (id, name) in dispatcher order. Returns a list of error strings."""
    errors = []
    seen = {}
    windows = table['syscalls']
    by_id = table['by_id']
    pins = ntdll_pins(table)
    matched = wine_only = pinned = 0
    for number, name in entries:
        if number in seen:
            errors.append(f'duplicate id {number:#x}: {seen[number]} and {name}')
        seen[number] = name
        if name in pins:
            want = pins[name]
            if number != want:
                errors.append(
                    f'{name} stub/slot {number:#x}, pinned {want:#x} '
                    f'(Windows {table["build"]} {table["arch"]} {windows[name]:#x})')
            else:
                pinned += 1
            continue
        if name in windows:
            want = windows[name]
            if number != want:
                errors.append(f'{name} stub/slot {number:#x}, Windows {table["build"]} {table["arch"]} wants {want:#x} ({by_id.get(want, "?")})')
            else:
                matched += 1
        else:
            wine_only += 1
            if number in by_id:
                errors.append(f'Wine-only {name} occupies Windows id {number:#x} ({by_id[number]})')
            if number >= 0x1000 and table['arch'] != 'win32k':
                errors.append(f'{name} id {number:#x} collides with the win32u table bit (bit 12)')
    if pins:
        owner = seen.get(NTDLL_PIN_EMPTY)
        if owner:
            errors.append(f'{NTDLL_PIN_EMPTY:#x} is {owner}; that Windows NtSetIoCompletion immediate stays empty')
        if len(entries) > 200:
            for name in pins:
                if name not in seen.values():
                    errors.append(f'{name} missing; pinned id is {pins[name]:#x}')
    # A Windows function Wine implements under some other number was already reported.
    # A Windows id with no entry is a hole. Holes must not be some other function; the
    # duplicate/occupation checks above cover that. Count them for the summary.
    holes = [number for number in by_id if number not in seen]
    return errors, {
        'matched': matched,
        'pinned': pinned,
        'wine_only': wine_only,
        'entries': len(entries),
        'windows': len(windows),
        'holes': len(holes),
        'max_id': max(seen) if seen else -1,
    }


def u16(buf, off):
    return struct.unpack_from('<H', buf, off)[0]


def u32(buf, off):
    return struct.unpack_from('<I', buf, off)[0]


def u64(buf, off):
    return struct.unpack_from('<Q', buf, off)[0]


def cstring(buf, off):
    end = buf.index(b'\0', off)
    return buf[off:end].decode('ascii', 'replace')


class PeImage:
    def __init__(self, blob):
        if len(blob) < 0x40 or u16(blob, 0) != IMAGE_DOS_SIGNATURE:
            raise ValueError('not a PE')
        lfanew = u32(blob, 0x3C)
        if u32(blob, lfanew) != IMAGE_NT_SIGNATURE:
            raise ValueError('not a PE')
        coff = lfanew + 4
        self.machine = u16(blob, coff)
        sections = u16(blob, coff + 2)
        opt_size = u16(blob, coff + 16)
        opt = coff + 20
        magic = u16(blob, opt)
        if magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC:
            self.bitness = 64
            data_dir = opt + 112
        elif magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC:
            self.bitness = 32
            data_dir = opt + 96
        else:
            raise ValueError(f'unknown optional magic {magic:#x}')
        self.sections = []
        sec = opt + opt_size
        for _ in range(sections):
            name = blob[sec:sec + 8].split(b'\0', 1)[0].decode('ascii', 'replace')
            virtual_size, virtual_addr, raw_size, raw_ptr = struct.unpack_from('<IIII', blob, sec + 8)
            self.sections.append((name, virtual_addr, virtual_size, raw_ptr, raw_size))
            sec += 40
        export_rva, export_size = struct.unpack_from('<II', blob, data_dir)
        self.blob = blob
        self.export_rva = export_rva
        self.export_size = export_size

    def rva_to_off(self, rva):
        for _name, virtual_addr, virtual_size, raw_ptr, raw_size in self.sections:
            span = max(virtual_size, raw_size)
            if virtual_addr <= rva < virtual_addr + span:
                return raw_ptr + (rva - virtual_addr)
        raise ValueError(f'RVA {rva:#x} is not in a section')

    def exports(self):
        """name -> ('stub', imm) or ('forward', target) or ('other', rva)."""
        if not self.export_rva:
            raise ValueError('no export directory')
        off = self.rva_to_off(self.export_rva)
        _flags, _stamp, _maj, _min, _name, base, nfuncs, nnames, funcs_rva, names_rva, ords_rva = struct.unpack_from('<IIHHIIIIIII', self.blob, off)
        funcs = self.rva_to_off(funcs_rva)
        names = self.rva_to_off(names_rva)
        ords = self.rva_to_off(ords_rva)
        found = {}
        for index in range(nnames):
            name_rva = u32(self.blob, names + 4 * index)
            name = cstring(self.blob, self.rva_to_off(name_rva))
            ordinal = u16(self.blob, ords + 2 * index)
            func_rva = u32(self.blob, funcs + 4 * ordinal)
            if self.export_rva <= func_rva < self.export_rva + self.export_size:
                found[name] = ('forward', cstring(self.blob, self.rva_to_off(func_rva)))
            else:
                found[name] = ('code', func_rva)
        return found

    def bytes_at_rva(self, rva, size):
        off = self.rva_to_off(rva)
        return self.blob[off:off + size]


def stub_immediate(blob, bitness):
    """The mov eax immediate of a Wine/Windows x64 or x86 syscall stub, or None."""
    if bitness == 64 and len(blob) >= 8 and blob[:3] == b'\x4c\x8b\xd1' and blob[3] == 0xB8:
        return struct.unpack_from('<I', blob, 4)[0]
    if bitness == 32 and len(blob) >= 5 and blob[0] == 0xB8:
        return struct.unpack_from('<I', blob, 1)[0]
    return None


def nt_name(name):
    if name.startswith('Zw') and len(name) > 2:
        return 'Nt' + name[2:]
    return name


def forward_target(text):
    # "NTDLL.NtClose" or "ntdll.NtClose"
    if '.' not in text:
        return None
    return text.split('.', 1)[1]


def classify_pe(image):
    exports = image.exports()
    nt = [name for name in exports if name.startswith('Nt')]
    user = [name for name in nt if name.startswith('NtUser') or name.startswith('NtGdi')]
    kind = 'win32u' if nt and len(user) * 2 > len(nt) else 'ntdll'
    return kind, exports


def pe_syscall_entries(image):
    """(id, Nt-name) for every syscall stub, following Zw forwarders."""
    _kind, exports = classify_pe(image)
    resolved = {}
    errors = []
    for name, (kind, payload) in exports.items():
        if not (name.startswith('Nt') or name.startswith('Zw')):
            continue
        if kind == 'forward':
            target = forward_target(payload)
            if not target:
                errors.append(f'{name} forward {payload!r} has no target')
                continue
            resolved.setdefault(name, ('forward', target))
            continue
        blob = image.bytes_at_rva(payload, 16)
        immediate = stub_immediate(blob, image.bitness)
        if immediate is None:
            resolved.setdefault(name, ('other', None))
            continue
        resolved[name] = ('stub', immediate)
    entries = []
    for name, (kind, payload) in sorted(resolved.items()):
        if kind == 'other':
            continue
        if kind == 'forward':
            target = resolved.get(payload)
            if target is None or target[0] != 'stub':
                errors.append(f'{name} forwards to {payload}, which is not a syscall stub')
                continue
            number = target[1]
        else:
            number = payload
        entries.append((number, nt_name(name) if name.startswith('Zw') else name))
    # Zw and Nt of the same function share a number; keep one row per Nt name.
    by_name = {}
    for number, name in entries:
        if name in by_name and by_name[name] != number:
            errors.append(f'{name} has two numbers {by_name[name]:#x} and {number:#x}')
        by_name[name] = number
    return errors, [(number, name) for name, number in by_name.items()]


class ElfImage:
    def __init__(self, blob):
        if blob[:4] != b'\x7fELF' or blob[4] != 2 or blob[5] != 1:
            raise ValueError('need an ELF64 little-endian image (unstripped ntdll.so or ntdll.prx.elf)')
        self.blob = blob
        self.e_type = u16(blob, 16)
        e_shoff = u64(blob, 40)
        e_shentsize = u16(blob, 58)
        e_shnum = u16(blob, 60)
        e_shstrndx = u16(blob, 62)
        self.sections = []
        for index in range(e_shnum):
            off = e_shoff + index * e_shentsize
            name_off, sh_type, _flags, addr, offset, size, link, _info, _align, entsize = struct.unpack_from('<IIQQQQIIQQ', blob, off)
            self.sections.append({
                'name_off': name_off, 'type': sh_type, 'addr': addr, 'offset': offset,
                'size': size, 'link': link, 'entsize': entsize,
            })
        shstr = self.sections[e_shstrndx]
        self.shstr = blob[shstr['offset']:shstr['offset'] + shstr['size']]
        for section in self.sections:
            section['name'] = cstring(self.shstr, section['name_off'])
        self.symbols = []
        self.relocs = {}  # r_offset -> (type, symbol_index, addend)
        for section in self.sections:
            if section['type'] in (SHT_SYMTAB, SHT_DYNSYM) and section['entsize']:
                strtab = self.sections[section['link']]
                strings = blob[strtab['offset']:strtab['offset'] + strtab['size']]
                for sym_off in range(0, section['size'], section['entsize']):
                    base = section['offset'] + sym_off
                    name_off, info, _other, shndx, value, size = struct.unpack_from('<IBBHQQ', blob, base)
                    name = cstring(strings, name_off) if name_off < len(strings) else ''
                    self.symbols.append({'name': name, 'info': info, 'shndx': shndx, 'value': value, 'size': size})
            if section['type'] == SHT_RELA and section['entsize']:
                for rel_off in range(0, section['size'], section['entsize']):
                    base = section['offset'] + rel_off
                    r_offset, r_info, r_addend = struct.unpack_from('<QQq', blob, base)
                    self.relocs[r_offset] = (r_info & 0xFFFFFFFF, r_info >> 32, r_addend)

    def file_off(self, va):
        for section in self.sections:
            if section['type'] != SHT_PROGBITS:
                continue
            if section['addr'] <= va < section['addr'] + section['size']:
                return section['offset'] + (va - section['addr'])
        raise ValueError(f'VA {va:#x} is not in a PROGBITS section')

    def symbol_named(self, name):
        found = [sym for sym in self.symbols if sym['name'] == name and sym['shndx'] != SHN_UNDEF]
        if not found:
            return None
        # Prefer a defined function or object with a non-zero size.
        found.sort(key=lambda sym: (sym['size'] == 0, (sym['info'] & 0xF) != STT_FUNC))
        return found[0]

    def name_at(self, va):
        best = None
        for sym in self.symbols:
            if sym['shndx'] == SHN_UNDEF or sym['value'] != va or not sym['name']:
                continue
            if sym['name'].startswith('.'):
                continue
            if best is None:
                best = sym
                continue
            # A function name beats a compiler local.
            if sym['name'].startswith('Nt') or sym['name'].startswith('Zw') or (sym['info'] & 0xF) == STT_FUNC:
                if not (best['name'].startswith('Nt') or (best['info'] & 0xF) == STT_FUNC):
                    best = sym
        return None if best is None else best['name']

    def reloc_target(self, va):
        reloc = self.relocs.get(va)
        if reloc is None:
            return None
        r_type, sym_index, addend = reloc
        if sym_index >= len(self.symbols):
            raise ValueError(f'reloc at {va:#x} has a bad symbol index')
        sym = self.symbols[sym_index]
        if r_type in (R_X86_64_64, R_X86_64_GLOB_DAT):
            target = sym['value'] + addend
        elif r_type == R_X86_64_RELATIVE:
            target = addend if self.e_type == ET_DYN else None
            if target is None:
                raise ValueError(f'R_X86_64_RELATIVE at {va:#x} in a non-ET_DYN image')
        else:
            raise ValueError(f'unhandled reloc type {r_type} at {va:#x}')
        # A reloc against the function symbol itself: trust its name when the addend is 0
        # and the symbol is not a section.
        if r_type in (R_X86_64_64, R_X86_64_GLOB_DAT) and addend == 0 and sym['name'] and not sym['name'].startswith('.'):
            return sym['name']
        name = self.name_at(target)
        if name is None and sym['name'] and not sym['name'].startswith('.'):
            return sym['name']
        return name

    def qword_at(self, va):
        off = self.file_off(va)
        return u64(self.blob, off)

    def dispatch_slots(self):
        """(limit, list of name-or-None indexed by syscall id) from the Unix table."""
        table = self.symbol_named('KeServiceDescriptorTable')
        array_symbol = None
        if table is not None:
            base = table['value']
            service = self.reloc_target(base)
            if service is None:
                pointer = self.qword_at(base)
                if pointer:
                    service_va = pointer
                else:
                    raise ValueError('KeServiceDescriptorTable[0].ServiceTable is empty and has no reloc')
            else:
                # reloc_target returns a name. Find that symbol's address. A section-relative
                # reloc returns the function name; the ServiceTable reloc points at the array.
                sym = self.symbol_named(service)
                if sym is None:
                    raise ValueError(f'ServiceTable reloc names {service}, which is not a symbol')
                service_va = sym['value']
            limit = self.qword_at(base + 16)
            array_va = service_va
        else:
            array_symbol = self.symbol_named('syscalls')
            if array_symbol is None or not array_symbol['size']:
                raise ValueError('no KeServiceDescriptorTable and no sized syscalls symbol; '
                                 'the Unix image looks stripped')
            array_va = array_symbol['value']
            if array_symbol['size'] % 8:
                raise ValueError(f'syscalls size {array_symbol["size"]} is not a pointer array')
            limit = array_symbol['size'] // 8
        if limit > 0x4000:
            raise ValueError(f'dispatch limit {limit:#x} is past the 0x4000 syscall id cap')
        slots = []
        for index in range(limit):
            slot = array_va + index * 8
            name = self.reloc_target(slot)
            if name is None:
                try:
                    stored = self.qword_at(slot)
                except ValueError:
                    stored = 0
                if stored:
                    name = self.name_at(stored)
                    if name is None:
                        raise ValueError(f'slot {index:#x} pointer {stored:#x} has no symbol')
            slots.append(name)
        return limit, slots


def dispatcher_entries(elf):
    _limit, slots = elf.dispatch_slots()
    return [(index, name) for index, name in enumerate(slots) if name]


def report(title, errors, summary):
    pinned = f', {summary["pinned"]} pinned' if summary.get('pinned') else ''
    print(f'{title}: {summary["matched"]} match Windows{pinned}, {summary["entries"]} entries, '
          f'{summary["wine_only"]} Wine-only, {summary["holes"]} Windows holes, max id {summary["max_id"]:#x}')
    for error in errors:
        print(f'  {error}')
    return not errors


def check_ntsyscalls(path, machine):
    text = Path(path).read_text(errors='replace')
    entries = parse_ntsyscalls(text)
    # win32u's header uses the same shape with ids at 0x1000. Detect from the ids.
    kind = 'win32u' if entries and min(number for number, _name in entries) >= 0x1000 else 'ntdll'
    table = windows_table_for(kind, machine)
    extra, summary = check_entries(entries, table)
    # Holes inside the array are fine only when the missing Windows functions are absent.
    # check_entries already treats a wrong occupant as an error. A dense packed array that
    # maps position to the wrong function fails the number compare.
    ok = report(f'{path} ({table["arch"]} {table["column"]})', extra, summary)
    return ok


def check_pe_and_unix(pe_path, unix_path):
    blob = Path(pe_path).read_bytes()
    image = PeImage(blob)
    kind, _exports = classify_pe(image)
    table = windows_table_for(kind, image.machine)
    pe_errors, entries = pe_syscall_entries(image)
    errors, summary = check_entries(entries, table)
    errors = pe_errors + errors
    arch = 'x64' if image.machine == IMAGE_FILE_MACHINE_AMD64 else 'x86'
    ok = report(f'{pe_path} {kind} {arch} against {table["column"]}', errors, summary)
    if unix_path is None:
        print(f'{pe_path}: no --unix image, dispatcher not checked')
        return False
    elf = ElfImage(Path(unix_path).read_bytes())
    slots = dispatcher_entries(elf)
    slot_errors, slot_summary = check_entries(slots, table)
    # Every PE stub number must dispatch to that same function.
    by_id = {number: name for number, name in slots}
    for number, name in entries:
        dispatched = by_id.get(number)
        if dispatched is None:
            slot_errors.append(f'{name} number {number:#x} has no dispatcher slot')
        elif nt_name(dispatched) != name:
            slot_errors.append(f'{name} number {number:#x} dispatches to {dispatched}')
    ok_slots = report(f'{unix_path} dispatcher', slot_errors, slot_summary)
    return ok and ok_slots and not slot_errors


def build_pe64(stubs, forwards):
    """Tiny PE32+ whose export stubs are the Wine x64 shape. stubs: name -> imm."""
    names = list(stubs) + list(forwards)
    # Layout: DOS+PE headers, one section at file 0x200 / VA 0x1000 containing code, export dir, strings.
    code = b''
    code_rva = {}
    for name, imm in stubs.items():
        code_rva[name] = 0x1000 + len(code)
        code += b'\x4c\x8b\xd1' + b'\xb8' + struct.pack('<I', imm)
        code += b'\xf6\x04\x25\x08\x03\xfe\x7f\x01\x75\x03\x0f\x05\xc3'
    code = code.ljust((len(code) + 15) & ~15, b'\x90')
    export_off = len(code)
    # Build export directory after we know string positions. Reserve the directory, then strings.
    n = len(names)
    dir_size = 40
    eats = 4 * n
    name_ptrs = 4 * n
    ords = 2 * n
    strings_at = export_off + dir_size + eats + name_ptrs + ords
    blob_strings = b''
    positions = {}
    dll_name_at = strings_at
    blob_strings += b'ntdll.dll\0'
    for name in names:
        positions[name] = strings_at + len(blob_strings)
        blob_strings += name.encode() + b'\0'
    forward_at = {}
    for name, target in forwards.items():
        forward_at[name] = strings_at + len(blob_strings)
        blob_strings += target.encode() + b'\0'
    export_rva = 0x1000 + export_off
    directory = struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, 0x1000 + dll_name_at, 1, n, n,
                            export_rva + dir_size, export_rva + dir_size + eats,
                            export_rva + dir_size + eats + name_ptrs)
    eats_b = b''
    ords_b = b''
    names_b = b''
    for index, name in enumerate(names):
        if name in forwards:
            eats_b += struct.pack('<I', 0x1000 + forward_at[name])
        else:
            eats_b += struct.pack('<I', code_rva[name])
        names_b += struct.pack('<I', 0x1000 + positions[name])
        ords_b += struct.pack('<H', index)
    section = code + directory + eats_b + names_b + ords_b + blob_strings
    section = section.ljust((len(section) + 0x1FF) & ~0x1FF, b'\0')

    dos = bytearray(0x80)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x80)
    pe = bytearray()
    pe += struct.pack('<I', IMAGE_NT_SIGNATURE)
    # COFF
    pe += struct.pack('<HHIIIHH', IMAGE_FILE_MACHINE_AMD64, 1, 0, 0, 0, 0xF0, 0x22)
    # Optional PE32+
    optional = bytearray(0xF0)
    struct.pack_into('<H', optional, 0, IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    struct.pack_into('<I', optional, 16, 0x1000)  # entry
    struct.pack_into('<Q', optional, 24, 0x140000000)  # image base
    struct.pack_into('<II', optional, 32, 0x1000, 0x200)  # alignments
    struct.pack_into('<HHH', optional, 40, 6, 0, 0)  # versions, skip the rest of the version block
    struct.pack_into('<I', optional, 56, 0x200 + len(section))  # size of image, not exact and unused
    struct.pack_into('<I', optional, 60, 0x200)  # size of headers
    struct.pack_into('<H', optional, 68, 3)  # subsystem
    struct.pack_into('<Q', optional, 72, 0x100000)  # stack reserve
    struct.pack_into('<I', optional, 108, 16)  # number of rva and sizes
    # Cover the forwarder strings, which sit in this section after the export directory.
    struct.pack_into('<II', optional, 112, export_rva, len(section) - export_off)
    pe += optional
    # Section header
    raw = 0x200
    pe += b'.text\0\0\0' + struct.pack('<IIIIII', len(section), 0x1000, len(section), raw, 0, 0)
    pe += struct.pack('<IIHHI', 0, 0, 0, 0, 0x60000020)
    headers = bytes(dos) + bytes(pe)
    headers = headers.ljust(raw, b'\0')
    return headers + section


def build_elf_dispatch(slots):
    """ELF64 ET_DYN whose KeServiceDescriptorTable[0] dispatches slots: id -> symbol name.

    slots maps an id to a function name. Missing ids are holes (no reloc, zero pointer).
    Relocations are R_X86_64_64 against the function symbols, which is what an unrelocated
    ntdll.so carries.
    """
    if not slots:
        raise ValueError('empty slots')
    limit = max(slots) + 1
    # VA layout: .text functions at 0x1000, one page each name in sorted order.
    # .data descriptor at 0x2000 (32 bytes) then the pointer array.
    names = sorted(set(slots.values()))
    func_va = {name: 0x1000 + index * 0x10 for index, name in enumerate(names)}
    desc_va = 0x3000
    array_va = 0x3020
    text = b'\xc3' * (0x10 * len(names))
    data = bytearray(32 + limit * 8)
    struct.pack_into('<Q', data, 16, limit)  # ServiceLimit
    # ServiceTable pointer and the slot pointers are relocs, file bytes stay 0.

    def strtab(strings):
        blob = bytearray(b'\0')
        offsets = {}
        for text_s in strings:
            offsets[text_s] = len(blob)
            blob += text_s.encode() + b'\0'
        return bytes(blob), offsets

    shstr, shstr_off = strtab(['.text', '.data', '.rela.data', '.strtab', '.symtab', '.shstrtab'])
    # symbols: undef, array, descriptor, functions, file-relative section symbols not needed
    sym_names = ['syscalls', 'KeServiceDescriptorTable'] + names
    strtab_b, str_off = strtab(sym_names)
    # section indexes: 0 null, 1 text, 2 data, 3 rela, 4 strtab, 5 symtab, 6 shstrtab
    symbols = bytearray()
    # null symbol
    symbols += struct.pack('<IBBHQQ', 0, 0, 0, 0, 0, 0)
    sym_index = {}
    # syscalls array in .data
    symbols += struct.pack('<IBBHQQ', str_off['syscalls'], (1 << 4) | 1, 0, 2, array_va, limit * 8)
    sym_index['syscalls'] = 1
    symbols += struct.pack('<IBBHQQ', str_off['KeServiceDescriptorTable'], (1 << 4) | 1, 0, 2, desc_va, 32)
    sym_index['KeServiceDescriptorTable'] = 2
    for index, name in enumerate(names):
        symbols += struct.pack('<IBBHQQ', str_off[name], (1 << 4) | STT_FUNC, 0, 1, func_va[name], 1)
        sym_index[name] = 3 + index
    relocs = bytearray()
    # ServiceTable -> syscalls
    relocs += struct.pack('<QQq', desc_va, (sym_index['syscalls'] << 32) | R_X86_64_64, 0)
    for number, name in slots.items():
        relocs += struct.pack('<QQq', array_va + number * 8, (sym_index[name] << 32) | R_X86_64_64, 0)

    def section(name, sh_type, addr, offset, size, link, entsize, flags=0):
        return struct.pack('<IIQQQQIIQQ', shstr_off[name], sh_type, flags, addr, offset, size, link, 0, 8, entsize)

    # File layout: ehdr, then contents, then section headers.
    ehdr_size = 64
    cursor = ehdr_size
    pieces = []

    def place(blob):
        nonlocal cursor
        off = cursor
        pieces.append(blob)
        cursor += len(blob)
        return off

    text_off = place(text)
    data_off = place(bytes(data))
    rela_off = place(bytes(relocs))
    str_off_file = place(strtab_b)
    sym_off = place(bytes(symbols))
    shstr_file = place(shstr)
    shoff = cursor
    shnum = 7
    shentsize = 64
    # null section
    sh = bytearray(shentsize)
    sh += section('.text', SHT_PROGBITS, 0x1000, text_off, len(text), 0, 0, 6)
    sh += section('.data', SHT_PROGBITS, 0x3000, data_off, len(data), 0, 0, 3)
    sh += section('.rela.data', SHT_RELA, 0, rela_off, len(relocs), 5, 24)
    sh += section('.strtab', SHT_STRTAB, 0, str_off_file, len(strtab_b), 0, 0)
    sh += section('.symtab', SHT_SYMTAB, 0, sym_off, len(symbols), 4, 24)
    sh += section('.shstrtab', SHT_STRTAB, 0, shstr_file, len(shstr), 0, 0)
    ehdr = bytearray(64)
    ehdr[0:4] = b'\x7fELF'
    ehdr[4] = 2  # ELFCLASS64
    ehdr[5] = 1  # ELFDATA2LSB
    ehdr[6] = 1  # EV_CURRENT
    # Elf64_Ehdr from e_type: type, machine, version, entry, phoff, shoff, flags,
    # ehsize, phentsize, phnum, shentsize, shnum, shstrndx.
    struct.pack_into('<HHIQQQIHHHHHH', ehdr, 16,
                     ET_DYN, 62, 1, 0, 0, shoff, 0, 64, 0, 0, shentsize, shnum, 6)
    return bytes(ehdr) + b''.join(pieces) + bytes(sh)


def self_test():
    table = load_table('win10-19045-x64.json')
    flush = table['syscalls']['NtFlushInstructionCache']
    completion = table['syscalls']['NtSetIoCompletion']
    if flush != 0xE9:
        print(f'self-test: embedded table has NtFlushInstructionCache {flush:#x}, want 0xe9')
        return False
    if table['by_id'][0xE9] != 'NtFlushInstructionCache':
        print('self-test: 0xe9 is not NtFlushInstructionCache')
        return False
    x86 = load_table('win10-19045-x86.json')
    if x86['syscalls']['NtFlushInstructionCache'] == flush:
        print('self-test: x86 and x64 NtFlushInstructionCache unexpectedly match')
        return False
    pinned_flush = NTDLL_PIN['NtFlushInstructionCache']
    pinned_completion = NTDLL_PIN['NtSetIoCompletion']
    header = (
        '/* test */\n#define ALL_SYSCALLS32 \\\n    SYSCALL_ENTRY( 0x0008, NtWriteFile, 36 )\n'
        '#ifdef _WIN64\n#define ALL_SYSCALLS \\\n'
        f'    SYSCALL_ENTRY( {pinned_completion:#06x}, NtSetIoCompletion, 40 ) \\\n'
        f'    SYSCALL_ENTRY( {pinned_flush:#06x}, NtFlushInstructionCache, 24 )\n'
        '#else\n#define ALL_SYSCALLS ALL_SYSCALLS32\n#endif\n'
    )
    entries = parse_ntsyscalls(header)
    errors, _summary = check_entries(entries, table)
    if errors:
        print('self-test: the pinned header failed:', errors)
        return False
    windows_header = (
        '/* test */\n#ifdef _WIN64\n#define ALL_SYSCALLS \\\n'
        f'    SYSCALL_ENTRY( {flush:#06x}, NtFlushInstructionCache, 24 ) \\\n'
        f'    SYSCALL_ENTRY( {completion:#06x}, NtSetIoCompletion, 40 )\n'
        '#else\n#define ALL_SYSCALLS ALL_SYSCALLS32\n#endif\n'
    )
    windows_errors, _summary = check_entries(parse_ntsyscalls(windows_header), table)
    if not any('NtSetIoCompletion' in error and f'{completion:#x}' in error for error in windows_errors):
        print('self-test: Windows 0x1a3 for NtSetIoCompletion was accepted', windows_errors)
        return False
    if not any('NtFlushInstructionCache' in error for error in windows_errors):
        print('self-test: NtFlushInstructionCache at 0xe9 was accepted', windows_errors)
        return False

    pe = build_pe64(
        {'NtFlushInstructionCache': pinned_flush, 'NtSetIoCompletion': pinned_completion},
        {'ZwFlushInstructionCache': 'NTDLL.NtFlushInstructionCache'},
    )
    image = PeImage(pe)
    pe_errors, pe_entries = pe_syscall_entries(image)
    if pe_errors:
        print('self-test pe:', pe_errors)
        return False
    got = {name: number for number, name in pe_entries}
    if got.get('NtFlushInstructionCache') != pinned_flush or got.get('NtSetIoCompletion') != pinned_completion:
        print('self-test pe parsed', got)
        return False
    pe_check, _summary = check_entries(pe_entries, table)
    if pe_check:
        print('self-test: pinned PE failed', pe_check)
        return False
    windows_pe = build_pe64(
        {'NtFlushInstructionCache': flush, 'NtSetIoCompletion': completion},
        {},
    )
    windows_pe_errors, _summary = check_entries(pe_syscall_entries(PeImage(windows_pe))[1], table)
    if not windows_pe_errors:
        print('self-test: a PE with the Windows 0x1a3 immediate was accepted')
        return False

    elf = build_elf_dispatch({pinned_completion: 'NtSetIoCompletion', pinned_flush: 'NtFlushInstructionCache'})
    parsed = ElfImage(elf)
    limit, slots = parsed.dispatch_slots()
    if (limit < pinned_flush or slots[pinned_completion] != 'NtSetIoCompletion'
            or slots[pinned_flush] != 'NtFlushInstructionCache' or slots[completion] is not None):
        print(f'self-test elf limit {limit:#x} completion {slots[pinned_completion] if limit > pinned_completion else None} '
              f'flush {slots[pinned_flush] if limit > pinned_flush else None} '
              f'windows slot {slots[completion] if limit > completion else None}')
        return False
    if slots[0] is not None:
        print('self-test: hole 0 is', slots[0])
        return False
    wrong = build_elf_dispatch({flush: 'NtFlushInstructionCache', completion: 'NtSetIoCompletion'})
    wrong_slots = dispatcher_entries(ElfImage(wrong))
    wrong_errors, _summary = check_entries(wrong_slots, table)
    if not any('NtSetIoCompletion' in error and f'{completion:#x}' in error for error in wrong_errors):
        print('self-test: dispatcher mapping 0x1a3 to NtSetIoCompletion was accepted', wrong_errors)
        return False
    print(f'self-test: Windows {table["build"]} {table["arch"]} NtFlushInstructionCache={flush:#x} '
          f'NtSetIoCompletion={completion:#x}; pinned NtSetIoCompletion={pinned_completion:#x} '
          f'NtFlushInstructionCache={pinned_flush:#x}; PE stub, Zw forwarder, ELF reloc table and the '
          f'Windows immediate all checked')
    return True


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--ntsyscalls', help='generated dlls/ntdll/ntsyscalls.h (or win32syscalls.h)')
    parser.add_argument('--pe', help='PE ntdll.dll or win32u.dll')
    parser.add_argument('--unix', help='unstripped Unix ntdll.so / ntdll.prx.elf (or win32u)')
    parser.add_argument('--windows-arch', choices=('x64', 'x86'), help='force the ntdll Windows table (default: the PE machine)')
    args = parser.parse_args(argv)
    if args.self_test:
        return 0 if self_test() else 1
    if not args.ntsyscalls and not args.pe:
        parser.error('give --self-test, --ntsyscalls, or --pe')
    ok = True
    if args.ntsyscalls:
        machine = IMAGE_FILE_MACHINE_I386 if args.windows_arch == 'x86' else IMAGE_FILE_MACHINE_AMD64
        try:
            ok = check_ntsyscalls(args.ntsyscalls, machine) and ok
        except (OSError, ValueError) as error:
            print(f'{args.ntsyscalls}: {error}', file=sys.stderr)
            return 2
    if args.pe:
        try:
            ok = check_pe_and_unix(args.pe, args.unix) and ok
        except (OSError, ValueError) as error:
            print(f'{args.pe}: {error}', file=sys.stderr)
            return 2
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
