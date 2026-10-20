
**Mario Party 4, natively on PC, phone and Meta Quest**, with online play for up
to four players. Party Board is a native port built on the GameCube
decompilation, not an emulator.

This repository does **not** contain any game assets or assembly. You need your
own copy of the game as a disc image.

Current release: **v0.19.0**.

## Getting started

| Platform | How |
|---|---|
| Windows | Install it from **CubeShelf**, or extract `PartyBoard-win-x64.zip` and run `Lancer PartyBoard.cmd` |
| Linux | Run the `.AppImage` |
| macOS (Apple silicon and Intel) | Open the app from the `.zip` |
| Android phone | Install the `phone` APK |
| Meta Quest | Install the `quest` APK; it updates itself afterwards |
| Nintendo Switch (homebrew) | `partyboard.nro` and a GameCube-style launcher build from source; not run on a console yet ([status](platforms/switch/README.md)) |

Everything is on the [releases page](https://github.com/zeranemesis/Marioparty4/releases).
On first launch, choose your disc image. **F1**, the gamepad's **Back/Select**
button or the screen controller's menu button opens the Party Board menu at any
time.

### Discs

| Disc | Region | |
|---|---|---|
| `GMPE01` rev 0 and rev 1 | USA | supported; rev 1 online |
| `GMPP01` | Europe | supported online too, in English, French, German, Spanish or Italian |
| `GMPJ01` | Japan | recognised, not supported yet |

ISO, RVZ, WIA and GCZ images are accepted. The image is checked by hash before
launch.

## The Party Board menu

Press **F1** (or **Back/Select**) in the game:

- **Settings** — everything below.
- **Achievements** — your RetroAchievements set, with progress, and your friends'.
- **Play Online** — opens the online lobby.
- **Reset Game** and **Quit Party Board**.

## Settings

### Prelaunch
- **Disc image** used to start the game.
- **Language** of the game: English, French, German, Spanish or Italian, with a
  European disc. The Party Board menus follow in English or French.
- **Graphics backend**: Auto, Direct3D 12, Direct3D 11, Vulkan, Metal, OpenGL,
  OpenGL ES or WebGPU, depending on the platform.
- **Save file type**: a memory-card image, or a folder of `.gci` saves.

### Video
- Fullscreen, and back to the default window size.
- **Frame rate** from 60 to 240 FPS. The game still runs at 60: higher rates
  smooth the 3D and 2D motion, cameras and fades without speeding up the game or
  delaying input.
- **Adaptive widescreen HUD**: uses the whole width of the window without
  stretching the scene, and moves the HUD towards the edges, clear of a phone's
  notch. Any shape up to 48:9; a screen narrower than 4:3 gets bars above and
  below. Or **lock 4:3** for the original picture.
- **Internal resolution** and **shadow resolution**.
- VSync, FPS counter, pause when the window loses focus.

### Input
- **Controller configuration**: buttons, sticks and triggers for each port, from
  a gamepad, a keyboard or the mouse; dead zones; triggers emulated from buttons,
  with their thresholds; rumble strength, with a test.
- **Background input**: keep playing when the window is not focused.
- **Screen controller**: a GameCube controller drawn on the screen for phones and
  tablets (automatic, always or never), with adjustable opacity.
- **Turbo key**: hold Tab to unlock the frame rate and speed the game up.

### Gameplay and cheats
- **Speedrun mode**.
- **Unlock all minigames** and **unlock Bowser's Gnarly Party**, without touching
  your save.

### RetroAchievements
- Log in to [retroachievements.org](https://retroachievements.org/game/25450)
  and unlock the Mario Party 4 set (softcore). The password is never saved.
- Online sessions do not count towards achievements.

### Interface
- **Check for updates** at startup, or now.
- **Open data folder**: settings, saves, logs.
- **Notifications** to show: achievements, controllers.
- **Skip the Party Board main menu** and **skip the game's boot sequence**, to
  reach the game faster.

## Online play

- **Two to four players**, each with their own copy of the same disc: the USA
  disc revision 1, or the European disc. Every player runs exactly the same game
  data, so a session never mixes the two.
- **With the European disc**, every player plays in the **host's language**,
  whatever their own setting.
- **Two ways to play**:
  - **Local network (LAN)**: the host creates a LAN game and the others pick it
    from **Parties en LAN**, with no code to copy. Nothing goes through the
    Internet.
  - **Online, in rooms**: a **private room** for friends, who join by invitation
    (from CubeShelf's friend list, or a code starting with `PB4`), or a **public
    room**, listed for every player with the same disc and the same release.
    The list shows each room's host, language, disc, players and mods, and the
    games under way. Public rooms are for two players.
- Only the host starts the game.
- The lobby shows who is in, their disc and their ping; **every disc is checked
  in full** before joining.
- **Mods**: the host chooses the lobby's mods; a guest whose list differs aligns
  on the host's in one click.
- **Connection**: the router is opened automatically (UPnP, PCP or NAT-PMP).
  When it cannot be (CGNAT, 4G, UPnP off), a two-player room goes through the
  **online relay** instead (Cloudflare): the lobby is encrypted end to end
  through it, and the game's packets are authenticated. A public room always
  goes through the relay, so the list never says where a host is.
- **Fair play**: both games run the same simulation in lockstep and compare their
  state every tick. If they ever diverge, the session stops rather than letting
  the two games drift apart.

Online play runs from **Windows** (`Play Online`, or `Jouer en ligne.cmd`) and
from **Android phones**, and a phone can play with a PC. All players need the same
release. Online sessions run at 60 Hz in 4:3, with a fresh profile: your own
memory-card saves are not loaded.

## CubeShelf

When the game is started from CubeShelf:

- **Mods** are applied on top of the disc image, never rewriting it
  ([how](docs/CUBESHELF_MODS.md)).
- **One RetroAchievements login** for every game CubeShelf launches.
- **Friends**: see which board, turn or minigame they are on, or whether they
  are away; see their achievement progress, and get a notification when they
  unlock one.
- **Invitations** to an online room are shared through CubeShelf, with the
  room's mods: a friend missing one installs it from GameBanana in the same click.
- **Online tab**: public rooms waiting for a player and games under way, and
  the room itself, played inside CubeShelf -- the lobby runs without a window
  of its own: every player's disc, mods and ping, Start for the host, the
  room's mods picked among those installed or added from GameBanana, Use the
  host's mods or Install the missing ones for a guest. Joining installs the
  mods a room needs for that game only; your own mod choice is not changed.
  LAN games stay in the game's own lobby.
- **In the game (F1, Friends tab)**: your room and the public rooms, joinable
  without leaving for CubeShelf when your mods already match.

## Meta Quest

- The game in **stereo 3D**, in **mixed reality**: the board stands on your real
  table as a model, found by the room scan, and stays where you placed it.
- **Quality presets** on the Y button (economy, balanced, sharp), and a 3D
  resolution that adapts to keep the headset's frame rate.

## Android phones

- **Touch controls**: an on-screen GameCube controller.
- Menus sized for phones, and the same online lobby as on PC.

## Building

See [building.md](building.md). Every push is built for Windows, Linux, macOS and
Android; every `v*` tag publishes a release for all of them and refreshes
CubeShelf.

## Testing

```
partyboard.exe --netplay-self-test
tools\run_all_tests.ps1 -DiscPath <disc image> -BinaryDirectory <build folder>
```

The game carries its own self-tests (`--*-self-test`), and `tools/run_all_tests.ps1`
runs every test script, including real two-instance online sessions, over a
simulated degraded network if wanted. Results always say how they were obtained:
**SCRIPTED** (generated input), **HUMAN** (a game played by a person) or
**REAL-NETWORK** (two machines). Known defects are recorded in the
[netplay defect register](docs/netplay_defect_register.md).

## Common problems

### An online session stops with "DESYNC"

The two games diverged and the session stopped on purpose. Make sure every player
runs the same release with the same disc, then open an issue with the reports.

### RenderDoc not working

RenderDoc conflicts with ASan. To turn ASan off, delete the two lines in
`CMakeLists.txt` that enable it for the DOL and the RELs:
`set_source_files_properties(..., -fsanitize=address)`.

# Credits

Special thanks to the GC/Wii decompilation community, the
[Aurora](https://github.com/encounter/aurora) developers, the Dusk developers, all
[contributors](https://github.com/mariopartyrd/partyboard/graphs/contributors),
[ImWhoreHay](https://x.com/ImWhoreHay) for the font, and justcamtro for designing
the assets.

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
