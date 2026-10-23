# PS5 Wine patch series

Reviewed 2026-10-07. The ordered series adapts GE-Proton's pinned Wine to PS5:
shared wineserver, user driver, Vulkan/display, memory/signals, files, audio,
controller interfaces, payload children/descendants and their lifecycle.

Production uses the payload owner/broker layout, default Mesa/Zink and helper
audio. Old no-process/headless-only descriptions refer to earlier patch stages.
Patch 0936's sparse recommit behavior now has focused console VM evidence;
qualification is no longer host-only. See
[current processes/memory](../../docs/WINDOWS_PROCESSES.md).

## Provenance and scope

The series derives from prospero-win's LGPL-2.1-or-later PS5 work, rebased onto
Wine `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`, GE-Proton11-7's pin.
Retain authorship, dates, licence headers and rebase notes. Source provenance
and dependency obligations are in [NOTICE.md](../../NOTICE.md) and
[LICENSING.md](../../LICENSING.md).

The IA-32 translator patches 0601, 0610 and 0710 were not imported. The work promoted from `win-32bit` to local `main`
runs PE32 natively under WoW64; its measured qualification is in
[IA32_DESIGN.md](../../docs/IA32_DESIGN.md). Deferred OpenGL/diagnostic patches are references, not additional
active patches to apply blindly.

Patches 2001-2062 are GE-Proton's ffmpeg + winedmo video rework, imported from the fork's `patches/ge-video-rework` (HEAD `e24b389e`, built on the same Wine pin) and applied after the PS5 series, as GE applies them after its own: winegstreamer is gone and winedmo demuxes, decodes, converts and muxes on FFmpeg for Media Foundation, DirectShow and the WM reader. 62 of the 66 apply; the RSX3D audio shim (one game's, a new DLL) and the three winepulse patches (no PulseAudio here) are left out. Four needed a rebase, each noted in its header: 2001 deletes a winegstreamer file Valve's tree still changes, 2004 keeps 0700's lock-free wait, 2006's wine.inf context differs, and the hunks no console build reads (MAINTAINERS, the macOS preloader, the GitLab image, winepulse) are dropped. Patches 2063 and 2064 are this project's fixes on top, both found with GE-Proton11-7's own Linux build: the source reader reports a decoder's new frame size (1920x1088 with a 1080 aperture) instead of keeping the old one, and MFShutdown waits for the work queues' callbacks without the lock a callback ending a source reader needs (it deadlocked in 5 of 8 runs). Patch 2065 counts, on the PS5 only, what winedmo's demuxers open, seek and read from their byte streams, for the helper's 10 s telemetry (`wine-child: media`). winedmo's Unix side is built by tools/build-wine-ps5.sh over the pinned FFmpeg ([NOTICE.md](../../NOTICE.md#media-winedmo-and-ffmpeg)).

Patch 0955 fixes the nested `INPUT32` in WoW64's internal hardware-input call, using
the same widening helper as public `SendInput`. HID reports otherwise become message
zero and stall ordinary message pumps. Paired raw-input and rendering probes qualify
the fix; see [CONFORMANCE.md](../../docs/CONFORMANCE.md).

Patch 0956 prepares the shared runtime-owned prefix bundle after wineboot and
before application imports. Fonts, ICU, Mesa, DirectX DLLs and i386 HID drivers
use one versioned installer; see [SHARED_PREFIX.md](../../docs/SHARED_PREFIX.md).

Patch 0957 preserves prior native signal handlers in SDK threads without a
Wine TEB, while Wine threads keep WoW64's raw, host-FS-first signal entry.
Patch 0958 retains each GDI window's pixels and composes in server Z order, so
background repaints cannot overwrite dialogs and closure reveals the background. Qualification is
tracked in [PLAN.md](../../docs/PLAN.md), including failures.

Patches 0992–0994 carry DWM surface state: dwmapi attributes, frame margins and
thumbnails reach owner-checked server state exported in snapshot v4 (0992);
uxtheme buffered-paint alpha and composited theme text implement painting over
glass (0993); the Mihawk taskbar becomes Mica with live window previews (0994).
See [Mihawk OS](../../docs/MIHAWK_OS.md#dwm-surfaces-rounded-corners-materials-and-previews-2026-10-10).

Patch 0995 makes the hidden shell idle (game mode, step 2): the Mihawk taskbar
follows window events instead of rescanning every window each second and
repaints only buttons that changed, its clock wakes once a minute, and Explorer
starts tabtip.exe only where it has work (its `SteamDeck=1` variable).

Patch 0996 lets Explorer be the title's desktop session root (game mode, step
3): with `MIHAWK_SESSION=1` it enables its shell on Default, initializes the
session wallpaper (formerly the Windows `mihawk.exe`), and makes its desktop a
Windows-style Progman window while the system keeps the desktop window, so the
title can end and restart Explorer; the Mihawk taskbar follows window creation
and destruction through window events, adopts windows already open, and
broadcasts `TaskbarCreated` so programs add their notification icons again
after a restart. Patch 0997 keeps that shell window the bottom-most window
whatever a `SetWindowPos` asks (`HWND_BOTTOM` lands just above it), marks it as
the snapshot's desktop background (publishing only when one is set, not on
`GetShellWindow`), and sends it the `WM_PARENTNOTIFY(WM_DESTROY)` the thread-less
desktop window would drop, so the taskbar forgets windows of a process that
ended without destroying them.

Patch 0998 gives a process another one started its catalog settings: every game
lives in the `mihawk` prefix and starts from its desktop shortcut as Explorer's
child. The PS5 spawn path passes the new process's executable and command line
to the title, which finds its `game.json` entry (`src/pw_app_environment.h`); the
loader sets the settings on the Unix side and `init_startup_info` on the Windows
side, through 0974's overlay, now one function for both.

Patch 0959 consults the live native address map when a Windows query finds no
Wine view. Its host-tested helper subtracts Wine-owned reservations, so native
carrier mappings are unavailable without hiding space Wine can allocate.
Production builds and the 127-patch replay pass; console qualification is pending.

## Applying and changing the series

The builders apply `*.patch` files in filename order. Never hand-edit a vendor
checkout as the final change. `tools/check-patches.sh` replays the entire series
in a temporary tree; `--write` regenerates [PATCH_REBASE.md](../../docs/PATCH_REBASE.md).
The active filenames and freshly generated replay report are authoritative;
do not rely on old hand-maintained patch counts.

`tools/rebase-next.sh` is a controlled rebase helper for the pinned work tree.
Resolve rejected hunks by intent, preserve attribution and inspect every changed
call/ABI. A clean apply or hunk count is not proof of correct runtime behavior.

After Wine changes, rebuild the Windows modules selected by the patched trees
and the matching native PRXs. Production's patched-module list is derived by
`tools/wine-patched-modules.py`; missing PE modules previously hid input fixes.
Use the production builder/deploy/gate sequence before a code commit.

## Checks

`tools/test-rebase.py` exercises rebase helpers in throwaway repositories.
`tools/check-wine-host.sh` validates an exported build with pinned local registry
inputs. Focused portable tests and the exact-source console gate qualify behavior.
The older 54/95-patch status and detailed rebase examples remain in
[the patch history](../../docs/history/2026-10-07/wine/patches/README.md).

The `mihawk-os` theme slice adds 0972 for ordinary themed caption buttons,
retaining classic fallback and existing hit geometry, and 0973 for shared
appearance receivers. The latter refreshes process-local theme metadata and
system-colour GDI caches before application notifications without republishing
registry preferences or metrics; retained HTHEME ownership is preserved.
Paired receiver probes reproduce the old cache failure and qualify the new
behavior. The complete pinned replay currently passes **141/141 patches**;
console code-commit qualification is tracked in the current PLAN gate record.

### 1172: a thread suspended in a system call's return reports the call's context (2026-10-16)

`__wine_syscall_dispatcher`'s return moves `%rsp` into the frame before it restores the program's registers, and from
there `is_inside_syscall` says no, so a suspend landing in that window reported the Unix registers: `%rip` in ntdll.so,
`%rsp` the frame + 0x70 in the kernel stack (upstream has the same window). CEF 150's stack sampler copied the main
thread's stack from that `%rsp` to `StackBase`; on the PS5 the kernel stack lies right below the Windows stack, so the
copy ran into the no-access page at the Windows stack's bottom (2026-10-16; record removed from the tree; in git history). 1172 starts such a
return again from the frame, so the thread suspends as one inside the call does; the return value, `%r15` and the
instrumentation tail's return address, which are outside the frame there, are found by four labels. Host, the pinned
Wine with only `signal_x86_64.c` rebuilt, `tests/stack-probe.exe` (gate case `suspend-context64`): 4405 of 144952
suspended contexts outside the stack without it, 0 of 151117 and 0 of 147189 with it, no register or return value lost.
Console (gate-2026-10-18-item3-01, 86 of 86, build 4cd26debd444632b): 46429 samples in 5 s, 0 outside the stack, 0 lost.

### 1173: a PS5 window surface flush takes no user lock (2026-10-18)

win32u flushes a surface under the surface's lock, from any thread that pumps messages, and `update_surface_region`
takes the user lock and then that lock. The driver's flush asked `get_window_long` (the user lock) for WS_EX_LAYERED
before publishing an input shape (0990): MihawkBrowser's UI thread and Chromium's occlusion-tracker thread deadlocked
in the desktop session (2026-10-18, record removed from the tree; in git history: both last requests 96,746 ms old, `get_window_region` and
`get_message`, neither in a server nor a sync-device wait; a blank or frozen page, a close that never arrived). The
driver keeps the style in its own window record, from WindowPosChanged and SetWindowStyle.

### 1137-1139: the Windows 11 prototype's chrome, pointers and monospace face (2026-10-16)

1137 makes user32 load each system cursor from the file the user's scheme names in `HKCU\Control Panel\Cursors`
(one shared cursor per process; an animated one survives `DestroyCursor`). 1138 lets the session root name the
Mihawk pointer scheme and Cascadia Mono (console face, Consolas substitute) once per prefix, before it creates a
window. 1139 is Windows 11's window frame: a framed top-level window's visible rect excludes the invisible part of
its resize border (the server's hit test keeps the window rect, so the frame still sizes it), the compositor draws
the default #66757575 border, the title bar is SM_CYCAPTION with no line, caption buttons are SM_CXSIZE x
SM_CYCAPTION from the inside rect's top right with hot tracking (`down = 2` through the theme hook), and
`DWMWA_EXTENDED_FRAME_BOUNDS` answers the visible bounds. Host oracle: `tests/theme-parts-probe.c`; console:
`gate-2026-10-16-win11-01` and `-02` (on the f3aa632 base).

### 1102, 1105, 1183: the desktop's icons as the prototype draws them (2026-10-16, 2026-10-17)

The Mihawk desktop is shell32's folder view in Progman (1100). 1102 gives that view the prototype's grid at its three icon sizes
(`src/pw_desktop_icons.h`: 32, 48 or 96 px icons in 75 x 75, 75 x 91 or 123 x 139 cells, with the desktop look, set before
any item is placed or painted; the list no longer auto-arranges), shows This PC and the Recycle Bin first, then the user's
items, keeps each icon's cell by name (`IconPositions` version 3, in 1/256 of a cell, so a size change keeps it) and moves
dragged icons to the nearest free cell. The icon size, Auto arrange and Align to grid are the desktop's view settings, kept
where Windows keeps them (`Shell\Bags\1\Desktop`: IconSize, FFlags) and changed through `IFolderView2`
(SetViewModeAndIconSize, SetCurrentFolderFlags) or Ctrl+Shift+1-4. Before, Wine's list arranged every icon in list order on
its first paint and on the redraw after filling (seen in a host trace), so a saved place did not hold (1105 skips that
arrange for the desktop). 1105 draws an icon view with `LVS_EX_TRANSPARENTSHADOWTEXT` as the prototype's desktop, sized by
the list's icon size: the item's plate for hover (the list now tracks the item under the pointer), selected, selected under
the pointer and selected without focus, the rubber band, and labels laid out as Blink lays them out and drawn from the
prototype's own glyphs in three layers (two shadows, then the text). Plates, band and glyphs are measured
(`themes/windows11/desktop`, through `tools/build-desktop-sprites.py`); a name with an unmeasured character keeps GDI's text.
1183 adds a 96 px list to shell32's icon cache for the large icons (Wine's lists stop at 48, then 256; ILD_SCALE is
unimplemented). Host test: `tests/test_pw_desktop_icons.c` (at each size: the text equal to Chromium's in every solid pixel,
the shadows within its spread, the grid equal to the prototype's boxes).

1184 starts a program opened from a folder in that folder, as Windows does. A Direct3D 12 game opened from a File Explorer
window started in Explorer's folder, so vkd3d-proton found no `vkd3d-proton.cache` beside the game ("Failed to map
read-only cache") and the game sat on a black window building every pipeline again; started in its folder it reached
its title screen. The shell view gave ShellExecuteEx the folder only when the folder, asked the attributes of its own
absolute ID list, said it was in the file system, and the item menu's verbs gave none; both now give the folder's path
whenever it has one. Checked by `tests/shell-open-probe.c` (gate case shell-open64), which opens itself through an
ExplorerBrowser's view both ways, the view's own open (Enter, a double click) and the item menu's default Open (its
right-click menu), and compares each opened copy's working folder with its own. Without 1184 the menu's copy starts in
the Windows folder, on upstream Wine 11.20 on Linux as well (`view=C:\shell-open menu=C:\windows`); the view's already
started in its folder, on the console too.

### 1214: the title's stop logs off the desktop session (2026-10-18)

1214 adds input type 8 to the title's server input (0931): the desktop session's log off, WM_CLOSE to the shell window
(Progman), as Start's Power does; with no shell window (Explorer unloaded in game mode) WM_CLOSE to the foreground window,
as type 4. The title's stop sent type 4, and in a desktop session with no program left the foreground window was one of
Explorer's own: Explorer took the close and went on, and the title ended the session by SIGKILL 10 s later
(two runs of 2026-10-18; record removed from the tree; in git history). Checked by gate case `desktop-stop`.

### 1210-1213: tooltips and the desktop's infotips as the prototype draws them (2026-10-18)

1210 draws every program's plain tooltip (no balloon or title, the system's font, colours and margins, characters the
prototype's glyphs cover) as the prototype's `.tooltip` (`src/pw_tooltip.h`): laid out as Blink lays it out (pre-line
white space, at most 320 px wide), its text from the prototype's own glyphs (`themes/windows11/tooltip`, through
`tools/build-tooltip-glyphs.py`) over the flyout's acrylic with 4 px corners, placed as `ui.position` places it (20 px
below the pointer, 8 px inside the work area), shown after 500 ms or 100 ms within 400 ms of another tip hiding, and
kept hidden after a press until the pointer reaches another tool. Without the compositor it is drawn opaque. 1211 gives
the list view infotips (`LVS_EX_INFOTIP`: one whole-list tool, `LVN_GETINFOTIP` for the item under the pointer, popped
when that item changes). 1212 makes the desktop ask for them and answers with the prototype's texts (the Recycle Bin's
description; a file's type, size and date modified; a folder's date created) unless the item has an `IQueryInfo`. 1213
gives a 4 px flyout the tooltip's `--shadow-8` (`PW_SHADOW_TOOLTIP`). Host test: `tests/test_pw_tooltip.c` (the box
Chromium's to the pixel and the text within a level, light and dark; the placement).

### 1150-1151: File Explorer as the prototype draws it (2026-10-16)

1150 hands the Mihawk File Explorer window to `wine/ps5/pw_shell_explorer.h`, which draws the prototype's Windows 11
Explorer: the client area takes the caption (WM_NCCALCSIZE keeps only the frame), a 48 px title bar of Mica Alt glass
(DWMSBT_TABBEDWINDOW, the compositor's `PW_BACKDROP_MICA_ALT` made the prototype's `.mica-alt`) with one tab and
46 x 48 caption buttons, the address row, the command bar, the navigation pane, Home (Quick access, Favorites, the
newest files of the user's folders) and the status bar, all laid out by `src/pw_explorer_layout.h` (equal to the
prototype's boxes) and, at 96 DPI, drawn with the prototype's own pixels (`themes/windows11/explorer`, through
`tools/build-explorer-sprites.py`). The shell view keeps the files: its list loses its client edge and, in details
view, takes the prototype's columns, column header and 32 px rows. Without a folder Explorer starts on Home; the
window's title is the location's name and " - File Explorer". Since then, all in `pw_shell_explorer.h`: the icon views
are the prototype's tiles (the list keeps the places `pw_explorer_tiles` gives), tabs are one browser each, Filter is
the browser's site answering `ICommDlgBrowser::IncludeObject` (Wine's ExplorerBrowser asks its site on every
navigation), and the details pane is the prototype's; the search box is drawn, not wired. 1151 makes shell32's
`IFolderView::ItemCount` count the selection for `SVGIO_SELECTION` (it counted every item, so Explorer's commands and
status bar took an unselected folder for a fully selected one).

### 1152: a navigation shows the new folder only once it is drawn as the prototype's (2026-10-16)

shell32's `CreateViewWindow` shows and updates the new view inside ExplorerBrowser's navigation, before the browser
reports it, so every folder to folder navigation first painted Wine's classic list. 1152 passes the browser's
navigation pending, view created and failed events to `pw_shell_explorer.h`, which holds the browser's window from
drawing (`WM_SETREDRAW`) from the start until the new view is styled; the old folder stays on screen meanwhile. Each new
view logs `pw-explorer: view created held= visible=`, and desktop-session requires every one invisible when created.

### 1200: the active window and every window's theme in the snapshot (2026-10-16)

A framed window's shadow is the prototype's `--shadow-window` while it is active and `--shadow-window-inactive`
otherwise, each in light and dark (`src/pw_window_effects.h`). 1200 marks the foreground thread's active window with
`PW_WINDOW_ACTIVE` and reports every top-level window's theme (`DWMWA_USE_IMMERSIVE_DARK_MODE`) as its material, not
only a glass window's; activating a window, bringing one to the foreground or attaching thread input republishes the
snapshot. `tests/window-effects-probe.c` checks that the foreground window's row, and only it, is marked.

### 1162: the Mihawk mark at every DPI (2026-10-18)

The Start button shows the project's own Mihawk mark (`themes/windows11/start.svg`, original artwork), not the Windows
logo, a Microsoft trademark, which it showed from 2026-10-16. One function draws it at every DPI
(`src/pw_taskbar_logo.h`; `tests/test_taskbar_logo.py`: within 1.5 levels on average of librsvg's drawing of the SVG at
22, 33 and 44 px), 22 px in the 40 px button at 96 DPI. There the prototype's hover and press plates go under it, as the
captured Start sprites it replaces did; at another DPI it replaces the PP mark's polygon.

### 1140-1141: the taskbar as the prototype draws it (2026-10-16)

1140 lays Explorer's taskbar out as the approved prototype does (`src/pw_taskbar_layout.h`, equal to its boxes) and,
at 96 DPI, draws the prototype's own pixels in each slot (`wine/ps5/pw_shell_taskbar.h`, sprites from
`themes/windows11/taskbar` through `tools/build-shell-sprites.py`): the search pill, Task View, the pinned File
Explorer, Settings, Notepad and Terminal (a pinned app's windows show on its pin; others follow), the hidden-icons
chevron, the status button and the two-line clock, with the prototype's plates and running pills (Start's mark is drawn: 1162). The clock's text is
the prototype's own glyphs placed on Skia's quarter-pixel pen grid (`src/pw_text_glyphs.h`, glyphs from
`themes/windows11/taskbar/clock-*.json` through `tools/build-shell-glyphs.py`), and its width lays the tray out; a
character without a measured glyph (another locale) keeps GDI's drawing. 1141 lets the
taskbar ask for a backdrop past Windows' own values (5), which the compositor draws as the prototype's taskbar
acrylic (`PW_BACKDROP_TASKBAR`, `src/pw_window_effects.h`) instead of Mica. Search opens Start; Task View, the hidden
icons and Quick Settings are drawn, not yet wired.

### 1160-1161: Start, Search and the taskbar's flyouts as the prototype draws them (2026-10-16)

1160 lets a window ask for two more backdrops past Windows' values: 6, the prototype's flyout acrylic-base (Start and
Search), and 7, its acrylic (Quick Settings, the notification centre and calendar, the hidden icons). The snapshot
reports them as `PW_BACKDROP_FLYOUT_BASE` and `PW_BACKDROP_FLYOUT` with `PW_SHADOW_FLYOUT` and the flyout stroke
(`src/pw_window_effects.h`): the taskbar's filter chain over a blur of only what lies under the flyout, the tint inside
the 1 px stroke, `--shadow-28`. 1161 wires the taskbar's search pill to Start's Search view, the status button to
Quick Settings, the clock to the notification centre and calendar, and the chevron to the hidden icons (the
notification area's own icon windows, which wait off the bar in the prototype's layout). The surfaces themselves are
`wine/ps5/pw_shell_start.c` and `wine/ps5/pw_shell_flyouts.c`, their fixed chrome the prototype's own pixels
(`themes/windows11/start`, `themes/windows11/flyouts`). Task View stays drawn, not wired.

### 1170: DXCore's adapters are GPUs (2026-10-16)

`IDXCoreAdapter::IsAttributeSupported` answered FALSE to everything, so Chromium's NPU scan (CEF 150 in
MihawkBrowser, `gpu/config/gpu_info_collector_win.cc`) took the console's GPU for an NPU and read its driver
description "PS5" into an eight-byte pointer: 0x00355350, and the browser's GPU thread faulted at startup
(2026-10-16; record removed from the tree; in git history). Every adapter dxcore lists is a wined3d GPU, so it now reports Direct3D 11 and 12
graphics and core compute.

### 1171: a kill waits for the fault handler to return (2026-10-16)

`quit_handler` ends a thread outside a syscall at once, and the trap and fault handlers run outside one, on the
signal stack beside the TEB. Both take the virtual mutex unmasked, and 1127's code conversion runs under it through
the dmem engine's lock, so a kill landing mid-conversion ended the thread with both held. With 16 KiB windows
winedevice.exe's exit stalled this way three times of three (`gate-2026-10-16-browser-01`: the lock's owner was the
first thread the exit killed, 004c, which printed "killed" and never released it). SIGQUIT joins those handlers'
mask, as `ps5_mmu4k_enter` already masks it for a run: the kill lands when the handler returns, in user mode with
nothing held or in a syscall where the thread is marked. The suspend handler stays killable.

### 0974: explicit initial application environment

Carries caller-selected catalog/launch settings separately from the owner's
runtime allowlist. Applies them after initial prefix/registry provisioning with
UTF-8 decoding and existing Wine case-insensitive environment replacement.
Removes all case variants before installing one selected value. A child's own
startup block remains authoritative except for the settings the title gives it
from the catalog (0998). The matching bootstrap ABI is in
`wine/ps5/pw_wine_process.h`; transport bounds are host-tested in `src/pw_environment`.
Paired console probes are required gate cases; qualification is recorded in the
current plan rather than inferred from the Linux oracle or a successful build.

`0975` publishes a bounded copy of Wine's authoritative input-desktop window
list, topmost first, after relevant requests and asynchronous window destruction.
Rows carry full HWND, lifetime generation, native/Windows process ownership,
owner HWND, visible geometry and independent visibility/unsupported-region flags.
Broker readers only copy the record under a separate mutex; they never walk
server objects. The private window-info response carries the matching lifetime
and native owner. The first slice qualifies this metadata with opposite-architecture
children, movement, hide/minimize/restore, destruction and explicit overflow;
it does not enable per-window pixel composition.


Patch 0976 publishes GDI surfaces by the server-authored HWND generation in the
opt-in independent-window route, including the actual local Explorer root and
Wine's padded surface offsets. Helper pages are ACKed and bounded; the broker
retains stable copies and uses patch 0975's window state. The legacy default,
GPU HWND association and shaped/layered content have separate proof limits in
[MIHAWK_OS.md](../../docs/MIHAWK_OS.md).

Patch 0977 publishes a coherent native cursor coordinate pair from Wine's
canonical server updates and input-desktop switches. SetCursorPos, SendInput
and title injection share the same position. Native display threads read copied
coordinates and never walk Wine objects. Both architectures now pass 19/19
mixed-window checks, including five actual pointer captures, in the complete
50/50 exact-source gate. Full cursor shape/visibility parity remains open.

Patch 0978 binds native Vulkan surfaces to full Wine HWND lifetimes and adds
canonical screen-space client clips plus independently rendered child HWNDs to
the version-2 snapshot. Client-surface registration remains Wine's; the broker
copies retained GPU pixels after its existing fence and matches the owner-bound
helper PID. The nested GPU probes and 52-case gate are not yet console-qualified.

Patch 0979 preserves ordinary parent GDI backing when the opt-in brokered HWND
route acquires a GPU pixel format. Captions and GDI children retain their normal
Wine surfaces and DC clips; GPU planes keep their canonical clips. It is pending
console qualification alongside 0978.

Patch 0980 keeps the primary-monitor work-area reservation in the existing
desktop shared object. PS5 SPI and monitor queries use its coherent raw rectangle
instead of a per-client cache, preserving normal DPI conversion and monitor
clipping. A successful SPI_SETWORKAREA sends the requested change notification
without requiring a registry write. The opposite-architecture diagnostic checks
new and already-running clients, monitor information, maximization and restoration.
Console qualification is pending; this does not yet implement the taskbar UI.


### 0988–0990: retained shapes, alpha and input coverage

0988 exports whole-window regions independently from GPU-client clips in the
version-3 snapshot. The compositor preserves empty regions and refuses invalid
or overflowing snapshots before touching the destination.

0989 carries Wine's existing surface alpha/colour-key coverage through version-2
retained pages. The page format is immutable; changing between opaque and
premultiplied pixels uses the existing acknowledged replacement path. Constant
opacity is applied once, including opacity-only `UpdateLayeredWindow` calls.
Top-level GDI layered windows are supported by this candidate; layered GPU or
child windows remain explicitly unsupported.

0990 publishes bounded coverage to the owning server HWND generation. Wine's
existing authoritative hit-test uses it without replacing application-visible
window regions. Ownership, dimensions and a 32 MiB total coverage budget are
checked before replacement; destruction releases coverage. This fixes the
observed zero-alpha click interception in both architectures. Focused console
results and the required full-battery status are in [PLAN.md](../../docs/PLAN.md);
these patches remain uncommitted until exact-source qualification completes.


0991 resets driver-owned opacity/input state when `WS_EX_LAYERED` changes,
before Wine's normal surface update. The expanded lifecycle probe exposed a
stale 50% factor after style removal; the new callback follows Wine's existing
attribute-reset contract. Focused requalification is pending in PLAN.md.

Patch 1127 runs a helper's code from JIT shared memory, the only memory a payload's kernel lets execute
(exec-kernel-2026-10-14-01 to -03). At an execute fault Wine allows in a private or image view, `virtual_handle_fault`
converts the faulting host page and its neighbours of the same protection, within one 64 KiB window (four host pages;
256 KiB before 2026-10-16: `evidence/cluster-ab-2026-10-16-ab.json`; 16 KiB waits for a gate), through
`__wine_ps5_exec_convert`; a fault the host already allows runs again under the virtual mutex, replacing 0954's
pre-lock wait; an execute fault nothing can serve (a section view) is counted. The hook is host-tested and
`vm-protect-probe` cases E1-E7 pass on the pinned desktop Wine; it has not run on the console.

Patch 1128 answers the hang of 2026-10-15 (late-library: "unwire timeout at 0xc80000000" while a helper with RADV up
unmapped a bare 28 GiB reservation; the freezes of the earlier emulator experiment (removed) had the same signature). `unmap_area` no longer unmaps a range the
direct-memory engine owns: it becomes a reservation and a reserved area again (the address pool still unmaps its
chunks). The engine (`wine/ps5/pw_wine_dmem.c`) never maps over, unmaps or reserves again over a kernel entry it did
not make: a commit over one is refused (EEXIST; `set_vprot` puts the page bytes back as they were, and a
`MEM_REPLACE_PLACEHOLDER` commit fails with the placeholder free again), a reservation in a kept reserved area is asked
about first, a release goes around it, a hole in a bare part is reserved again before it is trusted, and each one
becomes a system view at the next `map_view` or VirtualQuery (which reads it as occupied even before). The section
pager asks the same of every fault's range before its fixed mapping (`PwWineSectionOps.foreign`). What was wired at
0xc80000000 is not known; klog now lists the entries of every span of 1 GiB or more Wine unmaps, lays over or keeps.
Host-tested (`tests/test_pw_wine_dmem_ps5.c` models the crash; `tests/test_wine_virtual_guard.c` runs the patch's
virtual.c code); it has not run on the console.

Patches 1129 and 1130 close the paths an audit of every host memory call a helper makes found around the engine's
ownership (docs/PLAN.md, "The PS5 memory backend"). 1129: `NtLockVirtualMemory` wires nothing on PS5 (the engine lays,
converts and unmaps its runs without unlocking), the address pool's give-back is kept reserved like every other release
(it was a bare unmap of up to the size asked), MEM_RESET keeps the pages instead of an unmeasured madvise, and
the launcher client library's `.text` protection goes through `NtProtectVirtualMemory`. 1130: before a placed GPU mapping, win32u gives
the committed range back to a bare reservation the engine forgets (ntdll's `__wine_ps5_placed_range_bare`, under the
virtual mutex with its signals blocked), so the GPU client's wired mapping lies over a reservation and the engine treats
it as foreign; graphics patch 0013 makes the client map only over a bare reservation. The range is also lent
(`__wine_ps5_dmem_lend`) until Wine releases it, foreign by record, since the owner's shared memory over a reservation
is invisible to the kernel query; 0014 had placed direct memory only, which failed host-dmem-fallback64, and 0016 places
shared memory again. 1131: a helper (`WINE_PS5_CHILD`) never starts
win32u's AGC OpenGL stack, whose `eglInitialize` mapped wired GPU memory where the kernel chose; a helper's OpenGL is
Mesa/Zink. Host-tested by `tests/test_crash_replays.c` against `tests/fake_ps5_kernel.c` and
`tests/test_helper_opengl.py`; on the console (v1-backend and v2-backend-gate, 2026-10-15) 65 of 71 gate cases
passed; 1130's placed range failed host-dmem-fallback64 (below).

Patch 1132 keeps the DOS area (0x10000-0x110000) a bare reservation on PS5. Upstream maps it read-write for apps that
touch it unallocated; with 1128's guard VirtualQuery read that mapping as one not Wine's, and a 32-bit program found no
free block below 16 MiB (v3-gap-2026-10-15: "gap 0x10000-0x110000 reads occupied: protection 0x3").

1130 now also lends the placed range (`__wine_ps5_dmem_lend`): the engine treats it as foreign by record until Wine
releases it, since the owner's shared memory reads as the reservation under it; with graphics patch 0016 a WoW64
program's shared-memory buffer is placed again (v4-lend-2026-10-15: host-dmem-fallback64 passed, 20 made, 63 refused).

Patch 1133 fixes free_ranges_insert_view for a view that begins in a 64 KiB granule its neighbour keeps (an emulator splits
placeholders on 16 KiB pages): the lookup found the free range ending at that granule, the removal shrank to nothing,
and the free range the view runs into stayed listed under it. A HashLink game's 20 MiB section view at 0x2002c4000 left
0x2002d0000-0x2016c0000 free and create_view asserted 24 s in (2026-10-15; record removed from the tree; in git history); with the patch the game reached
gameplay (2026-10-15). Host: the "free-range order" replay fails without it.

Patch 1123 (a read-only page beside a written one reported to the program as written before the store was stepped) is
removed: its false reports made the earlier emulator experiment (removed) upload stale guest bytes over images the GPU drew (a Unity game's menu glyphs
as white blocks, 2026-10-15; record removed from the tree; in git history), and a write tracker must see the access violation at the store itself.
1125, 1127 and 1128 were regenerated without it (hunk positions and context only). Patch 1134 runs such stores through
the 4 KiB MMU instead (docs/MMU4K.md, wine/ps5/pw_wine_mmu4k.h): in 1122's branch of virtual_handle_fault a helper's
write fault starts a run on the faulting thread, outside the signal handler, which executes the program's instructions
one at a time with the stores to writable 4 KiB pages of held host pages going through an alias of the same direct
memory, and ends before any store into a protected page so that it faults natively; a run that cannot move takes
1122's step. Titles, WoW64 threads and `WINE_PS5_MMU4K=0` keep 1122's step. The census prints a `wine-child: mmu4k`
line. Host-tested (`tests/test_pw_wine_mmu4k.c` against the PC's 4 KiB pages, `tests/test_crash_replays.c`); the
console probes (`vm-protect-probe --truth/--speed/--contention/--rearm`, `section-view-probe`) pass on the PC under the
system Wine; nothing of it has run on the console.

Patch 1135 lets a helper end itself when Wine cannot: NtTerminateProcess stamps an exit before its write-behind flush
(`pw_ps5_child_doomed`, `wine/ps5/pw_wine_process.h`), and the GPU broker's client stamps GPU when it gives the broker
up for good (graphics patch 0017, through the lookup `wine/ps5/pw_wine_dl.c` hands it; not at win32u's
VK_ERROR_DEVICE_LOST, which a present the broker refused and answered returned in gate-hang-a-2026-10-15-01, the
programs presenting on); the loader's control thread, which takes no Wine lock, returns into `_exit` 5 s after a GPU
stamp or 20 s after an exit stamp unless the title closed the helper first (`src/pw_control_watch.h`). It also
sends `unix_private.h`'s mutex_lock and mutex_unlock for the virtual mutex to a lock-free owner record
(`wine/ps5/pw_wine_perf.h`): the census prints a `wine-child: vlock` line, the log thread a `wine-child: stall` line
once the mutex or the direct-memory engine's lock has been held 2 s, and image_stats is skipped while one owner has held
the mutex 1 s (a 2026-10-15 run, record removed from the tree; in git history, froze with the census stuck there for 160 s and no record of the holder).
Host-tested (`tests/test_pw_wine_perf.c`, `tests/test_pw_control_watch.c`, `tests/test_module_wiring.py`); the first
version ran in gate-hang-a-2026-10-15-01: 72 of 75 passed; the failures were a root-capture race in the window-effects
probe and the console resolver's SERVFAIL for dnsapi32/64 (docs/PLAN.md, "Hang safety, commit A"); this version has not
run on the console.

Patch 1136 makes a 4 KiB MMU run hand Wine's virtual mutex to a waiting thread: the run holds the mutex from its start
to its exit, and every 256 instructions reads 1135's count of threads blocked on it (`pw_wine_perf_vlock_waiters`),
ending with the exit `yield` when one waits. The unlock and relock it replaces handed the mutex over 0 times in 50 on
the host and emptied the run's page table each time; who takes the mutex after a `yield` is the mutex's and the
scheduler's choice (host: a blocked thread first 49-50 times in 50 after an 80 us gap; console not measured). No run
starts once `process_exiting` is set (mutex_lock does nothing then), a run that asked just before holds the mutex
with plain `pthread_mutex_lock`/`unlock` so runs still exclude each other, and a fault in the engine's own code, which
leaves the run's lock held, stamps
`PW_CHILD_DOOM_ENGINE` so the loader ends the helper 5 s later. With it, `wine/ps5/pw_wine_dmem_ps5.c`'s heap blocks
SIGUSR1, SIGQUIT and SIGINT across its direct-memory calls, which hold the engine's spin lock. Host-tested
(`tests/test_pw_wine_mmu4k.c` M5, M6, M8, M8-yield; `tests/test_crash_replays.c`, `tests/test_module_wiring.py`); not
run on the console.

Patch 1180 gives the title and every helper one synchronization device. Patch 0890's device kept its objects in the
title's heap and has been off since programs moved into helpers (patch 0925 left it out there), so each helper's
SetEvent, ResetEvent and wait was a round trip to the title's server thread: a Direct3D 12 game made 40k to 634k server
calls per 10 s, mostly `event_op` and `select` at 63 to 1600 us each, and drew 1 to 25 frames a second
(2026-10-16; record removed from the tree; in git history). The objects now live in shared memory the memory helper makes (`src/pw_ntsync.h`): the
title formats the region named by `WINE_PS5_NTSYNC_REGION` before its server starts, and the server sends that
descriptor to each helper with its first thread's reply (bit 2 of `inproc_device` says it follows), as upstream sends
the ntsync device. A helper that cannot join stops at once, since its waits on the device's objects would all fail at
the server. The title takes a helper out of the device before it kills it (or at its exit message) and once it has
ended. Its waits leave the queues at the first. Their places are kept until the second, since its threads may still
sleep on their words: a live wait that took one slept behind the dead thread, which the kernel wakes first, and a
restored Explorer held a session open that way (`stress-gmdx-2026-10-18-11`). The lock names its holder's process,
so a helper told gone and killed while a thread on its way in held it has it taken back at the second call. On by
default since a5a0d88
(`src/pw_speed_defaults.h`); `WINE_PS5_NTSYNC=0` turns it off for the whole session. Host-tested
(`tests/test_pw_ntsync.c`: processes started afresh, a helper gone while it waits and while it spins through the lock,
a live wait on the handle of a helper told gone whose thread still sleeps). On the console a Direct3D 12 game went from 30.5 to 60.0
fps with it on, its server round trips from 1.27 M to 36.6 k per 10 s (two runs of 2026-10-17,
record removed from the tree; in git history); the runs are in `docs/PLAN.md`.

Patch 1187 says which process publishes which display rate. Every process that first takes the desktop window
publishes the PS5 driver's one mode from its own `WINE_PS5_REFRESH`, and the last one wins; with `PROTON_120HZ=1` a
program in the Mihawk OS session was offered only 60 Hz although its own variable said 120 (`modes-p2-2026-10-17-02`).
One `wine-ps5: display published ...` line per publish names the rate, the variable and the native process.

Patch 1188 makes a virtual desktop offer the display's rate. The Mihawk OS session is a Wine virtual desktop, where
win32u reports the driver's source as detached and adds its own: its starting mode had the rate forced to 60, and
Proton's emulate_modeset collapsed the virtual modes (at 60 and the display's rate) to that mode and rebuilt them from
its 60, so with every process publishing 1920x1080@120 a program was still offered only 60 Hz
(`modes-p2-2026-10-17-03`). The virtual source now starts at the display's rate and keeps its generated modes.

Patch 1189 names what keeps a Mihawk OS session from ending, in the title's hang snapshot. The session ends when its last
user process does; in `gate-2026-10-18-defaults-01` game-mode-dxvk64's session did not end after its game's process
ended (once in 12 runs with every speed switch on). The snapshot now prints the counts the server's shutdown is decided
on and every process with its system mark, running threads, end and image name. Diagnostic only.

Patch 1240 makes the Windows user Mihawk. Proton's advapi32 answers GetUserNameA/W with a fixed `steamuser`, and every
profile path follows from it (shell32's `%USERPROFILE%`, wineboot's ProfileImagePath and Volatile Environment,
LookupAccountSid's first user), so a new prefix's profile is `C:\users\Mihawk`. A prefix made before v3.0.0-alpha-3 is
moved once by the title before Wine starts on it: the folder, its registry's paths and a link from the old name
(`src/pw_wine_profile.h`, `tests/test_pw_wine_profile.c`). The Unix `USER` the title gives Wine, and so
`WINEUSERNAME`, is Mihawk too. A new and a moved prefix passed on the base PS5 (`docs/PLAN.md`).

Patch 1241 publishes the display only from a process whose mode is new. Publishing (`update_display_cache( TRUE )`)
enumerates the Vulkan GPUs and loads RADV into the process, about 30 MiB of the console's system memory, and every
process that took the desktop window did it: a menu builder, a console host, an installer and a launcher's update
dialog each held a 40-43 MiB Unix heap where a process without it held 9 MiB (the Pro, 2026-10-20).
The desktop window now carries the mode last published (width, height, rate), and a process publishes only when its
own differs, so the last rate still wins as patch 1187 describes; the others read the display from the registry.

Patch 1242 makes a parked file answer file queries as the file. An idle file's descriptor is a socket
(`pw_wine_fdv.h`). `open_fd` then stored no inode and no unix name, `file_get_fd_type` returned `FD_TYPE_CHAR`, and
`get_handle_unix_name`, `set_fd_name`, `set_fd_disposition`, `file_open_file` and `NtCreateFile` by file id wrote
`STATUS_OBJECT_TYPE_MISMATCH` (`0xC0000024`) into an `IO_STATUS_BLOCK`. Stock Wine stats the path and returns the
file's times, size and name for `FileBasicInformation`, `FileStandardInformation`, `FileNameInformation` and
`FileAllInformation`. A real socket, fifo or device is unchanged, as are ntsync objects, consoles and d3dkmt.
`wine/ps5/pw_wine_file_query.h`, host test `tests/test_pw_file_query.c`. No console run.

Patch 1243 covers three cases patch 1242 left. `realpath` can fail after `stat` succeeded, and `unix_name` was then NULL, so `get_handle_unix_name`, `WineFileUnixNameInformation`, `set_fd_name`, `file_open_file` and the reparse FSCTLs still returned `STATUS_OBJECT_TYPE_MISMATCH`. The normalised path is kept. `NtQueryInformationFile` stopped when `server_get_unix_name` failed, which a `create_file_for_fd` handle always does, so `FileBasicInformation` stayed `STATUS_INVALID_INFO_CLASS` and `FileStandardInformation` described the socket. `get_view_file` stored `file->mode` from the live descriptor, so a parked module was `FD_TYPE_CHAR` and `NtQueryDirectoryFile` returned `STATUS_INVALID_PARAMETER`, a relative open `STATUS_BAD_DEVICE_TYPE`. Those paths pin the descriptor before `fstat`. A real socket, fifo or device is not tracked, so the pin does not change it. A mismatch still returned is one title-log line, at most 20 per process, with the syscall, the info class and the handle type. No console run.

Patch 1244 names every syscall that returns `STATUS_OBJECT_TYPE_MISMATCH` (`0xC0000024`) to the guest. WoW Classic's loader faults in `RtlTimeToTimeFields` reading that value, and the 1242/1243 mismatch lines did not fire, so the status did not come from those file paths. `__wine_syscall_dispatcher_return` writes one line, at most 48 per process: `wine-ps5: status 0xc0000024 syscall=%s id=%#x arg2=%#llx caller=%#llx`. `caller` is the guest return address. Stock type checks (the wrong handle type, ntsync, consoles, a real socket) are left as Windows returns them. `get_thread_times` on Prospero uses `getrusage` for this process and for the current thread; that call returns success, not `0xC0000024`. `wine/ps5/pw_wine_status.h`, host test `tests/test_pw_status_note.c`. No console run.

Patch 1245 is what that line showed. The three callers are ntdll syscall thunks. Inside the two processes that wrote Crash.txt the only `0xC0000024` is `NtSetIoCompletion`. That call has no `IO_STATUS_BLOCK`; a 32-bit store of the status into a zeroed pointer-sized slot is the pointer `RtlTimeToTimeFields` reads. The server accepts an object whose type name is `IoCompletion` when `completion_ops` differs between images, and checks `IO_COMPLETION_MODIFY_STATE` after the name matches. If ntsync cannot allocate the port's event, the port is still created with Wine's in-memory event. A file, pipe or device that `CreateIoCompletionPort` already bound is posted to that port and the call returns success. Windows returns `STATUS_OBJECT_TYPE_MISMATCH` for that non-port handle. An event, a section and a file with no bound port still return the mismatch. The same line now also records the handle (`r10`), the guest caller at `[rsp]` and the server type name. Success on the bound-port path is a separate line, `wine-ps5: NtSetIoCompletion handle=%08x posted to bound port`, at most 16 per server. Host test `tests/test_pw_status_note.c`. No console run.

Patch 1246 numbers the 64-bit ntdll list as Windows 10 build 19045. `0xe9` is `NtFlushInstructionCache` and `NtSetIoCompletion` is `0x1a3`. `make_specfiles` used to copy one spec number into both lists; `ALL_SYSCALLS32` stays that dense list, and the `_WIN64` list is `dlls/ntdll/win10_19045_x64.ids`. `NtAlertMultipleThreadByThreadId` is `0x1d9`. The service table uses `[id] = function`, a missing service is NULL, and the x64 dispatcher returns `0xC000001C` for that slot. `trace_syscall` and `ps5_syscall_name` index `[id] = name` and do not read past the array. `ps5_narrow_syscall_args` is unchanged in form; its ids are the numbers the stubs leave in `eax`. Reported build stays 19045. `tests/test_pw_status_note.c` still expects `NtSetIoCompletion` and id `0xe9`. `tools/check-syscall-numbers.py --self-test` exits 0. No console run.

Patch 1259 returns a status from the DirectComposition and Dxgk stubs. 1258 was installed (`bda8c378` / `507a8603`, dll `63b7dab8`). Mihawk's desktop came up, and WoW passed the `NtSetIoCompletion` probe. WowB then aborted on `win32u.NtDCompositionCommitSynchronizationObject`: the unix `stub_syscall` raises `EXCEPTION_WINE_STUB`, which is what upstream Wine does for `@ stub -syscall`. The guest RIP in that line is `000000DFFFD68357`. Windows 10 build 19045 already has every DirectComposition, Dxgk, flip and composition-surface name this spec exports; none were added, and `win32u.dll` was not rebuilt. The syscall id stays `0x1010`. 164 stubs return a status: `NtDxgk*`, `NtFlipObject*`, any name containing `Composition`, and `NtVisualCaptureBits`. The commit WowB reached returns `STATUS_SUCCESS`. A create, open, duplicate, query, get, enum, check, capture or read returns `STATUS_NOT_IMPLEMENTED` (`0xC0000002`). One line, at most 32 a process, is `wine-ps5: dcomp-stub`. Other `NtUser` and `NtGdi` stubs still abort. `wow64win` was not rebuilt. The 1258 `int 2e` path is unchanged, and `ntdll.prx.elf` stays `bda8c378`. `win32u.prx.elf` is `55313ff2` (2,473,676 bytes), `win32u.prx` is `b5bf2004` (2,192,546), the unstripped image is `0374a2ec` (8,745,472). This link's `NEEDED` entries are `ntdll.prx`, `libSceLibcInternal.sprx` and `libkernel.sprx`. The PS5 OpenGL SDK is not in this environment, so `55313ff2` must not replace the console `win32u`. The console file is 27,214,348 bytes, `d28717e3`, and `tools/patch_win32u_dcomp.py` rewrites the same 164 bodies inside it: `win32u.prx.elf` `22f62466` (27,214,348) and `win32u.prx` `d4d8cfc8` (25,576,771). No console run.

Patch 1258 resumes `int 2e` on every thread. 1257 was installed (`a094f685` / `a56da7ff`, dll `63b7dab8`). Mihawk's desktop appeared, then the process crashed. `syscall-int2e` armed 258 thunks, and hundreds of `syscall-emu op=cd2e restored=0` resumed. The faults are `#GP` error `0x172` at `cd 2e`: `ntdll+0x56a55` is `NtReleaseMutant` (`0x20`), `+0x56d75` is `NtFsControlFile` (`0x39`), and `+0x56eb5` is `NtContinue` (`0x43`). The emulator's depth counter was process-wide, so the thread that lost that race was delivered as an access violation and no `syscall-emu skip` line was written. The counter is now `ps5_syscall_emu` at TEB `0x354`. Another thread still resumes at the dispatcher prolog end. The opcode is read from the fetched page, without `virtual_mutex`, and the 64-bit ntdll range is cached at arm time. A stub in that image resumes even when CS equals the 32-bit selector. A 32-bit CS outside that image stays out. `handle_interrupt` vector `0x2e` uses the same resume. `NtContinue`, `NtCallbackReturn`, `NtRaiseException` and `NtTestAlert` stay on the Win10 bytes and use the thread's current frame. A same-thread nest deeper than 4 logs `why=reenter`. Other refusals log `why=range`, `why=stack` or `why=frame`, at most 16. After `path=int2e`, every 64-bit thunk on every thread executes `int 2e`, including the three that faulted. Calls before `handlers=ready` stay on `call [0x7ffe1000]`. Syscall numbers are unchanged. The compare of protocol `0x3a3` stays at VA `0x35f70`, and the `je` moved from `0x916f3` to `0x91a33`. The PE image was not rebuilt. No console run.

Patch 1257 makes each x64 ntdll thunk match the Win10 19045 bytes and traps the taken path as `int 2e`. 1253 executed `0f 05` with a Windows number and Prospero returned to the following `ret`, so the number enters the kernel. `SystemCall` stays 1. `ps5_arm_ntdll_syscalls` still leaves `call [0x7ffe1000]` in place and logs `path=call`, which is every process until its signal handlers exist. After `handlers=ready`, the tail `eb 01 c3 ff 14 25 00 10 fe 7f c3` becomes `cd 2e c3 0f 1f 84 00 00 00 00 00`. `jne +3` lands on `int 2e`. The fault resumes at the dispatcher prolog end: nothing is pushed, an `rsp` already inside the thread stack is left alone, and the epilogue returns to the stub's `ret`. `cd 29` raises non-continuable `STATUS_STACK_BUFFER_OVERRUN` (`0xC0000409`) with the code in `RCX`. SIGSYS uses the same resume for an ntdll stub and otherwise keeps `SIG_DFL`. After `path=int2e`, Mihawk, explorer, Battle.net and WowB execute `int 2e`. Calls made before that line stay on `call [0x7ffe1000]`. A 32-bit code segment is unchanged. The checker exits 0 with the 19045 line. The compare of protocol `0x3a3` stays at VA `0x35f70`. The `je` moved to VA `0x916f3`. `ntdll.prx.elf` is `a094f685` (1,438,628 bytes), `ntdll.prx` is `a56da7ff` (1,229,410), the unstripped image is `4e833216` (3,921,976). The dll stays `63b7dab8`. No console run.

Patch 1256 keeps `call [0x7ffe1000]` for an intact ntdll thunk and repairs the one stub the loader rewrote. 1255 was installed (`a586cf87` / `8ce100e8`, dll `63b7dab8`). Mihawk stayed on a black screen before WoW and Battle.net, and a log pull returned nothing. 1255 stored `KUSER_SHARED_DATA.SystemCall` as 0 after rewriting every thunk to `cd 2e`, so process startup and the shell executed `int 2e`. This build leaves the file's `0f 05` and leaves `SystemCall` at 1. A site that is already `cd 2e` is written back to `0f 05`. If bit 0 is clear it is set, and it is not stored as 0. Before the fifth argument, an `rsp` outside the thread stack, or not 8-aligned, calls `ps5_repair_patched_stub` on the kernel stack. The function changes `rsp` and `r10` only when the bytes at `frame->rip-0x14` are a Wine thunk whose first byte is no longer `4c 8b d1`. `5c 8b d1` puts the guest `rcx` saved at entry into `r10`, and the lowest stack slot that still holds the popped qword becomes `frame->rsp`. A thread whose stack is not set yet, and an `rsp` inside the stack, stay on the fast path. The Mihawk shell, explorer, service processes, wineserver clients and Battle.net do not enter the repair. A WowB syscall whose stub is still `4c 8b d1` does not enter it either, including the two `NtSetIoCompletion` returns through `ntdll+0x5835f`. The repaired entry is the WowB wine-child after `image-writecopy` at `ntdll+0x58340`. Other stubs in that process stay on the fast path until their own first byte is rewritten. 32-bit code is unchanged. The `int 2e` emulator stays and startup does not execute it. Every process writes `syscall-boot`, `syscall-arm` with `path=call`, and `signal-init` before and after the handlers. A repair is `stub-repair`, at most 32 lines, and a skip is `stub-repair skip`, at most 8. Syscall numbers are unchanged. The compare of protocol `0x3a3` is the same instruction, still at VA `0x35f70`. `movl $0x3a3, %edx` is still at VA `0x36284`. The `je` displacement moved to VA `0x90e63`. No console run.

Patch 1255 clears `KUSER_SHARED_DATA.SystemCall` after the `cd 2e` rewrite and logs the stub bytes on an ntdll access fault. 1254 was installed (`e21e5c4d` / `a706e789`, dll `63b7dab8`). `syscall-arm ntdll count=258` is in every child. There is no `syscall-emu` line. Play returns to Play and the newest Crash.txt is still `15.45.38`. wine-child 1133 and 1135 fault the same way. Two `NtSetIoCompletion` calls return `0xC0000024` with handle `-1` and type `Process` through `caller=ntdll+0x5835f`, then `image-writecopy` at `ntdll+0x58340`, then trap 9 error 0 at `ntdll+0x58354`. Those 16 bytes are the file's `ret`. `rsp` is `0xdfffa2faea`, the guest return of those calls. `rcx` equals that RIP because the dispatcher epilogue returns there (`frame->rip` was `0x5835f` minus `0xb`), not because the kernel executed `syscall`. `SystemCall` was 1, so `jne +3` skipped `int 2e`. Windows returns `STATUS_OBJECT_TYPE_MISMATCH` for `NtCurrentProcess` passed to `NtSetIoCompletion`; the guest continued, and that status is unchanged. The executing mapping is the JIT object `jit_over` installed with `MAP_FIXED`. A file or JIT page has no direct-memory alias, and this run's `mmu4k total runs=0`. An ntdll access fault now writes `wine-ps5: stub-bytes` with 32 bytes at `pc-0x14` from that address (`exec`) and from `pw_wine_alias_find` (`rw`, or `-` when no window exists). The find does not create a window. The same line records the dword at `0x7ffe0308`. After the 258 sites are `cd 2e`, the arm stores `SystemCall` as 0, which is the Windows 10 value, and the arm line is `syscall-arm ntdll count=%u systemcall=%#x`. The initializer stays 1 until that store, so a stub that still contains `syscall` takes `call [0x7ffe1000]`. Guest code starts after the signal handlers, which is after `load_ntdll` returns. `pop rsp` still runs before the flag test. `int 2e` then faults, and the 1254 handler puts `rsp` back on the stack slot that still holds that qword. The access-fault, image-protect, image-write, image-writecopy, syscall-emu and syscall-arm lines stay. Syscall numbers are unchanged. The compare of protocol `0x3a3` is the same instruction, still at VA `0x35f70`. `movl $0x3a3, %edx` is still at VA `0x36284`. The `je` displacement moved to VA `0x90863`. No console run.

Patch 1254 emulates `syscall` and `int 0x2e` from an ntdll image page. 1253 was installed (`dfbe8186` / `db0a6ff0`, dll `63b7dab8`). The two loader stores completed (`image-writecopy` at `ntdll+0x58340`, host protection `0x7`) and Crash.txt stopped. The stub then raised trap 9, error 0, at `ntdll+0x58354`. Those bytes are the file's `ret` after `syscall` (`0f 05` at `+0x12`). `rcx` equals that RIP, which is what the CPU stores when `syscall` runs. Prospero's kernel accepted the Windows number and returned. `rsp` is `0xdfffa2faea`, the guest return address of the two `NtSetIoCompletion` calls that had just returned `0xC0000024` through `call [0x7ffe1000]`. The thread stack is `0x1000200000`–`0x1000210000`. SEH reports the frame is outside that range and cannot dispatch, the thread is killed, and the payload logs `reason=exit` then `control-ended`. The 15.45 store of byte `0x5c` at the stub's first byte is `pop rsp` if it landed again; the tail at `+0x14` still matches the file. This DLL's `.text` holds the Win10 sequence `75 03 0f 05` at 258 sites. Those two bytes become `cd 2e`. Fifteen other `0f 05` sequences are displacements inside other instructions and stay. `int 2e` is the same length, so `jne +3` still reaches `call [0x7ffe1000]` when `KUSER_SHARED_DATA+0x308` bit 0 is set. That byte stays initialized to 1. When it reads 0, `int 2e` or a faulting `syscall` inside `ntdll.dll` enters `__wine_syscall_dispatcher` at its prolog end. `eax` is the 19045 id, `r10` is the first argument, `[rsp+0x28]` is the fifth. The dispatcher subtracts `0xb` from the return address, so the frame is given `rip+2+0xb` and the stub's `ret` runs with the status in `rax`. An RSP that is the popped return address is put back at the stack slot that still holds that qword. The prefix `5c 8b d1` means `mov r10, rcx` did not run, so `r10` is taken from `rcx`. At most 32 lines, `wine-ps5: syscall-emu`, and 8 `syscall-emu skip` lines. One `wine-ps5: syscall-arm` line names how many sites were rewritten. The `0xC0000024` tracer is that same dispatcher return. A 32-bit code segment is left to the 32-bit path. Syscall numbers are unchanged. The compare of protocol `0x3a3` is the same instruction. No console run.

Patch 1253 makes a write into an exec-converted image page a private writable copy. 1252 was installed (`7038bb30` / `703f7cbc`, dll `63b7dab8`). Crash.txt `15.45.38` is the same write of `WowB_loader+0x2017E3B` at ntdll+`0x538C0`. The access-fault lines name two stores from `WowB_loader+0x2007347` and `+0x2007367` into `kernel32+0x110e4` (`vprot=0xa5`) and `ntdll+0x58340` (`vprot=0x25`, `hostv=0xa5`). `0xa5` is `VPROT_COPIED|VPROT_COMMITTED|VPROT_EXEC|VPROT_READ`. `VPROT_WRITECOPY` is `0x08` and is not set. `unix=0x5` is read and execute. The exec conversion had made those pages JIT memory. The page byte has no write, so the 4 KiB step does not run (`mmu4k total runs=0`), and the store is delivered. The third fault is the Crash.txt line. Its 16 bytes are `movq %rdi, (%r15)` from `RtlSelfRelativeToAbsoluteSD+0x330` (file RVA `0x549c0`), and the target is a read-only map (`vprot=0x21`), not an image. Signal pc, context RIP and `ExceptionAddress` match on every fault. A write fault on a committed executable image page whose own byte, or another byte of its 16 KiB host page, has write, write-copy or copied, and whose own unix protection does not already include write, sets the faulting 4 KiB page and any executable neighbour that already has one of those bits to read, write, execute and copied, and clears write-copy. The host page is made writable and stays executable, so the whole 16 KiB page is kernel-writable. `sceKernelMprotect` is tried first. When the kernel still lacks write, the JIT object is replaced and protected again. `NtProtectVirtualMemory` of an image host page that asks for write, while the kernel still has execute and lacks write, applies that protection on the host the same way. A protection that dropped execute is applied as read-write, and the next execute fault converts again. At most 16 lines, `wine-ps5: image-writecopy`. The access-fault, image-protect and image-write lines stay. A plain read-execute host page, and the loader's read-only map, are not opened. Syscall numbers are unchanged. The compare of protocol `0x3a3` is the same instruction. No console run.

Patch 1252 logs an access violation before it is delivered, and logs `NtProtectVirtualMemory` and `NtWriteVirtualMemory` when the range overlaps `ntdll.dll` or `WowB_loader.dll`. 5653f96 was installed. Both WowB Crash.txt lines write `WowB_loader+0x2017E3B` at ntdll+`0x538C0`, the hotpatch NOP at `RtlEqualPrefixSid`. That function's file bytes do not store. One line, at most 32 a process, `wine-ps5: access-fault`, names the signal trap and error code, the signal RIP, the context RIP, the exception address, the access kind, 16 bytes at the signal RIP, the view and page byte of both addresses, and up to six return addresses as module+RVA. A protect or write of either image writes `wine-ps5: image-protect` or `wine-ps5: image-write`, at most 64 lines together, with the module and RVA, the address, the size, the new and old protection or the bytes written, and the status. The record, the RIP, the protections and the returned status stay as they were. Syscall numbers are unchanged. The compare of protocol `0x3a3` is the same instruction. No console run.

Patch 1251 logs every illegal or privileged instruction before it is delivered, and completes SGDT, SIDT, SLDT, SMSW and STR. 7f25807 was installed and WowB still wrote `ILLEGAL_INSTRUCTION`. Crash.txt `14.57.56` and `14.58.02` have no address. wine-child 819 and 821 logged `execute-fault` at `0x1000445ad0` and `0x1000445b60` and no `stack overflow` line, so patch 1250 did not run. One line, at most 16 a process, `wine-ps5: illegal-instruction`, names the signal, trap, error code, `si_code`, exception code, RIP, module and offset, 32 instruction bytes, `emulated=` and the general registers. A vectored handler that then sees the exception writes `wine-ps5: vectored-handler` with `continue` or `search`, at most 16 lines. SGDT, SIDT, SLDT, SMSW and STR are completed on `#UD` and on `#GP(0)`, with the values Linux stores in the kernel: GDT base `0xfffffffffffe0000`, IDT base `0xffffffffffff0000`, limits 0, SMSW `0x80050033`, SLDT 0, STR `0x40`. XGETBV, RDTSCP, CPUID, RDPID, IN/OUT and UD2 are not emulated. `wine/ps5/pw_wine_umip.h`, host test `tests/test_pw_umip.c`. Syscall numbers are unchanged. No console run.

Patch 1250 delivers an exception whose frame sits in the bottom 16 KiB host page of a thread stack. That page is left with no access, and a frame there used to abort the thread before `KiUserExceptionDispatcher`, so a vectored handler never ran. After patch 1249 the e019bd8 WowB process fetched `0x1000443fd0`, `0x1000445ad0` and `0x1000445b80` (`vprot=0x23`, `view=0x3`), then faulted at `WowB_loader.dll+0x168E90D` (`0xdffd1de90d`). The frame was 1856 bytes into the last host page. Crash.txt says `ILLEGAL_INSTRUCTION` and has no address, no registers and no stack. The page is committed once. At most eight lines, `err:virtual:ps5_open_last_stack_page stack overflow … code=… addr=… bytes=…`, name the exception code and 16 instruction bytes, and the original exception is delivered. A later frame in that page still aborts. JIT conversion is unchanged: `__wine_ps5_exec_convert` replaces the mapping, so a later write to that address hits the JIT object. CPUID, XGETBV and `KUSER_SHARED_DATA` `ProcessorFeatures` stay the hardware values. Syscall numbers are unchanged. No console run.

Patch 1249 makes an execute fault on committed private memory into JIT memory when Wine did not mark the page executable. The 2be3f2a WowB process fetched `0x1000445a50` and `0x1000443f40`, both inside the 16 GiB reservation at `0x1000000000`, and exited with `exec_refused` 0: the page byte has no execute, so the existing converter did not run. The host page has to be one private view, and every 4 KiB page in it committed, readable or writable, and not a guard. Those pages gain execute and become one JIT object through the existing `__wine_ps5_exec_convert`. A file or image view is left alone. At most eight `wine-ps5: execute-fault` lines name the address, the old page byte and the view protection. Syscall numbers are unchanged. No console run.

Patch 1248 stops `RtlIpv6StringToAddress` from writing a terminator whose bits 63:47 are set. That pointer is not in the user half. The dfbaa50 WowB process faulted on the store at ntdll+`0x52593` (`ipv6_string_to_address`, the `*terminator = str` line) with target `0xFFFFFC8009CC8C80`, and WoW's filter showed the launch dialog. The function returns `STATUS_ACCESS_VIOLATION` and does not raise. NULL and a user pointer are stored as before. At most eight `wine-ps5: ipv6-terminator` lines name the pointer and the caller of `RtlIpv6StringToAddressW`. Syscall numbers are unchanged. No console run.

Patch 1247 records the exception `user_callback_handler` swallows. That handler is the SEH frame on `KiUserCallbackDispatcher`. It unwinds and returns the exception code, so an access violation inside a user callback never becomes unhandled and WoW writes no Crash.txt. The PE handler and the Unix signal path each write `wine-ps5: callback-fault`, at most 64 lines a process, with the `EXCEPTION_RECORD` (code, flags, fault address, read/write flag, target), the callback id, that id's name in `include/ntuser.h` `ALL_USER32_CALLBACKS`, the module and offset of the faulting RIP, the registers, sixteen qwords at the guest `rsp` and sixteen `rbp` return addresses. Ids `0` through `24` are user32 functions win32u enters with `KeUserModeCallback`. `WINE_PS5_CB_NOSWALLOW=1`, or `HKCU\Software\Wine\PS5` value `CallbackNoSwallow` DWORD `1`, skips the unwind. An environment value that is present and is not `1` forces the unwind even when the registry is `1`. The default is still the unwind. Single-step and breakpoints are not logged. Syscall numbers are unchanged. No console run.
