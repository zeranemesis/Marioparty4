# SDL3 for the Switch (libnx)

Aurora (and through it PartyBoard) talks to the platform through SDL3, but
devkitPro only ships SDL2 for the Switch and SDL's official Switch port is
under NDA. This directory builds Aurora's SDL3 release (`release-3.4.10`,
`AURORA_SDL3_REF`) with a homebrew libnx backend added by patches:

| Patch | What it does |
|-------|--------------|
| `0001-libnx-backend.patch` | The backend: a `switch` video driver (one 1280x720 fullscreen window on the default `NWindow`, EGL + OpenGL ES through switch-mesa), AUDOUT audio (48 kHz stereo S16) and HID joysticks with a full gamepad mapping, plus SDL's POSIX threads, timers, filesystem and storage. |
| `0002-egl-context-attributes.patch` | The EGL context honours `SDL_GL_SetAttribute` (OpenGL ES 3 when asked) instead of always ES 2. |
| `0003-audio-keep-buffers-queued.patch` | PartyBoard's fix: keep four AUDOUT buffers in flight. The original driver queued one buffer and waited for it to finish before mixing the next, which leaves the hardware idle between buffers (crackles). |

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
