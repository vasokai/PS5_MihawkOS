# Execute on the application pool

2026-10-22, with the public-record and PS5LM sections added 2026-10-23. No console
run. No kernel byte was written. No new eboot, payload, or `param.json`.

Windows commits and the Unix heap already come from the application pool
(`sceKernelAllocateDirectMemoryForApp`, 11,712 MiB). Executable copies do not.
The kernel drops execute from direct memory, so each helper's generated code
lives in a JIT object charged to flexible memory. A multi-process Chromium-style
session has put about 15–57 MiB of that copy in each helper, about 725 MiB
across 13 helpers. The raw resident census then spikes through the firmware 12
mark of 1600 MiB and the title waits in `vmwait`. The application pool still
has on the order of 10 GiB free while that happens.

This note is why a kernel patch for that was not written.

## What was asked

Three options, in order, only if the worst case of a failure is a kernel panic
or a reboot. A failure that can brick the console or corrupt storage is not
implemented.

1. Allow execute on direct-memory mappings of this title's processes only, by a
   live patch of the protection check, reverted on the way out.
2. Otherwise raise this title's flexible budget at runtime, from the process
   budget or by moving budget from the direct partition, to several GiB.
3. Otherwise the next change that stays inside the same rule.

The patch has to be off unless a flag says otherwise, has to check the installed
firmware and the bytes at every site, and has to change nothing when those do
not match. It may touch only live RAM. It must not write flash, NOR, a system
partition, `/system`, `/preinst`, firmware, the registry, an SFO, or a boot
file. It must not touch a storage driver, the filesystem, or the SAMU.

## The firmware words already in this tree

`kern.sdk_version` is the installed firmware. `sceKernelGetSystemSwVersion` is
spoofed and is not the word a patch may trust (`src/pw_firmware.h`).

| Console | Installed word | Where it was read |
| --- | --- | --- |
| The session behind the 2026-10-22 flexible-pressure log | 12.20 | The title log quoted in [PLAN.md](PLAN.md) |
| The base PS5 this tree's gate ran on | 13.40 (`0x13400002`) | `kern.sdk_version`, 2026-10-20 |
| The Pro this tree's gate ran on | 10.01 (`0x10010000`) | `kern.sdk_version`, 2026-10-20 |

The public payload SDK (the same major line as the pinned v0.42 crt) has
kernel *data* offsets for 12.00 through 12.70 as one group, which includes
12.20, and a separate group for 13.40 and 13.42. Those offsets find the
process list and a vmspace. They are not an instruction site, and they were
not checked against a dump of any of these three consoles for this note.
This repository does not contain a firmware image. [PROVENANCE.md](PROVENANCE.md)
keeps it that way.

## What the jailbreak actually gives this process

kstuff is loaded by hand each boot. It is not part of this repository. It does
not patch kernel text in place. Kernel text is execute-only under the
hypervisor, so a payload cannot read the bytes at a code address and a store
to that address does not change the instruction. kstuff sets a debug breakpoint
and skips a few instructions when it hits. One of those breakpoints is the
general execute check inside `mprotect`. It is already active on a console that
can create JIT memory.

The payload SDK's crt then offers `kernel_copyout` / `kernel_copyin` over the
pipe kstuff hands the payload, and `kernel_set_vmem_protection`, which walks
the vm_map without the map lock and writes one protection byte at offset 0x64
of the entry. Lapy uses the same pipe for a credential copy. It checks that
exactly one credential slot matches and refuses otherwise. It does not change
a mapping's protection. `platform/lapy/README.md` records that.

Helpers in this tree wrap both `kernel_mprotect` and
`kernel_set_vmem_protection` so the call returns `EPERM` and the write is never
made (`native/proton_payload_child.c`). Wine asks the kernel with
`sceKernelMprotect` (system call 74) instead.

## What the console already measured

These runs exist. Nothing below is a new claim.

**The kernel drops execute, including after kstuff's breakpoint.**
`exec-kernel-2026-10-14-02` and `-03`, with the SDK write wrapped out
([PLAN.md](PLAN.md), "Execute through JIT memory"):

- A JIT object mapped read-write, then `sceKernelMprotect(7)`, stays `0x7`.
  The code runs.
- `sceKernelMapDirectMemory` with protection 7 fails `0x80020016`.
- An anonymous mapping asked for as read-write-execute comes back `0x3`.
- `sceKernelMprotect` on direct, flexible, file and granted memory returns
  success and the queried protection has no execute bit.
- 4,096 JIT objects of 16 KiB stayed executable and were charged to resident
  flexible memory (69,136 KiB while held).

So the breakpoint kstuff already installs is not the clip that refuses direct
memory. There is a second check. Its address is not in this repository, and it
cannot be confirmed by reading kernel text.

**The vm_map byte is not a safe place to add execute.** The SDK walk finds the
entry with kernel reads and stores the protection byte. It does not hold the
map lock. A splay tree rotates under another thread's `mmap` or `mprotect`,
the walk misses, and the grant comes back as `EPERM`. A miss is not the only
failure: a store into the wrong entry corrupts the map. That write was removed
after the 2026-10-14 freeze review. Separately, the console was factory-reset
twice after hard hangs ([PLAN.md](PLAN.md)). This note does not claim the
protection store was the only cause of those resets. It does refuse to repeat
the store. Stopping the process first would remove one race and would leave
every other way a bad byte in the map can stick the pager. A hang there is not
known to stop at a panic, and the resets that followed the earlier hangs were
data loss.

**Sharing the JIT object does not shrink the charge.** One 16 MiB object added
16 MiB to the taker's resident flexible memory and left the maker at 16 MiB.
Two mappings in one process counted 32 MiB. A later session with sharing on
ended in the guard more often than the same session with sharing off
([PLAN.md](PLAN.md), the 2026-10-20 RSS note). The hook that would share
identical bytes stays off.

**The flexible budget cannot be raised through `param.json`.** The kernel
reads `kernel.flexibleMemorySize` from the title folder at launch
(`docs/history/2026-10-07/docs/GRAPHICS_PROCESS_BROKER.md`):

| Run | Request | Result |
| --- | --- | --- |
| `flexible-memory-param-01` | 1,073,741,824 | `FMEM size: 0x40000000`. Free flexible memory at start 1,037,041,664 (was 433,061,888). Direct memory 12,259,885,056 (was 12,863,864,832). |
| `flexible-memory-param-02` | 2,147,483,648 | Launch refused, `0x80020016`. The kernel log says the property must be between 2,097,152 and 1,073,741,824. |
| `flexible-memory-param-03` | 1,073,741,824 | Restored. The 1 GiB request is accepted again. |

The 576 MiB added to the flexible pool was taken out of the direct pool. The
total did not grow. Production `ps5/sce_sys/param.json` already asks for
1,073,741,824, the top of the range the kernel accepts. Helpers stay budget 0,
so their JIT is not paid from that 1 GiB title pool alone. The resident census
the firmware 12 swap and `vmwait` behaviour follows was measured at 1600 MiB
(`src/pw_memory_guard.h`, base PS5 13.40, checked on a second 13.x console).

## Why option A is not in this change

A store to kernel text fails the byte check before it starts: the bytes cannot
be read, because the text is execute-only. Shipping a breakpoint at an address
taken from a different firmware, or from a data offset, fails the same rule.
The public data offsets for 12.20 and 13.40 were not produced by tracing the
direct-memory clip on these consoles.

The write that *can* be byte-checked is the vm_map protection byte, because
that object is kernel data. Checking it and storing execute is the operation
this tree already removed. Doing it again, limited to this title's pids, still
writes a live map. The earlier hard hangs were not known to stop at a panic,
and they were followed by factory resets. That is data loss. The rule for this
change says not to ship an approach whose worst case cannot be kept to a panic
or a reboot.

A store that changes a vm object's type, or a page-table entry, so the pager
will treat an application page as a file page, can write that page to the SSD.
That can corrupt storage. It is not shipped either. No site for it was
verified, so there is no "expected bytes" to compare.

## Why option B is not in this change

The supported request is already at the kernel's maximum. A larger
`flexibleMemorySize` does not boot. Raising a budget field inside a process
structure at runtime is a kernel-data store with no verified layout for 12.20
or 13.40. A value that merely raises an accounting cap, while the physical
flexible pool stays the size the 1600 MiB census measured, does not give the
session several GiB. It lets the title allocate until the pager runs, which is
the freeze this change was meant to avoid.

Moving pages from the direct partition onto the flexible free list is the same
allocator the buffer cache uses. A bad splice can hand a file page to the
title or a title page to the filesystem. The worst case is not a panic. It is
a write to the SSD. That option is not implemented.

## Worst case

| Approach | Worst case that can actually be claimed | Shipped |
| --- | --- | --- |
| No write. This change. | Nothing on the console changes. A reboot is the reboot the console already does. | Yes |
| Breakpoint or store at an unverified code site | A clean panic is possible. So is a hang in the pager of the kind that was followed by a factory reset. Not guaranteed to stop at a panic. | No |
| vm_map protection byte, the SDK write | A corrupt map, then a pager hang. Hangs of that period were followed by a factory reset. Not guaranteed to stop at a panic. | No |
| Budget or page-list edit | A pager write of the wrong page. SSD corruption, not only a reboot. | No |

## Public record, 2026-10-23

No console run. No kernel byte was written. The sources below are public
project pages and the measurements already in this tree. Nothing here was
re-run.

### Scene work on executable direct memory

No public project found for this note has a firmware-matched site that makes
the application pool executable on 11.x, 12.x or 13.x.

kstuff does not patch kernel text. Kernel text is execute-only under the
hypervisor (`nda` / `xotext` on [psdevwiki's kernel page](https://www.psdevwiki.com/ps5/Kernel);
the same constraint is how [PS5_kldload](https://github.com/buzzer-re/PS5_kldload)
describes kstuff). kstuff sets a debug breakpoint and skips six bytes
(`mprotect_fix_start` through `+6`). Sleirsgoevy's porting tool finds that
site by tracing `mprotect` of one page with protection 3 and again with
protection 7 and taking the first instruction that differs. EchoStretch's
kstuff-lite publishes a 12.20 table (`prosper0gdb/offsets/12_20.h` in commit
`9adc150`): `allproc` `0x2885e00`, `mprotect_fix_start` `-0x9D4F23`. The 12.02
table in the same commit uses `-0x9d4da3` for that site, so a 12.02 address is
the wrong instruction on 12.20. The 12.20 file does not mark the code sites
confirmed. The data offset `0x2885e00` matches the payload SDK's one group for
`0x12000000` through `0x12700000`, which includes 12.20 (`crt/kernel.c` on
current master). That group finds the process list. It is not an instruction.

The breakpoint is the one this tree already measured. `exec-kernel-2026-10-14-02`
and `-03` ran with it active, and direct, flexible, file and granted memory
still came back without execute. Publishing the 12.20 address does not add a
second clip. A breakpoint taken from 12.02, or from a data offset, fails the
byte check before it starts: the bytes cannot be read.

The payload SDK's `kernel_set_vmem_protection` walks the splay tree with
`kernel_copyout` and stores one byte at offset `0x64` (`crt/kernel.c`). It
does not take the map lock. HackTricks' PS5 note describes the same edit and
says the execute bit has to be added to `protection` and to `max_protection`.
The SDK writes only the byte at `0x64`. elfldr uses that write for `PF_X`
segments. This tree wrapped the call out after the 2026-10-14 freeze review.

JIT shared memory is the path the scene actually ships.
`sceKernelJitCreateSharedMemory` plus an alias is what homebrew uses for
executable pages (the reversing.codes ELF-injection note; Gezine's loaders).
That object is the one this tree measured: it stays executable, and it is
charged to resident flexible memory (K7, 69,136 KiB for 4,096 objects of
16 KiB).

GPU command-buffer DMA and page-table edits (the lapse / YarP2JB family, and
`ftpsrv`'s `mmap_self`, which swings a vnode pager-ops pointer) are kernel
data writes aimed at a physical page or a pager. A wrong target can be a file
page. They are not a CPU execute path for the application pool, and they are
not a candidate under the storage rule.

ShadPS4 and PS5PCEM model direct and flexible memory in an emulator. They do
not change a console kernel. ShadPS4's `extraDmemInMbytes` is an emulator
setting.

### PS5LM's 8 GiB, and why 1600 MiB is a different number

[cobanov/PS5LM](https://github.com/cobanov/PS5LM) `docs/CONSOLE.md` and
`probes/memprobe/main.c` measure one payload process. The page's setup line
is a Slim (CFI-2015) on firmware **13.60**, kstuff-lite 1.11 and etaHEN 2.5B.
It is not a 12.20 reading. This tree has no 12.20 reading of
`sceKernelConfiguredFlexibleMemorySize`.

| PS5LM, one budget-0 payload, firmware 13.60 | Value |
| --- | --- |
| Flexible configured / free | 8.00 / 7.97 GiB |
| `malloc` ceiling, 256 MiB steps, pages touched | 6.25 GiB |
| Largest `posix_memalign` block | 6.00 GiB |
| Direct memory | size 0; allocate fails `0x80020023` |

`0x80020023` is `0x80020000 + EAGAIN` (35). The pool is empty. The same code
is what this tree's ordinary payload allocator returned for a 2 GiB direct
allocation, while `sceKernelAllocateDirectMemoryForApp` succeeded
([WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)). elfldr reaches that budget
by `sys_budget_set(0)` (syscall `0x23b`) before it execs `SceSpZeroConf`
(`ps5-payload-dev/elfldr`, `elfldr.c`). etaHEN's jailbreak request, a rename
of `{"PID":n}` onto `/download0/etahen_jailbreak`, changes uid, sandbox and
authid. PS5LM's page says it does not change the budget, and that kstuff
patches no budgets.

The longer write-up of the same console in abhi245y's PS5LM `docs/CONSOLE.md`
reads the budget with `budget_getid` (`0x261`) and `budget_get` (`0x23a`).
Budget id 0, with the native app open: resource 3 limit 8 GiB, 104 MiB used,
which that page identifies as the flexible memory the payload reports.
Resource 1 is 688 MiB limit, 632 MiB used. Resource 2 is a 16 GiB limit with
7.0 to 7.2 GiB used; the page says that resource is not identified. Other
budget ids up to 4095 answer 3. The title's own budget is not readable from
the payload. Those syscalls were not run for this note.

Holding about 6 GiB of that pool froze the home screen until the payload
process ended. PS5LM's own plan is 4 to 5 GiB. Unlocked pages of a background
payload are paged to disk: models over about 1 GiB ran 10 to 65 times slower,
and an idle process was found holding 6 MiB of a 600 MiB model. `mlock` of
1.4 GiB stayed resident and was fine. `mlock` of 2.7 GiB froze the console;
the page does not say that freeze ended when the process did. The program
then refuses a locked file over 1.6 GiB.

Mihawk's 1600 MiB is not that 8 GiB cap, and it is not a per-budget field.

`sceKernelGetResidentFmemCount` is summed over every process except the title
(`src/pw_memory_helper.h`). The sum double-counts a shared mapping (K14).
With nothing of Wine running it is about 245 MiB on the Pro and 731 to
1223 MiB at the start of a launch on the base PS5 (87 processes). The guard
in `src/pw_memory_guard.h` is a line on that sum:

| Installed firmware | Guard | Why that number |
| --- | --- | --- |
| Below 12.00 (the Pro, 10.01) | 870 / 940 MiB | SceShellCore's VM Stats denominator is about 989 to 1025.8 MiB, and a launcher that sat at 865 to 970 MiB stopped answering (2026-10-07). A climb through 919 MiB froze the console (2026-10-11). |
| 12.00 and later | 1600 / 1670 MiB | On 13.40 the console's own resident total reached 1447 MiB over a gate, 1453 on a second 13.x console, with swap-out at 13.4 to 19.5 MiB. Marks tied to the first census ended every launch. |

The 12.20 session log is the case where the raw sum climbed from 944 to
1601 MiB in about a minute, flexible free stayed at 946 to 948 MiB, and the
application pool still had about 10.5 GiB of 11.7 GiB. The climb (657 MiB)
matched direct memory the session committed (653 MiB). The title waited in
`vmwait`. Flexible free did not move. The 1600 MiB line is where that sum,
and the guard that watches it, meet the pager. It is a global resident
figure. Budget 0's resource-3 limit of 8 GiB, on 13.60, is an accounting cap
on a different counter.

PS5LM's `sysmemprobe`, a payload, with the app open and no model loaded, read
2.48 GiB resident across 89 processes (SceShellUI 0.81 GiB of that). The home
screen was up. A resident sum above 1600 MiB can exist when the pages are not
one process's locked working set. One payload holding 6 GiB, or locking
2.7 GiB, is the freeze. Mihawk's executable copies are resident flexible
pages (K7). Thirteen helpers holding 15 to 57 MiB of code each, about 725 MiB,
sit on top of a base-PS5 idle sum that is already 731 to 1223 MiB. That is
how a session reaches 1600 MiB with the flexible-free counter still near a
gigabyte and the application pool still near 10 GiB.

### Could a helper's executable allowance grow?

Helpers already stay on budget 0. No budget patch is applied. Their ordinary
direct allocation fails `0x80020023`; `sceKernelAllocateDirectMemoryForApp`
is how they reach the shared 11,712 MiB pool. Their JIT is charged to the
resident flexible sum, which is the pool whose accounting cap reads 8 GiB on
13.60 and whose physical use froze PS5LM's home screen at about 6 GiB held.

Putting a helper onto the title's budget would move its JIT into the 1 GiB
title pool, which is smaller. `ps5/sce_sys/param.json` already asks for
1,073,741,824, and the kernel refuses 2 GiB. PS5LM reproduced that refusal
on 13.60: 1,543,503,872 (1.4375 GiB) did not launch, the klog named
`kernel.flexibleMemorySize` and the range 2,097,152 to 1,073,741,824, and the
failure was an error dialog. At 1 GiB the app had 1.00 GiB flexible and
11.44 GiB direct. The 576 MiB came out of direct memory. A title's total is
a fixed 12.44 GiB that `param.json` divides. PS5LM's page says a larger total
is the kernel's `launch bigapp` path, not a key in the file. That path was
not traced, and it is not a candidate: there is no verified field, and a
launch-budget edit is the allocator this note already refuses.

websrv's `hbldr.c` used to call `sys_budget_set(0)` inside a bigapp. The call
is compiled out. The comment says it crashes syscore when the bigapp
terminates (commit `f6be30f`). Moving the title onto budget 0 is a known
syscore crash on the way out, and a payload on that budget has no direct
memory. It does not enlarge executable flexible memory.

etaHEN's request does not add memory. kstuff does not patch a budget. An
authid or ucred store changes rights. Lapy already does a checked credential
copy and does not touch a mapping. A credential is not an execute bit.

### Page-table fields in `param.json`

ProsperoAI's `param.json` adds an `amm` block and kernel page-table sizes.
PS5LM issue 4 names `amm.vaRangeInGib` 512, `amm.multimapVaRangeInGib` 512
and `amm.pagetableMemorySizeInMib` 256, plus the kernel page-table sizes.
SharpProspero's field list gives the byte keys `kernel.cpuPageTableSize` and
`kernel.gpuPageTableSize`. abhi245y's CONSOLE.md measured the effect on the
same Slim: with those blocks the direct pool and RADV's heap were 11.44 GiB;
without them `sceKernelGetDirectMemorySize` reported 12.00 GiB and the app
had 11.58 GiB free before a model, against 11.01 GiB with the blocks and the
dashboard up. The PS5LM-Plus changelog counts 768 MiB (256 MiB for `amm`,
256 MiB CPU, 256 MiB GPU) taken out of the title's own memory.

`radv_enable_unified_heap_on_apu` is a RADV reporting switch. With it the
device heap is the 11.44 GiB direct pool; without it RADV reports 8 GiB.
That 8 GiB is a userspace heap cap. It is not budget 0's flexible cap, and
it does not make a CPU mapping executable. This tree already places
host-visible GPU memory in the application pool (prot `0x33`, 2026-10-12).

Mihawk's `param.json` sets `kernel.flexibleMemorySize` to 1,073,741,824 and
has no `amm` block and no page-table keys. The measured direct pool at that
setting is 12,259,885,056 bytes, which is 11,712 MiB: 12 GiB minus the
576 MiB the 1 GiB flexible request takes from the default. Adding ProsperoAI's
page-table blocks would take a further hundreds of MiB from that pool. The
blocks reserve page-table pages. They do not add an executable flexible
allowance.

### Data-only kernel fields

Each of these can be read back. None is a verified write on 12.20.

| Field | What a read can show first | Failure if it is stored |
| --- | --- | --- |
| Budget 0 resource 3, the 8 GiB flexible limit, via `budget_get` | The limit and the used count, on this firmware, with no store. `budget_set` is a different call. | Raising the number leaves the physical pool the size the census and PS5LM's freeze measured. The title allocates until the pager. On a bigapp, `budget_set(0)` is the call websrv removed because syscore crashes on the way out. |
| `ucred` authid, caps, attrs | Lapy reads them and requires exactly one matching credential slot. | Rights change. They do not set execute on a mapping. A wrong credential can give the process a filesystem identity. That is a storage write waiting for the next file call. |
| `vm_map_entry` byte at `0x64` | FreeBSD 11.4 lays `protection` at `0x64` and `max_protection` at `0x65` (`sys/vm/vm_map.h`, release 11.4.0): `start` at `0x20`, `end` at `0x28`, `left` at `0x10`, `right` at `0x18`, which matches the SDK walk. A read that matches `sceKernelQueryMemoryProtection` identifies the entry. The byte at `0x65` is only a candidate until a JIT page and a direct page are read on 12.20. | The SDK store does not hold the map lock. A splay rotation or a clip between the walk and the store writes another object. The 2026-10-14 hangs were followed by factory resets. Stopping user threads leaves kernel threads, the pager included. A one-byte store of the current protection does not update the page table; the CPU execute bit moves only when the kernel's own fault or `mprotect` path installs it. |
| `max_protection`, the next byte if the layout matches | Same read. On a JIT page the candidate should already include execute. On a direct page it should not. A mismatch of `0x64` with the query means the layout is wrong, and the probe stops. | Or-ing execute into the wrong byte can hit `eflags` (a vnode write-count flag lives there in FreeBSD) or `wired_count`. A vnode flag is a filesystem operation. That is the storage case. Even a correct byte, stored unlocked, has the wrong-entry failure above. Calling `vm_map_protect` so the kernel holds the lock needs a code address, and kernel text cannot be byte-checked. |
| A page-table execute bit, or a `vm_object` type | The read can show the bit. It cannot show that the pager will treat the page as anonymous. | The pager can write an application page as a file page. SSD corruption. Refused in the section above. |
| Splicing direct pages onto the flexible free list | There is no public layout for 12.20 that a read could check against a known free-list head. | The buffer cache uses that allocator. A bad splice hands a file page to the title or a title page to the filesystem. SSD corruption. |

`sceKernelMapDirectMemory` with protection 7 fails `0x80020016`, which is
`0x80020000 + EINVAL` (22), the same code as an out-of-range
`flexibleMemorySize`. The entry is never created with execute in its maximum.
A later store of the current protection byte does not replace that check.

### Routes that stay in userspace

These do not make the 11.7 GiB pool executable. They are the routes whose
failure stays in the process.

The 1 GiB flexible maximum is already requested. Helpers do not pay their JIT
from that pool alone. Identical JIT objects are charged in full in every
process that maps them (K14, and the 2026-10-20 RSS revert). A FEX-style or
box64-style cache still has to execute from a flexible JIT window. The bytes
can live in the application pool as data. The window that runs them cannot,
and each helper that maps the window pays for it again. A smaller window with
eviction lowers the resident peak. It does not give a shared multi-gigabyte
execute mapping. Patch 1127's cluster is already 16 KiB, down from 64 KiB,
because the 64 KiB cluster pushed the browser's census by 120 MiB or more in
8 s.

Fewer Chromium-style processes lowers the sum directly, because the charge is
per process. MihawkBrowser already asks for one renderer
(`renderer-process-limit=1`, `--disable-site-isolation-trials`) and jitless
V8, so that browser does not compile JavaScript. A game's own multi-process
runtime is the session that reached about 725 MiB of code copies across
13 helpers. Cutting those processes is a session policy. It is not a kernel
change, and it does not enlarge the pool. `--single-process` is not measured
here; CEF's own note in [MIHAWK_BROWSER.md](MIHAWK_BROWSER.md) is why the
browser does not use it.

Moving the accounting of a code cache onto the application pool is the direct
memory the session already uses for anonymous data. The execute copy remains
the flexible charge. There is no userspace counter that reclassifies a JIT
page as direct memory. `sceKernelGetResidentFmemCount` is the kernel's.

## Rank

A home-screen freeze that ends when the process ends is the failure PS5LM
recorded at about 6 GiB, and it is an acceptable worst case for this note.
A hang in the pager, an unwire timeout, or a write to the SSD is not. A
panic or a reboot stays acceptable. `vmwait` inside the title is not in the
acceptable class: the 12.20 session did not end itself.

| Rank | Route | Worst case that can be claimed | Verdict |
| --- | --- | --- | --- |
| 1 | Read-only probe below. No store. | A bad kernel read can panic. The probe allocates nothing, so it does not fill the pool. | The one probe. Not shipped in this change. |
| 2 | Fewer helper processes, and a smaller JIT window with eviction. Userspace. | The process crashes, or the session is slower. The flexible sum drops by the copies that go away. | The mitigation that stays inside the rule. It does not make the application pool executable. |
| 3 | Treat budget 0's 8 GiB as a flexible allowance and allocate toward it. | On 13.60, about 6 GiB held froze the home screen until the process ended. 2.7 GiB locked froze the console, recovery not stated. On 12.20 the observed failure at a 1600 MiB census was `vmwait` in the title. The 8 GiB figure is not a 12.20 measurement. | Do not allocate toward it. Helpers are already on this budget. |
| 4 | `param.json` `amm` and page-table sizes, or `radv_enable_unified_heap_on_apu`. | The page-table blocks take hundreds of MiB out of the direct pool. An out-of-range flexible size refuses the launch with a dialog. The RADV switch changes a reported heap size. | Do not add the blocks. The RADV switch is not a CPU execute path. |
| 5 | etaHEN jailbreak request, or a ucred / authid store. | Rights change. A wrong credential is a later filesystem write. No extra memory. | Already the rights path. Not a memory change. |
| 6 | Store `max_protection` or the byte at `0x64`, then call `sceKernelMprotect`. | Unlocked walk. Wrong entry, or the vnode flag beside the protection bytes. Pager hang or an SSD write. The factory resets after the earlier hangs are the record. | Refused. A read of the bytes is part of the probe. The store is not. |
| 7 | A second breakpoint at kstuff-lite's 12.20 `mprotect_fix_start`. | The site is the breakpoint class already measured. Kernel text cannot be checked. A wrong skip can leave a lock held. | Refused. The address is public and is not a new clip. |
| 8 | `budget_set`, a budget-limit store, or a free-list splice from direct memory. | Syscore crash on title exit for `budget_set(0)` on a bigapp. A free-list splice can write the wrong page to the SSD. | Refused. |
| 9 | GPU DMA, a PTE execute bit, or a pager-ops swap. | A wrong physical page or a vnode pager. SSD corruption. | Refused. |

## The one probe

Not in this change. No payload, no eboot, no `param.json`, no
`memory.conf`. If it is built later, it is a separate program, off unless an
environment flag names 12.20, and it stops at the first error. It allocates
nothing past one page of each kind, it maps those pages read-write, and it
unmaps them before it exits. It does not call `budget_set`. It does not store
to a vm_map, a budget record, a page table, a free list, or a credential.

1. Read `kern.sdk_version`. Print it. If the two high bytes are not the BCD
   for 12.20, exit. `sceKernelGetSystemSwVersion` is spoofed and is not the
   word (`src/pw_firmware.h`). 13.40 in this tree is `0x13400002` and 10.01 is
   `0x10010000`, so the low half may be non-zero on 12.20. The high half is
   the check.
2. Read `sceKernelConfiguredFlexibleMemorySize` and
   `sceKernelAvailableFlexibleMemorySize`, the direct-memory size and the
   available direct size, and one resident census. Print them. If a call
   fails, exit. Do not allocate a ladder.
3. `budget_getid`. If it is not 0, print it and exit. `budget_get` of that id
   only. Print each resource's limit and used count. If the call fails, exit.
   Do not call it for any other id.
4. Map one direct page with protection 3 and one JIT page, then
   `sceKernelMprotect` the JIT page to 5. Both calls are the ones
   `exec-kernel-2026-10-14` already ran. Query both. Through the existing
   kernel read pipe, read 32 bytes at the vm_map entry the SDK walk returns,
   twice. The second read has to show the same `start` and `end`. The byte at
   `0x64` has to match the queried protection. Print the byte at `0x65` for
   both pages. If `0x64` does not match, or the two reads differ, exit. Do
   not store.
5. Read the bytes of `sceKernelMprotect` in the loaded libkernel and record
   whether the wrapper clears the execute bit before the syscall. User memory
   is readable. Do not patch it. If the wrapper already passes the bit
   through, the kernel clip is the one the 2026-10-14 run measured, and the
   probe does not issue a new protection.
6. Unmap both pages. Print the firmware word again. Exit. If the kernel log
   shows a pager error, an unwire timeout, or a storage line, stop. Do not
   start a game session on that boot.

A store of `0x65`, a raw syscall with execute on the direct page, and a
breakpoint at `-0x9D4F23` are not steps of this probe. Each needs the record
from this one first, and each is still refused by the rank above until a
failure mode better than a pager hang is shown. This note does not claim the
read will find a usable byte.

Until that probe has a record, this tree has nothing to deploy for it.

## What to deploy

Nothing. Do not replace `eboot.bin`, the runtime, or `param.json`. Do not add
`/data/ps5proton/memory.conf` for this. The title on the console today is
unchanged by this note.

There is no flag to turn on. A flag that enabled a write is what this note
refuses to add.

## Safe test

Do not load a new payload from this change. There isn't one.

Confirm the console still starts the title it already has, then close it.
That is the whole test. A kernel log with no new storage line is the expected
result, because nothing new ran.
