#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Incremental title rebuild. Reuses build/ps5, .deps and the staged runtime.
# Does not configure Wine, RADV, Mesa or the runtime pack. A tree without
# build/ps5/build.ninja has to be made once by tools/build-production.sh.
#
#   tools/build-eboot-fast.sh
#
# Writes dist/<title id>/eboot.bin and prints its SHA-256. link-title.sh also
# refreshes sce_sys/, sce_module/libc.prx and shaders/glsl from sources that
# this script does not change.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=$root/build/ps5
if [[ ! -f $build/build.ninja ]]; then
    echo "build-eboot-fast: no $build/build.ninja; run tools/build-production.sh once" >&2
    exit 1
fi
# prospero-clang finds clang through llvm-config on PATH. The pinned LLVM 23.1.3
# tree is what the production link was built with. Do not export LLVM_CONFIG, CC or CXX.
if [[ -x $HOME/pins/llvm-23.1.3/bin/llvm-config ]]; then
    export PATH="$HOME/.local/bin:$HOME/pins/llvm-23.1.3/bin:$PATH"
fi
unset LLVM_CONFIG CC CXX
ninja=$(command -v ninja || true)
if [[ -z $ninja && -x $HOME/.local/bin/ninja ]]; then
    ninja=$HOME/.local/bin/ninja
fi
if [[ -z $ninja ]]; then
    echo "build-eboot-fast: ninja is not on PATH" >&2
    exit 1
fi
"$ninja" -C "$build" title
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
eboot=$root/dist/$title/eboot.bin
[[ -f $eboot ]] || { echo "build-eboot-fast: missing $eboot" >&2; exit 1; }
sha256sum "$eboot"
