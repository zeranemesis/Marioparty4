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
