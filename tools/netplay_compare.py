#!/usr/bin/env python3
"""Compare two PartyBoard netplay desync reports and name the divergence.

Usage:
    python tools/netplay_compare.py peer1.log peer2.log

Each peer writes its own report when the canonical state stream stops matching
(see docs/NETPLAY_DIAGNOSTICS.md). The two files describe the same frame from
each machine's point of view, so the first differing value between them is the
smallest gameplay difference the instrumentation can see.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


@dataclass
class Report:
    path: str
    header: Dict[str, str] = field(default_factory=dict)
    subsystems: Dict[str, Tuple[str, str, str]] = field(default_factory=dict)
    fields: List[Tuple[int, str, str, str]] = field(default_factory=list)
    field_frame: Optional[int] = None
    digests: Dict[Tuple[str, int], Dict[str, str]] = field(default_factory=dict)
    inputs: Dict[int, Dict[str, str]] = field(default_factory=dict)

    @property
    def player(self) -> str:
        return self.header.get("player", "?")

    @property
    def first_desync_frame(self) -> Optional[int]:
        value = self.header.get("first_desync_frame")
        return int(value) if value is not None else None


def _split_pairs(text: str) -> Dict[str, str]:
    pairs = {}
    for token in text.split():
        if "=" in token:
            key, _, value = token.partition("=")
            pairs[key] = value
    return pairs


def parse(path: str) -> Report:
    report = Report(path=path)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if not line:
                continue
            head, _, rest = line.partition(" ")
            if head == "SUBSYSTEM":
                parts = rest.split()
                if len(parts) >= 4:
                    name = parts[0]
                    values = _split_pairs(" ".join(parts[1:3]))
                    report.subsystems[name] = (
                        values.get("local", "?"),
                        values.get("remote", "?"),
                        parts[3],
                    )
            elif head == "FIELDS":
                pairs = _split_pairs(rest)
                if pairs.get("count", "0") != "0":
                    report.field_frame = int(pairs.get("frame", "-1"))
            elif head == "FIELD":
                parts = rest.split()
                if len(parts) >= 4:
                    report.fields.append(
                        (int(parts[0]), parts[1], parts[2], parts[3])
                    )
            elif head == "DIGEST":
                parts = rest.split(None, 1)
                if len(parts) == 2:
                    pairs = _split_pairs(parts[1])
                    frame = pairs.get("frame")
                    if frame is not None:
                        report.digests[(parts[0], int(frame))] = pairs
            elif head == "INPUT":
                pairs = _split_pairs(rest)
                frame = pairs.pop("frame", None)
                if frame is not None:
                    report.inputs[int(frame)] = pairs
            elif head in ("PARTYBOARD_NETPLAY_DESYNC_REPORT", "END"):
                report.header.update(_split_pairs(rest))
            else:
                report.header.update(_split_pairs(line))
    return report


def compare_environment(a: Report, b: Report) -> List[str]:
    """Differences that invalidate any deeper comparison."""
    problems = []
    for key, label in (
        ("build_describe", "build"),
        ("build_revision", "build revision"),
        # The git macros are empty in a local build, so this is usually the only
        # value that actually tells two binaries apart.
        ("build_stamp", "build stamp"),
        ("protocol", "network protocol"),
        ("hash_version", "canonical hash version"),
        ("disc", "game disc"),
        ("disc_version", "disc revision"),
        ("input_delay", "input delay"),
        ("full_game", "full-game mode"),
        ("rollback", "rollback mode"),
    ):
        left, right = a.header.get(key), b.header.get(key)
        if left != right:
            problems.append(f"{label}: {left!r} vs {right!r}")
    return problems


def first_field_difference(
    a: Report, b: Report
) -> Optional[Tuple[int, str, str, str, str]]:
    """(index, subsystem, name, value_a, value_b) of the first differing field."""
    for index in range(min(len(a.fields), len(b.fields))):
        _, sub_a, name_a, value_a = a.fields[index]
        _, sub_b, name_b, value_b = b.fields[index]
        if value_a != value_b or name_a != name_b or sub_a != sub_b:
            name = name_a if name_a == name_b else f"{name_a}|{name_b}"
            subsystem = sub_a if sub_a == sub_b else f"{sub_a}|{sub_b}"
            return index, subsystem, name, value_a, value_b
    if len(a.fields) != len(b.fields):
        index = min(len(a.fields), len(b.fields))
        longer = a if len(a.fields) > len(b.fields) else b
        _, subsystem, name, _ = longer.fields[index]
        return index, subsystem, name, str(len(a.fields)), str(len(b.fields))
    return None


def last_matching_frame(a: Report, b: Report) -> Optional[int]:
    frames = sorted({frame for role, frame in a.digests if role == "local"}
                    & {frame for role, frame in b.digests if role == "local"})
    matching = None
    for frame in frames:
        left = a.digests[("local", frame)].get("hash")
        right = b.digests[("local", frame)].get("hash")
        if left != right:
            break
        matching = frame
    return matching


def input_differences(a: Report, b: Report, around: int, span: int) -> List[str]:
    """An input applied at the wrong frame diverges before any state does."""
    lines = []
    for frame in range(around - span, around + span + 1):
        left, right = a.inputs.get(frame), b.inputs.get(frame)
        if left is None and right is None:
            continue
        # Peer 1's "local" is peer 2's "remote" and vice versa.
        pairs = (
            (left.get("local") if left else None, right.get("remote") if right else None),
            (left.get("remote") if left else None, right.get("local") if right else None),
        )
        for role, (seen, mirrored) in zip(("player1", "player2"), pairs):
            if seen is not None and mirrored is not None and seen != mirrored:
                lines.append(f"  frame {frame} {role}: {seen} vs {mirrored}")
    return lines


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first")
    parser.add_argument("second")
    parser.add_argument(
        "--input-span",
        type=int,
        default=8,
        help="frames around the divergence to check for mismatched inputs",
    )
    args = parser.parse_args(argv)

    a, b = parse(args.first), parse(args.second)
    print(f"peer A: {a.path} (player {a.player})")
    print(f"peer B: {b.path} (player {b.player})")

    environment = compare_environment(a, b)
    if environment:
        print("\nINCOMPARABLE: the two peers did not run the same configuration.")
        for problem in environment:
            print(f"  {problem}")
        return 2

    frame_a, frame_b = a.first_desync_frame, b.first_desync_frame
    if frame_a is None or frame_b is None:
        print("\nNo first_desync_frame recorded in one of the reports.")
        return 2
    if frame_a != frame_b:
        print(
            f"\nWARNING: peers reported different first divergent frames "
            f"({frame_a} vs {frame_b}). Using the earlier one."
        )
    frame = min(frame_a, frame_b)
    previous = last_matching_frame(a, b)

    print(f"\nFIRST DESYNC: frame {frame}")
    if previous is not None:
        print(f"previous matching frame: {previous}")
    print(f"context: {a.header.get('context')} vs {b.header.get('context')}")
    print(f"overlay: {a.header.get('overlay')} vs {b.header.get('overlay')}")
    print(f"minigame: {a.header.get('minigame')} vs {b.header.get('minigame')}")

    differing = [
        name
        for name, (left, right, verdict) in a.subsystems.items()
        if verdict == "DIFFERENT"
    ]
    print("\ncategory: " + (", ".join(differing) if differing else a.header.get("category", "?")))
    for name in sorted(set(a.subsystems) | set(b.subsystems)):
        left = a.subsystems.get(name, ("?", "?", "?"))[0]
        right = b.subsystems.get(name, ("?", "?", "?"))[0]
        mark = "DIFFERENT" if left != right else "OK"
        print(f"  {name:<10} A={left} B={right} {mark}")

    if a.field_frame != frame or b.field_frame != frame:
        print(
            f"\nNo field-level evidence retained for frame {frame} "
            f"(A={a.field_frame}, B={b.field_frame})."
        )
    else:
        difference = first_field_difference(a, b)
        if difference is None:
            print(
                "\nEvery retained field matches: the divergence is in state that "
                "the canonical hash does not cover yet."
            )
        else:
            index, subsystem, name, left, right = difference
            print("\nFIRST DIFFERENT FIELD")
            print(f"  index:     {index}")
            print(f"  category:  {subsystem}")
            print(f"  field:     {name}")
            print(f"  peer A:    0x{left}")
            print(f"  peer B:    0x{right}")

    mismatched = input_differences(a, b, frame, args.input_span)
    if mismatched:
        print("\nINPUT MISMATCH (an input reached different frames on each peer):")
        for line in mismatched:
            print(line)

    print("\nRNG (state at the moment the session stopped)")
    for key in ("frand", "rand8", "boardrand",
                "frand_calls", "rand8_calls", "boardrand_calls"):
        left, right = a.header.get(key, "?"), b.header.get(key, "?")
        mark = "" if left == right else "   <-- DIFFERENT"
        print(f"  {key:<16} A={left} B={right}{mark}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
