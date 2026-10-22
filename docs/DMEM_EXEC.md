# Execute on the application pool

2026-10-22. No console run. No kernel byte was written. No new eboot, payload, or `param.json`.

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

## The next change that stays inside the rule

There is no smaller userspace place to move the copies. Data is already on the
application pool. Identical JIT objects are already known not to help, because
each mapping is charged in full. The flexible cap is already 1 GiB.

What is left is a probe that does not write, run on the owner's console, on
the installed firmware and no other:

1. Read `kern.sdk_version`. If it is not the word the probe was built for,
   exit. Print the word. Change nothing.
2. Trace the direct-memory clip the same way kstuff's existing execute
   breakpoint was found: one `mprotect` of a direct page without execute, one
   with execute, the first instruction that differs. Record the firmware word
   beside that site. Do not record a firmware image.
3. Only then consider a breakpoint that skips that clip, cleared on the way
   out, and only while the current process is this title. Default off. An
   environment flag has to ask for it. A mismatch of the firmware word clears
   nothing because nothing was set, and exits.
4. The first run maps one page, executes one instruction, puts the protection
   back, and stops. If the kernel log shows a pager error, an unwire timeout,
   or a storage line, stop. Do not start a game session on that boot.

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
