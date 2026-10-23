# System-pool budget: what was measured, what this change does

The freeze guard sums `sceKernelGetResidentFmemCount` over processes. Firmware 12's line is 1600 MiB (1670 MiB on the
warn path, `src/pw_memory_policy.h`). An idle Mihawk desktop sits near 1460 MiB, about 140 MiB under that line. The
application pool is about 11,712 MiB and is not in the census. Execute does not stick on it (PR #4, PR #7): a direct
mapping asked for execute comes back without it. This change does not write the kernel, `param.json`, or a system
partition. The worst case is `CreateProcess` returning `EAGAIN`, or one process dying if a module's data run cannot
be put back. Nothing here is installed by the change; the install steps are for after review.

No console run was made for this change. The numbers below are from records already in the tree, or they are marked
as not yet measured. A hash that was not computed is not listed.

## What the census is made of

| Kind | Counted in fmem? | Can this change move it? |
| --- | --- | --- |
| JIT code (`sceKernelJitCreateSharedMemory`) | Yes, in every process that maps the object | No. Sharing one object still charges it in each process (K14, K18). |
| Private anonymous flexible data (heaps, buffers, `.prx.elf` non-exec runs) | Yes | Yes, onto the application pool, when the bytes are not executable. |
| File-backed pages | Charged per mapping; a clean page may already be one physical page | Only the dirty private copy. The walk cannot see which pages are clean. |
| Stacks | Yes | Left where the kernel put them. |
| Direct / application pool | No | Already outside the census. |
| The shell and the title | Yes, in the global sum | Not a Windows helper. This change does not move them. |

K14: one 16 MiB shared JIT object added 16 MiB to the taker and left the maker at 16 MiB; two mappings in one process
counted 32 MiB. K18 (2026-10-21): 28,054 shared clusters, 448,864 KiB of mappings, helper census 1532 to 1568 MiB,
and 4 guard ends against 0 with sharing off. `pw_wine_jit_share` stays off.

Preferred-base loading does not remove the charge. The staged `.prx.elf` modules are linked at virtual address 0
(`src/pw_prx_elf.h`) and are relocated, then the executed pages are copied into a per-process JIT object.

Anonymous memory with no address and no execute already takes a direct region on this branch
(`pw_wine_anon_fresh_direct`). Anonymous sections above one host page, except user shared data (16 KiB) and the
wineserver session mapping (1,327,104 bytes at Wine `1c785966`), register for direct memory. PE images are read into
direct regions. The new piece is the non-executable runs of a user-space `.prx.elf`, which the loader maps as private
anonymous flexible memory and never sends through `__wine_ps5_mmap`.

## Measurement

`native/proton_wine_child.c` already walks the helper's mappings. It now adds one line:

```text
wine-child: fmem pid=N jit=J data=D file=F stack=S direct=R other=O KiB name=class:KiB,...
```

Classes are resident KiB (`src/pw_fmem_census.h`). Up to eight groups of at least 1 MiB, jit or data or file, are
named. `tools/fmem-breakdown.py LOG` keeps the last line per pid and prints:

* `duplicate_code` — `(copies - 1)` times the smallest JIT resident of that name. An upper bound on repeated code.
  Not a saving.
* `movable` — data plus file. An upper bound on walked bytes that are not code. Kernel charge that is in no mapping
  is not in this number.

Host checks: `tests/test_pw_fmem_census.c`, `tests/test_fmem_breakdown.py`.

### Already measured (MihawkBrowser, standard PS5)

From `evidence/gate-std-2026-10-21-01-mihawk-browser.json`, `about:blank`, fmem truncated to MiB, JIT from
`converted_kib`:

| Process | System pool | JIT | Non-JIT |
| --- | --- | --- | --- |
| browser | 144 MiB | 63,552 KiB (62.06 MiB) | 83,904 KiB (81.94 MiB) |
| renderer | 68 MiB | 34,960 KiB (34.14 MiB) | 34,672 KiB (33.86 MiB) |
| utility `storage.mojom.StorageService` | 45 MiB | 12,576 KiB (12.28 MiB) | 33,504 KiB (32.72 MiB) |

The three processes held **152,080 KiB (148.52 MiB)** that is not a JIT copy. That is an upper bound, not a promised
saving. The walk's flexible resident was below the JIT copy for the browser (54 MiB) and the renderer (27 MiB), so
those two records do not separate mappings from kernel structures. The storage helper's walk (27 MiB) against 12 MiB
of JIT leaves about 15 MiB of walk-visible non-JIT and about 18 MiB of kernel charge in no mapping the walk listed.

At 16 KiB JIT clusters, five browser processes copied 15 to 57 MiB each, 181 MiB total (257 MiB at 64 KiB). At 64 KiB
each renderer start raised the census by 120 MiB or more. At 16 KiB a ten-minute run had 0 guard ends.

A launcher sign-in on the standard PS5 (build `09acfc9`) held helpers at 886 MiB across 16 processes, census peak
1631 MiB, no guard end. Another 15-minute run peaked at 1671 MiB. After sync-device chunking, launcher guard-end
helpers were 613 to 647 MiB across 15 or 16 processes (was 831 to 881). Idle desktop, 9 helpers: the sync device
alone was 160 MiB (the whole table 214 MiB; the device off, 153 MiB). The 1460 MiB idle figure is the global census,
not the helpers alone.

There is no per-module breakdown yet for the Chromium launcher or for the large game. Those sessions were kept off
this tree because the logs name the program. The new line is what the next run records. Do not treat the MihawkBrowser
rows as those programs' measurements.

## What changed, and the expected MiB

| Change | Desktop | Chromium launcher | Large game |
| --- | --- | --- | --- |
| Share identical JIT, or load at a preferred base | 0 | 0 | 0 |
| Renderer cap, default 2 (`WINE_PS5_RENDERER_LIMIT`) | 0 (not a Chromium host) | Each renderer that is not started is that process's whole resident fmem. From the MihawkBrowser renderer: about 68 MiB total, of which about 34 MiB is JIT. Four extra renderers not started are on the order of 270 MiB, estimated from that one renderer, not measured on the launcher. The 15-to-16-helper launcher sessions (613 to 886 MiB) are what an uncapped host produced. | 0. The cap does not shrink the game's own JIT. It leaves the headroom the extra renderers would have taken. |
| `--in-process-gpu` and the other switches, patch 1250 | MihawkBrowser already sets them (`src/pw_browser_policy.c`) | One fewer GPU process if Chromium honors the switch: about one process of JIT, 15 to 57 MiB in the MihawkBrowser cluster record. Needs a full `tools/build-wine-ps5.sh` so patch 1250 is inside ntdll. | Unchanged. The image is not a Chromium host. |
| `.prx.elf` non-exec runs onto the application pool | Unknown until `pw_prx_elf: data direct=` is logged | Same | Same. This is the game's data saving from this change. Its heaps already use direct memory on this branch. |
| Non-JIT 148.52 MiB | — | Upper bound for three MihawkBrowser processes, not a promised saving | — |
| Wine services | Already gated by `WINE_PS5_SERVICES` (patch 0933). About 2 to 4 MiB of JIT each in a browser gate. Not tightened: the game's controller path wants plugplay and rpcss. | Same | Same |

`duplicate_code` from the new tool is reported so the next run can see the repeated JIT. It is not a row in the
savings: sharing it does not lower the census.

The cap refuses the spawn in the title's ntdll (`wine/ps5/pw_wine_process.c`) with `EAGAIN` once that many
`--type=renderer` children of a Chromium host are live, including one whose spawn has not finished. A helper forwards
the spawn to the title, so the count is the title's table. `WINE_PS5_CHROMIUM_BUDGET=0` turns the cap and the
switches off. `WINE_PS5_RENDERER_LIMIT=0` turns the cap off and skips the switches. `WINE_PS5_RENDERER_LIMIT=4`
allows four. Anything that is not a decimal from 1 to 32 is the default, 2.

The switches are appended only when that switch is absent, so a second `--disable-features` cannot replace one the
program already set. `--single-process` is not used. Hardware acceleration stays on.

`.prx.elf` data: one run at a time, `sceKernelAllocateDirectMemoryForApp` (type `0x0c`), mapped read-write (protection
`0x03`, fixed flag `0x10`). Execute is never requested. `WINE_PS5_PRX_DATA_DMEM=0` keeps today's `mprotect`. If the
direct mapping cannot be made, the flexible mapping is left, or put back from the scratch. If it was removed and
cannot be put back, the load fails and that process ends (`PW_PRX_DATA_BROKEN`). The signed SELF text that
`payload_prepare_module` wires is not touched: unwiring it hangs the console. Firmware 12.20 loads the sibling
`.prx.elf`, so this path is the one that runs.

While one run is moving, the scratch is also flexible, so the peak extra fmem is one data run, during load, before
the program runs.

## Build, and why there are no hashes

This environment has no payload SDK and no prospero-clang, so it cannot produce `ntdll.prx.elf` or
`proton-payload-child.elf`, and it must not invent a SHA-256. After review, on a machine that has the pinned SDK
(`611893fc25ef3e138f5bc2c90d829d8b58174cd2`) and the PRX foundation pin (`30597512539e7edfde079cbcaf4a626bc0a948c5`):

```bash
bash tools/build-payload-loader.sh
# build/proton-payload-child.elf  — the .prx.elf data move and the fmem class line
bash tools/build-wine-ps5.sh
# .deps/wine-ps5/prx/ntdll.elf    — patch 1250 and the renderer cap
sha256sum build/proton-payload-child.elf .deps/wine-ps5/prx/ntdll.elf
```

`python3 tools/build-payload-runtime.py` relinks ntdll from the prebuilt base and recompiles `pw_wine_process.c` and
`src/pw_fmem_budget.c`, so the renderer cap is in that `ntdll.elf`. The command-line switches are in Wine's
`dlls/ntdll/unix/process.c` (patch 1250). They are in the relink only when the base objects were built from this
series. A full `tools/build-wine-ps5.sh` is the build that has both.

`tools/stage-wine.py` copies `ntdll.elf` to `lib/wine/x86_64-unix/ntdll.prx.elf` and
`build/proton-payload-child.elf` to `run/proton-payload-child.elf`. The user-space loader opens the `.prx.elf`
sibling, not the `.prx`.

## Install and rollback

Close MihawkOS first. Copy only under `/data/ps5proton` and the homebrew title folder. Do not write a system
partition, `param.json`, or the kernel.

Back up, then replace:

| Install path | Built file | Replaces |
| --- | --- | --- |
| `/data/ps5proton/run/proton-payload-child.elf` | `build/proton-payload-child.elf` | the payload loader |
| `/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix/ntdll.prx.elf` | `.deps/wine-ps5/prx/ntdll.elf` | the user-space ntdll |

Leave these as they are: `ntdll.prx`, `wineserver.prx.elf`, `eboot.bin`, `param.json`, and the prefix. If a process
still maps the signed `ntdll.prx`, the cap is not in that process; the log line is still in the payload loader.

Record `sha256sum` of each file you copy, next to the backup's hash.

Rollback: copy the two backups back over those paths and start the title from the home screen. There is no kernel
state and no system-partition write to undo.

## Test

1. Idle desktop. Read the global fmem sum and every `wine-child: fmem pid=` line. `python3 tools/fmem-breakdown.py`
   on the log. Note `pw_prx_elf: data direct=` per module.
2. The Chromium launcher, then the large game, then both. The interesting lines are
   `wine-ps5: chromium renderer refused live=N limit=2` and a census that stays under 1600 MiB.
3. `WINE_PS5_CHROMIUM_BUDGET=0` is the kill switch for the cap and the switches.
   `WINE_PS5_PRX_DATA_DMEM=0` is the kill switch for the data move. `WINE_PS5_RENDERER_LIMIT=0` drops the cap.

A pass is a census under the line with both programs up, and the class lines naming where the remaining fmem is. A
failure that kills one process is the bound of this change. A console freeze is not: stop and roll back.
