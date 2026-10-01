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
