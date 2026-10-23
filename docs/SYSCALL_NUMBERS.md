# Syscall numbers: what the WoW loader's 0xe9 requires

This is an audit of the planned renumber of Wine's system-call ids to Windows
numbers. It does not renumber them. No console run was made.

The pin is ValveSoftware/wine `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`
(GE-Proton 11-7, `VERSION` says Wine 11.0, committed 2026-09-22). Line numbers
for `dlls/` and `tools/make_specfiles` are that checkout. It is not in this
repository; `tools/build-wine-ps5.sh` checks it out and applies `wine/patches/`.

## Which Windows table

This Wine reports Windows 10 build **19045** (22H2):

| Where a guest reads it | What it gets |
| --- | --- |
| `dlls/ntdll/version.c:171` | default `WIN10` row is 10.0.**19045** |
| `dlls/ntdll/version.c:176` | `WIN11` is 10.0.**22000**, and it is not the default |
| `dlls/ntdll/version.c:484` | `current_version = WIN10` |
| `dlls/ntdll/version.c:532-534` | that row is copied into the PEB |
| `dlls/ntdll/unix/env.c:1870-1872` | `init_peb` writes 10.0.19045 before `version_init` |
| `loader/wine.inf.in:1179-1180` | `CurrentBuild` and `CurrentBuildNumber` are `"19045"` |
| `programs/wineboot/wineboot.c:359-365` | `KUSER_SHARED_DATA` `NtBuildNumber` / `NtMajorVersion` / `NtMinorVersion` are copied from `RtlGetVersion` |

`virtual.c` does not set those version fields. `dlls/ntdll/unix/virtual.c:4818`
sets `KUSER_SHARED_DATA.SystemCall` (`wdm.h` offset `0x308`) to **1**. The x64
thunk in `include/wine/asm.h:287-301` tests byte `0x7ffe0308` and, because that
bit is set, skips its `syscall` instruction and `call`s `[0x7ffe1000]`. Patch
0530 keeps that slot inside the host page. A guest that executes its own
`syscall` instruction is a different path: `tools/build-wine-ps5.sh:1043-1054`
records that the kernel kills it (`SYSTEM_ILLEGAL_FUNCTION_CALL`). The only
functions allowed to contain that instruction in a PRX are
`__wine_syscall_dispatcher` and `__wine_unix_call_dispatcher`.

Two registry strings are stale and must not pick the table:
`wine.inf.in:1190` `DisplayVersion` is `"21H1"`, and `:1194` `ReleaseId` is
`"2009"`. Those names belong to the 19041/19043 era.

On **x64 build 19045 / 22H2** (j00ru column `Windows 10 (22H2)`, fetched
2026-10-23, 473 services, ids `0` through `0x1d8`, no holes):

| Id | Function |
| --- | --- |
| `0xe9` | **NtFlushInstructionCache** |
| `0x1a3` | NtSetIoCompletion |
| `0x7` | NtDeviceIoControlFile |
| `0x8` | NtWriteFile |

The same column is identical to `Windows 10 (21H2)` / build 19044 (0 differences).
Other builds, same column family, x64:

| Build | NtFlushInstructionCache | Names that differ from 22H2 |
| --- | --- | --- |
| 2004, 20H2, 21H1 (19041–19043) | `0xe8` | 310 of the 473 22H2 names differ or are absent (those tables have 471 names) |
| Server 2022 | `0xed` | 323 |
| Windows 11 21H2 (22000) | `0xee` | 323 |
| Windows 11 22H2 and 23H2 | `0xef` | 323 |
| Windows 11 24H2, 25H2, Server 2025 | `0xf1` | 363 |

x86 build 19045 is a different numbering. 469 of 473 names differ from x64.
`NtFlushInstructionCache` is `0x130`, `NtClose` is `0x18f`, `NtSetIoCompletion`
is `0x47`. x86 21H1 against x86 22H2 differs in 89 names; `NtFlushInstructionCache`
stays `0x130`.

The table for this crash is **x64 19045**, which is the same as x64 19044. Leave
the reported version at 10.0.19045. Moving `current_version` to `WIN11` would
make the guest ask for build 22000, whose `0xe9` is not `NtFlushInstructionCache`.

## What this pin actually numbers

`tools/make_specfiles:338-375` assigns ids. An explicit `-syscall=0xNNNN` is
kept. Every other `-syscall` takes the next free id, in alphabetical order.
Lines 353-357 abort when `scalar(@ids) - base > count`, so the ids have to be
a dense permutation of `base .. base+count-1`. `tools/winebuild/parser.c:495-502`
range-checks `-syscall=` (`< 0x4000`) and then **discards the number**. The
number that reaches the stub is the one in the generated `ntsyscalls.h`, via
`__ASM_SYSCALL_FUNC` (`dlls/ntdll/signal_x86_64.c:46-47`, `include/wine/asm.h:291-292`).

Replaying that assignment on this pin's `dlls/ntdll/ntdll.spec`:

| | 64-bit `ALL_SYSCALLS` | 32-bit `ALL_SYSCALLS32` |
| --- | --- | --- |
| Count | 258, ids `0..257` | 264, ids `0..263` |
| Extra names | | six `NtWow64*` (`-arch=win32`, spec lines 453-458) |
| Shared names | 253, **0 id differences** | same ids |

| Name | This pin | Windows 10 19045 x64 |
| --- | --- | --- |
| `0xe9` | **NtSetIoCompletion** | **NtFlushInstructionCache** |
| NtFlushInstructionCache | `0x93` | `0xe9` |
| NtSetIoCompletion | `0xe9` | `0x1a3` |
| NtSetIoCompletionEx | `0xea` | `0x1a4` |
| NtSetEaFile | `0xe2` | `0x193` |
| NtWriteFile | `0x8` | `0x8` |
| NtDeviceIoControlFile | `0x7` | `0x7` |
| NtClose | `0xf` | `0xf` |
| NtAllocateVirtualMemory | `0x18` | `0x18` |
| NtProtectVirtualMemory | `0x50` | `0x50` |
| NtQueryInformationProcess | `0x19` | `0x19` |

100 spec lines carry an explicit id (max `0xa6`). All 100 match 19045 x64,
including five `@ stub` lines (`NtMapUserPhysicalPagesScatter` `0x3`,
`NtApphelpCacheControl` `0x4c`, `NtCreateProcessEx` `0x4d`, `NtTraceEvent` `0x5e`,
`NtWaitForMultipleObjects32` `0x1a`). Of the 257 Windows names this pin
implements, **100 match and 157 do not**. Wine-only
`NtAlertMultipleThreadByThreadId` sits at `0x64`, which on 19045 x64 is
`NtAccessCheckByTypeResultList`. 216 Windows 19045 names have no Wine entry.
The highest Windows id among the implemented names is `0x1d4`
(`NtWaitForKeyedEvent`).

`tools/check-syscall-numbers.py --ntsyscalls` on a header generated by that
replay exits 1. The summary line is `100 match Windows, 258 entries, 1 Wine-only,
215 Windows holes, max id 0x101`, and the `0xe9` line is
`NtSetIoCompletion stub/slot 0xe9, Windows 19045 x64 wants 0x1a3`. The hole count
is 215 because `0x64` is occupied by the Wine-only name, so it is reported as
an occupation rather than an empty hole. 215 + 1 + 257 = 473.

The thunk the tracer logged is Wine's own. It is `0x20` bytes. `call [0x7ffe1000]`
is at `+0x18` and the following `ret` is at `+0x1f` (`asm.h:299-301`). The
dispatcher prolog pops that return address into `frame->rip` (`signal_x86_64.c:3351`)
and then subtracts `0xb` (`:3385`, for Legends of Runeterra). Patch 1244 adds
`0xb` back, so a logged caller inside one of these thunks ends in `0x1f`.
`docs/PLAN.md` callers `ntdll+0x5565F`, `+0x5563F` and `+0x5727F` all end in
`0x1f`. That is the guest entering the stub whose immediate is that id. It is
the same address whether the guest resolved the export or scanned ntdll for
`mov eax, imm` and called the stub it found. A raw `syscall` in the loader
would not return `0xC0000024`; the kernel kills it.

`NtFlushInstructionCache`'s first argument is a process handle.
`NtCurrentProcess()` is `(HANDLE)-1`. The function that owns `0xe9` today is
`NtSetIoCompletion`, whose first argument is an IoCompletion. A process handle
there is `STATUS_OBJECT_TYPE_MISMATCH` (`0xC0000024`), and a loader that then
reads that status as a pointer faults in `RtlTimeToTimeFields`. Renumbering
makes a scan for the Windows immediate call `NtFlushInstructionCache`.

`tests/test_pw_status_note.c:36-49` feeds the formatter the string
`NtSetIoCompletion`, id `0xe9`, handle `0x1c4`. That is a log-line fixture, and
it matches the completion-key lines `docs/PLAN.md` records. Those can be real
`NtSetIoCompletion` export calls on the path patch 1245 changes. Renumbering
does not fix that path. Leave the fixture on `NtSetIoCompletion` / `0xe9`.

## Upstream Wine and Proton

This Proton pin **is** Wine 11's scheme. It is not a full Windows numbering.

Wine **10.0** (`tools/make_specfiles` `get_syscalls_str`, tag `wine-10.0`)
sorts names and sets `id = index`. It ignores `-syscall=` numbers. Its
`ntsyscalls.h` has `ALL_SYSCALLS64`, 241 entries, ids `0..240`, **0 matches**
against 19045 x64. `0xe9` is `NtWaitForMultipleObjects`.
`NtFlushInstructionCache` is `0x47`.

wine-mirror **master**, fetched 2026-10-23 (not a pinned commit), still uses
the partial-pin header. 265 entries, max id `0x108`, 100 match 19045 x64, 164
differ. `0xe9` is `NtSetEaFile`. `NtFlushInstructionCache` is `0x99`.
`NtSetIoCompletion` is `0xf0`. Copying upstream's 100 pins leaves `0xe9` wrong.
Upstream does not fix this loader.

`make_specfiles:443-444` builds ntdll from base 0 and win32u from base `0x1000`.
This pin's `win32u.spec` has **481** syscalls and **zero** explicit numbers, so
they fill `0x1000..0x11e0`. Windows win32k 19045 x64 has 1316 entries, all in
table 1, `0x1000..0x1523`, identical between 21H2 and 22H2. Win32u is not the
`0xe9` bug. Renumbering it is a separate ship: `win32u.dll`, `win32u.prx.elf`,
and `wow64win.dll` together.

## The invariant that breaks

Every `#define SYSCALL_ENTRY(id,name,args) name,` array is packed. The id
argument is ignored. **Index equals id only while the ids are `0..N-1`.**
A Windows id of `0x1d4` in a 258-long array is out of range, and a packed
array that merely reorders the 258 names cannot land on `0xe9` =
`NtFlushInstructionCache` while also landing on `0x1a3` = `NtSetIoCompletion`.

Windows 19045 x64 has no holes. The holes are the 216 Windows services this
Wine does not implement. Two ways to satisfy `make_specfiles`'s dense check:

1. **Preferred.** Designated initializers, `[id] = fn`, so `ARRAY_SIZE` is
   `max_id+1` and a missing service is NULL. Relax the too-high check. Put
   `NtAlertMultipleThreadByThreadId` at `0x1d9` (still table 0, still below
   `0x1000`). Teach the dispatcher to treat a NULL slot as `bad_syscall`
   (`0xC000001C`). Today it does not: `signal_x86_64.c:3468-3470` compares
   `ServiceLimit` and then loads `ServiceTable[index]` with no NULL test.
2. **Reject for this loader.** Add `@ stub -syscall=` for the 216 missing
   names. Count becomes 474, max Windows id `0x1d8` passes the check, and the
   slot calls `stub_syscall` (`loader.c:116-126`), which raises
   `EXCEPTION_WINE_STUB` (`0x80000100`). A loader that treats the return as a
   pointer crashes on that status too. The five stubs already in the spec do
   this today; do not add 216 more as the hole behaviour.

`0xC000001C` is what `bad_syscall` returns (`signal_x86_64.c:3646-3647`, and
`signal_i386.c:2790-2791`). `Wow64SystemServiceEx` returns the same status when
`id >= ServiceLimit` (`dlls/wow64/syscall.c:1047-1050`) and also does not test
for NULL.

## Minimal port for this crash

64-bit loader only:

1. Retarget the `_WIN64` half, `ALL_SYSCALLS`, to the 19045 x64 numbers.
   Leave `ALL_SYSCALLS32` dense and unchanged, so `i386-windows/ntdll.dll` and
   `wow64.dll` stay as they are.
2. That requires a split `make_specfiles` does not have. One `-syscall=` value
   is pushed onto both lists (`make_specfiles:402-403`). Shared names currently
   have the same id, which is why a single number has worked. x64 numbers
   applied to the 32-bit list would be wrong for 469 of 473 names.
3. Rebuild and install **both** the PE and the Unix module. The stub immediate
   lives in `lib/wine/x86_64-windows/ntdll.dll` (`tools/stage-wine.py:254`
   copies the host tree, `:333-339` overlays `.deps/wine-ps5/pe/x86_64-windows`
   into the runtime and into `system32`). The dispatcher lives in
   `lib/wine/x86_64-unix/ntdll.prx.elf` (`stage-wine.py:307-310`). Firmware 12
   maps the `.prx.elf` sibling (`docs/PLAN.md`). Replacing only the elf
   dispatches every export through the old immediate into the new table.
4. `wineserver` is not on the syscall-id path. Patch 1245's completion-port
   behaviour is a separate change. Do not rebuild it for the renumber.
5. Revert is both PE `ntdll.dll` and `ntdll.prx.elf`, restored as a pair.
   The signed `ntdll.prx` is not what firmware 12 maps; shipping a new `.prx`
   beside an old `.prx.elf` leaves the old dispatcher in place. Nothing here
   touches the kernel or firmware.

## Sites indexed by the syscall id

Packed arrays in the pin. These break if the ids stop being `0..N-1` and the
macro stays `name,` rather than `[id] = name`:

| Site | Lines | What the index is |
| --- | --- | --- |
| `dlls/ntdll/unix/loader.c` `syscalls[]` | 142-147 | Unix `KeServiceDescriptorTable[0].ServiceTable` |
| same file `syscall_args[]` | 149-154 | `ArgumentTable`; the dispatcher reads one byte per id (`signal_x86_64.c:3471-3472`) |
| same file `KeServiceDescriptorTable[0]` | 156-159 | `ServiceLimit = ARRAY_SIZE(syscalls)` |
| same file `ntsyscall_names[]` | 161-166 | names for the tracer |
| same file `trace_syscall` | 729-734 | `names[id & 0xfff]` with **no length check** |
| `dlls/ntdll/unix/signal_x86_64.c` dispatch | 3459-3472 | table = bits 12-13 (`(eax>>8)&0x30`, 32-byte `SYSTEM_SERVICE_TABLE` stride), index = `eax & 0xfff`, then `ServiceTable[index]` |
| `dlls/ntdll/unix/signal_i386.c` dispatch | 2667-2669 | same split; this unix file is not linked into the PS5 x86_64 ntdll |
| `dlls/wow64/syscall.c` `syscall_thunks[]` / `syscall_args[]` | 46-62 | `ALL_SYSCALLS32`, `ServiceLimit = ARRAY_SIZE` |
| `dlls/wow64/syscall.c` `Wow64SystemServiceEx` | 1041-1052 | `id = num & 0xfff`, `table = (num>>12)&3`, calls `wow64_NtFoo` by **name**, not by the 64-bit number |
| `dlls/wow64win/syscall.c` `win32_syscalls[]` | 49-67 | win32u's `ALL_SYSCALLS32` |
| `dlls/win32u/syscall.c` `syscalls[]` / `arguments[]` / `syscall_names[]` | 64-83 | packed; `KeAddSystemServiceTable(..., 1)` at line 103 registers table 1 |
| `dlls/ntdll/unix/server.c` | 1773 | `thread_data->syscall_table = KeServiceDescriptorTable` |
| `dlls/ntdll/unix/thread.c` | 1140 | same |
| `dlls/ntdll/unix/unix_private.h` | 105, 124 | `syscall_table` at TEB offset `0x370` on x64; the dispatcher loads it (`signal_x86_64.c:3464`) |

These **use the id** and follow a renumber, as long as the PE and the Unix
module are rebuilt from the same header:

| Site | Lines |
| --- | --- |
| `include/wine/asm.h` `__ASM_SYSCALL_FUNC` | 227-301 (x64 form 287-301: `mov eax, id`) |
| `dlls/ntdll/signal_x86_64.c` PE thunks | 46-47 |
| `dlls/ntdll/signal_i386.c` PE thunks | 85 |
| `dlls/win32u/main.c` thunks | 2563-2569 (`__ASM_SYSCALL_FUNC(id, name)`). The enum at 42-44 is the arm64ec build, which this title does not ship |

`tools/make_specfiles:353-357` rejects a max id past `count-1`. A faithful
19045 assignment (max implemented id `0x1d4`, or `0x1d9` with the Wine-only
service parked after the Windows range) does not pass that check at count 258.
`tools/winebuild/parser.c:495-502` will not preserve a number you add to the
spec; the generated header is the only consumer.

### In this repository

| Site | Lines | On a renumber |
| --- | --- | --- |
| `wine/patches/0916-ntdll-ps5-extend-narrow-syscall-arguments.patch` | 46-51 define `PS5_SYSCALL_<name> = id` and `ps5_narrow_syscall_args[0x4000]` with designated initializers. 113-116 index it with `frame->syscall_id & 0x3fff` | **Survives**, if every id stays `< 0x4000` and every ntdll id stays `< 0x1000`. `0x1d9` does. The mask keeps bit 12, so a win32u id indexes the high half of the same array |
| `wine/patches/0917-ntdll-ps5-extend-win32u-narrow-syscall-arguments.patch` | 96-103 add `PS5_WIN32_*` from `win32syscalls.h` into that same `[0x4000]` array. The live table is this patch, not 0916 | **Survives** under the same cap. A ntdll id `>= 0x1000` collides with a win32u slot. Five win32u entries are narrowed: `NtUserSetClassWord`, `NtUserSetWindowWord`, `NtUserVkKeyScanEx`, `NtGdiDoPalette`, `NtUserSetLayeredWindowAttributes` |
| `wine/patches/1244-ntdll-ps5-name-the-syscall-that-returns-type-mismatch.patch` | 36-43 `ps5_syscall_name`: `idx = (id>>12)&3`, `num = id&0xfff`, returns `ntsyscall_names[num]` when `idx==0` and `num < ARRAY_SIZE` | **Breaks** if `ntsyscall_names` stays packed. The comment says the array is "the 32-bit numbers". On the 64-bit unix build `ALL_SYSCALLS` is the 64-bit list. The two lists happen to share ids today, so the comment is only true until the split |
| `wine/patches/1245-ntdll-ps5-post-iocompletion-onto-the-port.patch` | extends the 1244 line (handle, guest, type). It does not index by id | Unaffected. wineserver is not required for the renumber |
| `wine/patches/1113-ntdll-ps5-say-which-system-call-a-sampled-thread-is-inside.patch` | 21-30 `pw_wine_describe_thread` returns `0x10000 \| frame->syscall_id` | The id is whatever `eax` was. No table. The printed number changes when the stubs change, which is what a profile should show |
| `native/proton_wine_child.c` | 883-889 stores that value; 943-944 prints `call & 0xffff` | Keeps the low 16 bits, including table bit 12. No hardcoded map |
| `tools/profile-report.py` | 30-42 look for `ALL_SYSCALLS64`. Wine 11 has no such macro (`#ifdef _WIN64` / `ALL_SYSCALLS`). `str.find` returns -1, and `text[-1:]` is the last character, so the map stays empty and every call is printed `?`. Line 142 then masks `call & 0xfff`, which drops the win32u table bit when a map does exist | Already blind on this pin. Point it at the `_WIN64` half or the new numbers never get names. `docs/PROFILING.md:9` still says `ALL_SYSCALLS64` |
| `tests/test_pw_status_note.c` | 36-49 | Formatter fixture. Keep `NtSetIoCompletion` and id `0xe9` |
| `wine/ps5/pw_wine_status.h` | 15-21 | Comments on the completion-port path. Not a table |
| `tools/build-wine-ps5.sh` | 1013, 1043-1054 `SYSCALL_ALLOWED` | Unchanged by renumbering. A new raw `syscall` inside a PRX function fails the build. The PE thunk's `syscall` byte is in `ntdll.dll`, which this scan does not disassemble; `SystemCall=1` skips it |
| `wine/prospero-adaptations.patch` | 1210 | Older copy of that allow list |
| `tools/stage-wine.py` | 82, 254, 307-310, 333-339 | Where the PE and the `.prx.elf` are installed. Both have to move together |
| `tools/ia32_decode_oracle.c` | 396 | `0xe9` is an XOP opcode byte in a decoder fixture. Not a syscall id |

`docs/SYSCALL_TRACE.md` describes a direct-syscall tracer that is not on this
branch (it lives on `archive/direct-syscall-tracer-2026-10-10`). Nothing in the
current dispatcher records a private syscall number from the guest beyond
`frame->syscall_id`, which is `eax` at the thunk.

## Risks

1. **Half install.** A new `ntdll.prx.elf` with an old `ntdll.dll`, or the
   reverse, sends every export to the wrong function. GetProcAddress callers,
   which are fine if both halves move together, break if only one does.
   Firmware 12 maps `.prx.elf`. Revert the pair.
2. **NULL slot.** Designated holes are NULL. The x64 dispatcher calls the
   slot with no NULL test (`signal_x86_64.c:3468-3470`). A missing service
   must return `0xC000001C` before that load. `EXCEPTION_WINE_STUB` is the
   wrong status for a hole.
3. **Name array still packed.** `ps5_syscall_name` (patch 1244:36-43) and
   `trace_syscall` (`loader.c:729-734`) index `ntsyscall_names[id & 0xfff]`.
   After the renumber the name at a Windows id is wrong, or the read is past
   the array, until that array is `[id] = #name` and its size is `max_id+1`.
   The 1244 comment about "32-bit numbers" becomes false the moment 32-bit and
   64-bit ids diverge.
4. **One spec number for both arches.** Leaving `make_specfiles` as it is and
   writing 19045 x64 numbers into `-syscall=` retargets `ALL_SYSCALLS32` too.
   Wow64 and the 32-bit ntdll then use x64 numbers. 469 of 473 x86 names differ.
5. **Id `>= 0x1000` on an ntdll service.** Bit 12 selects the win32u table in
   the dispatcher, and it indexes `ps5_narrow_syscall_args` in the same
   `0x4000` array as win32u. `0x1d9` is safe. `0x1000+` is not.
6. **Version drift.** `DisplayVersion=21H1` must not choose the 19043 table
   (`0xe8`). Flipping the default to `WIN11` (22000) chooses `0xee`.
7. **Win32u pulled in by accident.** Its 481 packed entries have the same
   invariant. The `0xe9` fix does not need a win32u rebuild. A win32u rebuild
   from a half-edited spec, installed as `win32u.prx.elf`, breaks Win32k
   callers. The `--no-opengl` win32u this tree sometimes links is not a
   replacement for the title's OpenGL `win32u.prx.elf`.
8. **The formatter test "fixed" to NtFlush.** `test_pw_status_note.c` would
   then reject the log line patch 1245 is testing. The fixture is the string
   the tracer prints for whatever function returned `0xC0000024`, and that
   function's current id is `0xe9`.
9. **Profiler stays unnamed.** `tools/profile-report.py:38` does not see a
   Wine 11 header. A console profile after the renumber still prints `?`
   until that lookup reads the `_WIN64` half.
10. **Licence of the oracle.** j00ru/windows-syscalls published no licence
    file on 2026-10-23. `tools/syscall-numbers/*.json` is the name-to-number
    column only, recorded in `NOTICE.md`. It is not linked into the title.

## Checks the implementer must pass

Host, before any console claim:

1. `python3 tools/check-syscall-numbers.py --self-test` exits 0.
   `tools/host-tests.sh syscall_numbers` runs that and nothing else.
2. `python3 tools/check-syscall-numbers.py --ntsyscalls <generated dlls/ntdll/ntsyscalls.h>`
   exits 0. The script reads the `#ifdef _WIN64` half and compares it to
   `tools/syscall-numbers/win10-19045-x64.json`. Required results: `0xe9` is
   `NtFlushInstructionCache`, `NtSetIoCompletion` is `0x1a3`, no Wine-only name
   sits on a Windows id, no ntdll id is `>= 0x1000`. Empty Windows ids are
   counted as holes and are not errors.
3. The `#else` half (`ALL_SYSCALLS32`) is unchanged from this pin if 32-bit
   ids were not deliberately retargeted. Do not require the x86 Windows table
   for this bug. The checker's `--windows-arch x86` flag still parses the
   `_WIN64` half; it is the wrong command for the 32-bit list.
4. `python3 tools/check-syscall-numbers.py --pe <lib/wine/x86_64-windows/ntdll.dll> --unix <unstripped ntdll.so or ntdll.prx.elf>`
   exits 0. Every exported `Nt*`/`Zw*` stub's `mov eax` immediate equals the
   19045 x64 number, and `KeServiceDescriptorTable[0]` maps that id to the
   same `Nt*` symbol. `--pe` without `--unix` fails. A stripped elf fails.
   `Zw*` forwarders must resolve to the `Nt*` stub.
5. A Windows id this Wine does not implement is a NULL slot or
   `id >= ServiceLimit`, and the dispatcher returns `0xC000001C`. It must not
   call through NULL and must not raise `EXCEPTION_WINE_STUB`. The number
   checker does not prove this; the NULL test in `signal_x86_64.c` has to be
   visible in the diff.
6. `ps5_narrow_syscall_args` still builds. Every id is `< 0x4000`. Every ntdll
   id is `< 0x1000`.
7. `RtlGetVersion` / the PEB / `CurrentBuildNumber` stay 10.0.19045.
   `DisplayVersion` is left alone.
8. `tests/test_pw_status_note.c` still expects `syscall=NtSetIoCompletion` and
   `id=0xe9`.

Then one console run, reported at the same detail as a failure: the loader's
`0xe9` return is named `NtFlushInstructionCache`, and that id does not return
`0xC0000024` for handle `-1`. The 64-bit gate battery still passes, because
export calls move the stub immediate and the table together. This audit did
not run the console and does not claim that result.

`python3 tools/check-syscall-numbers.py --self-test` on this branch prints
`self-test: Windows 19045 x64 NtFlushInstructionCache=0xe9 NtSetIoCompletion=0x1a3`
and exits 0. A header or a dispatcher that places `NtSetIoCompletion` at `0xe9`
fails that test. `tools/host-tests.sh syscall_numbers` is the same command. It
first runs `tools/setup-graphics-foundation.sh`, which exports a pinned tree
from `../PS5_Vulkan`. That sibling is not in this checkout, so the wrapper
exited at the archive step. The python command itself is what passed.
