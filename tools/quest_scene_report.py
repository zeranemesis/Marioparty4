#!/usr/bin/env python3
"""Per-scene performance of a Quest session, from its logcat.

    python tools/quest_scene_report.py build/quest-session-20261009/logcat-231.txt [more logs...]

For each scene (board, minigame, menu) the game showed in 3D: images a second
(72 wanted), stutters, the eyes' resolution, draws per eye, the time spent
recording them, and the headset's own counters for the game's process (VrApi:
GPU time per frame, GPU clock and load). The slowest scenes come first, so the
next optimization starts where it pays.

Scene numbers are the game's overlay numbers (include/ovl_table.h); their
names come from configure.py's comments.
"""

import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def overlay_names():
    """Overlay number -> name, the non-Japanese build's order."""
    order = []
    skip = False
    with open(os.path.join(ROOT, 'include', 'ovl_table.h'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('#if VERSION_JP'):
                skip = True
            elif line.startswith('#else'):
                skip = False
            elif line.startswith('#endif'):
                skip = False
            m = re.match(r'DLL\((\w+)\)', line)
            if m and not skip:
                order.append(m.group(1))
    titles = {}
    with open(os.path.join(ROOT, 'configure.py'), encoding='utf-8') as f:
        for line in f:
            m = re.search(r'"(\w+)",\s*#\s*(.+)$', line)
            if m:
                titles[m.group(1)] = m.group(2).strip()
    return {i: (name, titles.get(name, '')) for i, name in enumerate(order)}


class Scene:
    def __init__(self):
        self.windows = 0
        self.fps = 0.0
        self.stutters = 0
        self.record = 0.0
        self.res = []
        self.presented = []
        self.draws = []
        self.app_ms = []
        self.gpu_mhz = []
        self.gpu_busy = []
        self.culled = []


GAME_PERF = re.compile(r'Game perf: scene=(\d+) ([\d.]+) frames/s.*stutters=(\d+) record avg=([\d.]+)ms')
SCENE = re.compile(r'Scene (\d+): (classic screen|spatial rendering)')
STEREO = re.compile(r'Stereo perf: .*presented=([\d.]+)Hz.*res=(\d+)%')
DRAWS = re.compile(r'Stereo draws: \d+ frames, world (\d+) avg')
VRAPI = re.compile(r'\(\s*(\d+)\): FPS=.*App=([\d.]+)ms.*?')
CLOCK = re.compile(r'CPU4/GPU=\d+/\d+,\d+/(\d+)MHz')
BUSY = re.compile(r'GPU%=([\d.]+)')
CULL = re.compile(r'Object culling: (\d+) of (\d+) pieces.*\((\d+)%\)')
GAME_PID = re.compile(r'I/PartyBoardQuest\(\s*(\d+)\)')


def read(paths):
    scenes = collections.defaultdict(Scene)
    for path in paths:
        scene = None
        spatial = False
        game_pid = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if game_pid is None:
                    m = GAME_PID.search(line)
                    if m:
                        game_pid = m.group(1)
                m = SCENE.search(line)
                if m:
                    scene = int(m.group(1))
                    spatial = m.group(2) == 'spatial rendering'
                    continue
                if scene is None or not spatial:
                    continue
                s = scenes[scene]
                m = GAME_PERF.search(line)
                if m and int(m.group(1)) == scene:
                    fps = float(m.group(2))
                    if fps < 15:  # loading, not playing
                        continue
                    s.windows += 1
                    s.fps += fps
                    s.stutters += int(m.group(3))
                    s.record += float(m.group(4))
                    continue
                m = STEREO.search(line)
                if m:
                    s.presented.append(float(m.group(1)))
                    s.res.append(int(m.group(2)))
                    continue
                m = DRAWS.search(line)
                if m:
                    s.draws.append(int(m.group(1)))
                    continue
                m = CULL.search(line)
                if m:
                    s.culled.append(int(m.group(3)))
                    continue
                m = VRAPI.search(line)
                if m and (game_pid is None or m.group(1) == game_pid):
                    s.app_ms.append(float(m.group(2)))
                    c = CLOCK.search(line)
                    if c:
                        s.gpu_mhz.append(int(c.group(1)))
                    b = BUSY.search(line)
                    if b:
                        s.gpu_busy.append(float(b.group(1)))
    return scenes


def avg(values):
    return sum(values) / len(values) if values else float('nan')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    names = overlay_names()
    scenes = read(sys.argv[1:])
    rows = []
    for number, s in scenes.items():
        if s.windows == 0:
            continue
        name, title = names.get(number, (str(number), ''))
        rows.append((s.fps / s.windows, number, name, title, s))
    rows.sort(key=lambda r: r[0])
    print('%-4s %-34s %5s %6s %5s %5s %6s %6s %5s %5s %5s' % (
        'n', 'scene', 'img/s', 'stut/5', 'res', 'draws', 'record', 'GPU ms', 'MHz', 'GPU%', 'cull'))
    for fps, number, name, title, s in rows:
        label = (title or name)[:34]
        print('%-4d %-34s %5.1f %6.1f %4.0f%% %5.0f %5.2fms %6.2f %5.0f %4.0f%% %4.0f%%' % (
            number, label, fps, s.stutters / s.windows, avg(s.res), avg(s.draws), s.record / s.windows,
            avg(s.app_ms), avg(s.gpu_mhz), avg(s.gpu_busy) * 100, avg(s.culled)))
    print()
    print('img/s: the game\'s images a second (72 wanted), 5 s windows past loading. GPU ms: VrApi App, the')
    print('game\'s GPU time a displayed frame; MHz and GPU%: its clock and load. cull: pieces outside both eyes.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
