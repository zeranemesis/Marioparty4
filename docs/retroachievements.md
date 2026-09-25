# RetroAchievements

Party Board unlocks the Mario Party 4 achievement set
([game 25450](https://retroachievements.org/game/25450)) through the official
[rcheevos](https://github.com/RetroAchievements/rcheevos) client. Log in from
**Settings > RetroAchievements**; the set appears under **F1 > Achievements**.

## How it works

| Piece | Where |
|---|---|
| rcheevos v12.5.0, fetched and checksummed by CMake | `cmake/rcheevos.cmake` |
| Client: login, game identification, per-tick evaluation, notifications | `src/port/retroachievements.cpp` |
| GameCube RAM addresses translated to the port's variables | `src/port/retroachievements_memory.cpp` |
| HTTPS through each platform's own stack | `src/port/http.cpp`, `src/port/http_apple.m` |
| Login fields and status | Settings > RetroAchievements (`src/port/ui/settings.cpp`) |
| Achievement list with progress | F1 > Achievements (`src/port/ui/achievements.cpp`) |
| Badges: downloaded once, kept on disk, shown in the list and the unlock notification | `src/port/retroachievements_badges.cpp` |

**Identification.** The disc is hashed with rcheevos' own GameCube method (disc
header, apploader and `main.dol` segments), read through nod, so an RVZ image
hashes exactly like the ISO it came from.

**Memory.** The set reads console addresses, but the port does not run in
console memory. For each variable the set uses, the translator rebuilds the
bytes the console held at that address, big-endian: `GWPlayerCfg`, `GWPlayer`,
`GWSystem`, `GWGameStat`, the overlay history, `ReadDataStat[0].dirId` and the
leading fields of `Hu3DData`. Under `TARGET_PC` these structures already keep
the console's offsets and bitfield values, so a copy plus the existing
save-data byte swaps gives the console image; `static_assert`s pin their sizes
to `config/GMPE01_00/symbols.txt`. An address that is not translated fails to
read, and rcheevos disables whatever uses it rather than evaluating made-up
data.

State of the set: 58/58 achievements and 15 leaderboards active. Two addresses
stay unmapped, `0x80425523` and `0x80425527`: beach volleyball scores in the
minigame's dynamically loaded memory, used only in the rich presence text.

**Timing.** Achievements are evaluated once per 60 Hz logic tick, the rate the
console evaluated them at, whatever the display refresh rate. They are
suspended during online sessions, whose rollback replays ticks.

## Limits

- **Softcore only.** The server does not recognise this client and turns
  hardcore unlocks into softcore ones; each session shows its "Unknown
  Emulator" warning (0 points). Hardcore needs the client to be validated by
  the RetroAchievements team (RAdmin).
- **USA disc only.** RetroAchievements' GameCube hash library links a single
  disc to game 25450 (`1122a3eed3b4219dbaf6f711d5695532`, the USA Rev 1 release;
  the translated addresses are identical in Rev 0 and Rev 1) and
  has no separate PAL or Japanese entry. Other discs report "This disc has no
  achievement set". The translator serves the USA layout whatever the disc, so
  a disc linked later would work without changes.
- **Platforms.** Only the Windows backend (WinHTTP) has been compiled so far.
  macOS/iOS/tvOS (NSURLSession), Android (HttpURLConnection over JNI) and Linux
  (libcurl, optional) are written but unbuilt.

## Privacy

The password is handed to rcheevos and never stored; only the session token the
server returns is kept in the settings, as every RetroAchievements client does.

Badges are plain GETs to `media.retroachievements.org`, the address rcheevos
gives for each achievement, and carry no credentials. Each is downloaded once,
when a set loads, into `<config>/retroachievements/badges/<name>.png`; deleting
that folder only makes the next session fetch them again. Only the unlocked
picture is downloaded: the locked one is the same picture greyed on the spot,
as the site greys its own. Offline, the list shows an empty square where each
missing badge goes, and gives up after three requests fail in a row.

## Debugging

| Variable | Effect |
|---|---|
| `PARTYBOARD_RA_SPECTATOR=1` | Evaluates the set and shows notifications, submits nothing |
| `PARTYBOARD_RA_DUMP_SET=<file>` | Writes the set definitions the server sent (no credentials) |

The log lists the rich presence whenever it changes. It is computed from the
same translated addresses, so "In the title screen", the board or the minigame
name are a quick check that the translation is right.
