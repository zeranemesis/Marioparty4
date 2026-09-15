#!/usr/bin/env python3
"""Names the field that two peers disagree on, given both sides' reports.

    python3 compare_desync.py <hote> <client>

Each argument is either a `netplay_desync_*.log` or a whole diagnostic bundle
(`Diagnostic-PartyBoard *.txt`) - the report is found inside the bundle.

Why this exists: on 2026-09-13 a session stopped on `category=ANIMATION`, and
the report said which of the sixteen subsystems differed but not which of its
7704 fields, because each machine only records its own values. The two reports
list the same fields in the same order, so having both turns "somewhere in
ANIMATION" into a field name. The bundle now carries the report so this can be
run on what the two players export.

It prints the first divergent field and its neighbours, because a field almost
never diverges alone and the ones around it say what kind of thing moved.
"""

import re
import sys

FIELD = re.compile(r"^FIELD (\d+) (\w+) (.+?) ([0-9a-fA-F]{8})\s*$")
HEADER = re.compile(r"^(failure|category|context|first_desync_frame|last_good_frame"
                    r"|build_revision|session|protocol|hash_version)=")


def load(path):
    """Returns (header lines, {index: (subsystem, name, value)})."""
    fields, header = {}, []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if HEADER.match(line):
                header.append(line)
                continue
            # Header fields also appear on one packed line in some reports.
            if line.startswith("failure=") or line.startswith("context="):
                header.append(line)
            m = FIELD.match(line)
            if m:
                fields[int(m.group(1))] = (m.group(2), m.group(3), m.group(4).lower())
    return header, fields


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    names = ("hote", "client")
    loaded = []
    for path in sys.argv[1:3]:
        header, fields = load(path)
        if not fields:
            print("ERREUR: aucun champ FIELD dans %s" % path)
            print("        Un paquet de diagnostic anterieur au 2026-09-13 ne contient pas")
            print("        le rapport de desynchronisation. Prenez le fichier")
            print("        netplay/netplay_desync_*.log directement sur la machine.")
            return 2
        loaded.append((header, fields))

    for label, (header, fields) in zip(names, loaded):
        print("=== %s : %d champs ===" % (label, len(fields)))
        for line in header[:6]:
            print("    " + line)
    print()

    (_, a), (_, b) = loaded
    common = sorted(set(a) & set(b))
    if len(a) != len(b):
        print("ATTENTION: %d champs contre %d. Les deux rapports ne decrivent pas la"
              % (len(a), len(b)))
        print("           meme frame, ou l'un est tronque. Comparaison sur les %d communs."
              % len(common))
        print()

    differing = [i for i in common if a[i][2] != b[i][2]]
    if not differing:
        print("Aucun champ ne differe sur les %d champs communs." % len(common))
        print("Si un sous-systeme etait declare DIFFERENT, les deux rapports viennent")
        print("probablement de la meme machine, ou de deux frames differentes.")
        return 0

    first = differing[0]
    print("PREMIER CHAMP DIVERGENT : index %d, sous-systeme %s" % (first, a[first][0]))
    print()
    print("  %-6s %-11s %-44s %-9s %-9s" % ("index", "sous-syst.", "champ", "hote", "client"))
    lo = max(min(common), first - 4)
    hi = min(max(common), first + 8)
    for i in common:
        if not lo <= i <= hi:
            continue
        mark = "  <<<" if a[i][2] != b[i][2] else ""
        print("  %-6d %-11s %-44s %-9s %-9s%s"
              % (i, a[i][0], a[i][1][:44], a[i][2], b[i][2], mark))
    print()

    by_subsystem = {}
    for i in differing:
        by_subsystem[a[i][0]] = by_subsystem.get(a[i][0], 0) + 1
    print("Total : %d champs divergents sur %d." % (len(differing), len(common)))
    for subsystem, count in sorted(by_subsystem.items(), key=lambda kv: -kv[1]):
        print("    %-11s %d" % (subsystem, count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
