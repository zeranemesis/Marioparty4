#!/usr/bin/env python3
"""Generate replay input files for PartyBoard, so coverage can be bought in
machine-nights instead of human hours.

WHY THIS WORKS AT ALL
---------------------
The replay format is nine integers per player per frame, and
``--netplay-replay-input`` already consumes it. A generated file is
indistinguishable from a recorded one, so driving the game unattended needs no
engine work whatsoever.

And for the properties a campaign validates, playing *well* is irrelevant:
DETERMINISM asks whether the two peers computed the same state, STABILITY
whether a process disappeared. A crash found by random input is a real crash -
the game must not crash on any input - and it arrives with a replayable
recording attached.

WHAT IT DOES NOT PROVE
----------------------
That anybody enjoyed it. A generated run can move a matrix cell to PARTIAL and
never to PASS; only a human game does that. tools/merge_session.ps1 stamps
``coverage_source`` for exactly this reason, and so does this file's manifest
fragment.

THE TWO GENERATORS
------------------
*navigator* replays a timed button script. Menus are deterministic state
machines: "wait 120 frames, press A, wait 30, press A" reaches the same screen
every time. Used for the approach prefix that gets from boot to a board.

*monkey* presses buttons from a seeded PRNG over a mask that excludes anything
able to leave the game. Same seed, same file, forever - which is what makes a
crash it finds reproducible rather than an anecdote.

SAFETY, AND A CORRECTION TO IT
------------------------------
START used to be excluded outright, on the reasoning that it opens the pause
menu and a monkey that quits reports a clean exit having tested nothing.

That reasoning was wrong, and the code says so. Quitting a board goes
pause.c:197 -> BoardKill() -> main.c:229 -> omOvlReturnEx(), which pops back up
the overlay stack to the MENU that called the board. There is no path from the
board pause menu to the title screen, and none at all to leaving the program.

So quitting is not an escape, it is a MOVE:

    menu -> board A -> pause -> quit -> menu -> board B

It is the cheapest way for an unattended run to see more than one board, and
banning it removed the only exit a monkey had from a board it was done with.
START is allowed at a low weight: often enough to leave a board eventually,
rarely enough not to spend the whole run in menus.

USAGE
-----
    tools/generate_input.py monkey --frames 60000 --seed 1 -o work/gen/m1.txt
    tools/generate_input.py navigator --script tools/scripts/boot-to-w04.txt -o work/gen/prefix.txt
    tools/generate_input.py concat prefix.txt monkey.txt -o work/gen/full.txt
    tools/generate_input.py describe work/netplay-recordings/walk.txt
    tools/generate_input.py slice board-replay.txt --end 5961 -o prefix-w04.txt

THE APPROACH PREFIX, FOR FREE
-----------------------------
Getting from boot to a board takes about 5500 frames of menu navigation, and
every recording already contains one. ``slice`` cuts it out: the first 5961
frames of board-replay.txt ARE a working approach to Boo's Haunted Bash, and
the first 5385 of walk.txt are one to Toad's Midway Madness. Two boards cost
nothing at all; only the seven boards with no recording yet need five human
minutes each.

Find the frame to cut at in the .overlays file beside the recording - it is the
frame the board overlay first appears on.

REACHING A BOARD NOBODY RECORDED
--------------------------------
``inject`` overwrites a frame range of an existing recording with one chosen
input. That is how a known-good approach becomes an approach to a DIFFERENT
board: the menu runs between two known frames, the board list is a cursor, and a
stick pulse dropped inside that window moves the cursor before the confirm that
follows. The run then reports which board it actually loaded, so the mapping is
measured rather than guessed.

It is also how a run leaves a board it has finished with - a START pulse opens
the pause menu, and quitting there returns to the menu that called the board
rather than to the title screen.
"""

import argparse
import os
import random
import sys

# --- the wire format -------------------------------------------------------
# frame seat buttons stickX stickY substickX substickY triggerL triggerR
# Two rows per frame, seat 0 then seat 1. Parsed by loadReplaySamples() in
# src/port/netplay_runtime.cpp, which ignores rows with seat > 1 or an
# implausible frame, so a malformed row is dropped rather than fatal.
SEATS = 2

# Button bits, from extern/aurora/include/dolphin/pad.h. Read from the header
# rather than remembered: these are the values the engine actually compares.
PAD_BUTTON_LEFT = 0x0001
PAD_BUTTON_RIGHT = 0x0002
PAD_BUTTON_DOWN = 0x0004
PAD_BUTTON_UP = 0x0008
PAD_TRIGGER_Z = 0x0010
PAD_TRIGGER_R = 0x0020
PAD_TRIGGER_L = 0x0040
PAD_BUTTON_A = 0x0100
PAD_BUTTON_B = 0x0200
PAD_BUTTON_X = 0x0400
PAD_BUTTON_Y = 0x0800
PAD_BUTTON_START = 0x1000

NAMED_BUTTONS = {
    "A": PAD_BUTTON_A, "B": PAD_BUTTON_B, "X": PAD_BUTTON_X, "Y": PAD_BUTTON_Y,
    "Z": PAD_TRIGGER_Z, "L": PAD_TRIGGER_L, "R": PAD_TRIGGER_R,
    "UP": PAD_BUTTON_UP, "DOWN": PAD_BUTTON_DOWN,
    "LEFT": PAD_BUTTON_LEFT, "RIGHT": PAD_BUTTON_RIGHT,
    "START": PAD_BUTTON_START,
    "NONE": 0,
}

# Everything a monkey is allowed to press, with weights. START is deliberately
# absent: it opens the pause menu, which can quit to the title screen, and a run
# that quits itself reports a clean exit having tested nothing.
#
# WHY WEIGHTED, AND NOT UNIFORM. The first unattended run measured it: seed 1001
# drove Boo's Haunted Bash for 51843 frames - fourteen minutes - and reached
# TURN 1. It entered one minigame and then stalled.
#
# A board game is a corridor of confirmation dialogs, and A is what walks
# through them. Picking uniformly from nine buttons presses A on about one
# active period in nine, so the monkey spends most of its time pressing things
# that do nothing to a dialog waiting for A. Weighting A heavily is not making
# the monkey play well - it still chooses at random - it is giving it the key to
# the doors that are actually in front of it.
#
# B is second because it cancels and backs out, which is how a monkey escapes a
# submenu it wandered into. The d-pad and Z are kept low but non-zero: they
# reach things A never will, and a monkey that only presses A is a script.
# NO D-PAD. Not a preference - the engine discards it. src/game/pad.c:272:
#
#   HuPadBtn[i] = _PadBtn[i] & ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT
#                                | PAD_BUTTON_UP | PAD_BUTTON_DOWN);
#
# The game never sees a d-pad press at all. Weighting the four of them at 5%
# each meant a fifth of everything this monkey pressed was thrown away before
# any game code looked at it. Direction comes from the analog stick, which
# PadADConv turns into HuPadDStk - which is why STICK_PROBABILITY below is high
# rather than incidental.
WEIGHTED_BUTTONS = [
    (PAD_BUTTON_A, 60),
    (PAD_BUTTON_B, 18),
    (PAD_BUTTON_X, 7),
    (PAD_BUTTON_Y, 7),
    (PAD_TRIGGER_Z, 8),
    # Low, and deliberately non-zero. See the correction at the top of the file:
    # this is how a run leaves a board it is done with and reaches another.
    (PAD_BUTTON_START, 3),
]

# How often an active period also moves the stick. Menus and the board are
# navigated with it and with nothing else, so this is the monkey's only way to
# choose anything - a board, a character, a direction at a junction.
STICK_PROBABILITY = 0.75

# TWO PHASES, because one weighting cannot serve both jobs.
#
# Measured: eight monkeys started from a menu prefix, and all eight landed on
# board 0 - the position the cursor already sat on. With A at 60% they pressed
# it before ever moving, so they confirmed the default rather than choosing.
# Lowering A would fix that and break the other half: A at 60% is what finally
# got a monkey off turn 1 on a board, where the game is a corridor of
# confirmation dialogs.
#
# So the monkey alternates. EXPLORE moves and rarely commits; CONFIRM commits
# and rarely moves. A menu gets wandered before something is chosen, a board
# still gets its dialogs cleared, and neither is starved.
EXPLORE_WEIGHTS = [
    (PAD_BUTTON_A, 6),
    (PAD_BUTTON_B, 10),
    (PAD_BUTTON_X, 4),
    (PAD_BUTTON_Y, 4),
    (PAD_TRIGGER_Z, 4),
    (PAD_BUTTON_START, 2),
]
EXPLORE_STICK_PROBABILITY = 0.95
# Seconds per phase, drawn between these bounds. Long enough for a cursor to
# travel a list, short enough that a stuck dialog is not held for a minute.
PHASE_MIN_FRAMES = 90
PHASE_MAX_FRAMES = 400
# The safety mask stays a plain list, derived from the weights so the two can
# never drift apart: what may be pressed is defined in exactly one place.
SAFE_BUTTONS = [button for button, _ in WEIGHTED_BUTTONS]

STICK_MAX = 72  # what the game clamps an analog stick to; see PADClamp.


class Frame:
    """One frame's input for both seats."""

    __slots__ = ("seats",)

    def __init__(self):
        self.seats = [dict(buttons=0, sx=0, sy=0, cx=0, cy=0, tl=0, tr=0)
                      for _ in range(SEATS)]


def write_frames(frames, path):
    """Write frames in the engine's format. Directories are created."""
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as handle:
        for index, frame in enumerate(frames):
            for seat in range(SEATS):
                s = frame.seats[seat]
                handle.write("%d %d %d %d %d %d %d %d %d\n" % (
                    index, seat, s["buttons"], s["sx"], s["sy"],
                    s["cx"], s["cy"], s["tl"], s["tr"]))
    return len(frames)


def read_frames(path):
    """Read a recording back. Tolerates the engine's own tolerances."""
    frames = {}
    with open(path, "r", encoding="ascii", errors="replace") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 9:
                continue
            try:
                values = [int(p) for p in parts]
            except ValueError:
                continue
            frame_index, seat = values[0], values[1]
            if seat > 1:
                continue
            frame = frames.setdefault(frame_index, Frame())
            frame.seats[seat] = dict(
                buttons=values[2], sx=values[3], sy=values[4],
                cx=values[5], cy=values[6], tl=values[7], tr=values[8])
    if not frames:
        return []
    return [frames.get(i, Frame()) for i in range(max(frames) + 1)]


# --- navigator -------------------------------------------------------------

def parse_script(path):
    """A navigator script: one directive per line, '#' comments.

        wait 120           hold nothing for 120 frames
        press A 4 30       press A for 4 frames, then wait 30
        hold LEFT 90       hold LEFT for 90 frames
        stick 0 -72 60     hold the stick at (0,-72) for 60 frames
        seat 1             everything after this applies to seat 1
        both               everything after this applies to both seats

    Timed button sequences, because menus are deterministic state machines:
    the same waits reach the same screen every time.
    """
    steps = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            verb = parts[0].lower()
            try:
                if verb == "seat":
                    steps.append(("seat", int(parts[1])))
                elif verb == "both":
                    steps.append(("seat", -1))
                elif verb == "wait":
                    steps.append(("wait", int(parts[1])))
                elif verb == "press":
                    button = NAMED_BUTTONS[parts[1].upper()]
                    steps.append(("press", button, int(parts[2]), int(parts[3])))
                elif verb == "hold":
                    button = NAMED_BUTTONS[parts[1].upper()]
                    steps.append(("hold", button, int(parts[2])))
                elif verb == "stick":
                    steps.append(("stick", int(parts[1]), int(parts[2]), int(parts[3])))
                else:
                    raise ValueError("unknown directive %r" % verb)
            except (IndexError, KeyError, ValueError) as error:
                raise SystemExit("%s:%d: %s" % (path, number, error))
    return steps


def run_navigator(steps):
    frames = []
    seat_selector = -1  # -1 means both

    def targets():
        return range(SEATS) if seat_selector < 0 else [seat_selector]

    def emit(count, buttons=0, sx=0, sy=0):
        for _ in range(max(0, count)):
            frame = Frame()
            for seat in targets():
                frame.seats[seat].update(buttons=buttons, sx=sx, sy=sy)
            frames.append(frame)

    for step in steps:
        kind = step[0]
        if kind == "seat":
            seat_selector = step[1]
        elif kind == "wait":
            emit(step[1])
        elif kind == "press":
            _, button, held, after = step
            emit(held, buttons=button)
            emit(after)
        elif kind == "hold":
            emit(step[2], buttons=step[1])
        elif kind == "stick":
            _, sx, sy, count = step
            emit(count, sx=max(-STICK_MAX, min(STICK_MAX, sx)),
                 sy=max(-STICK_MAX, min(STICK_MAX, sy)))
    return frames


# --- monkey ----------------------------------------------------------------

def weighted_button(rng, table=None):
    """Pick a button from a weight table. Uses the rng passed in, so the file
    stays reproducible from its seed."""
    if table is None:
        table = WEIGHTED_BUTTONS
    WEIGHTED_BUTTONS_LOCAL = table
    total = sum(weight for _, weight in WEIGHTED_BUTTONS_LOCAL)
    roll = rng.randint(1, total)
    running = 0
    for button, weight in WEIGHTED_BUTTONS_LOCAL:
        running += weight
        if roll <= running:
            return button
    return WEIGHTED_BUTTONS_LOCAL[0][0]


def run_monkey(frames_wanted, seed, hold_min, hold_max, idle_bias):
    """Seeded random input over the safe mask.

    Inputs are held for a run of frames rather than re-rolled every frame: a
    button that flickers for one frame is often swallowed by the game's own
    edge detection, so per-frame randomness presses far less than it appears
    to. Holding for 4-20 frames is roughly what a person does.

    `idle_bias` is the share of runs that press nothing. Some idle time is what
    lets animations finish and turns advance; a monkey that mashes constantly
    can sit in a confirmation dialog forever.
    """
    rng = random.Random(seed)
    frames = []
    # Independent streams per seat, so one seat's timing does not shadow the
    # other's. Derived from the one seed, so the file stays reproducible.
    seat_rngs = [random.Random(rng.getrandbits(64)) for _ in range(SEATS)]
    seat_state = [dict(remaining=0, buttons=0, sx=0, sy=0,
                       phase_left=0, exploring=False) for _ in range(SEATS)]

    while len(frames) < frames_wanted:
        frame = Frame()
        for seat in range(SEATS):
            state = seat_state[seat]
            seat_rng = seat_rngs[seat]
            # Flip between exploring and confirming on its own clock, so the
            # two never line up with the input runs below.
            if state["phase_left"] <= 0:
                state["phase_left"] = seat_rng.randint(PHASE_MIN_FRAMES, PHASE_MAX_FRAMES)
                state["exploring"] = not state["exploring"]
            state["phase_left"] -= 1
            if state["remaining"] <= 0:
                state["remaining"] = seat_rng.randint(hold_min, hold_max)
                if seat_rng.random() < idle_bias:
                    state["buttons"] = 0
                    state["sx"] = state["sy"] = 0
                else:
                    table = EXPLORE_WEIGHTS if state["exploring"] else WEIGHTED_BUTTONS
                    state["buttons"] = weighted_button(seat_rng, table)
                    # A stick position more often than not: board movement and
                    # most minigames are analog, and a button-only monkey never
                    # walks anywhere.
                    threshold = EXPLORE_STICK_PROBABILITY if state["exploring"] else STICK_PROBABILITY
                    if seat_rng.random() < threshold:
                        angle = seat_rng.uniform(0, 6.283185307179586)
                        magnitude = seat_rng.randint(STICK_MAX // 2, STICK_MAX)
                        import math
                        state["sx"] = int(magnitude * math.cos(angle))
                        state["sy"] = int(magnitude * math.sin(angle))
                    else:
                        state["sx"] = state["sy"] = 0
            state["remaining"] -= 1
            frame.seats[seat].update(
                buttons=state["buttons"], sx=state["sx"], sy=state["sy"])
        frames.append(frame)
    return frames[:frames_wanted]


# --- describe --------------------------------------------------------------

def describe(path):
    frames = read_frames(path)
    if not frames:
        print("%s: no usable rows" % path)
        return 2
    print("%s" % path)
    print("  frames        : %d  (%.1f s at 60 Hz)" % (len(frames), len(frames) / 60.0))
    for seat in range(SEATS):
        pressed = sum(1 for f in frames if f.seats[seat]["buttons"])
        moved = sum(1 for f in frames if f.seats[seat]["sx"] or f.seats[seat]["sy"])
        mask = 0
        for f in frames:
            mask |= f.seats[seat]["buttons"]
        names = sorted(n for n, b in NAMED_BUTTONS.items() if b and (mask & b))
        print("  seat %d        : buttons on %d frames (%.1f%%), stick on %d (%.1f%%)"
              % (seat, pressed, 100.0 * pressed / len(frames),
                 moved, 100.0 * moved / len(frames)))
        print("                  buttons seen: %s" % (", ".join(names) or "none"))
        if mask & PAD_BUTTON_START:
            # WHERE it appears is the whole question. A sliced human prefix
            # legitimately presses START to work the menus; a monkey never may.
            # A warning that fires on every valid file is a warning nobody reads,
            # so this names the range and lets the reader decide.
            hits = [i for i, f in enumerate(frames) if f.seats[seat]["buttons"] & PAD_BUTTON_START]
            print("                  START on %d frames, first %d, last %d"
                  % (len(hits), hits[0], hits[-1]))
            print("                  ^ expected inside a recorded approach prefix, never after it")
    return 0


# --- entry point -----------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    monkey = sub.add_parser("monkey", help="seeded random input over the safe mask")
    monkey.add_argument("--frames", type=int, required=True)
    monkey.add_argument("--seed", type=int, required=True)
    monkey.add_argument("--hold-min", type=int, default=4)
    monkey.add_argument("--hold-max", type=int, default=20)
    monkey.add_argument("--idle-bias", type=float, default=0.25)
    monkey.add_argument("-o", "--output", required=True)

    navigator = sub.add_parser("navigator", help="replay a timed button script")
    navigator.add_argument("--script", required=True)
    navigator.add_argument("-o", "--output", required=True)

    concat = sub.add_parser("concat", help="join files end to end, renumbering frames")
    concat.add_argument("inputs", nargs="+")
    concat.add_argument("-o", "--output", required=True)

    show = sub.add_parser("describe", help="report what a file contains")
    show.add_argument("input")

    inject = sub.add_parser("inject", help="overwrite a frame range with one input")
    inject.add_argument("input")
    inject.add_argument("--at", type=int, required=True, help="first frame to overwrite")
    inject.add_argument("--frames", type=int, default=6, help="how many frames to hold it")
    inject.add_argument("--stick-x", type=int, default=0)
    inject.add_argument("--stick-y", type=int, default=0)
    inject.add_argument("--button", default="NONE", help="a name from NAMED_BUTTONS")
    inject.add_argument("--seat", type=int, default=-1, help="-1 for both seats")
    inject.add_argument("-o", "--output", required=True)

    cut = sub.add_parser("slice", help="take a frame range out of a recording")
    cut.add_argument("input")
    cut.add_argument("--start", type=int, default=0)
    cut.add_argument("--end", type=int, required=True,
                     help="exclusive; the frame the slice stops before")
    cut.add_argument("-o", "--output", required=True)

    args = parser.parse_args(argv)

    if args.command == "monkey":
        if args.hold_min < 1 or args.hold_max < args.hold_min:
            raise SystemExit("--hold-min must be >= 1 and <= --hold-max")
        if not 0.0 <= args.idle_bias <= 1.0:
            raise SystemExit("--idle-bias must be between 0 and 1")
        frames = run_monkey(args.frames, args.seed, args.hold_min, args.hold_max,
                            args.idle_bias)
        count = write_frames(frames, args.output)
        print("monkey seed=%d frames=%d -> %s" % (args.seed, count, args.output))
        return 0

    if args.command == "navigator":
        frames = run_navigator(parse_script(args.script))
        if not frames:
            raise SystemExit("the script produced no frames")
        count = write_frames(frames, args.output)
        print("navigator script=%s frames=%d -> %s" % (args.script, count, args.output))
        return 0

    if args.command == "concat":
        frames = []
        for path in args.inputs:
            part = read_frames(path)
            if not part:
                raise SystemExit("%s produced no frames" % path)
            print("  + %-50s %d frames" % (path, len(part)))
            frames.extend(part)
        count = write_frames(frames, args.output)
        print("concat frames=%d -> %s" % (count, args.output))
        return 0

    if args.command == "inject":
        frames = read_frames(args.input)
        if not frames:
            raise SystemExit("%s produced no frames" % args.input)
        if args.at < 0 or args.at >= len(frames):
            raise SystemExit("--at %d is outside 0..%d" % (args.at, len(frames) - 1))
        if args.button.upper() not in NAMED_BUTTONS:
            raise SystemExit("unknown button %r" % args.button)
        button = NAMED_BUTTONS[args.button.upper()]
        sx = max(-STICK_MAX, min(STICK_MAX, args.stick_x))
        sy = max(-STICK_MAX, min(STICK_MAX, args.stick_y))
        seats = range(SEATS) if args.seat < 0 else [args.seat]
        end = min(len(frames), args.at + max(1, args.frames))
        for index in range(args.at, end):
            for seat in seats:
                frames[index].seats[seat].update(buttons=button, sx=sx, sy=sy)
        count = write_frames(frames, args.output)
        print("inject %s at %d for %d frames (buttons=%s stick=%d,%d) -> %s"
              % (args.input, args.at, end - args.at, args.button.upper(), sx, sy, args.output))
        return 0

    if args.command == "slice":
        frames = read_frames(args.input)
        if not frames:
            raise SystemExit("%s produced no frames" % args.input)
        if args.end > len(frames):
            raise SystemExit("%s has %d frames; --end %d is past the end"
                             % (args.input, len(frames), args.end))
        if args.start >= args.end:
            raise SystemExit("--start must be less than --end")
        count = write_frames(frames[args.start:args.end], args.output)
        print("slice %s [%d,%d) frames=%d -> %s"
              % (args.input, args.start, args.end, count, args.output))
        return 0

    if args.command == "describe":
        return describe(args.input)

    return 2


if __name__ == "__main__":
    sys.exit(main())
