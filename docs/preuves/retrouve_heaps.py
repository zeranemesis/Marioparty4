#!/usr/bin/env python3
"""Recovers the peer's heap figures from its subsystem hash alone.

A desync report carries every field value for the machine that wrote it, and
only a 32-bit hash for the other one. For a subsystem with thousands of fields
that hash is a dead end. HEAPS has fifteen: three heaps, five values each. That
is small enough to simply search.

The per-subsystem hash is FNV-1a/32 over the big-endian bytes of each field, in
order, seeded with 2166136261 (include/port/netplay_state.hpp:52,96). So the
local figures plus a candidate delta reproduce a candidate hash, and the delta
that lands on the remote hash is the remote's state - no second file needed.

Usage: python3 retrouve_heaps.py <rapport de desync>
"""

import re
import sys

SEED = 2166136261
MASK = 0xFFFFFFFF


def fnv(values):
    h = SEED
    for value in values:
        for shift in (24, 16, 8, 0):
            h = ((h ^ ((value >> shift) & 0xFF)) * 16777619) & MASK
    return h


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]

    fields, target, category = [], None, None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^FIELD \d+ HEAPS (.+?) ([0-9a-fA-F]{8})\s*$", line)
        if m:
            fields.append([m.group(1), int(m.group(2), 16)])
        m = re.match(r"^SUBSYSTEM HEAPS\s+local=([0-9a-f]{8}) remote=([0-9a-f]{8})", line)
        if m:
            local_hash, target = int(m.group(1), 16), int(m.group(2), 16)
        if line.startswith("category="):
            category = line.strip()

    if not fields or target is None:
        print("Ce rapport ne porte pas de divergence HEAPS exploitable.")
        return 2

    values = [v for _, v in fields]
    print("%s, %d champs HEAPS" % (category, len(fields)))
    print("empreinte locale recalculee : %08x" % fnv(values))
    print("empreinte locale annoncee   : %08x" % local_hash)
    if fnv(values) != local_hash:
        print()
        print("ARRET : je ne reproduis pas l'empreinte locale, donc mon modele de")
        print("        hachage est faux et toute recherche serait du bruit.")
        return 2
    print("empreinte distante visee    : %08x" % target)
    print()

    # Heaps are laid out as (id, ready, size, used, blocks). Identity and size
    # are fixed by the build; only occupancy can legitimately differ.
    heaps = [(i, fields[i][1], fields[i + 3][1], fields[i + 4][1])
             for i in range(0, len(fields), 5)]
    print("tas releves : " + ", ".join(
        "heap %d = %d blocs / %d octets" % (h[1], h[3], h[2]) for h in heaps))
    print()

    hits = []
    for base, heap_id, used, blocks in heaps:
        for db in range(-64, 65):
            if blocks + db < 0:
                continue
            for du in range(-262144, 262145, 32):
                if used + du < 0:
                    continue
                trial = list(values)
                trial[base + 3] = used + du
                trial[base + 4] = blocks + db
                if fnv(trial) == target:
                    hits.append((heap_id, db, du, blocks + db, used + du))

    if not hits:
        print("Aucune difference sur un seul tas ne reproduit l'empreinte distante")
        print("dans la fenetre exploree (+/-64 blocs, +/-256 Ko par pas de 32).")
        print("La difference porte donc sur plusieurs tas a la fois, ou sur un")
        print("champ autre que l'occupation. Il faut les deux rapports.")
        return 1

    print("SOLUTION%s :" % ("S" if len(hits) > 1 else ""))
    for heap_id, db, du, blocks, used in hits:
        print("  heap %d : %+d bloc(s), %+d octet(s)  ->  distant = %d blocs / %d octets"
              % (heap_id, db, du, blocks, used))
        if db:
            print("           soit %.0f octets par bloc" % (du / db))
    if len(hits) > 1:
        print()
        print("Plusieurs candidats : une collision FNV sur 32 bits reste possible.")
        print("Le plus petit ecart est le plus vraisemblable, sans etre certain.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
