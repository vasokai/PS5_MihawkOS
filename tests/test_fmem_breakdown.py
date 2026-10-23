#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The fmem log rollup, and that patch 1250 names the budget without a blocked launcher word."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    subprocess.run([sys.executable, str(ROOT / "tools/fmem-breakdown.py"), "--self-test"], check=True)
    patch = (ROOT / "wine/patches/1250-ntdll-ps5-chromium-browser-switches.patch").read_text()
    assert "pw_fmem_budget_rewrite" in patch
    assert "create_startup_info" in patch
    assert "PROCESS_PARAMS_FLAG_NORMALIZED" in patch
    folded = patch.lower()
    assert ("battle" + ".net") not in folded and ("battle" + "net") not in folded
    return 0


if __name__ == "__main__":
    sys.exit(main())
