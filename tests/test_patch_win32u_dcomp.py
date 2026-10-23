#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The OpenGL win32u composition patch: status rule, and a synthetic module."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import patch_win32u_dcomp as patch  # noqa: E402


def _elf(pieces):
    """ELF64 0xFE18 with one RX PT_LOAD and one R PT_LOAD. pieces: (va, bytes, flags)."""
    loads = []
    file_off = 0x200
    for va, blob, flags in pieces:
        loads.append((file_off, va, blob, flags))
        file_off += (len(blob) + 0xFFF) & ~0xFFF
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2
    header[5] = 1
    struct.pack_into("<HHIQQQIHHHHHH", header, 16,
                     0xFE18, 62, 1, 0, 64, 0, 0, 64, 56, len(loads), 0, 0, 0)
    image = bytearray(header)
    image += b"\x00" * (64 + len(loads) * 56 - len(image))
    for index, (file_off, va, blob, flags) in enumerate(loads):
        struct.pack_into("<IIQQQQQQ", image, 64 + index * 56,
                         1, flags, file_off, va, va, len(blob), len(blob), 0x1000)
    for file_off, _va, blob, _flags in loads:
        if len(image) < file_off:
            image += b"\x00" * (file_off - len(image))
        image += blob
    return bytes(image)


def _stub(stub_va, name_va, mod_va):
    body = bytearray(patch.STUB_SIZE)
    body[:len(patch.PROLOG)] = patch.PROLOG
    body[patch.MOVABS_AT:patch.MOVABS_AT + len(patch.MOVABS)] = patch.MOVABS
    mod_disp = mod_va - (stub_va + patch.MOD_LEA + 7)
    name_disp = name_va - (stub_va + patch.NAME_LEA + 7)
    body[patch.MOD_LEA:patch.MOD_LEA + 7] = b"\x48\x8d\x0d" + struct.pack("<i", mod_disp)
    body[patch.NAME_LEA:patch.NAME_LEA + 7] = b"\x48\x8d\x05" + struct.pack("<i", name_disp)
    body[patch.RET_AT] = 0xC3
    body[patch.RET_AT + 1] = 0xCC
    body[patch.RET_AT + 2] = 0xCC
    return bytes(body)


class PatchWin32uDcomp(unittest.TestCase):
    def test_rule_matches_the_header(self):
        cases = {
            "NtDCompositionCommitSynchronizationObject": 0,
            "NtDCompositionWaitForCompositorClock": 0,
            "NtDCompositionCreateChannel": 0xC0000002,
            "NtDCompositionGetFrameStatistics": 0xC0000002,
            "NtDxgkIsFeatureEnabled": 0xC0000002,
            "NtFlipObjectReadNextMessageToProducer": 0xC0000002,
            "NtFlipObjectCreate": 0xC0000002,
            "NtQueryCompositionInputIsImplicit": 0xC0000002,
            "NtVisualCaptureBits": 0xC0000002,
            "NtUserCreateDCompositionHwndTarget": 0xC0000002,
            "NtSetIoCompletion": None,
            "NtUserGetDC": None,
            "NtGdiBitBlt": None,
        }
        for name, expect in cases.items():
            self.assertEqual(patch.applies(name), expect is not None, name)
            if expect is not None:
                self.assertEqual(patch.status_for(name), expect, name)

    def test_synthetic_module_changes_only_the_matching_bodies(self):
        mod_va = 0x4000
        names = {
            "NtDCompositionCommitSynchronizationObject": 0x4010,
            "NtDCompositionCreateChannel": 0x4080,
            "NtUserGetDC": 0x40C0,
        }
        strings = bytearray(0x200)
        strings[0:6] = b"win32u"
        for name, va in names.items():
            raw = name.encode() + b"\x00"
            strings[va - mod_va:va - mod_va + len(raw)] = raw
        stubs = b"".join(_stub(0x2000 + index * patch.STUB_SIZE, va, mod_va)
                         for index, va in enumerate(names.values()))
        image = _elf([(0x2000, stubs, 5), (mod_va, bytes(strings), 4)])
        patched, report = patch.patch_bytes(image)
        self.assertEqual(report["stubs"], 3)
        self.assertEqual(len(report["patched"]), 2)
        by_name = {item["name"]: item["status"] for item in report["patched"]}
        self.assertEqual(by_name["NtDCompositionCommitSynchronizationObject"], 0)
        self.assertEqual(by_name["NtDCompositionCreateChannel"], 0xC0000002)
        ranges = patch.diff_ranges(image, patched)
        self.assertEqual(len(ranges), 2)
        self.assertEqual(patched[0x200:0x206], b"\xb8\x00\x00\x00\x00\xc3")
        self.assertEqual(patched[0x200 + patch.STUB_SIZE:0x206 + patch.STUB_SIZE],
                         b"\xb8\x02\x00\x00\xc0\xc3")
        kept = patched[0x200 + 2 * patch.STUB_SIZE:0x200 + 3 * patch.STUB_SIZE]
        self.assertEqual(kept, stubs[2 * patch.STUB_SIZE:])
        self.assertEqual(patched.count(patch.MOVABS), 1)


if __name__ == "__main__":
    unittest.main()
