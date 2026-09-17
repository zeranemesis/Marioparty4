# Generates one input file per minigame reachable from Free Play.
#
# THE RULE THIS ENCODES. A screen in this game loads for a thousand frames or
# more before it reads a pad, and that duration moves by hundreds of frames
# depending on what ran before it. Twenty-nine probes aimed at fixed frames
# inside an overlay and every one of them changed nothing. So nothing here aims
# at a frame it has not measured:
#
#   - A and START alternate forever from the moment the mode is chosen, so
#     whichever screen is up eventually receives both.
#   - The upstream menus (mode select, the mgmode menu, the category selector)
#     read only LEFT/RIGHT. The Free Play list alone reads UP/DOWN. A vertical
#     pulse is therefore ignored everywhere except where it is wanted, and can
#     be placed without knowing which screen is showing.
#   - The one frame that IS used - when the list opens - was measured, and it
#     only holds because every variant plays an identical input up to that
#     point. Change anything earlier and it must be measured again.
#
#   python3 tools/sweep_minigames.py --list-frame 5695 --rows 16 --out work/gen
#
# Writes mg-c<category>-r<row>.txt and a scenarios manifest naming them all.

import argparse
import collections
import io
import json
import os

PAD_A = 0x0100
PAD_START = 0x1000
STICK = 72
# After the mode is confirmed the recorded prefix belongs to a completely
# different sequence of screens - it was taken from a run that went to a board.
# Letting it play pressed A inside these menus at times nobody chose, which is
# what made the first careful attempt behave worse than blind mashing.
CLEAN_FROM = 2520


def read_prefix(path):
    frames = collections.defaultdict(dict)
    for line in io.open(path):
        parts = line.split()
        if len(parts) != 9:
            continue
        values = [int(p) for p in parts]
        frames[values[0]][values[1]] = values[2:]
    return frames


def build(prefix, category, row, list_frame, total, out):
    events = {}

    def hold(start, count, button=0, sx=0, sy=0):
        for frame in range(start, start + count):
            events[frame] = (button, sx, sy)

    # Mode select: two notches right reaches entry 2, the minigame mode. Its
    # cursor loop was measured at 2408 and this part of the input never varies,
    # so these two frames are safe.
    hold(2420, 8, 0, STICK, 0)
    hold(2450, 8, 0, STICK, 0)

    # The category, chosen with horizontal pulses the list will ignore. Placed
    # early and repeated: the category selector is the only screen above the
    # list that reads them, so a pulse landing elsewhere costs nothing.
    frame = 4600
    for _ in range(category):
        hold(frame, 8, 0, STICK, 0)
        frame += 40

    # A and START forever, except across the window where the row is chosen.
    quiet_from = list_frame + 10
    quiet_to = list_frame + 30 + row * 40 + 40
    frame = 4060
    while frame < total - 120:
        if not (quiet_from <= frame < quiet_to):
            hold(frame, 6, PAD_A)
        if not (quiet_from <= frame + 45 < quiet_to):
            hold(frame + 45, 6, PAD_START)
        frame += 90

    # The row, inside the quiet window so no A confirms before the cursor has
    # finished moving.
    frame = list_frame + 30
    for _ in range(row):
        hold(frame, 8, 0, 0, -STICK)
        frame += 40

    rows = []
    for index in range(total):
        for seat in (0, 1):
            if index in events:
                button, sx, sy = events[index]
                sample = [button, sx, sy, 0, 0, 0, 0]
            elif index < CLEAN_FROM:
                sample = prefix.get(index, {}).get(seat, [0, 0, 0, 0, 0, 0, 0])
            else:
                sample = [0, 0, 0, 0, 0, 0, 0]
            rows.append("%d %d %s" % (index, seat, " ".join(str(v) for v in sample)))
    io.open(out, "w", newline="\n").write("\n".join(rows) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", default="work/gen/prefix-w04.txt")
    # Measured from a run's own freeplay_list trace, not assumed.
    parser.add_argument("--list-frame", type=int, required=True)
    parser.add_argument("--categories", type=int, default=4)
    parser.add_argument("--rows", type=int, default=16)
    parser.add_argument("--total", type=int, default=40000)
    parser.add_argument("--out", default="work/gen")
    parser.add_argument("--manifest", default="work/minigame-sweep.json")
    args = parser.parse_args()

    prefix = read_prefix(args.prefix)
    os.makedirs(args.out, exist_ok=True)
    scenarios = []
    for category in range(args.categories):
        for row in range(args.rows):
            name = "mg-c%d-r%d" % (category, row)
            path = os.path.join(args.out, name + ".txt").replace("\\", "/")
            build(prefix, category, row, args.list_frame, args.total, path)
            scenarios.append({
                "id": name,
                "description": "Free Play, categorie %d, ligne %d." % (category, row),
                "board": "unknown",
                "kind": "replay",
                "replay": path,
                "durationSeconds": 300,
                "minFrames": 500,
                "repeats": 1,
                "netplayDelay": 3,
                "seed": "recorded",
                "coverageSource": "SCRIPTED",
            })

    document = {
        "version": 1,
        "note": ("Balayage des mini-jeux de Free Play. Le mini-jeu atteint est lu "
                 "dans le chemin d'overlays de chaque run, jamais suppose depuis "
                 "la position dans la liste."),
        "scenarios": scenarios,
    }
    io.open(args.manifest, "w", newline="\n").write(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n")
    print("%d scenarios ecrits dans %s" % (len(scenarios), args.manifest))


if __name__ == "__main__":
    main()
