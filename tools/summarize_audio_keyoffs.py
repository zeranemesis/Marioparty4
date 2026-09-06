"""Compare fresh MusyX diagnostics; counts are evidence, not a fidelity score."""
import argparse
import collections
import re
from pathlib import Path


def summarize(path):
    counts = collections.defaultdict(collections.Counter)
    minimum = None
    maximum = 0
    first_immediate = None
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            if line.startswith("frame="):
                maximum = max(maximum, int(line.split()[0].split("=")[1]))
            if not line.startswith(("TARGET_VOICE_START ", "TARGET_VOICE_KEYOFF ")):
                continue
            values = dict(re.findall(r"(\w+)=([^\s]+)", line))
            frame = int(values["frame"])
            minimum = frame if minimum is None else min(minimum, frame)
            maximum = max(maximum, frame)
            key = (frame // 12000, int(values["track"]))
            if line.startswith("TARGET_VOICE_START "):
                counts[key]["starts"] += 1
            else:
                counts[key]["keyoffs"] += 1
                if values["newVoice"] == "1" and values["subframe"] == values["mixStart"]:
                    counts[key]["immediate_keyoffs"] += 1
                    if first_immediate is None:
                        first_immediate = frame
    print(path)
    print(f"Last rendered time: {maximum * .005:.3f}s; first tracked start: {minimum}")
    print(f"First immediate keyoff frame: {first_immediate}")
    print("Minute | Track | Starts | Keyoffs | Immediate keyoffs")
    for (minute, track), values in sorted(counts.items()):
        if minute < 15:
            print(minute, track, values["starts"], values["keyoffs"],
                  values["immediate_keyoffs"], sep=" | ")
    print("Note: only instrument tracks selected by runtime diagnostics are traced;")
    print("trace event limits can also cap these counts on very long runs.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    for log in parser.parse_args().logs:
        summarize(log)
