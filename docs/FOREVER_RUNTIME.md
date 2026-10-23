# Forever Wine as a second runtime

Assessed 2026-10-23 from bizkut/ps5-forever-wine `ee64b784239b73f4ff7aa30adc8b3dcd1966ded0`
(`ps5/release/deps.json` content `d465aa69e0625f57591634e1a0ffe753c329d13f1f7c089b2d39dafd12761453`,
describe `wine-8.0-21023-forever-01.000.021`; the tree's `VERSION` file says Wine 11.0)
and from bizkut/ps5-forever `ps5/build-wine.sh` and `ps5/paths.sh`. No build was started.
No console run.

The Wine tree's `LICENSE` is the GNU Lesser General Public License, version 2.1 or
later. It is not a GPL-3.0-only grant. DXVK and VKD3D-Proton are separate pins in
that `deps.json` and are not in this checkout.

## Verdict

A second directory next to `/data/ps5proton/runtime/ge11-7` is the right place for
an experiment, and deleting that directory is a safe revert only while nothing in
the title opens it. Copying Forever's Wine into that directory does not make it a
runtime Mihawk can select. The current launcher and the payload child both load
`ntdll.prx` through Mihawk's PRX descriptor and child protocol. Forever's Unix
side is `ntdll.so`, opened with `dlopen`, and it expects a different title ABI.
The build is possible on this SDK after FreeType and GnuTLS. The objects that
build would produce are not loadable by the current boot path, so the build was
not started.

## What the launcher does now

`native/proton_wine.cpp` sets the runtime to `/data/ps5proton/runtime/ge11-7` and
loads `lib/wine/x86_64-unix/ntdll.prx` with `sceKernelLoadStartModule`. It resolves
`__wine_main` and the other entries through the `PRXDESC1` table
(`src/pw_wine_start.h`, `wine/ps5/pw_wine_prx.h`). The same process is the shell.
A helper is a new payload. `native/proton_wine_child.c` reads `boot.runtime` and
loads `$runtime/ntdll.prx` the same way. The bootstrap word is
`PW_CHILD_PROTOCOL` `0x50573511` (`wine/ps5/pw_wine_process.h`). The child refuses
an ntdll that does not export the payload hooks from that round, including
`__wine_ps5_set_payload_thread_start`. The ntdll descriptor linked into Mihawk's
module names 131 exports (`__wine_main`, the present sink, the GPU broker, the
memory helper, ntsync, the thread registry, and the rest of that list).

DXVK and VKD3D-Proton are not inside ntdll. The title sets
`WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,d3d9,d3d12,d3d12core=n` and
`WINE_D3D_CONFIG=renderer=vulkan`. Those native DLLs come from the ge11-7
runtime. The title owns RADV and hands the child a GPU broker fd.

## What Forever's loader requires

`loader/main.c`, under `__PROSPERO__` and `WINEPS5_IN_TITLE=1`, reads
`WINEPS5_TITLE_RUNTIME` as a pointer to `struct wine_ps5_title_runtime` and
requires `version == 12` (`include/wine/ps5_title.h`). The required callbacks are
`create_thread`, `join_thread`, `create_key`, `get_specific`, `set_specific`,
`delete_key`, and a writable `stdout_fd`. `vulkan_proc` is the title's one RADV.
`audio_open` / `audio_output` / `audio_close`, `spawn_process`, `payload_spawn`,
`desktop_connect`, `desktop_surface`, `game_launch`, and `get_hdr_luminance` are
optional. The loader `dlopen`s `ntdll.so` and requires `__wine_main`,
`__wine_ps5_set_title_runtime`, and `__wine_ps5_shutdown_title_runtime`
(`dlls/ntdll/unix/server.c`). A search of Forever's `dlls/ntdll` finds no
`pw_wine_` symbol.

Forever's wineserver is `wineserver.so` in the same process (`loader/main.c`
renames `main` to `wineserver_main`). `SERVER_PROTOCOL_VERSION` is 930 in
Forever's `include/wine/server_protocol.h` and 931 in the GE-Proton tree this
runtime is built from. The two servers cannot serve each other's ntdll.

`ps5/paths.sh` compiles `PS5_WINE_ROOT` as `/data/homebrew/PPSA99030/wine`.
`ps5/build-wine.sh` passes that directory as `UNIXLIB_SONAME_DIR` and as the
absolute sonames of FreeType, GnuTLS, and Vulkan. The PS5 loader opens
`DT_NEEDED` by that path. A folder copied to `/data/ps5proton/runtime/...`
without a rebuild does not resolve. `paths.sh` says a change there needs
configure, a full build, a FreeType rebuild, and a stage.

## Glue a WoW-only path would need

The shell process keeps loading ge11-7. A WoW or Battle.net payload would be a
different entry, used only when the second directory is present, so deleting
the directory leaves the current boot. That entry is not a path string change.
`proton_wine_child.c` would still look for `ntdll.prx` and `0x50573511`.

The new entry has to:

- leave ge11-7 unmapped in that process
- implement `wine_ps5_title_runtime` version 12 with the thread and TLS
  callbacks, a stdout fd, and `vulkan_proc` for one RADV
- `dlopen` Forever's `ntdll.so` and call `__wine_ps5_set_title_runtime` before
  `__wine_main`
- run Forever's private `wineserver.so` (protocol 930), not Mihawk's wineserver
- install DXVK and VKD3D-Proton from Forever's pins
  (`ps5-forever-dxvk` `9f8db7dae7e9218eb6f0197ac7b3a61367e3044d`,
  `ps5-forever-vkd3d-proton` `db62a5510f5cbd1d53976755109630c473dcaa0d`),
  which are not in this checkout
- build the tree with `PS5_WINE_ROOT` set to the second directory, so the
  sonames match the folder that can be deleted

Forever's comment on `vulkan_proc` says the native title owns one RADV and a
payload must not `dlopen` a second copy. Mihawk's proven RADV is the title's,
reached by the GPU broker. A payload that calls `vulkan_proc` needs that same
library in the payload, or a broker the Forever unix side does not call.
Forever's Mesa pin is `f10c4bac9b71c45a4c794ecc3c227eb6f6a56361` and its Vulkan
pin is `2cf759774168c9f99e9af1b1d87934fb98d8229e`. Those are not this tree's
RADV.

## Build, on this machine

`prospero-clang` is present, target `x86_64-sie-ps5`, and it defines
`__PROSPERO__` and `__FreeBSD__ 9`. `prospero-clang --version` here is Ubuntu
clang 18.1.3. Forever's `deps.json` and `tools/build-lapy-validation.py` name
the same public SDK zip, payload-sdk v0.42, sha256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`. Forever also
pins a SDK fork, `fdb2763789a50a4fcc9a98dcae13f5e336c6a1ee`, which this
extracted SDK does not identify. Host bison, flex, pkg-config, and FreeType
headers are present. Forever's `configure` is not generated. PS5 FreeType and
PS5 GnuTLS shared libraries are not built here (`ps5/build-freetype.sh` wants
FreeType 2.14.3 with soname `$WINE_ROOT/lib/libfreetype.so`; GnuTLS is the
step before `ps5/build-wine.sh configure`).

`ps5/build-wine.sh` configures a native tools tree, then the PS5 tree with
`--host=x86_64-unknown-freebsd11`, `--enable-archs=i386,x86_64`,
`CC=prospero-clang`, and `--without-opengl`. The default make targets are
`loader/all`, `server/all`, and `dlls/ntdll/all`. A runtime that can start a
game is `ps5/build-wine.sh all` after FreeType and GnuTLS, then their DXVK and
VKD3D-Proton builds, then the loader above. Disk has the room. None of that
output is a `ntdll.prx` the current child will load.

## Boot risk

Leaving the second directory unread keeps the ge11-7 boot exactly as it is.
Pointing `proton_wine.cpp`'s runtime string at Forever's tree makes the title
miss `ntdll.prx` and stop in `fail("Wine: runtime missing")`, or, if a file of
that name is planted, load an image without the 131 exports and the
`0x50573511` hooks. That is the shell process, so the desktop does not come up.
The safe selector is a new payload entry that runs only when the directory
exists and only for the chosen image. Until that entry exists, installing the
folder does not change boot, and it also does not run WoW.

This assessment was not run on a console. It does not claim Forever's window
transfers to a Mihawk payload.
