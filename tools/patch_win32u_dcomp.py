#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch an OpenGL win32u.prx.elf so composition stubs return a status.

The console's win32u.prx.elf is the unsigned module the payload maps
(native/pw_prx_elf_ps5.c loads "<name>.prx" by opening "<name>.prx.elf").
That image contains the PS5 OpenGL SDK. Replacing it with a win32u linked
without the SDK removes the driver.

Clang inlined the original stub_syscall into each `@ stub -syscall`. Each
one is 192 bytes, raises EXCEPTION_WINE_STUB, and is named by a
RIP-relative LEA. The service table (KeAddSystemServiceTable) points at
those addresses, and __wine_syscall_dispatcher_return reads eax after
`callq *%r12`. This writes `mov eax, status; ret` over the body and fills
the rest of the old frame with int3. Bytes outside those bodies stay as
they were, including the OpenGL driver and the eh_frame of the old body.
The function returns before any call, so that frame is not used.

The status rule is wine/ps5/pw_wine_dcomp.h. A create, open, duplicate,
query, get, enum, check, capture or read returns STATUS_NOT_IMPLEMENTED
(0xC0000002). NtDCompositionCommitSynchronizationObject returns
STATUS_SUCCESS. Other NtUser and NtGdi stubs still raise. There is no
log line: a printf would need new code.

  patch_win32u_dcomp.py IN OUT
"""

import hashlib
import struct
import sys

STUB_SIZE = 192
MOVABS_AT = 0x27
MOD_LEA = 0x31
NAME_LEA = 0x6F
RET_AT = 0xBD
PROLOG = bytes.fromhex(
    "554889e5534881ec68050000488d9d90faffffbad004000031f64889df")
MOVABS = bytes.fromhex("48b80001008001000000")
SUCCESS = 0
NOT_IMPLEMENTED = 0xC0000002

PT_LOAD = 1
PT_DYNAMIC = 2
PF_X = 1
DT_NULL = 0
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
R_X86_64_RELATIVE = 8
ELF_RELA = 24


class PatchError(Exception):
    pass


def applies(name):
    """Same predicate as pw_dcomp_stub_applies."""
    if not name:
        return False
    if name.startswith("NtDxgk") or name.startswith("NtFlipObject"):
        return True
    if "Composition" in name or name == "NtVisualCaptureBits":
        return True
    return False


def writes_output(name):
    """Same predicate as pw_dcomp_writes_output."""
    if not name:
        return False
    for word in ("Create", "Open", "Duplicate", "Query", "Get", "Enum",
                 "Check", "Is", "Capture", "Read"):
        if word in name:
            return True
    return False


def status_for(name):
    """Same result as pw_dcomp_stub_status for a name that applies."""
    if not applies(name) or writes_output(name):
        return NOT_IMPLEMENTED
    return SUCCESS


def _loads(data):
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise PatchError("not an ELF64 little-endian image")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_machine != 62:
        raise PatchError("not an x86-64 image")
    if e_type not in (3, 0xFE18):
        raise PatchError("not a shared module (ET_DYN or PS5 0xFE18)")
    e_phoff, = struct.unpack_from("<Q", data, 32)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    if e_phentsize != 56 or e_phoff + e_phnum * 56 > len(data):
        raise PatchError("program headers are truncated")
    loads = []
    dynamic = None
    for i in range(e_phnum):
        off = e_phoff + i * 56
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
            "<IIQQQQQQ", data, off)
        if p_type == PT_LOAD and p_filesz:
            if p_offset + p_filesz > len(data):
                raise PatchError("a PT_LOAD is truncated")
            loads.append((p_offset, p_vaddr, p_filesz, p_flags))
        elif p_type == PT_DYNAMIC and p_filesz:
            dynamic = (p_offset, p_vaddr, p_filesz)
    if not loads:
        raise PatchError("no PT_LOAD")
    return loads, dynamic


def _va_to_off(loads, va, size=1):
    for file_off, virt, filesz, _flags in loads:
        if virt <= va and va + size <= virt + filesz:
            return file_off + (va - virt)
    return None


def _cstring(data, loads, va):
    off = _va_to_off(loads, va)
    if off is None:
        return None
    end = data.find(b"\x00", off, off + 200)
    if end < 0:
        return None
    text = data[off:end]
    if not text or any(byte < 32 or byte > 126 for byte in text):
        return None
    return text.decode()


def _rela(data, loads, dynamic):
    if dynamic is None:
        return []
    file_off, virt, filesz = dynamic
    rela_va = rela_size = rela_ent = None
    pos = 0
    while pos + 16 <= filesz:
        tag, value = struct.unpack_from("<qQ", data, file_off + pos)
        if tag == DT_NULL:
            break
        if tag == DT_RELA:
            rela_va = value
        elif tag == DT_RELASZ:
            rela_size = value
        elif tag == DT_RELAENT:
            rela_ent = value
        pos += 16
    if not rela_va or not rela_size:
        return []
    if rela_ent not in (None, ELF_RELA) or rela_size % ELF_RELA:
        raise PatchError("DT_RELA is not 24-byte entries")
    rela_off = _va_to_off(loads, rela_va, rela_size)
    if rela_off is None:
        raise PatchError("DT_RELA is outside the loaded image")
    entries = []
    for index in range(rela_size // ELF_RELA):
        offset, info, _addend = struct.unpack_from("<QQq", data, rela_off + index * ELF_RELA)
        entries.append((offset, info & 0xFFFFFFFF))
    return entries


def find_stubs(data):
    """Return (loads, [{va, name, module}, ...]) for every inlined abort stub."""
    loads, dynamic = _loads(data)
    found = []
    seen = set()
    for file_off, virt, filesz, flags in loads:
        if not flags & PF_X:
            continue
        blob = data[file_off:file_off + filesz]
        start = 0
        while True:
            at = blob.find(MOVABS, start)
            if at < 0:
                break
            start = at + 1
            if at < MOVABS_AT:
                continue
            va = virt + at - MOVABS_AT
            if va in seen:
                continue
            body_off = file_off + at - MOVABS_AT
            if body_off < 0 or body_off + STUB_SIZE > len(data):
                continue
            body = data[body_off:body_off + STUB_SIZE]
            if body[:len(PROLOG)] != PROLOG or body[RET_AT] != 0xC3:
                raise PatchError("movabs $0x180000100 at VA %#x is not an abort stub" % va)
            if body[MOD_LEA:MOD_LEA + 3] != b"\x48\x8d\x0d" or body[NAME_LEA:NAME_LEA + 3] != b"\x48\x8d\x05":
                raise PatchError("abort stub at VA %#x has no name LEA" % va)
            mod_disp = struct.unpack_from("<i", body, MOD_LEA + 3)[0]
            name_disp = struct.unpack_from("<i", body, NAME_LEA + 3)[0]
            module = _cstring(data, loads, va + MOD_LEA + 7 + mod_disp)
            name = _cstring(data, loads, va + NAME_LEA + 7 + name_disp)
            if module != "win32u" or not name:
                raise PatchError("abort stub at VA %#x does not name a win32u function" % va)
            seen.add(va)
            found.append({"va": va, "name": name, "file_off": body_off})
    if not found:
        raise PatchError("no inlined win32u abort stubs")
    relocs = _rela(data, loads, dynamic)
    for stub in found:
        span = range(stub["va"], stub["va"] + RET_AT + 1)
        # A loader writes 8 bytes at each RELA offset. One inside the body
        # would replace the mov/ret or the int3 fill after the image is mapped.
        if any(offset in span or (stub["va"] <= offset + 7 < stub["va"] + RET_AT + 1)
               for offset, _typ in relocs):
            raise PatchError("a relocation lands in the stub at VA %#x" % stub["va"])
    found.sort(key=lambda item: item["va"])
    return found


def encode_return(status):
    """mov eax, imm32; ret; then int3 up to and including the old ret."""
    word = struct.pack("<I", status & 0xFFFFFFFF)
    body = b"\xb8" + word + b"\xc3"
    body += b"\xcc" * (RET_AT + 1 - len(body))
    if len(body) != RET_AT + 1:
        raise PatchError("return encoding is the wrong length")
    return body


def patch_bytes(data):
    """Return (patched image, report). `data` is not modified."""
    original = bytes(data)
    stubs = find_stubs(original)
    out = bytearray(original)
    patched = []
    for stub in stubs:
        if not applies(stub["name"]):
            continue
        status = status_for(stub["name"])
        out[stub["file_off"]:stub["file_off"] + RET_AT + 1] = encode_return(status)
        patched.append({**stub, "status": status})
    if not patched:
        raise PatchError("no composition, Dxgk or flip stub to patch")
    report = {
        "stubs": len(stubs),
        "patched": patched,
        "unchanged_stubs": len(stubs) - len(patched),
    }
    return bytes(out), report


def diff_ranges(before, after):
    ranges = []
    index = 0
    size = min(len(before), len(after))
    while index < size:
        if before[index] == after[index]:
            index += 1
            continue
        start = index
        while index < size and before[index] != after[index]:
            index += 1
        ranges.append((start, index))
    if len(before) != len(after):
        raise PatchError("patched image changed size")
    return ranges


def main(argv):
    if len(argv) != 3:
        print("usage: patch_win32u_dcomp.py IN OUT", file=sys.stderr)
        return 2
    original = open(argv[1], "rb").read()
    patched, report = patch_bytes(original)
    ranges = diff_ranges(original, patched)
    spans = [(item["file_off"], item["file_off"] + RET_AT + 1, item) for item in report["patched"]]
    for start, end in ranges:
        if not any(lo <= start and end <= hi for lo, hi, _item in spans):
            raise PatchError("a changed byte is outside a patched stub")
    for lo, _hi, item in spans:
        encoded = encode_return(item["status"])
        if patched[lo:lo + len(encoded)] != encoded:
            raise PatchError("stub %s does not return status %#x" % (item["name"], item["status"]))
        if patched[lo:lo + 6] == original[lo:lo + 6]:
            raise PatchError("stub %s already began with the return" % item["name"])
    # The two int3s that already padded each function to 192 bytes.
    for item in report["patched"]:
        off = item["file_off"] + RET_AT + 1
        if original[off:off + 2] != patched[off:off + 2]:
            raise PatchError("padding after %s changed" % item["name"])
    open(argv[2], "wb").write(patched)
    success = sum(1 for item in report["patched"] if item["status"] == SUCCESS)
    missing = sum(1 for item in report["patched"] if item["status"] == NOT_IMPLEMENTED)
    digest = hashlib.sha256(patched).hexdigest()
    print("stubs %d patched %d success %d not_implemented %d unchanged_abort %d" % (
        report["stubs"], len(report["patched"]), success, missing, report["unchanged_stubs"]))
    print("bytes %d sha256 %s" % (len(patched), digest))
    print("diff_ranges %d diff_bytes %d" % (len(ranges), sum(end - start for start, end in ranges)))
    for item in report["patched"]:
        print("%08x %s %s" % (item["status"], item["name"], hex(item["va"])))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except PatchError as error:
        print("patch_win32u_dcomp: %s" % error, file=sys.stderr)
        sys.exit(1)
