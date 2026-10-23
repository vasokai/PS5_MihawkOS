#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add up the helper fmem lines from a run.

Each helper prints one line from its address-space walk (native/proton_wine_child.c):

    wine-child: fmem pid=N jit=J data=D file=F stack=S direct=R other=O KiB name=class:KiB,...

The classes are KiB of resident bytes (src/pw_fmem_census.h). A later line for the same pid replaces the earlier
one: the walk is repeated. Named groups are only the largest eight of at least 1 MiB that are jit, data or file, so a
duplicate computed from the names can miss a small module.

duplicate_code is (copies - 1) times the smallest jit resident of that name. It is an upper bound on bytes that are
the same code. Sharing them does not lower sceKernelGetResidentFmemCount (K14, K18). movable is data plus file.

    python3 tools/fmem-breakdown.py LOG
    python3 tools/fmem-breakdown.py --self-test
"""
import re
import sys

LINE = re.compile(
    r"wine-child: fmem pid=(\d+) jit=(\d+) data=(\d+) file=(\d+) stack=(\d+) direct=(\d+) other=(\d+) KiB(.*)$"
)
GROUP = re.compile(r"^(.+)=([a-z]+):(\d+)$")
CLASSES = ("jit", "data", "file", "stack", "direct", "other")


def parse(text):
    """Last sample per pid. Each sample is classes (KiB) and a list of (name, class, KiB)."""
    latest = {}
    order = []
    for raw in text.splitlines():
        match = LINE.search(raw.strip())
        if not match:
            continue
        pid = int(match.group(1))
        classes = {name: int(match.group(i + 2)) for i, name in enumerate(CLASSES)}
        groups = []
        tail = match.group(8).strip()
        if tail:
            for piece in tail.split(","):
                group = GROUP.match(piece.strip())
                if not group:
                    continue
                groups.append((group.group(1), group.group(2), int(group.group(3))))
        if pid not in latest:
            order.append(pid)
        latest[pid] = (classes, groups)
    return [(pid, latest[pid][0], latest[pid][1]) for pid in order]


def rollup(samples):
    """duplicate_code and movable, in KiB, from the same rule as pw_fmem_rollup."""
    totals = {name: 0 for name in CLASSES}
    # name -> pid -> kib, jit only
    by_name = {}
    for pid, classes, groups in samples:
        for name in CLASSES:
            totals[name] += classes[name]
        for gname, klass, kib in groups:
            if klass != "jit":
                continue
            by_name.setdefault(gname, {})
            by_name[gname][pid] = by_name[gname].get(pid, 0) + kib
    duplicate = 0
    for pids in by_name.values():
        if len(pids) < 2:
            continue
        duplicate += (len(pids) - 1) * min(pids.values())
    return {
        "processes": len(samples),
        "classes_kib": totals,
        "duplicate_code_kib": duplicate,
        "movable_kib": totals["data"] + totals["file"],
    }


def report(text):
    samples = parse(text)
    rolled = rollup(samples)
    lines = [f"processes={rolled['processes']}"]
    classes = rolled["classes_kib"]
    lines.append(
        " ".join(f"{name}={classes[name]}" for name in CLASSES) + " KiB"
    )
    lines.append(
        f"duplicate_code={rolled['duplicate_code_kib']} KiB (not a saving; sharing does not lower the census)"
    )
    lines.append(f"movable={rolled['movable_kib']} KiB (data+file, an upper bound on walked anonymous and file bytes)")
    for pid, classes, groups in samples:
        named = ",".join(f"{name}={klass}:{kib}" for name, klass, kib in groups)
        lines.append(
            f"pid={pid} jit={classes['jit']} data={classes['data']} file={classes['file']} {named}".rstrip()
        )
    return rolled, "\n".join(lines) + "\n"


def self_test():
    text = """
wine-child: vm resident=1 MiB
wine-child: fmem pid=10 jit=1 data=1 file=0 stack=0 direct=0 other=0 KiB
wine-child: fmem pid=10 jit=63488 data=40960 file=4096 stack=1024 direct=614400 other=0 KiB libcef.dll=jit:63488,libc heap=data:40960,/data/ntdll.prx.elf=file:4096
wine-child: fmem pid=11 jit=36864 data=10240 file=0 stack=0 direct=0 other=0 KiB libcef.dll=jit:36864,libc heap=data:10240
noise without the marker
"""
    rolled, printed = report(text)
    assert rolled["processes"] == 2, rolled
    assert rolled["classes_kib"]["jit"] == 63488 + 36864
    assert rolled["duplicate_code_kib"] == 36864, rolled
    assert rolled["movable_kib"] == 40960 + 4096 + 10240
    assert "pid=10" in printed and "not a saving" in printed
    assert "pid=10 jit=1 " not in printed  # the first pid 10 line was replaced
    print("fmem-breakdown: self-test ok")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path = argv[1]
    text = sys.stdin.read() if path == "-" else open(path, encoding="utf-8", errors="replace").read()
    _rolled, printed = report(text)
    sys.stdout.write(printed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
