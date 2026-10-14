# nodlite

nod's C API (`include/nod.h`, from [nod](https://github.com/encounter/nod)
v2.0.0-alpha.10, the version Aurora pins) for GameCube discs, written in
C++ with no Rust toolchain. It exists for the Switch build: devkitA64 has no
Rust target for libnx, and Aurora's DVD layer
(`extern/aurora/lib/dolphin/dvd`), the disc check in
`src/port/iso_validate.cpp` and RetroAchievements hashing all talk to nod
through this header. With nodlite they build for the Switch unchanged.

The launcher (`../launcher`) already reads its game shelf through it, so
compressed images show their banner too.

## What it reads

| Container | Notes |
|-----------|-------|
| ISO / GCM | raw image |
| CISO | sparse blocks (Wii Backup Manager); missing blocks read as zeroes |
| GCZ | Dolphin's zlib blocks; Adler-32 checked per block |
| WIA | uncompressed, bzip2 or Zstandard groups |
| RVZ | the same, plus junk rebuilt from its seeds with nod's Lagged Fibonacci generator |

The container comes from the file's magic, never its extension. Images can
be opened from a path (`nod_disc_open`) or from read callbacks
(`nod_disc_open_stream`, which Aurora uses for its SDL streams). Every
function in `nod.h` is implemented; reads are thread-safe, and each handle
keeps its own position and `nod_buf_read` window.

Not supported, with a clear `nod_error_message()`:

- **Wii discs**: refused when opened (nod decrypts them; PartyBoard has no use for that).
- **WIA "purge" and LZMA/LZMA2**: Dolphin only writes those when asked; its
  defaults (and nod's) are RVZ with Zstandard.
- **NFS, WBFS, TGC**: Wii and dev formats.

## Same behaviour as nod

The test suite (`tests/nodlite_tests.cpp`) also builds against the real nod
library, and every check runs on both:

```sh
tools/check-against-nod.sh            # clones nod at the pinned tag, needs cargo
tools/check-against-nod.sh ~/src/nod  # or an existing checkout
```

That pins down the parts callers can observe: the bytes of each disc, file
and metadata blob; FST iteration and case-insensitive lookup; seeks (a file
handle is clamped to `[0, size]`, a disc handle refuses only positions before
0); and the result codes (a directory index or the `UPDATE` partition kind is
`NOD_RESULT_ERR_FORMAT`, an index past the FST `NOD_RESULT_ERR_NOT_FOUND`; a
GameCube disc lists no partitions and is opened as partition 0, kind `DATA`).

A few checks are nodlite-only and skipped against nod:

- nodlite refuses Wii discs up front;
- it accepts GCZ with 16 KiB blocks (older Dolphin builds), which nod
  rejects as not a multiple of a sector;
- it does not verify WIA/RVZ header hashes, and reads zeroes up to the end of
  a GameCube disc in a CISO whose block map stops short.

It is also lazier than nod: opening the data partition reads only the FST;
the boot header, apploader and DOL blobs are loaded on the first
`nod_partition_meta`, so the launcher can list a library without decoding
every game's DOL.

## Tests

```sh
cmake -S platforms/switch/nodlite -B build/nodlite
cmake --build build/nodlite
ctest --test-dir build/nodlite
```

The test disc (`tests/test_disc.hpp`) is a small but complete GameCube image:
boot header, bi2, apploader, DOL, a nested FST, files with noise, zero runs
and repeats, and junk padding wherever mastering would leave it. The tests
wrap it as ISO, CISO, GCZ and WIA themselves; `tests/fixtures/*.rvz` were
made from it by nod's own `nodtool` (`tools/make-fixtures.sh`). nodtool only
stores junk as a seed when it matches its generator, so the small
`test-none.rvz` also shows that nodlite's generator matches nod's.

## Licence

`include/nod.h` is nod's header and `src/lfg.*` ports nod's `util/lfg.rs`;
nod is MIT or Apache-2.0, and `LICENSE-MIT` is its MIT licence. The WIA/RVZ
layout follows Dolphin's `docs/WiaAndRvz.md` (CC0-1.0).
