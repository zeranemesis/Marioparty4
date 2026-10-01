# PartyBoard Nintendo Switch bootstrap

This directory is the first native Nintendo Switch rendering milestone for PartyBoard.

It intentionally does **not** try to compile the full game yet. The current PC/Android port already routes game rendering through Aurora and exposes OpenGL/OpenGL ES backends. This probe validates the closest open homebrew path first:

- AArch64 via devkitA64/libnx
- the default `NWindow`
- EGL + Mesa/Nouveau
- OpenGL GPU rendering
- libnx controller input
- a native `.nro` output

The probe renders a hardware-accelerated RGB triangle on a dark background. Press **+** to exit.

## Why OpenGL first?

PartyBoard's existing `src/port/portmain.cpp` already recognizes `BACKEND_OPENGL` and `BACKEND_OPENGLES`. Proving the Switch OpenGL stack before touching Aurora keeps the first milestone small and gives us a direct path to wiring the existing GX/Aurora renderer later.

The Android/Quest target remains the closest platform architecture in the repository because it is already ARM64 and uses the portable port layer. The Switch-specific code should therefore stay thin and avoid duplicating game logic.

## Build requirements

Install devkitPro with Switch support, then install the OpenGL packages used by the official switchbrew examples:

```sh
dkp-pacman -S switch-dev switch-mesa switch-glad
```

On systems where devkitPro's pacman command is named `pacman`, use that instead.

Then:

```sh
cd platforms/switch
make
```

Expected output:

```text
partyboard-switch-probe.nro
```

## Next integration milestone

Once this renders correctly on hardware, the next step is to replace the test scene with the PartyBoard/Aurora startup sequence:

1. make the root build understand a `NintendoSwitch` target;
2. keep the existing `src/port` game/OS abstraction rather than forking game logic;
3. add a homebrew-compatible SDL3/Aurora platform bridge, or adapt Aurora's window creation to use libnx `NWindow`;
4. select Aurora's OpenGL/OpenGL ES backend on Switch;
5. bring up GX clear/frame presentation before loading models;
6. then add HID mapping, filesystem paths, audio and REL loading one subsystem at a time.

This probe is deliberately isolated so it cannot regress the current Windows/Android/Apple targets.


## GameCube launcher

`launcher/` holds a Switch 2-style GameCube front end for the port: a shelf
of the GameCube discs on the SD card (with their real banners), a cube boot
animation, options and controller screens. It is its own NRO and chain-loads
the engine. The preset below builds it as `partyboard_switch_launcher_nro`;
see [launcher/README.md](launcher/README.md) for the SD layout, the engine
arguments and the headless preview.

## CMake bring-up builds

The Switch target is now integrated into the repository's root CMake without
configuring the desktop PartyBoard dependency stack.

Normal native probe:

```sh
cmake --preset switch-libnx-bootstrap
cmake --build build/switch-libnx-bootstrap --target partyboard_switch_nro --parallel
```

This compiles:

- the libnx/EGL/OpenGL render probe;
- the native four-player GameCube `PADRead` backend;
- Aurora's real `dolphin/mtx` implementation for AArch64;
- the Aurora MTX runtime self-test;
- the EGL extension check required by Dawn's OpenGL backend.

The generated file is:

```text
build/switch-libnx-bootstrap/platforms/switch/partyboard-switch-probe.nro
```

### Experimental Dawn probe

Dawn is the renderer used below Aurora's GX layer. It does not officially know
libnx, so its Switch work is kept behind a separate opt-in target:

```sh
cmake --preset switch-libnx-dawn-probe
cmake --build build/switch-libnx-dawn-probe --target partyboard_switch_dawn_nro --parallel
```

The experiment uses the same Dawn revision pinned by Aurora and enables only
the OpenGL ES backend. Build-only patches (`cmake/PatchDawnForSwitch.cmake`
and `cmake/PatchDawnDependenciesForSwitch.cmake`, applied to FetchContent's
copy; the upstream Aurora/Dawn submodules remain untouched) teach Dawn that
`__SWITCH__` is a POSIX-like platform and fill the gaps libnx leaves: no
dynamic loader, executable path, `pipe`/`poll` events or `mmap` (Abseil's
low-level allocator gets heap pages instead), newlib's POSIX-only
declarations under strict C++, and the Switch's native EGL types in Khronos'
`eglplatform.h`. With them the probe compiles and links: a 12 MB NRO with
Dawn's OpenGL ES backend, Tint's GLSL writer and the `NWindow` surface.

If the NRO reaches hardware, its screen is intentionally diagnostic:

- **green**: external libnx EGL display -> Dawn OpenGLES adapter -> WebGPU device
  -> WebGPU texture succeeded;
- **red**: one of those stages failed.

Dawn itself requires `EGL_EXT_create_context_robustness` and either
`EGL_KHR_fence_sync` or `EGL_KHR_reusable_sync` for this adapter path. The
normal probe prints whether the Switch Mesa/Nouveau EGL stack exposes them.

### SDL3 probe

Aurora's window, input, events and file access go through SDL3, which
devkitPro does not ship for the Switch. `sdl3/` builds Aurora's SDL3 release
with a homebrew libnx backend (video on the default `NWindow`, AUDOUT audio,
HID gamepads) and exposes it as `SDL3::SDL3-static`; see
[sdl3/README.md](sdl3/README.md).

```sh
cmake --preset switch-libnx-sdl3-probe
cmake --build build/switch-libnx-sdl3-probe --target partyboard_switch_sdl3_nro --parallel
```

The NRO shows SDL's drivers, the base and pref paths, a file round trip
through `SDL_IOStream`, and every connected gamepad live; hold **A** for a
440 Hz tone, **+** quits.

### Aurora probe

Aurora itself (its GX-on-WebGPU renderer, window, input and caches) now
builds for the Switch, on top of the two probes above:

```sh
# CI applies patches/aurora-*.patch to extern/aurora first, Switch last.
cmake --preset switch-libnx-aurora-probe
cmake --build build/switch-libnx-aurora-probe --target partyboard_switch_aurora_nro --parallel
```

`partyboard-switch-aurora-probe.nro` (`source/aurora_probe.c`) uses Aurora
the way the game does: `GXInit`, then each frame clears the EFB to a cycling
colour and draws, under an orthographic 640x480 projection, a spinning
triangle with per-vertex colours (`GX_PASSCLR`) and a tinted quad textured
with an I8 checkerboard in GameCube tile layout (`GX_MODULATE`). A black
screen, a frozen frame or a missing shape points at the failing stage; **+**
quits. `aurora/SwitchAurora.cmake` builds
`extern/aurora` against the Switch Dawn and SDL3 and devkitPro's portlibs
(zlib, libpng, FreeType, zstd). What the Switch needs on top:

- `patches/aurora-switch.patch`: the WebGPU surface goes on libnx's default
  `NWindow` through Dawn's EGL entry point, Dawn gets switch-mesa's EGL
  display and loader, and SDL stays out of EGL. It also asks for the
  `Compatibility` feature level on OpenGL and OpenGL ES, the only level Dawn
  offers there; Aurora asked for `Core`, which finds no adapter on those
  backends (the OpenGL ES fallback on other platforms has the same problem).
  Dawn's OpenGL backends keep their EGL context current on the thread that
  created the device, and another thread cannot take it (`EGL_BAD_ACCESS`,
  device lost): on those backends Aurora's render worker stays off and its
  work runs on the game thread, as its pipeline-cache worker already did.
- SQLite (Aurora's pipeline caches) is built with `SQLITE_OS_OTHER` and
  `aurora/sqlite_vfs_switch.c`: SQLite's demo VFS for embedded systems plus
  pthread mutexes, since its unix VFS needs `ioctl`, `mmap` and file locks.
- Tracy's thread-id and login lookups get a libnx case
  (`aurora/PatchTracyForSwitch.cmake`), and Dear ImGui's fork/exec "open in
  shell" is compiled out.

The same probe builds for Linux (`aurora/host/`) to check that renderer
configuration without a console: Dawn's OpenGL ES backend at the
`Compatibility` level on Mesa's llvmpipe, with Vulkan (lavapipe) as the
reference. CI's "Aurora on OpenGL ES (Mesa)" job runs both for 300 frames,
fails on any Aurora error, and uploads a screenshot of each; locally:

```sh
# extern/aurora carries the same patches as above.
cmake -S platforms/switch/aurora/host -B build/aurora-host -G Ninja
cmake --build build/aurora-host --target aurora_probe
platforms/switch/aurora/host/run-probe.sh build/aurora-host build/aurora-probe-shots
```

`--backend vulkan` and `--frames N` select the backend and stop after N
frames when running the probe by hand.

### The game

The game itself now compiles and links for the Switch into one NRO, with
its `res/` directory in the romfs:

```sh
# CI also applies patches/musyx-partyboard.patch to extern/musyx.
cmake --preset switch-libnx-game
cmake --build build/switch-libnx-game --target partyboard_switch_game_nro --parallel
```

`game/SwitchGame.cmake` builds the main module (`dol`: the decompiled game
plus `src/port`) as on the other platforms, with RmlUi and Aurora's DVD layer
over nodlite. What differs:

- **Overlays are linked in.** libnx has no dynamic loader, so each REL is
  linked into one relocatable object (`ld -r`, `game/overlay.ld`) whose only
  global symbol is its renamed `ObjectSetup`; a generated table maps
  `_ovltbl`'s names to them, and `objdll.c` looks there under
  `PARTYBOARD_STATIC_OVERLAYS` (`include/port/static_overlays.h`). A shared
  library starts from fresh globals each time it is opened, and the game
  relies on it, so each overlay's writable data and bss are gathered into
  sections of their own and reset whenever the game links it again.
- **Mbed TLS** gets a Switch configuration (`game/mbedtls_switch_config.h`):
  no clock or timer, entropy from libnx's random generator.
- **Process setup** (`game/switch_app_init.c`, libnx's `userAppInit`):
  romfs mounted and made the working directory (the port opens `res/...`
  relative to it), BSD sockets, nxlink stdio, and SDL's preference path on
  `sdmc:/switch/partyboard`.

It has not run on a console yet; the next work is the runtime: the disc path
from the launcher, input, audio, and Aurora's renderer at Dawn's
`Compatibility` level.

## Current integration boundary

The intended renderer chain is now:

```text
Mario Party 4 GX calls
        |
        v
Aurora GX / WebGPU
        |
        v
Dawn OpenGLES
        |
        v
PartyBoardSwitch EGL boundary
        |
        v
Mesa / Nouveau -> NWindow -> libnx
```

The next renderer milestone is not to duplicate GX in a Switch-only renderer.
It is to make Dawn's headless/external-EGL OpenGLES device compile on libnx,
then replace Aurora's desktop `wgpu::Surface` presentation with a Switch
presentation bridge.
