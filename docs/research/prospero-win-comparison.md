# prospero-win and MihawkOS, judged for WoW Forever

Research note, 2026-10-23. No console run. prospero-win was cloned at
`df9802f0cc2f9ed8e653304a08cacc1b1d5993f5` (2026-10-18, “pw_install: pin DXVK
2.6.2-prospero3”). MihawkOS line numbers below are `origin/main` at `b91f6ee`.
Where a measurement exists only on `cursor/prx-elf-rebuild-71c6` (`b4d63f4`),
that commit is named.

WoW Forever here means the 64-bit Direct3D 12 client plus the Battle.net
launcher. The question for every prospero-win piece is whether it raises that
game’s frame rate or keeps that session up, and how to measure the claim.

## What would move WoW Forever, and what would not

prospero-win cannot run this game. Its README lists Direct3D 8, 9, 10 and 11
through DXVK and stops there (`README.md:27-30`). A search of that tree finds
no vkd3d, no `d3d12`, and no Proton DX12 stack. Patch 0550 makes
`NtCreateUserProcess` and `__wine_unix_spawnvp` return `STATUS_NOT_SUPPORTED`
(`docs/WINE_PS5_BUILD.md:95`): one process, prefix built offline. Battle.net
is a tree of processes. MihawkOS already has the two things that stack needs:
VKD3D-Proton `af89350cc2eacd9da2293fbae96bd9ab4987c9bb` (2026-09-15) and helper
processes (`tools/fetch-runtime.sh:25`, `NOTICE.md:520`).

The frame-time evidence already in this tree, from another Direct3D 12 game
on VKD3D-Proton, is the right scale for WoW until WoW itself is traced
(`docs/PLAN.md:554-566`, 2026-10-17):

- The GPU was busy about 2 ms of a 32.6 ms frame. The game was CPU-bound near 30 fps.
- `vkQueuePresentKHR` on VKD3D-Proton’s queue thread cost 5.8 ms, of which 5.6 ms was a CPU copy into VideoOut’s tiled buffer. The next submit on that queue waited.
- The helper made 1.27 million wineserver round trips in 10 s (about 127,000/s, 4,200 a frame): 41 thread-seconds, mostly `event_op` and `select`. At that date a helper had no in-process sync device.

Those two CPU costs dwarf anything in prospero-win’s 32-bit translator, its
DXVK 2.6.2 fork, or its RADV timing wrappers.

### Already in MihawkOS: turn it on and measure it

**One ntsync device for the helper** (`wine/patches/1180-ntsync-ps5-one-synchronization-device-for-the-title-and-its-helpers.patch:4-16`).
This is the patch aimed at the 40k–634k server calls / 10 s and the 1–25 fps
that produced. It is opt-in: the title creates the region only with
`WINE_PS5_NTSYNC=1`. prospero-win’s shared-mutex and shared-sync words
(patches 0790, 0810, 0820, 0885–0888) are a userspace substitute for the same
waits, written for a single process that has no ntsync device. They are the
design to read if the WoW helper cannot join Mihawk’s device. They are not a
faster replacement for it.

Measure, one outdoor scene, three runs, profiling off: fps, 1% low, and the
helper’s `event_op` / `select` counts per 10 s with `WINE_PS5_NTSYNC` unset
and set to `1`. A real win is those counts falling by an order of magnitude
and the frame time dropping with them. If the counts do not fall, the helper
did not join; fix that before looking at prospero-win’s sync patches.

**Helper round-robin is already the default** (`src/pw_helper_sched.h:6-26`).
Equal `SCHED_FIFO` threads never share a CPU: 32 busy FIFO threads at priority
700 gave a periodic thread 0 of 500 periods; `SCHED_RR` gave 497 of 500, p99
7 ms (`evidence/r4-sched-02.json`). WoW runs in a helper, so its threads
already take RR at 700 with the loader at 767. prospero-win’s patch 0882 is
the same mapping and is still opt-in (`wine/patches/0882-server-ps5-round-robin-scheduling-and-priorities.patch:13-21`,
`WINE_PS5_SCHED=1` or `/data/prospero-win/pw_sched`). Mihawk’s in-title server
path is the same opt-in (`wine/patches/0896-server-ps5-native-thread-scheduling.patch:14-15`).
Copying 0882 again does not change the helper WoW runs in.

### Worth taking from prospero-win

**1. Report the CPUs the process may run on, not the 16 online.**
Patch 0881 (`wine/patches/0881-ntdll-ps5-report-the-cpus-a-game-runs-on.patch:1-13`,
`:31-40`). Wine’s `sysconf` count is 16. A game process’s affinity is
`0x1fff`, CPUs 0–12. They measured 13 busy threads starting together and a
14th waiting until one blocked, because equal-priority FIFO threads do not
share a CPU. A worker per reported CPU therefore had three that did not run.
DXVK’s compiler pool and modern engines size themselves this way; VKD3D-Proton’s
worker pools do the same kind of thing.

Mihawk logs that mask and does not feed it to Windows
(`wine/ps5/pw_wine_process.c:1073-1094`). Patch 0620 only invents a topology
when `GetLogicalProcessorInformation` is missing
(`wine/patches/0620-ntdll-generic-processor-topology.patch:1-13`). Under
helper RR the three extra threads share CPUs instead of sleeping forever, so
this is hitching and cache traffic, not a hard stall. Expected effect: a
small frame-time change, larger if a job queue pins work to a thread the
scheduler rarely runs. Risk: low, a wrong mask under-subscribes the chip.
Effort: the function in 0881, then the gate.

Measure: `GetSystemInfo.dwNumberOfProcessors` and
`GetProcessAffinityMask` inside the WoW helper must equal the popcount of
`cpuset_getaffinity` (13 on the consoles measured so far). Then the same
three-run outdoor scene, plus a count of runnable threads against that mask.

**2. Stop CPU-copying a 1920×1080 present, using their VideoOut rule as the spec.**
prospero-win’s RADV path makes a 1920×1080 or 3840×2160 swapchain *be* the
VideoOut framebuffers, with no copy. Any other size is blitted, scaled and
centred, into the smallest of those two that holds it
(`docs/WINE_PS5_BUILD.md:676-686`). That needs their PS5_Mesa at `9d3cd41` or
later; older builds refused the odd sizes, leaked direct memory, and crashed
DXVK. One display, one swapchain presenting at a time (`:697-700`).

Mihawk’s broker still copies a 1920×1080 frame into the `64KB_R_X` buffer on
the CPU. The old pixel loop was 4.3–7.7 ms in a Direct3D 12 present; the SSE2
line copy is faster and is still a full-frame CPU copy
(`src/pw_display_copy.h:7-16`, `docs/PLAN.md:557-561`). At a 32.6 ms
CPU-bound frame, 5.6 ms of serial present is the difference between that frame
and about 27 ms, if the copy stays on VKD3D’s queue thread the way it did on
2026-10-17. That is the largest graphics number in either tree. It is not a
drop-in. prospero-win can skip the copy because the game process owns
VideoOut. Mihawk’s game is a helper and the title owns the display
(`docs/WINE_PS5_BUILD.md:671-675` is their “release the title’s video output
before RADV enumerates” step, which is the opposite ownership).

Expected effect if a 1080p WoW frame is still copied on the queue thread: on
the order of that 5–6 ms, and a higher ceiling only when the GPU really is
the 2 ms they measured. Risk: high. A helper that opens VideoOut fights the
desktop and the broker; a wrong tile format tears or blacks the frame.
Effort: a GPU blit or a scanout image in the broker, proved against
`pw_display_copy_lines`, not a transplant of their WSI. Measure: the existing
present milliseconds, fps, 1% low, and GPU-busy, same scene, copy path versus
blit path. Do this at WoW’s real resolution. A 1440p or windowed client takes
their blit path, not the zero-copy path.

**3. TSC-backed `QueryPerformanceCounter`, after a trace says it is hot.**
Patch 0900 and `wine/ps5/time/pw_qpc_clock.h:19-25`. The anchor is 10 MHz
(Windows’ QPC frequency), built from a validated TSC, and it expires within
one second so a migrated or stale clock cannot run away. The PE reader is
`rdtsc` plus a multiply, and a rejected anchor falls back to
`NtQueryPerformanceCounter`. It is opt-in. Mihawk has no equivalent; QPC in
this tree still goes through the native call (`wine/wineps5/unix.c:759`
reads it for the audio clock).

Expected effect: zero if QPC is a rare call; a visible CPU win if the client
or the loader calls it thousands of times a frame. Risk: medium. This loader
already issues direct syscalls and will notice a counter that jumps or does
not match the wall clock. Their one-second expiry is the right guard; it is
not a proof the loader accepts it. Effort: medium (the patch is PE and Unix,
and WoW64 has to pass the anchor through, which they did for 32-bit).
Measure first: a 30 s combat sample of syscall counts. Port it only if
`NtQueryPerformanceCounter` is among the top costs. Then fps, 1% low, and
whether the protection loader still gets past its check.

**4. Release image-view file descriptors on a long launcher session.**
Patch 0870, written up in `docs/IMAGE_VIEW_FD_LIFETIME.md:8-22`. A 600 s
capture released 68 image descriptors and sat at 343 open descriptors against
402–405 in earlier captures, with no file-limit error. Half-Life 2 timedemo:
59.41 fps with the option off, 59.37 on, 59.38 control. They publish that as
no timing change. Expected effect on WoW fps: none. Expected effect on a long
Battle.net session: staying under the process fd ceiling if image views are
what is climbing. Risk: a debugger attached later has no file handle for a
released view (`:65-71`); WoW’s loader is not that debugger, Battle.net’s
updater might care. Effort: medium, the patch is fussy about inode lifetime.
Measure: fd count at 1 min, 10 min and 60 min of the launcher plus client.
Port it when that curve climbs, not before.

### Plainly no help for this game

- **Direct3D 12.** No vkd3d-proton, so no DX12 device, no DX12 shader-cache
  format, no DX12 present settings, no wave32 workaround. Their DXVK
  2.6.2-prospero3 fork is a D3D9/10/11 stack. Mihawk’s DXVK pin
  `601930949d111edbbcf9dd463948426d9f8f6ddd` is 2026-09-15, later than that
  2.6.2 line (`tools/fetch-runtime.sh:24`, `NOTICE.md:51`). Downgrading DXVK
  to pick up their CS profiler would slow the launcher if Battle.net’s UI is
  D3D11, and it does not touch the WoW client.
- **Syscall numbers.** Their Wine 11.17 table does not make `0xe9` mean
  `NtFlushInstructionCache`. Detail in the next section. Taking it would move
  the ids WoW’s loader already misses.
- **The 32-bit DBT** (`wine/wowprospero`, `docs/DBT_BENCHMARK.md`). WoW Forever
  is 64-bit. The translator never runs its code.
- **PE32 Vulkan command-stream batching** (`docs/vulkan-present-runtime.md:1-8`).
  It batches 32-bit Unix crossings. A 64-bit client calls Vulkan natively.
- **Executable direct memory, copied into a helper.** Their self-check
  `mprotect`s a direct-memory page to read-execute and disables direct memory
  for the process if that fails (`wine/ps5/pw_wine_dmem_ps5.c:147-159`).
  Mihawk’s payload kernel keeps execute only on JIT memory and converts at
  the first execute (`wine/ps5/pw_wine_dmem_ps5.c:12-14`,
  `__wine_ps5_exec_convert` at `:821`). A helper that adopted their self-check
  would turn direct memory off on the first refused execute, which grows the
  small pool instead of shrinking it. `b4d63f4` already backs fresh anonymous
  mappings and registered sections with direct memory and still refuses
  execute there.
- **Desktop flicker.** Drag-select and icon hover are Mihawk’s shell list-view
  (`wine/patches/README.md` on the desktop plates and rubber band). prospero-win
  has no icon view. Its GDI path composites every visible top-level window
  into one buffer on flush and hands it to `pw_wine_present`
  (`docs/WINE_PS5_BUILD.md:437-439`). Its Vulkan path is one swapchain on one
  VideoOut display. Neither path is the desktop compositor. Replacing Mihawk’s
  retained window surfaces with that full-frame composite would repaint the
  whole desktop on every hover.

### RADV / shader-cache tooling is a measuring stick

`wine/ps5/pw_vk_radv_profile.h:7-18` and `docs/cs-thread-profile.md:12-27`
time every wrapped RADV entry and log whether `MESA_SHADER_CACHE_DIR` is
writable. The wrappers cost a few microseconds a frame plus two TSC reads per
call (`docs/cs-thread-profile.md:64-67`). Their Zink build disables the
shader cache (`tools/build_mesa_zink.sh:71`, `-Dshader-cache=disabled`). They
have no vkd3d pipeline cache. Mihawk already lost a DX12 launch to a missing
`vkd3d-proton.cache` when the process started in the wrong directory
(`docs/PLAN.md:1107-1108`). The WoW fix is a persistent cache directory and the right working
directory, which is local work. Borrow their per-frame RADV line for one
profiling session, then turn it off. `wine/ps5/vulkan/pw_vk_progress_guard.h:38-43`
keeps DXVK compile workers from holding a PE32 stream lock (GTA IV cold
cache: 300–500 ms frames). That lock is not the broker lock around
VKD3D-Proton’s `vkCreateGraphicsPipelines`.

## 1. Syscall numbers

Both trees use Wine’s hybrid table. Neither uses a Windows 10 19045 SSDT, and
neither renumbers in its PS5 patches. A search of both `wine/patches`
directories finds no edit to `ntdll.spec` or `ntsyscalls.h`. prospero-win says
so for one fast path: “No syscall number or dispatch-table entry changes”
(`docs/shared-input-fastpath.md:14`).

Wine assigns ids in `tools/make_specfiles` (`get_syscalls_str`). An export
marked `-syscall=0xNNNN` keeps that number. Exports marked bare `-syscall`
are sorted by name and fill the lowest free ids from 0. The explicit numbers
are the historical Windows ids Wine has written into the spec (NtWriteFile is
`0x8`, NtDeviceIoControlFile is `0x7`, NtProtectVirtualMemory is `0x50`).
They stop at `0xa6` in both pins. `NtFlushInstructionCache` is bare
`-syscall` in both specs.

The algorithm was checked against Wine 11.17’s committed
`dlls/ntdll/ntsyscalls.h` at prospero-win’s pin
`490f6d5dcbb2a5047345b8af88d114bbcaad69a8` (Wine version 11.17,
`tools/build_wine_ps5.sh:62`): 264 x64 entries, 0 mismatches.
`NtFlushInstructionCache` is `0x99` (header line 157), `0xe9` is
`NtSetInformationDebugObject` (line 237), `NtSetIoCompletion` is `0xef`
(line 243).

MihawkOS pins Valve wine `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`, Wine 11.0,
GE-Proton11-7 (`tools/build-wine-ps5.sh:59`, `tools/fetch-runtime.sh:22`).
The same algorithm on that `ntdll.spec` gives 258 x64 entries:

| Call | Mihawk Wine 11.0 | prospero Wine 11.17 | Win10 19045, as used by the loader |
| --- | --- | --- | --- |
| `NtWriteFile` | `0x8` (explicit) | `0x8` | `0x8` |
| `NtDeviceIoControlFile` | `0x7` (explicit) | `0x7` | `0x7` |
| `NtProtectVirtualMemory` | `0x50` (explicit) | `0x50` | not the question |
| `NtFlushInstructionCache` | `0x93` | `0x99` | **`0xe9`** |
| `NtSetIoCompletion` | **`0xe9`** | `0xef` | not `0xe9` |
| slot `0xe9` | `NtSetIoCompletion` | `NtSetInformationDebugObject` | `NtFlushInstructionCache` |

The six names Wine 11.17 added (`NtAlpc*`) shift the alphabetical fillers, so
149 shared names differ. `b4d63f4`’s `docs/PLAN.md:5-7` already read the
running Mihawk ntdll: thunks 0x20 apart, `NtWriteFile` id `0x8`,
`NtSetIoCompletion` id `0xe9`. That matches this table and is the console
evidence. A direct `syscall` with `eax = 0xe9` on today’s Mihawk ntdll enters
`NtSetIoCompletion`. On prospero-win’s ntdll it would enter
`NtSetInformationDebugObject`. Neither flushes the instruction cache.

Win32k/win32u ids are a second table. Wine 11.17’s generator places them at
base `0x1000` (`tools/make_specfiles` line 440 at that commit). That is
Wine’s base, not build 19045’s win32k base.

Conclusion for the renumber in progress: prospero-win is not a source for the
19045 table. Shipping their Wine 11.17 ids would point `0xe9` at a third
function and move every filler the loader might also call. The explicit
`-syscall=0xNNNN` entries are the only Windows-shaped ids either tree has,
and `NtFlushInstructionCache` is not among them. The 19045 map has to come
from a 19045 ntdll, then be checked with one PE that performs `syscall` `0xe9`
and checks that the instruction cache was the service that ran.

## 2. Memory

prospero-win’s model, firmware 12.02 (`docs/WINE_PS5_BUILD.md:160-190`): about
440 MiB of flexible memory for ordinary anonymous maps, up to 12 GiB of direct
memory, a 16 GiB virtual area at `0x1000000000`, host pages of 16 KiB with
Windows’ 4 KiB granularity on top (patch 0510, `:90`). Patch 0600 sends
`mmap` / `munmap` / `mprotect` inside the reserved areas through
`wine/ps5/pw_wine_dmem_ps5.c`. Map as read-write, then
`sceKernelMprotect`. Their comment says execute is refused at map time and
granted by `mprotect` (`pw_wine_dmem_ps5.c:6-12`). The self-check requires a
read-execute `mprotect` to succeed (`:155-158`). Private file views return
`ENODEV` so ntdll `pread`s into direct memory (`docs/WINE_PS5_BUILD.md:175-179`).
Patch 0895 also reserves `0x68000000-0x7f000000`, which had been holding
Battle.net’s 32-bit DLLs as flexible memory, 279 MiB
(`docs/WINE_PS5_BUILD.md:119`). Patch 0897 backs a fixed reservation outside
those areas with direct memory, because a bare reservation stays unbacked and
Steam’s 64-bit protection faulted on the first write (`:121`).

Mihawk kept that recipe and then split it for payload processes. The header
still says `mprotect` can grant execute, and the next sentence records the
later measurement: a payload’s kernel keeps execute only on JIT memory
(`wine/ps5/pw_wine_dmem_ps5.c:7-14`). `__wine_ps5_exec_convert` (`:821`)
copies the pages into a `sceKernelJitCreateSharedMemory` object, releases the
direct memory, and checks that the kernel now allows execute. JIT is charged
to the small resident pool. `b4d63f4`’s notes say sharing a JIT object does
not shrink that pool: the kernel charges the object in full in every process
that maps it (a launcher session moved from 1532 to 1568 MiB). Executable
pages cannot take direct memory there. That 1.5–1.6 GiB figure is the pool
the question is about; prospero-win’s “440 MiB flexible” is the title-process
accounting, not this helper accounting.

What that means for multitasking and for WoW:

- Data, heaps, anonymous sections and non-exec file views belong on direct
  memory. Both trees do this. `b4d63f4` extends it to fresh anonymous maps
  and to registered shared sections. prospero-win’s 0600/0897 are already the
  ancestors of Mihawk patches 0600 and 0790.
- Code that must run in a helper becomes JIT at first execute and stays in
  the small pool. Their self-check would, in a helper, fail and disable
  direct memory. Leave the convert path.
- 16 KiB host pages versus Windows 4 KiB: both have patch 0510. Mihawk also
  has the software 4 KiB MMU for stores beside a read-only 4 KiB page
  (`docs/MMU4K.md:1-8`, patch 1134). prospero-win does not. WoW’s allocator
  and the loader’s page protections are 4 KiB. The MMU is the local fix;
  their translator’s page tables are for i386 guests.
- Patch 0893 zeroes a decommit that shares a 16 KiB host page with pages that
  stay committed. Mihawk’s patch 0936 is the same bug. Already present.
- File descriptors: see the WoW section. No published kernel max in their
  tree; the evidence is the 343 versus 402 census and a 600 s run with no
  `EMFILE`.
- Multitasking: they start one game by replacing the process and cannot
  `fork`. Mihawk’s helpers are how a desktop and a game coexist. The pool
  cost of that is one JIT charge per helper, which their single-process
  model never pays and cannot teach us how to share.

## 3. Graphics, present, and the desktop blink

| | prospero-win | MihawkOS |
| --- | --- | --- |
| D3D9–11 | DXVK 2.6.2-prospero3, Win32 WSI, DLLs beside the game (`docs/WINE_PS5_BUILD.md:710-713`) | DXVK `60193094` (2026-09-15), both architectures, `platform/dxvk/patches/` |
| D3D12 | none | VKD3D-Proton `af89350` (2026-09-15) |
| Vulkan | ps5vk **or** RADV (`libvulkan_radeon.ps5.a` plus a two-function ICD shim, `docs/WINE_PS5_BUILD.md:656-665`). Mesa for Zink is `e4607ef697fe3df8d1ebc329bf1917d041843306` | RADV via PS5_Vulkan’s link, broker WSI. `NOTICE.md` records PS5_Mesa `7b59ef27` for the broker winsys |
| Present | `VK_KHR_win32_surface` becomes `vkCreateDisplayPlaneSurfaceKHR` on the one display (`docs/WINE_PS5_BUILD.md:666-675`). 1080p and 4K scan out directly; other sizes blit | Broker copy into `64KB_R_X` (`src/pw_display_copy.h:7-16`) |
| GDI | One 32-bpp surface per window, composed on flush to `pw_wine_present` (`docs/WINE_PS5_BUILD.md:437-439`) | Retained per-window surfaces, Z-order compose, present into another process’s window |

Their shader-cache story is the environment line in the RADV profile
(`docs/cs-thread-profile.md:20-24`): if the process cannot write the cache
directory, every pipeline is compiled again. That sentence is the useful
part, and Mihawk has already seen it as a missing `vkd3d-proton.cache`.

The desktop blink (rubber band, icon hover) is a shell repaint. prospero-win’s
compose copies every visible top-level window into one buffer whenever a
surface flushes. A hover that invalidated one icon would republish the whole
screen. Mihawk’s list-view draws the hover plate and the rubber band into
retained surfaces (`docs/PLAN.md` shell scorecards; patch 1105 and the
desktop list). Nothing in their present path identifies a partial update or
a front/back buffer swap of that plate. Their patch 0965 stops
`WM_WINE_SETPIXELFORMAT` from being posted to an unowned desktop, which had
reposted forever and pinned one core (Syndicate, black screen,
`docs/WINE_PS5_BUILD.md:126`). Mihawk’s explorer owns the desktop, so that
loop is a different bug; it is worth a log line if a Vulkan surface on the
desktop window ever spins a core during the launcher, and it is not the
blink.

## 4. The 32-bit translator

`wowprospero` is an i386-to-x86-64 re-encoder: guest GPRs pinned in host
registers, arithmetic flags left in `RFLAGS`, blocks chained, fault markers
instead of a per-access guard (`docs/DBT_BENCHMARK.md:191-230`). `wow64native`
is a separate “run 32-bit code on the CPU” backend for when the console
allows it; 0614 falls back to the translator when that backend does not start
(`docs/WINE_PS5_BUILD.md:104`).

Benchmarks, `docs/DBT_BENCHMARK.md`:

- Host, i7-12700H, one P-core, 7-Zip `7za b -mmt1 -md22`, DBT versus the same
  i386 binary under Wine’s `wow64cpu`: **91%** of native total rating after
  direct links, copied operands and superblocks (`:16`, `:287-291`). The
  first cut was about 12%.
- nbench x87 integer/FP **94% / 97%**, SSE2 **92% / 93%**, pi 4.2M digits
  **98%** (`:17-19`).
- Console, firmware 12.02, 2026-09-28: 7-Zip **3361–3369** total MIPS, up
  from 1383 (`:21-22`, `:503`). Against x64 builds run natively on the same
  console (a different binary: more registers, SSE2), they estimate the DBT
  at **85–93%** of native (`:523-528`).

Mihawk runs PE32 natively in WoW64 and did not take patches 0601, 0610 or
0710 (`wine/patches/README.md:21-24`). For a 64-bit WoW client this work does
not execute. `wine/wowprospero/smc_pages.h:12` names QEMU user-mode as the
approach for a code-page check; the tree does not vendor QEMU sources.

## 5. Fixes each tree has

prospero-win has 95 patches. MihawkOS has 310. 41 filenames match, and every
one of those 41 differs in size: Mihawk rebased them onto Wine
`1c785966`. The early series (0100–0730, the in-process server, the user
driver, direct memory, signals, the Vulkan display plane) is the shared
trunk, which `NOTICE.md` already credits.

prospero-only patches that still matter, beside the ones in the WoW section:

| Patch | What it fixes | Mihawk |
| --- | --- | --- |
| 0881 | CPU count from the process affinity | missing; log only |
| 0900 | validated TSC QPC | missing |
| 0870 | image-view fd release | missing |
| 0879 | a drained wake pipe left `QS_DRIVER` set, so a zero-timeout `GetDeviceState` spun while a button was held (`docs/WINE_PS5_BUILD.md:109`) | no equivalent found |
| 0891 | `VirtualProtect` on a written image page returns `PAGE_READWRITE` | patch 1004, on for every process, from the same Chromium failure (`wine/patches/1004-ntdll-ps5-written-image-pages-are-read-write.patch:1-19`) |
| 0882 | `SCHED_RR` and priority bands | patch 0896, and helper RR by default |
| 0884 | poll directory changes | patch 0902 |
| 0889 / 0898 | processor times and memory counters from the process | patches 0906 and 0905 |
| 0770 / 0790 | sync requests and immediate mutex on the client thread | patches 0918–0920, then ntsync 1180 |
| 0902 | extended-key flag on arrows and the right-hand modifiers | patch 1001 |
| 0965 | pixel-format message loop on an unowned desktop | explorer owns the desktop |
| 0878 | CA bundle beside the runtime | `share/wine/ca-certificates.crt` is already pinned in `NOTICE.md` |
| 0550 | no `NtCreateUserProcess` | Mihawk creates helper processes; required for Battle.net |
| 0610–0614, wowprospero | i386 DBT | native WoW64 instead |

Mihawk-only, and the reason a WoW port is not “rebase onto their tree”:
VKD3D-Proton and the DX12 device, helper processes and the GPU broker,
ntsync across processes, the 4 KiB MMU, JIT conversion, the shell, the
winedmo/FFmpeg media stack (patches 2001–2064), and the completion-port fix
on `b4d63f4` (their loader was storing `STATUS_OBJECT_TYPE_MISMATCH` from
`NtSetIoCompletion`, which is the call sitting on id `0xe9` today).

## 6. Safety

prospero-win’s app does not hard-code firmware offsets. The README and the
getting-started note both say the firmware-specific pieces are the jailbreak,
the ELF loader, and the bundled Lapy helper’s runtime layout checks
(`README.md:84-90`, `docs/GETTING_STARTED.md:160-165`).

Lapy is not Wine. `lapy.elf` is a prebuilt
[PS5 Lapy JB Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon), MIT,
Copyright (c) 2026 Arksama (`THIRD_PARTY.md:27-32`). The client in this repo
reads that ELF from `/app0/lapy.elf`, sends it to the local elfldr on port
9021, and exchanges a 24-byte message
(`native/pw_lapy_elevation.c:16-21`, `:141-148`). The only capability is
`LAPY_ELEVATION_FILESYSTEM` (`native/lapy_elevation_protocol.h:17-18`). After
the helper has checked the pid, the title calls `seteuid(geteuid())` as the
“credentials cloned” handshake (`pw_lapy_elevation.c:123-127`). The client
source here does not write kernel memory and does not embed a firmware
offset. The daemon it launches is firmware-specific and lives in another
repository; that is the piece their own docs say must match the firmware.
Copying the client does not import a brick path. Running an unmatched Lapy
build is the jailbreak’s risk, not a Wine patch’s.

Nothing in either Wine patch series programs the southbridge, the flash, or
a kernel text offset. Direct memory, JIT objects and VideoOut are the
ordinary title syscalls.

## 7. Licensing

prospero-win is LGPL-2.1-or-later, chosen so it can take Wine and, if they
ever needed it, sit beside GPL-2.0-only QEMU (`LICENSING.md:1-7`, `:31-47`).
MihawkOS is GPL-3.0-or-later (`LICENSING.md:1-8`). LGPL-2.1 section 3 lets a
recipient convey a copy of the library under the GPL, version 2 or any later
version. GPL-3.0-or-later is such a version. Their code may be copied into
this tree.

Conditions that matter here:

- Keep the copyright lines and say what changed. Their patch headers and
  `SPDX-License-Identifier: LGPL-2.1-or-later` are the attribution.
- A patch to Wine stays under Wine’s licence so it can be merged there.
  Mihawk already does this (`LICENSING.md:27-36`): `wine/patches/` is
  LGPL-2.1-or-later even though the title is GPL-3.0-or-later. New ports of
  their ntdll/win32u/server patches belong in that series, not rewritten
  without the notice.
- Files copied whole (`wine/ps5/time/pw_qpc_clock.h`, the image-fd server
  change, patch 0881) keep their LGPL-2.1-or-later header, the same way
  `wine/ps5/pw_ws2_32_libc.c` already records a prospero-win origin
  (`LICENSING.md:30-32`).
- Do not copy GPL-2.0-only QEMU. Their DBT cites it as prior art and does not
  vendor it. GPL-2.0-only cannot be combined with GPL-3.0-only code; this
  project’s “or later” does not repair a GPL-2.0-only inbound file.
- DXVK is zlib. Their 2.6.2-prospero fork’s patches, if any were wanted,
  need their copyright on top of DXVK’s. There is no WoW reason to take them.
- Lapy’s client glue in `native/pw_lapy_elevation.c` is LGPL-2.1-or-later.
  The prebuilt `lapy.elf` is MIT and is not in the prospero-win source tree.
  Mihawk’s own elevation path stays the one to ship.
- `NOTICE.md` already names prospero-win as the origin of the PS5 series.
  A new port adds a line for the files it brings, with `df9802f0` and the
  path.

## Ranked actions

Ordered for WoW Forever fps and for staying up. “Port” means code that is
theirs. The first row is ours, and it is first because their sync code is
the wrong place to spend the next change.

| # | Action | Expected effect | Risk | Effort | How to measure |
| --- | --- | --- | --- | --- | --- |
| 1 | Turn on Mihawk patch 1180 for the WoW helper (`WINE_PS5_NTSYNC=1`). Do not port their shared-mutex. | The other DX12 helper spent ~41 thread-seconds / 10 s in server trips and sat near 30 fps with the GPU busy 2 ms. Cutting `event_op` is the largest CPU number on record. | Medium: a helper that half-joins fails every wait (`1180` patch text). | Small to enable; medium if WoW’s waits misbehave. | fps, 1% low, `event_op` and `select` per 10 s, three runs, on and off. |
| 2 | Port 0881: `dwNumberOfProcessors` and the process affinity from `cpuset_getaffinity`. | Stops a 16-wide worker pool on a 13-CPU set. Under helper RR this is extra context switches, not a permanent stall. Small fps, fewer hitches. | Low. | Small. | The two Win32 queries inside the helper equal popcount(`0x1fff`) on that console; then the same scene. |
| 3 | Replace the CPU frame copy with a GPU blit or a scanout image, using their 1080p/4K rule as the acceptance test. | Up to ~5.6 ms off VKD3D’s queue thread when the frame is 1920×1080 and the copy is still serial. Less when WoW is not 1080p. | High: display ownership, tiling, desktop still has to composite. | Large. | `present` ms, fps, 1% low, GPU-busy, copy versus blit, at WoW’s resolution. |
| 4 | Port 0900 only if a combat trace shows QPC is hot. | A syscall removed from a hot path; nothing if it is cold. | Medium: the protection loader checks time. | Medium. | Syscall histogram first. Then fps and whether the loader still passes. |
| 5 | Port 0870 when a long session’s fd count climbs. | No fps change in their HL2 A/B (59.4 either way). Avoids a late `EMFILE`. | Medium for anything that debugs an image view. | Medium. | fd count at 1, 10 and 60 minutes. |
| 6 | Borrow the RADV per-frame profile for one session. | It costs time while it is on. It names the entry points in a slow frame. | Low if it stays off in play. | Small. | One logged scene, then delete the env var. |
| 7 | Finish the 19045 syscall map locally. Do not import their `ntsyscalls.h`. | `syscall` `0xe9` becomes `NtFlushInstructionCache`. Their table makes it `NtSetInformationDebugObject`. | High if ids move without a test that the loader’s other direct syscalls still hit the right services. | The renumber already underway. | One PE, `eax = 0xe9`, and a check that the flush ran. Repeat for every direct id the loader issues. |

Leave their DBT, DXVK fork, PE32 batching, Lapy daemon, and direct-memory
execute self-check out of this game’s plan.
