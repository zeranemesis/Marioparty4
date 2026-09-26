# Android Shell

This directory contains a minimal SDLActivity-based Android app wrapper for Party Board.

## Prerequisites

- Android SDK installed (`ANDROID_HOME`)
- Android NDK version used by CMake presets (`ANDROID_NDK_VERSION`)
- JDK 17+

Example:

```bash
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_NDK_VERSION="29.0.14206865"
export JAVA_HOME="/usr/lib/jvm/java-17-openjdk"
```

## Build Native Libraries

```bash
cmake --preset android-arm64
cmake --build --preset android-arm64

cmake --preset android-x86_64
cmake --build --preset android-x86_64
```

These builds produce:

- `build/android-arm64/Binaries/libmain.so`
- `build/android-x86_64/Binaries/libmain.so`

## Stage Libraries Into APK Project

```bash
./android/scripts/stage-jni-libs.sh
```

This copies:

- `libmain.so` -> `android/app/src/main/jniLibs/arm64-v8a/`
- `libmain.so` -> `android/app/src/main/jniLibs/x86_64/`

## Refresh SDL Java Shim (Optional)

If you update SDL and want to refresh the embedded Java shim files:

```bash
./android/scripts/sync-sdl-java.sh
```

## Build APK

```bash
cd android
./gradlew :app:assembleDebug
```

Output APK:

- `android/app/build/outputs/apk/debug/app-debug.apk`

## Signing And Updates

Android only installs an update signed with the same key as the installed app;
uninstalling first deletes the saves, the settings and the CubeShelf profile.
So every build uses one key:

- local debug builds and CI builds: `partyboard-test.keystore` (password
  `android`, alias `androiddebugkey`). It is public, like Android's own debug
  key, so it is only for testing;
- CI builds of a repository that sets the `ANDROID_KEYSTORE_BASE64`,
  `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD`
  secrets: that key instead (`app-<abi>-signed.apk`). Switching from the test
  key to it needs one last uninstall.

CI sets `versionCode` to its run number, so a newer build always installs over
an older one.

## Updates From GitHub

Each change that reaches `audio-local` (or a `v*` tag, or a hand-run of the
Build workflow) publishes its APK to the GitHub release
`partyboard-android-latest` (`scripts/publish-update.sh`):

- `PartyBoard-android-arm64-v8a.apk`: the newest build under a name that never
  changes, to install the app the first time;
- `PartyBoard-android-arm64-v8a-<build>.apk`: the five newest builds;
- `android-update.json`: the manifest the app reads: build number, APK URL,
  SHA-256 and the commits since the previous published build.

When Party Board starts (Settings > Interface > Check for Updates, on by
default) it reads the manifest. A newer build shows under the version on the
home screen; pressing Update lists the changes and, once confirmed, opens the
APK's download in the browser. Opening the downloaded file installs it over the
app, keeping the saves and settings. The app asks for no install permission of
its own: Play Protect blocks unknown apps that do. Without network the check
says nothing and the game plays offline as usual.

## Meta Quest 3 / 2

The same APK (`arm64-v8a`) runs on Meta Quest 3, 3S, 2 and Pro. On a headset
the game stands on the player's table in mixed reality (the room stays
visible around it), played with the Touch controllers; on a phone nothing
changes.

- `quest/QuestVr.java` and `src/main/cpp/` (`libpartyboard_quest.so`, built by
  Gradle with the Khronos OpenXR loader from Maven): an OpenXR session whose
  layers are the room (`XR_FB_passthrough`), the game's screen (a quad fed by
  an `XR_KHR_android_surface_swapchain` surface) and, while placing, a help
  panel. SDL gets the game's surface in place of the SurfaceView's
  (`SDLSurface.setExternalSurface`), so Aurora renders exactly as on a phone.
- `table_anchor.cpp`: the place on the table is a spatial anchor
  (`XR_FB_spatial_entity`), so it stays on the real table from one session to
  the next; `files/quest_table.txt` keeps it with the size, resolution and
  room visibility.
- `org/libsdl/app/QuestControllers.java`: the controllers as one SDL gamepad
  ("Meta Quest Touch"), and the game's rumble on both controllers.
- The manifest's `com.oculus.intent.category.VR` starts the game immersive on
  Horizon OS; phones ignore it.

### Placement

The experimental 3D diorama draws camera 0's perspective geometry for each
eye at the table anchor. The original screen remains behind it for menus,
text and scenes using other cameras. This is not yet a complete conversion
of all boards and minigames. Table placement is manual; no table detection
or real-world occlusion is implemented.

Minigames m401–m463 use their initial camera framing as a provisional scale
and center, frozen until the overlay changes. Board maps retain their world
coordinates. This fallback still needs visual calibration per minigame;
moving cameras do not automatically drag the arena around the real room.

The first start opens placement mode; afterwards, click the right thumbstick.
The screen turns blue and the game waits meanwhile.

| Control | Effect |
| --- | --- |
| Right trigger | The screen stands where the controller's tip touches the table, facing you |
| Right grip (hold) | Carry it with the controller |
| Right thumbstick ↕ / ↔ | Size / turn |
| Left thumbstick ↕ / ↔ | Height / diorama scale |
| Left thumbstick click | Enable or disable the 3D diorama (during placement) |
| X | Room visible (mixed reality) or not |
| Y | Resolution: 1080p, 1440p, 4K (3840×2160), live |
| A, B, menu or right thumbstick click | Done |

### Picture and performance

- The screen's resolution is the game's: with Internal Resolution on Auto,
  4K renders the game in 4K. The compositor's supersampling filters 1440p and
  4K down to the headset's pixels; 1080p is sharpened instead. The menus keep
  their size at every resolution.
- The session asks for the CPU and GPU's sustained high levels
  (`XR_EXT_performance_settings`), and the Frame Rate setting offers the
  headset's rates (72 to 120 Hz); 60 FPS runs on 120 Hz so every frame shows
  evenly.
- The room's cameras are paused while it is hidden.

### Game controls

| Quest | GameCube |
| --- | --- |
| A / B (right), X / Y (left) | A / B / X / Y |
| Left thumbstick | Control stick |
| Left grip + left thumbstick | D-pad |
| Right thumbstick | C stick |
| Left / right trigger | L / R (analog) |
| Right grip | Z |
| Left menu button (≡) | Start |
| Left thumbstick click | Party Board menu |

A Bluetooth gamepad paired with the headset also works.

Install (developer mode on, headset plugged in):

```bash
adb install -r app/build/outputs/apk/quest/debug/app-quest-arm64-v8a-debug.apk
```

The game is then under Library > Unknown Sources. Choosing the disc image
opens Android's file picker as a window; the game comes back once it is chosen.
Logs: `adb logcat -s PartyBoardQuest SDL/APP`.
`Stereo bridge: imported`, `first perspective pass` and
`first eye image copied to OpenXR` identify the game-to-headset rendering path.

On Windows, `tools/test_quest_stereo.ps1 -Serial <adb-serial>` compiles and runs
the native image-lease regression test on an ARM64 headset or x86_64 emulator.
It checks empty cameras, exhausted rings, GPU ownership and stale callbacks;
it does not validate the visual result or headset performance.

## Launch With Runtime Args (adb)

You can pass command-line args through the activity intent:

# TODO
```bash
adb shell am start -n com.mariopartyrd.partyboard/.PartyBoardActivity \
  --es partyboard_args "'/sdcard/Download/GMPE01_00.iso'"
```

Supported extras:

- `partyboard_args`: single shell-like argument string
- `partyboard_argv`: string-array argv
- `partyboard_disc`: compatibility shortcut (single ISO path)

## Online Play

**Play Online** (menu bar or prelaunch) opens the phone's lobby, the
counterpart of `PartyBoardOnline.exe` (`app/src/main/java/.../online/`, a Java
port of `tools/online`). Like on Windows the lobby is its own process
(`:online`) and restarts the game with the `--netplay-*` arguments, because
the game reads them once at start:

- `LobbyActivity`: nickname, disc (full SHA-256, Mario Party 4 USA rev 1),
  2 to 4 players, create/join, invitation copy/share/paste, players with disc
  agreement and ping, start (host only), diagnostic export. French or English,
  following the game's language. Sharing a text that contains a `PB4.`
  invitation to Party Board opens the lobby on it and joins once the disc is
  verified; so does a CubeShelf friend's invitation.
- `OnlineService`: a foreground service holding the session, the TLS control
  channels and the authenticated UDP relay while the game runs.
- `Session`/`Lobby`/`Bridge`/`MeshRelay`/`Gateway`/`Wire`/`Invitation`: the
  same protocol as the PC companion (PB4 invitations, PBAUTO3 handshake,
  PCP → NAT-PMP → UPnP temporary mappings, HMAC-sealed game datagrams).
- The start barrier: Windows named events become a loopback TCP link
  (`GameLink`, `PARTYBOARD_ONLINE_BARRIER`, `src/port/portmain.cpp`), which
  also tells the lobby when the game has exited.

Differences from the PC:

- Hosting needs Wi-Fi or Ethernet (a mobile operator's shared NAT cannot be
  opened); joining works on mobile data.
- If the box refuses every automatic mapping, the lobby is still created for
  the same Wi-Fi: its invitation then carries only the host's local address.
- The build hash is the installed APK's, prefixed `partyboard-android`: phones
  play with phones running the same build, never with a PC (another
  architecture would not stay in lockstep).

Tests: `./gradlew :app:testDebugUnitTest` (invitations, datagrams, replay
window, lobby state machine, pinned TLS handshake).

Debug builds accept `--es online_test_public 10.0.2.2` on `LobbyActivity` so
two emulators can play through the host PC's port forwarding.

### Quest validation notes

The experimental spatial renderer currently enables only w01�w06 and m401�m463.
Title/selection/instruction scenes retain the original screen composition.
Once a world image is presented, the classic screen is hidden; original orthographic
sprites/messages are placed on a spatial panel. This is a compatibility bridge,
not a completed MR interface; multicamera games and per-draw clipping still need validation.
Eye images use a 0.5 resolution scale and new placements default to a 1080p screen.

Debug builds accept `--ez partyboard_audio_diagnostics true` on PartyBoardActivity.
This writes `audio_timing_diagnostic.log`, `audio_render_diagnostic.log` and, when
sequenced music runs, `sequence_diagnostic.log` in the private files directory.
Use a fresh ordinary launch to disable this opt-in diagnostic mode.

The board's playable-space bounds determine its initial center, scale and lowest
surface. Later game-camera pans, rotations and zoom-distance changes move the
board relative to that initial view. This behavior follows the game rather than
keeping the diorama fixed. Field-of-view changes are not yet reproduced.

Stereo source images remain leased while an asynchronous GL copy fence is pending.
Older submitted images are reclaimed only after their Aurora fence signals.
`PartyBoardQuest` logs scene transitions and `Stereo perf` windows with source and
presented rates, ring exhaustion, maximum copy call duration and slot ownership.
