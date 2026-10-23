# Windows processes

Current production guide, reviewed 2026-10-07. The chronological investigation,
including failed runs and obsolete opt-ins, is retained in
[the process history](history/2026-10-07/docs/WINDOWS_PROCESSES.md).

## Bootstrap and ownership

Every executable defaults to the owner/broker layout. The PS5 title hosts the
shared wineserver, graphics/display, input and audio. Windows programs are
separate native payload processes created through resident websrv/elfldr.
Descendant requests return to the owning title; Wine preserves their actual
Windows parent relationships, startup parameters and inherited handles.

A rendezvous transfers production's packet control connection, distinct from
the wineserver connection and GPU/input/audio transports. The loader acknowledges
the endpoint before the owner closes its copy, and the owner keeps the child's
GPU-broker socket until READY: a unix socket whose every reference is in flight
can arrive flushed on the console (readable, first read 0 bytes). The loader and
runtime must agree on the bootstrap contract. Control EOF ends a payload. The owner checks
native-PID absence and GPU completion before reclaiming resources.

The 32-entry tables are implementation capacity, not a 32-child console claim.
Eight simultaneous Windows children and four rendering Windows processes were
verified in separate tests. Both counts exclude any claim of universal game-scale
concurrency. Sony-managed spawning belongs to earlier experiments; its production
spawning adapter was removed. The one-graphics-client arrangement remains an older
diagnostic mode, not the current default.

## Process behavior already checked

- Arguments with spaces, inherited environment, working directory and pipes.
- Inheritable/non-inheritable handles, completion events and full exit codes.
- Suspended creation/resume, termination and repeated resource reuse.
- Descendant chains and spawning after an immediate Windows parent exits.
- Cleanup when the owning application ends.
- Java ProcessBuilder chains in small interpreted, non-graphical JVM tests.

Representative evidence: [eight children](../evidence/payload-eight-02.json),
[descendants](../evidence/payload-descendants-02.json),
[owner replacement](../evidence/payload-owner-close-02.json),
[graphics](../evidence/payload-graphics-10.json) and
[Java](../evidence/java-process-console-03.json). Earlier Java and process results
must be repeated where the current loader/memory behavior matters.

## Graphics, desktop, input and audio

The GPU broker lets children and descendants render through the owner's GPU
context and display. Four DXVK rendering processes passed correct pixels,
controller input and native cleanup. Ordinary desktop windows use the compositor;
touchpad motion and clicks are routed through the shared Wine desktop.

OpenGL defaults to **Mesa WGL/Zink → Vulkan/RADV → GPU broker**. Audio defaults
to per-helper rings feeding the owner's mixer. The controller is available through
shared state/HID reports with process-specific Windows device identity.
A .NET (MonoGame) game's human input and manual-launch music were confirmed on the console.

## Manual launch defaults, 2026-10-06

Earlier automated tests enabled Zink/audio explicitly, leaving ordinary launches
without them. Production now enables both for the owner/broker layout, independent
of game name. The loading display hands over only when the game presenter opens
VideoOut, rather than when broker initialization starts.

[manual-default-audio-2026-10-06-02](../evidence/manual-default-audio-2026-10-06-02.json)
passed without graphics/audio overrides: correct title screen of a .NET (MonoGame) game, loading
handoff after 16.2 s, audible music, closure and native cleanup. A subsequent human
launch through the menu confirmed loading, menu and music together. Fresh-prefix
first-launch Mesa staging remains open.

## Networking, setup and Windows services

Helpers ask the title to resolve website names; the title owns the console resolver
library. HTTP and strict HTTPS passed, including trust checks with the console's
clock set correctly. GnuTLS and pinned certificate roots back Windows TLS.
See the 2026-10-05 battery run (record removed from the tree; in git history) and the latest gate's helper TLS case.

Earlier fresh-prefix tests created the Windows environment on-console, started
services, installed a 64-bit MSI and ran the installed program. Mono/Gecko are
pinned runtime components. Services run during required prefix setup or when
explicitly requested, rather than being enabled for every game. These setup
results predate today's full payload/Zink defaults; repeat them before claiming
current first-launch acceptance. See [BOOT_STATE.md](BOOT_STATE.md).

## Application memory in payload helpers, 2026-10-07

`sceKernelAllocateDirectMemoryForApp` explicitly selects the application pool.
The payload passes it to Wine's existing direct-memory mapper through
`__wine_ps5_set_payload_direct_memory`. Helpers retain budget 0, with no budget
reassignment or kernel data patch. Only the payload imports `libkernel_sys`.

The console reports **11,712 MiB (11.4375 GiB)**, shared by the title, graphics and
all helpers. Windows commits and Wine's Unix heap use this backing by default.
Reservations remain virtual until committed; decommit/release returns backing.
Native modules, bootstrap and some shared transport still use system memory.

| Test | Result |
| --- | --- |
| Ordinary payload allocator, 2 GiB | Refused allocation with `0x80020023`; retained negative control |
| Application allocator, 2 GiB | Allocate/map/touch/verify/release passed; system residency 3,504 KiB |
| Two Windows helpers | Each held 2 GiB simultaneously; two verify/decommit/zeroed-recommit/free cycles passed; each direct peak 2,075 MiB and system residency 14 MiB |
| Executable-memory probe | 32 executions, 480 adjacent writes/queries, 32 sparse-zero cycles, preserved neighbors; 16 GiB virtual reservation with small committed pieces |
| Full shell closure | Graphics helper held 2 GiB; 3/3 recorded helper PIDs absent after closure; no CPU/GPU fault |

See [capacity](../evidence/gate-payload-forapp-2026-10-07-02-payload-memory.json),
[VM behavior](../evidence/payload-forapp-vm-2026-10-07-02.json) and
[shell-close validation](../evidence/payload-forapp-shell-close-2026-10-07-01-validation.json).
The shell deliberately interrupted that run; the program-run file has no ordinary
completion verdict. The separate validation file proves the cleanup outcome.

The first VM attempt failed before Windows startup because the callback was not
in the PRX export descriptor. The export was added. The first nine-case gate then
failed its guard case because `WINE_PS5_PAYLOAD_DMEM=0` was not forwarded to helpers.
Inheritance was corrected and the full gate repeated. Neither failed run authorized
a code commit. Both remain in `evidence/` and the history.

## Latest application results

| Run (2026-10-07) | What it proves |
| --- | --- |
| A game launcher's client | Normal command line, 120-second budget, status 0 on close and return to launcher; system peak 721 MiB; application-pool peak 2,714 MiB including owner/GPU |
| A .NET (MonoGame) game | Default Zink path, loading handoff 15.8 s, captured world, normal requested closure/return; helper direct peak 1,024 MiB, system peak 246 MiB |

The launcher's swap-out counter did not show the earlier sustained growth. One browser
helper still recorded a nonfatal `wbemprox.dll` text-protection warning and a mapper
operation failure; it did not report a physical allocation refusal. The person
reports unusual UI on both console and host Wine. Login, library use, game launch
and visual correctness remain unproved; do not assign the UI's cause from this
observation alone.

Evidence: the four 2026-10-07 run and memory records (removed from the tree; in git history).

## Diagnostics and remaining limits

`PROTON_LAYOUT=title` and `single`, `PROTON_ZINK=0`,
`WINE_PS5_HELPER_AUDIO=0` and `WINE_PS5_PAYLOAD_DMEM=0` are explicit diagnostics,
not recommended game defaults. The old `WINE_PS5_PAYLOAD_CHILDREN=0` Sony-spawn
opt-out is no longer implemented by the production process adapter. Historical
Sony probes/build tools remain separate from the shipped payload route.

The system-memory guard stays enabled because application backing does not remove
all system-pool consumption. JIT copies, native modules and some shared transport
still use flexible memory. An anonymous section above one host page takes direct
memory, except the wineserver's session mapping and user shared data, which stay
on the file the server writes. A shared mapping of any other file stays flexible. The guard's input is the resident census minus
direct-memory growth since the session's start (`src/pw_memory_policy.h`), so a
helper that commits the application pool is not, by that growth alone, treated as
exhausting the flexible pool. Flexible free at or below 96 MiB, or a raw resident
census at or above 6144 MiB (or the session's limit, when that is higher), still
ends a program. `PROTON_SYSTEM_MEMORY_RAW=1` feeds the raw census. Closing under
synthetic pressure passed in the latest gate, on the older input. Do not equate a
bounded clean close with crash recovery or unlimited stability.

A session sets `PROTON_SYSTEM_MEMORY_LIMIT` and `PROTON_SYSTEM_MEMORY_END` in
`/data/ps5proton/memory.conf` (one `KEY=value` per line, `#` comments) or in the
title's environment. The environment wins. `0` turns the guard off, including the
flexible-pool floor and the raw-census backstop. A value above the firmware
default is kept. A program launched from the desktop does not change the title's
guard by its own `game.json`.

The guarded figure has its own governor (`src/pw_system_pool.h`), for a climb of flexible
memory the subtraction above does not explain. The title reads that figure about every
200 ms. At or below the firmware limit nothing is paused: an idle desktop near 1460 MiB,
and a launcher whose raw census is past 1600 while the guarded figure is not, keep
running. Firmware 12's warn line is 1600 MiB and its kill line is the hard end, 1670 MiB.
A rise of 32 MiB while already above the warn line and within 32 MiB of that end, or the
end itself, SIGSTOPs the helper that grew. Still at the end 250 ms later, that helper is
SIGKILLed. The title, the memory helper, and the desktop root are never signalled. The
helper is the newest when the per-process flexible charges match, and the largest when
one charge is at least 64 MiB above the newest, at least twice it, and at least 96 MiB.
It stands down 64 MiB below the warning and continues a paused helper. The flight
recorder warns at 2 s and ends the Windows helpers at 5 s when the picture has already
been shown once and then shows nothing new, and either the title waits in `vmwait` or
the raw census is already at the firmware limit (1600 MiB on firmware 12; the watchdog
keeps that mark).
The title posts a notice. Before that SIGKILL the registry is flushed and the prefix
directory is fsynced, and the prefix's files are synced in the background every 30 s.
The same `memory.conf` keys (`PROTON_SYSTEM_POOL`, `PROTON_SYSTEM_POOL_WARN`,
`PROTON_SYSTEM_POOL_KILL`, `PROTON_SYSTEM_POOL_HOLD`, `PROTON_SYSTEM_POOL_RELEASE`,
`PROTON_SYSTEM_POOL_SPIKE`, `PROTON_HANG_DISPLAY_MS`, `PROTON_HANG_DISPLAY_WARN_MS`)
override the defaults; the title environment wins. `PROTON_SYSTEM_POOL=0` and
`PROTON_HANG_DISPLAY_MS=0` turn each one off. No console run has exercised this governor.

Open targets: complete operation of a game launcher; current-build Java-launcher chain;
fresh-prefix Zink staging; broader concurrent graphics/audio workloads; refresh of
the broad Wine/DirectX baselines. General 32-bit compatibility and rumble remain outside the
verified feature set. Follow [PLAN.md](PLAN.md) and
[NEXT_CONSOLE_SESSION.md](NEXT_CONSOLE_SESSION.md).
