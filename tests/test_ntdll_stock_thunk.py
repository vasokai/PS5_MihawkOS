#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch 1260 leaves the GE-Proton x64 thunk tail. It does not store int 2e."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/1260-ntdll-ps5-keep-stock-syscall-thunk.patch"
PE = ROOT / ".deps/wine-ps5/build/dlls/ntdll/x86_64-windows/ntdll.dll"
STUB_RVA = 0x58340
STOCK = bytes.fromhex(
    "4c8bd1b8a3010000f604250803fe7f0175030f05c3eb01c3ff14250010fe7fc3"
)


def fail(message):
    print(f"ntdll_stock_thunk: {message}", file=sys.stderr)
    sys.exit(1)


def added_function(text):
    lines = []
    in_fn = False
    for line in text.splitlines():
        if line.startswith("+static void ps5_keep_stock_thunks"):
            in_fn = True
        if not in_fn:
            continue
        if line.startswith("+"):
            lines.append(line[1:])
        elif line.startswith("-") or line.startswith("\\"):
            continue
        else:
            break
    return "\n".join(lines)


def pe_bytes(blob, rva, size):
    if blob[:2] != b"MZ":
        fail("ntdll.dll is not a PE")
    e_lfanew = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        fail("ntdll.dll has no PE signature")
    file_header = e_lfanew + 4
    sections = struct.unpack_from("<H", blob, file_header + 2)[0]
    optional = struct.unpack_from("<H", blob, file_header + 16)[0]
    section = file_header + 20 + optional
    for index in range(sections):
        off = section + index * 40
        virtual_size, virtual_address, raw_size, raw_ptr = struct.unpack_from("<IIII", blob, off + 8)
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            return blob[raw_ptr + (rva - virtual_address):raw_ptr + (rva - virtual_address) + size]
    fail(f"RVA {rva:#x} is not in ntdll.dll")


def main():
    text = PATCH.read_text()
    if "ps5_keep_stock_thunks" not in text:
        fail("patch does not add ps5_keep_stock_thunks")
    if "path=call tail=eb01+call" not in text:
        fail("patch does not log the stock call tail")
    if "-static void ps5_enable_windows_thunks" not in text:
        fail("patch does not remove the int 2e rewrite")
    body = added_function(text)
    if "ps5_keep_stock_thunks" not in body:
        fail("could not isolate the new function")
    if "virtual_uninterrupted_write_memory" in body or "NtProtectVirtualMemory" in body:
        fail("the stock-tail count stores into a thunk")
    if "0xcd, 0x2e, 0xc3, 0x0f, 0x1f" in body:
        fail("the new function still builds the Win10 tail replacement")
    if PE.is_file():
        stub = pe_bytes(PE.read_bytes(), STUB_RVA, len(STOCK))
        if stub != STOCK:
            fail(f"NtSetIoCompletion at {STUB_RVA:#x} is {stub.hex()}")
        data = PE.read_bytes()
        wine = bytes.fromhex("f604250803fe7f0175030f05c3eb01c3ff14250010fe7fc3")
        win = bytes.fromhex("f604250803fe7f0175030f05c3cd2ec30f1f840000000000")
        if data.count(wine) != 258 or data.count(win) != 0:
            fail(f"PE tails wine={data.count(wine)} win10={data.count(win)}")
    print("ntdll_stock_thunk: ok")


if __name__ == "__main__":
    main()
