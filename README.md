<div align="center">
  <img src="res/logo.png" alt="Logo" width="800">

  <p align="center">
    <a href="https://discord.gg/T4faGveujK">Discord</a>
  </p>
</div>

# Overview

[Build Status]: https://github.com/mariopartyrd/partyboard/actions/workflows/build.yml/badge.svg
[actions]: https://github.com/mariopartyrd/partyboard/actions/workflows/build.yml
[Discord Badge]: https://img.shields.io/discord/994839212618690590?color=%237289DA&logo=discord&logoColor=%23FFFFFF
[discord]: https://discord.gg/T4faGveujK

A work-in-progress Windows/Linux/macOS/Android/iOS port of Mario Party 4.

This repository does **not** contain any game assets or assembly whatsoever. An existing copy of the game is required.

## Online rollback build

This branch includes the Windows online build used for two-player testing:

- rollback netcode with deterministic input and coordinated state checkpoints;
- host-created lobbies with short join codes, host-only start permission and
  automatic peer status/ping display;
- full disc-image hash and game-version verification before joining;
- F1 opens the Party Board menu at any time, including a **Play Online** entry
  that starts `PartyBoardOnline.exe`;
- language selection between **English** and **French**, covering the main menu,
  settings, pre-launch verification, controller setup, achievements, dialogs and
  notifications;
- automatic online 4:3 viewport, native internal resolution and 60 Hz simulation
  to keep HUD and effects deterministic;
- GitHub update management in the online launcher: version check, signed SHA-256
  download and automatic replacement on restart;
- rollback-safe animation, texture, shadow, audio, wipe and scene state, with
  recovery from temporary packet loss.

### Technologies

The port is written in C and C++20 and built with CMake and Ninja/MSBuild. The
desktop frontend uses SDL3 for windowing, input, processes and networking,
RmlUi for the navigable interface, Aurora for the platform/rendering layer, and
Direct3D 12, Vulkan or WebGPU backends where available. The online companion
uses authenticated TLS control messages and UDP transport for low-latency game
input. Native self-tests cover rollback snapshots, replay, packet loss,
authentication and lobby rules.

### Windows quick start

Download the release ZIP, extract the complete folder on both PCs, and launch
`Jouer en ligne.cmd`. The host creates the lobby and shares the short code; the
client joins it. In the game, press F1 to reopen the menu. Select **Settings →
Language → French** or **English**, then restart if Party Board requests it.

The online launcher checks the GitHub manifest in the background at startup. Use
**Vérifier les mises à jour** to check again manually. If a newer version is
available, the launcher verifies its SHA-256, installs it after closing, and
starts the updated launcher automatically.

Version Completion:

- `GMPE01_00`: Rev 0 (USA)
- `GMPE01_01`: Rev 1 (USA)

### 1. Download [Party Board](https://github.com/mariopartyrd/partyboard/releases)

### 2. Setup the game

- Extract the .zip file
- Launch partyboard or partyboard.exe depending on your platform.

# Building

If you'd like to build Party Board from source, please read the [build instructions](building.md).

## Common problems

### RenderDoc not working

RenderDoc has some conflict with asan. To turn off asan, you should delete the two lines in `CMakeLists.txt` that enable ASAN for the DOL and the RELs: `set_source_files_properties(..., -fsanitize=address)`

# Credits

Special thanks to the GC/Wii decompilation community, the [Aurora](https://github.com/encounter/aurora) developers, the Dusk developers, all [contributors](https://github.com/mariopartyrd/partyboard/graphs/contributors), [ImWhoreHay](https://x.com/ImWhoreHay) for the font, and justcamtro for designing the assets.

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
