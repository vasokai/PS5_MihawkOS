#!/usr/bin/env bash
# Build the Wine helper loader as a payload ELF.
#
#   tools/build-payload-loader.sh [output]
#
# The same loader sources the title-executable helper uses (tools/build-wine-child.py),
# linked as a plain payload instead of converted into a SONY SELF: no converter, no
# signing, no preload mask, because elfldr starts a plain ELF rather than Sony's
# local-process service.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bash "$root/tools/setup-graphics-foundation.sh" > /dev/null
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}
output=${1:-$root/build/proton-payload-child.elf}

[ -x "$sdk/bin/prospero-clang" ] || {
    echo "no payload SDK at $sdk (set PS5_PAYLOAD_SDK)" >&2
    exit 1
}

sources=(
    "$root/native/proton_payload_child.c"
    "$root/src/pw_wine_start.c"
    "$root/src/pw_environment.c"
    "$root/wine/ps5/pw_wine_prx.c"
    "$root/wine/ps5/pw_wine_process_io.c"
    "$root/src/pw_memory_helper.c"
    "$root/src/pw_memory_file.c"
    "$root/src/pw_memory_service.c"
    "$root/src/pw_vm_census.c"
    "$root/src/pw_fmem_census.c"
    "$root/src/pw_prx_data.c"
    "$root/src/pw_helper_sched.c"
    "$root/src/pw_cpu_placement.c"
    "$root/src/pw_prx_elf.c"
    "$root/native/pw_prx_elf_ps5.c"
    "$root/src/pw_firmware.c"
    "$root/native/pw_firmware_ps5.c"
)

mkdir -p "$(dirname -- "$output")"
# PW_MEMORY_HELPER_CENSUS: the memory helper takes the census of the console's system memory (src/pw_memory_helper.h).
"$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -g -DPW_MEMORY_HELPER_CENSUS -DPW_MEMORY_HELPER_PHYSICAL -DPW_MEMORY_HELPER_DMEM \
    -I"$root/src" -I"$root/wine/ps5" -I"$root/native" \
    -Wl,--wrap=kernel_mprotect -Wl,--wrap=kernel_set_vmem_protection \
    -o "$output" "${sources[@]}" \
    "$sdk/target/lib/libps5platform.a" -lkernel_sys
# The SDK's grant of execute writes the kernel's vm_map entry without its lock (native/proton_payload_child.c): the
# wrap above refuses it, and a loader whose code can still reach it is not kept.
objdump=$(command -v llvm-objdump-18 || command -v llvm-objdump || echo "$sdk/bin/llvm-objdump")
no_sdk_write() {
    local listing
    listing=$("$objdump" -d --no-show-raw-insn "$1")
    if grep -Eq '(call|jmp)[a-z]*[[:space:]].*<kernel_(mprotect|set_vmem_protection)>' <<<"$listing" ||
       ! grep -q '<__wrap_kernel_mprotect>:' <<<"$listing"; then
        rm -f "$1"
        echo "$1 can still call the SDK's kernel_mprotect or kernel_set_vmem_protection; removed" >&2
        exit 1
    fi
}
no_sdk_write "$output"
echo "built $output"
sha256sum "$output"

python3 - "$root" "$sdk" "$output" <<'PYMANIFEST'
import hashlib,json,sys
from pathlib import Path
root,sdk,out=map(Path,sys.argv[1:]);digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
inputs=['native/proton_payload_child.c','native/proton_wine_child.c','src/pw_wine_start.c','src/pw_control_watch.h','src/pw_environment.c','src/pw_environment.h','wine/ps5/pw_wine_prx.c','wine/ps5/pw_wine_process_io.c','src/pw_memory_helper.c','src/pw_memory_helper.h','src/pw_vm_census.c','src/pw_vm_census.h','src/pw_fmem_census.c','src/pw_fmem_census.h','src/pw_prx_data.c','src/pw_prx_data.h','native/pw_physical_shared.h','native/pw_dmem_share.h','src/pw_shared_page.h','src/pw_memory_file.c','src/pw_memory_file.h','src/pw_memory_service.c','src/pw_memory_service.h','wine/ps5/pw_wine_process.h','wine/ps5/pw_payload_rendezvous.h','wine/ps5/pw_wine_process_io.h','src/pw_gpu_broker.h','src/pw_helper_sched.c','src/pw_helper_sched.h','src/pw_cpu_placement.c','src/pw_cpu_placement.h','src/pw_prx_elf.c','src/pw_prx_elf.h','native/pw_prx_elf_ps5.c','native/pw_prx_elf_ps5.h','src/pw_firmware.c','src/pw_firmware.h','native/pw_firmware_ps5.c','src/pw_log_sessions.h','wine/ps5/pw_wine_threads.h','wine/ps5/pw_wine_perf.h','tools/build-payload-loader.sh']
manifest={'graphics_foundation_revision':(root/'.deps/graphics-foundation/.revision').read_text().strip(),'payload':True,'gpu_broker':True,'sdk_revision':(sdk/'.ps5-sdk-revision').read_text().strip(),'sha256':digest(out),'sources':{n:digest(root/n) for n in inputs}}
out.with_suffix('.json').write_text(json.dumps(manifest,indent=2)+'\n')
PYMANIFEST

"$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Wl,--wrap=kernel_mprotect -Wl,--wrap=kernel_set_vmem_protection \
    "$root/tools/payload-lifecycle-probe.c" -o "$(dirname -- "$output")/payload-lifecycle-probe.elf"
no_sdk_write "$(dirname -- "$output")/payload-lifecycle-probe.elf"
