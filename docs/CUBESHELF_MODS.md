# CubeShelf mod overlay

PartyBoard boots from a GameCube disc image, so mods are applied as a *virtual*
overlay on top of that image. Nothing on disc and nothing on the host filesystem
is rewritten: enabling, disabling or reordering mods only costs a relaunch.

## Runtime contract

CubeShelf starts `partyboard.exe` with a single environment variable:

```text
PARTYBOARD_MOD_LIST=<absolute path to active-mods.txt>
```

`active-mods.txt` is UTF-8 text, one absolute mod content root per line, sorted
**highest priority first**. Blank lines and lines starting with `#` are ignored,
and a UTF-8 BOM is tolerated. A relative line is resolved against the directory
holding the list. Roots that no longer exist are skipped with a warning.

```text
C:\Users\me\AppData\Local\CubeShelf\Mods\GMPE01_00\546878\files
C:\Users\me\AppData\Local\CubeShelf\Mods\GMPE01_00\407132\files
```

A content root maps onto the **root of the disc**. Given the line above, the
file `…\546878\files\data\board.bin` replaces `/data/board.bin` on the disc.

When the variable is unset, empty, or names a list with no usable root, the game
boots the untouched disc image.

## How it is applied

`PartyBoard_InitMods()` (`src/port/mods.cpp`) runs in `port_main()` right after
`aurora_dvd_open()` and before any file is read. It walks each content root,
resolves conflicts, and hands the result to Aurora's DVD overlay
(`aurora_dvd_overlay_callbacks` / `aurora_dvd_overlay_files`).

Because the overlay is installed in the FST itself, every disc entry point sees
the modded file: `DVDOpen()`, `DVDConvertPathToEntrynum()` + `DVDFastOpen()`
(the path taken by `HuDataInit()` for `data/*.bin` and by the MSM audio system
for `/sound/*`), `DVDReadPrio()` and `DVDReadAsyncPrio()`.

Rules applied while collecting:

- **Load order** — roots are visited in list order and the first one claiming a
  path keeps it, so the highest priority mod wins a conflict.
- **Case** — path matching is ASCII case-insensitive, like the real FST.
- **New files** — a mod may add a file the disc does not contain; Aurora assigns
  it a fresh entry number and creates the parent directories.
- **`cubeshelf-mod.json`** — launcher metadata, never exposed to the game.
- **Limits** — files above 4 GiB are skipped, and collection stops at 65 536
  files so a runaway directory tree cannot stall the boot.

## Online play

The overlay is applied in online sessions too, and the game logs a warning when
that happens. Every player must run the same mod list: a mod that changes game
logic on one side only will show up as a netplay desync.

## Testing

```bash
partyboard --mods-self-test
```

Builds a throwaway mod tree in the temporary directory and checks list parsing,
load order, path mapping and the empty-list fallback. It needs no disc image and
runs in CI on every `cubeshelf-windows` build.
