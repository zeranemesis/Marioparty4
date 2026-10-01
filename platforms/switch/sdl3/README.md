# SDL3 for the Switch (libnx)

Aurora (and through it PartyBoard) talks to the platform through SDL3, but
devkitPro's packages (as of the CI image) only have SDL2 for the Switch, and
SDL's official Switch port is under NDA. This directory builds Aurora's SDL3 release (`release-3.4.10`,
`AURORA_SDL3_REF`) with a homebrew libnx backend added by patches:

| Patch | What it does |
|-------|--------------|
| `0001-libnx-backend.patch` | The backend: a `switch` video driver (one 1280x720 fullscreen window on the default `NWindow`, EGL + OpenGL ES through switch-mesa), AUDOUT audio (48 kHz stereo S16) and HID joysticks with a full gamepad mapping, plus SDL's POSIX threads, timers, filesystem and storage. |
| `0002-egl-context-attributes.patch` | The EGL context honours `SDL_GL_SetAttribute` (OpenGL ES 3 when asked) instead of always ES 2. |
| `0003-audio-keep-buffers-queued.patch` | PartyBoard's fix: keep four AUDOUT buffers in flight. The original driver queued one buffer and waited for it to finish before mixing the next, which leaves the hardware idle between buffers (crackles). |
| `0004-joystick-local-players.patch` | PartyBoard's joystick driver, for local multiplayer: one gamepad per player (Npad No1 to No4, the console's attached Joy-Cons being player 1), added and removed as controllers connect, with the Npad's number as player index. Its mapping is PartyBoard's GameCube layout (`../source/switch_pad.cpp`, the launcher's controller screen): face buttons by position (Switch B is GameCube A), ZR is Z, L or ZL is L, R is R. Single Joy-Cons are held sideways (stick and face buttons turned with them, SL/SR as L/R); GameCube controllers on the USB adapter keep their own labels and analog triggers, and rumble with their motor; the others have HD rumble. The original driver exposed one gamepad, player 1 only. |

`SwitchSDL3.cmake` fetches SDL with git, applies the patches and builds it as
an ExternalProject with devkitPro's own `Switch.cmake` toolchain (the
backend keys on its `NintendoSwitch` system name), then exposes the result as
`SDL3::SDL3-static`, the target name Aurora looks for.

## Probe

```sh
cmake --preset switch-libnx-sdl3-probe
cmake --build build/switch-libnx-sdl3-probe --target partyboard_switch_sdl3_nro
```

`partyboard-switch-sdl3-probe.nro` uses SDL3 the way Aurora does: window,
gamepads (names, types, buttons, sticks), events, base and pref paths, file
I/O through `SDL_IOStream`, an audio stream (hold **A** for a 440 Hz tone),
and the 2D renderer to show it all. **+** quits.

## What the engine will need on top

Aurora renders through Dawn, not through SDL's GL context: on the Switch it
will create its WebGPU surface on the same default `NWindow` with
`dawn::native::opengl::CreateSurfaceFromEGLNativeWindow` (see the Dawn probe)
and must not create an SDL GL context or renderer at the same time.

## Origin and licence

`0001` and `0002` come from
[saekaze/th07-switch](https://github.com/saekaze/th07-switch)
(`platform/switch/`, CC0), whose libnx backend is
[neomody77/sdl3-switch](https://github.com/neomody77/sdl3-switch) (zlib, like
SDL; its author notes the backend code was AI-generated). Paths were
rewritten from `external/SDL3/` to SDL's root; the content is unchanged.
`0003` and `0004` are PartyBoard's; `0004`'s sideways Joy-Con layout follows
the joystick driver of devkitPro's own SDL3 port (the `switch-sdl-3.4` branch
of [devkitPro/SDL](https://github.com/devkitPro/SDL), zlib), SDL 3.4.0 with
a full libnx backend, which may replace these patches once devkitPro ships it.
