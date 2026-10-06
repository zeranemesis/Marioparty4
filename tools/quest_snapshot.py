"""Takes a headset snapshot (debug.partyboard.snapshot) and converts it to PNG.

The game writes the next image shown with its interface: its left eye and its
interface, raw ("RGBA", width, height, then the image's rows), into its files.

    py -3 tools/quest_snapshot.py [name]

PNGs land in build/quest-snapshots/<name>-eye.png and -hud.png.
"""

import os
import struct
import subprocess
import sys
import time
import zlib

PACKAGE = "com.mariopartyrd.partyboard"
ADB = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk", "platform-tools", "adb.exe")


def adb(*args, binary=False):
    out = subprocess.run([ADB, *args], capture_output=True, check=True)
    return out.stdout if binary else out.stdout.decode(errors="replace")


def png(path, width, height, rgba_bottom_first):
    stride = width * 4
    # GL reads rows bottom first, but the headset's images hold them top
    # first (the compositor flips them): written as they are.
    rows = [rgba_bottom_first[y * stride:(y + 1) * stride] for y in range(height)]
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else time.strftime("%H%M%S")
    before = set(adb("shell", "run-as", PACKAGE, "ls", "files").split())
    adb("shell", "setprop", "debug.partyboard.hud_log", name)  # the interface's draws, in logcat
    adb("shell", "setprop", "debug.partyboard.snapshot", name)
    new = []
    for _ in range(40):
        time.sleep(0.25)
        files = set(adb("shell", "run-as", PACKAGE, "ls", "files").split())
        new = sorted(f for f in files - before if f.startswith("snapshot-") and f.endswith(".rgba"))
        if len(new) >= 2:
            break
    if not new:
        print("no snapshot (is a minigame or board on show in 3D?)")
        return 1
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "quest-snapshots")
    os.makedirs(out_dir, exist_ok=True)
    for file in new:
        data = adb("exec-out", "run-as", PACKAGE, "cat", "files/" + file, binary=True)
        adb("shell", "run-as", PACKAGE, "rm", "files/" + file)
        if data[:4] != b"RGBA":
            continue
        width, height = struct.unpack("<II", data[4:12])
        kind = "eye" if file.endswith("-eye.rgba") else "hud"
        path = os.path.join(out_dir, f"{name}-{kind}.png")
        png(path, width, height, data[12:12 + width * height * 4])
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
