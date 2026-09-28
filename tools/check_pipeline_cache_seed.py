#!/usr/bin/env python3
"""Check the committed pipeline cache seed is usable by the engine.

Aurora drops a draw whose pipeline is still compiling rather than delaying it
(extern/aurora/lib/gx/pipeline.cpp:18), so a player with an empty cache sees
missing geometry for a few seconds on every scene they visit for the first
time. seed_pipeline_cache() prevents that by merging initial_pipeline_cache.db
into the live cache at startup -- but it refuses a seed whose schema version
does not match, and it does so with nothing louder than a Log.warn. That is
exactly the kind of failure that ships unnoticed, which is what this guards.

A missing seed is reported but not fatal: the repository has shipped without
one for its whole history, and blocking CI on that would be a change of policy
rather than a check. A seed that is present but unusable IS fatal, because it
is indistinguishable from a working one at a glance.
"""

import re
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEED = ROOT / "initial_pipeline_cache.db"
PIPELINE_CACHE_CPP = ROOT / "extern/aurora/lib/gfx/pipeline_cache.cpp"


def expected_schema() -> int:
    """Read PipelineCacheSchema from the engine rather than duplicating it here.

    Hardcoding the number would let the two drift apart silently, which is the
    very failure this script exists to catch.
    """
    source = PIPELINE_CACHE_CPP.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"constexpr\s+int\s+PipelineCacheSchema\s*=\s*(\d+)\s*;", source)
    if match is None:
        raise SystemExit(
            f"could not find PipelineCacheSchema in {PIPELINE_CACHE_CPP.relative_to(ROOT)}; "
            "the constant was renamed or moved, so this check needs updating"
        )
    return int(match.group(1))


def main() -> int:
    schema = expected_schema()

    if not SEED.exists():
        print(f"note: no {SEED.name} committed, so players start with an empty pipeline cache")
        print("      and will see missing geometry on each scene's first visit.")
        print("      Produce one with tools/capture_pipeline_cache.ps1 after a play session.")
        return 0

    size = SEED.stat().st_size
    if size < 8192:
        print(f"error: {SEED.name} is {size} bytes, which is an empty database")
        return 1

    try:
        # immutable=1, not just mode=ro: opening a WAL database read-only still
        # creates -wal and -shm sidecars next to it, which would litter the repo
        # (and the CI checkout) with files that look committable. Declaring the
        # file immutable skips that machinery entirely, and is accurate here --
        # nothing writes the seed during a check.
        con = sqlite3.connect(f"file:{SEED}?immutable=1", uri=True)
    except sqlite3.Error as err:
        print(f"error: cannot open {SEED.name}: {err}")
        return 1

    try:
        found = con.execute("SELECT value FROM aurora_schema").fetchall()
        entries = con.execute("SELECT COUNT(*) FROM pipeline_cache").fetchone()[0]
    except sqlite3.Error as err:
        print(f"error: {SEED.name} is not a pipeline cache database: {err}")
        return 1
    finally:
        con.close()

    values = [row[0] for row in found]
    if schema not in values:
        print(f"error: {SEED.name} has schema version(s) {values}, but the engine requires {schema}.")
        print("       seed_pipeline_cache() will reject it with only a warning, shipping a build")
        print("       that behaves as if no seed existed. Recapture it with")
        print("       tools/capture_pipeline_cache.ps1 against the current build.")
        return 1

    if entries == 0:
        print(f"error: {SEED.name} has the right schema but records no pipelines")
        return 1

    print(f"ok: {SEED.name} records {entries} pipelines at schema version {schema}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
