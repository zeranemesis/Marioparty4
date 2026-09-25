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
