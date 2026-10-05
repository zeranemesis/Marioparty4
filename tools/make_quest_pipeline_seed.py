"""Builds the Quest's bundled pipeline cache (initial_pipeline_cache_quest.db).

A headset's own cache (files/pipeline_cache.db, pulled with run-as, its -wal
and -shm beside it) holds every pipeline the game built there, the eyes'
variants included (both eyes in one draw, half-precision TEV, 16-bit depth,
batched draws). Bundled, they are built before the first frame on a fresh
install too, instead of each object popping in on its first view. Rows of a
config version the headset does not use are left out: Aurora skips them.

    py -3 tools/make_quest_pipeline_seed.py build/pipeline-dump3/pipeline_cache.db

Then gradle's quest flavor bundles it as initial_pipeline_cache.db.
"""

import argparse
import os
import sqlite3
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("headset", help="the headset's pipeline_cache.db (with its -wal beside it)")
    parser.add_argument("--seed", default="initial_pipeline_cache.db", help="the PC's bundled cache, merged in")
    parser.add_argument("--out", default="initial_pipeline_cache_quest.db")
    args = parser.parse_args()

    headset = sqlite3.connect(args.headset)
    versions = {(t, v) for t, v in headset.execute("SELECT DISTINCT type, config_version FROM pipeline_cache")}
    schema = [sql for (sql,) in headset.execute(
        "SELECT sql FROM sqlite_master WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%'")]

    if os.path.exists(args.out):
        os.remove(args.out)
    out = sqlite3.connect(args.out)
    for sql in schema:
        out.execute(sql)
    out.executemany("INSERT INTO aurora_schema(value) VALUES (?)", headset.execute("SELECT value FROM aurora_schema"))

    columns = "type, hash, config_version, config_size, config, first_frame_used"
    insert = (f"INSERT INTO pipeline_cache ({columns}) VALUES (?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(type, hash) DO UPDATE SET "
              "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)")
    counts = {}
    sources = [("headset", headset)]
    if os.path.exists(args.seed):
        sources.append(("seed", sqlite3.connect(args.seed)))
    for name, db in sources:
        for row in db.execute(f"SELECT {columns} FROM pipeline_cache"):
            if (row[0], row[2]) not in versions:
                counts[f"{name} skipped (config version)"] = counts.get(f"{name} skipped (config version)", 0) + 1
                continue
            out.execute(insert, row)
            counts[name] = counts.get(name, 0) + 1
    out.commit()
    out.execute("VACUUM")
    out.close()

    final = sqlite3.connect(args.out)
    per_type = final.execute("SELECT type, config_version, COUNT(*) FROM pipeline_cache GROUP BY type, config_version")
    print(f"{args.out}: {os.path.getsize(args.out)} bytes; rows by (type, version): {per_type.fetchall()}; {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
