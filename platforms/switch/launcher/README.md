# PartyBoard GameCube launcher (Nintendo Switch)

A homebrew front end for the Switch port, modelled on the GameCube app of
Nintendo Switch Online on Switch 2: a shelf of GameCube cases, a boot
animation before the game, and option and controller screens. It runs as its
own NRO (`partyboard-launcher.nro`) and chain-loads the engine NRO through
the homebrew loader.

CI uploads the reference screenshots as the `partyboard-switch-launcher-screens`
artifact (see [Previewing without a console](#previewing-without-a-console)).

## What it does

- **Game shelf.** It scans the SD card for GameCube images (`.iso`, `.gcm`,
  `.ciso`, `.gcz`, `.rvz`, `.wia`, one sub-folder level allowed) and reads
  each disc's boot header and `opening.bnr`: the 96x32 banner, title, maker
  and description, in French on PAL discs when the launcher is in French.
  Catalogue games show their real box art (below); the disc's own banner
  sits next to the title under the shelf. Unknown discs get a generated case
  with their banner. RVZ/WIA only expose the header, so they have no banner.
  A PNG in `partyboard/covers/<GAMEID>.png` replaces any case.
- **Catalogue.** As in CubeShelf, the games are data: `assets/catalog.json`
  lists each game's accepted discs (a six-character id for every revision, or
  `GMPE01_00` for one), its year, genre, players, description, box art and
  the Switch NRO that runs it. It ships Mario Party 4 (PartyBoard, NTSC-U and
  PAL; NTSC-J recognised but not playable yet, as in
  `src/port/iso_validate.cpp`), plus Soulcalibur II (Ring Out) and Super Mario
  Strikers (Strikers), which CubeShelf runs on PC and which show here as "not
  on Switch yet". `sdmc:/config/partyboard/catalog.json` can add entries or
  replace one by id, so a future Switch runtime needs no new launcher build.
- **Box art.** Catalogue games use CubeShelf's European (PAL) front and
  spine scans, whatever the disc's region, drawn as a case seen slightly
  from the side; the info panel below shows the game
  sheet (year, genre, players, runtime) and the disc's real banner.
- **Mods, CubeShelf's way.** Copy CubeShelf's `Mods` folder
  (`%LOCALAPPDATA%\CubeShelf\Mods`) to `sdmc:/cubeshelf/Mods`. **R** opens
  the selected game's mods: switch them on and off with **A**, change the load
  order with **L**/**R**. The launcher re-roots the Windows paths stored in
  `installed.json`, saves the choices back in CubeShelf's format (original
  paths kept, so the folder can return to the PC) and writes the
  `active-mods.txt` PartyBoard reads, passed as `--mod-list`. Mods switched
  off from inside the game (`player-disabled.json`) and folders missing from
  the card are shown as such.
- **Boot animation.** Played between **Play** and the engine. Hold **ZR**
  while it runs for the party-coloured variant and its alternate chime. **A**,
  **B** or **+** skips it.
- **Options.** Startup animation, aspect ratio (4:3, stretched 16:9,
  widescreen 16:9) and language (automatic/French/English). Saved to
  `config/partyboard/launcher.ini`; aspect and language are passed to the
  engine, which applies them for the session without touching its own config.
- **Controllers.** Shows the GameCube controller with the button each Switch
  control produces, which players are connected, and opens the system
  "Change grip/order" applet with **Y**.
- **Switch details.** Clock and battery in the header, player lamps in the
  footer, the console's shared system font, 1080p rendering when docked,
  procedurally synthesised menu sounds.

## Artwork

Two sources, both packed into the NRO's romfs at build time:

- PartyBoard's own `res/`, the files the PC pre-launch screen loads:
  `logo.png`, `icon.png` (the star), `prelaunch-bg.png` (purple stripes with
  the Mario Party 4 cast), the N64 Party face for headings and FOT-NewRodin
  for titles.
- `assets/`, taken from [CubeShelf](https://github.com/zeranemesis/CubeShelf-Launcher):
  the catalogue, the European front and spine box art of each catalogue game
  (scaled to 720 px high) and the GameCube logo.

Body text uses the console's shared system font. The boot animation and every
sound are generated in code.

## Controls

| Screen | Controls |
| --- | --- |
| Shelf | Left/Right or left stick: choose · **A** play · **X** options · **Y** controllers · **R** mods · **-** rescan · **+** quit |
| Mods | Up/Down: choose · **A** on/off · **L**/**R** load order · **B** back |
| Options | Up/Down: choose · Left/Right or **A**: change · **B** back |
| Controllers | Up/Down: highlight a control · **Y** change grip/order · **B** back |
| Boot animation | Hold **ZR**: surprise · **A**/**B**/**+**: skip |

## SD card layout

```text
sdmc:/
├── switch/
│   ├── partyboard-launcher.nro          this launcher
│   └── partyboard/
│       ├── partyboard.nro               the engine (also accepted: switch/partyboard.nro)
│       └── games/                       alternative games folder
├── cubeshelf/Mods/GMPE01_00/            CubeShelf's Mods folder, copied from the PC
├── partyboard/
│   ├── games/                           disc images
│   │   ├── Mario Party 4 (USA).iso
│   │   └── Mario Party 4 [GMPP01]/game.ciso
│   └── covers/GMPE01.png                optional box art
└── config/partyboard/
    ├── launcher.ini                     launcher options
    └── catalog.json                     optional catalogue additions
```

`launcher.ini` also accepts `engine=<path>` to point at another engine NRO.

## Engine contract

The launcher starts the engine with `envSetNextLoad`, so it needs a loader
that supports it (hbmenu/hbloader). The arguments are:

```text
sdmc:/switch/partyboard/partyboard.nro
  --disc-image=<path>          same meaning as PARTYBOARD_DISC_IMAGE on desktop
  --aspect=4:3|stretch|wide    locked 4:3, stretched, adaptive widescreen
  --lang=fr|en                 game language on PAL discs
  --mod-list=<active-mods.txt> CubeShelf mod roots, same as PARTYBOARD_MOD_LIST on desktop
  --launcher=<launcher NRO>    so the engine can return to the shelf on exit
```

The engine parses these in `src/port/launch_args.cpp` (applied at the config
Override layer, never saved); the launcher's unit tests link that parser to
check both ends agree. The full engine does not run on Switch yet (see
`../README.md`). Until it does, choosing a game shows **PartyBoard engine not
found** with the path it expects; the options panel also shows whether the
engine is installed.

## Building

The NRO builds with the existing Switch preset, next to the bring-up probe:

```sh
cmake --preset switch-libnx-bootstrap
cmake --build build/switch-libnx-bootstrap --target partyboard_switch_launcher_nro
```

Output: `build/switch-libnx-bootstrap/platforms/switch/launcher/partyboard-launcher.nro`
(about 15 MB, most of it the romfs artwork and fonts). It links FreeType,
HarfBuzz and libpng from devkitPro's portlibs (`switch-freetype`,
`switch-libpng`, already in the `devkitpro/devkita64` image).

## Previewing without a console

Everything except `switch/switch_main.cpp` is portable C++ over OpenGL ES 3.0.
On a Linux host the same `App` and `Renderer` run on a headless Mesa context:

```sh
sudo apt-get install libegl-dev libgles-dev libegl-mesa0 libgl1-mesa-dri \
  libfreetype-dev libpng-dev zlib1g-dev
cmake -S platforms/switch/launcher -B build/launcher-host
cmake --build build/launcher-host
ctest --test-dir build/launcher-host                              # unit tests
platforms/switch/launcher/tools/render-previews.sh build/launcher-host build/launcher-screens
```

`render-previews.sh` builds a throwaway SD card of synthetic discs (real
headers, FSTs and banners, no game data) and drives the UI with scripted
input at a fixed 60 Hz clock. Run `partyboard_launcher_preview` directly for
other scenarios; its script commands are listed at the top of
`host/preview_main.cpp`. The preview reads the artwork and fonts straight
from `res/`, with Inter standing in for the console's system font.
`--icon icon.png` re-renders the homebrew menu icon (`icon.jpg` is that render
converted to JPEG).

## Source map

| Path | Role |
| --- | --- |
| `src/app.*` | screens, input, launch flow |
| `src/boot.*` | boot animation timeline (pure function of time) |
| `src/render.*`, `src/font.*` | batched SDF 2D renderer, lit cubes, FreeType atlas |
| `src/disc.*`, `src/library.*` | disc containers, banners, game shelf |
| `src/catalog.*`, `assets/catalog.json` | game catalogue (after CubeShelf's games.json) |
| `src/mods.*` | CubeShelf mod folders, active-mods.txt |
| `src/json.*` | JSON reader/writer for the two above |
| `src/settings.*`, `src/paths.hpp` | options file, engine arguments, SD layout |
| `src/sound.*` | synthesised sounds and the audio mixer |
| `src/i18n.*` | French and English strings |
| `switch/switch_main.cpp` | libnx platform: EGL, HID, audout, pl font, psm, applets |
| `host/` | headless preview and synthetic disc generator |
| `tests/` | unit tests |

## Status

The NRO is built in the devkitA64 container and CI; the UI, banners and boot
animation are verified through the host preview. It has not been run on
Switch hardware yet, so the libnx-specific parts (audio output, controller
applet, docked resolution) still need a console test.
