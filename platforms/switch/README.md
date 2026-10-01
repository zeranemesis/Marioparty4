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
the OpenGL ES backend. A build-only patch teaches Dawn that `__SWITCH__` is a
POSIX-like platform; the upstream Aurora/Dawn submodules remain untouched.

If the NRO reaches hardware, its screen is intentionally diagnostic:

- **green**: external libnx EGL display -> Dawn OpenGLES adapter -> WebGPU device
  -> WebGPU texture succeeded;
- **red**: one of those stages failed.

Dawn itself requires `EGL_EXT_create_context_robustness` and either
`EGL_KHR_fence_sync` or `EGL_KHR_reusable_sync` for this adapter path. The
normal probe prints whether the Switch Mesa/Nouveau EGL stack exposes them.

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
