import re, sys
M = 1 << 32
def step(p):
    r2 = p // 0x1F31D
    r3 = p - r2 * 0x1F31D
    p = (r2 * 0xB14) % M
    p = (p - r3 * 0x41A7) % M
    return p

path = sys.argv[1] if len(sys.argv) > 1 else "netplay_desync_2026-09-13_110521_player1.log"
loc, rem = {}, {}
for line in open(path, encoding="utf-8", errors="replace"):
    m = re.match(r"DIGEST (local|remote) frame=(\d+) .*? frand=([0-9a-f]{8})", line)
    if m:
        (loc if m.group(1) == "local" else rem)[int(m.group(2))] = int(m.group(3), 16)

def steps(a, b, limit=4000):
    p = a
    for i in range(limit + 1):
        if p == b:
            return i
        p = step(p)
    return None

frames = sorted(set(loc) & set(rem))
print("frames couverts : %d -> %d" % (frames[0], frames[-1]))
print()
print("frame | tirages frand local | tirages remote | ecart")
prev = None
diffs = []
for f in frames:
    if prev is not None and f == prev + 1:
        nl = steps(loc[prev], loc[f]); nr = steps(rem[prev], rem[f])
        diffs.append((f, nl, nr))
    prev = f
# only show frames where the two differ, plus the last few
bad = [d for d in diffs if d[1] != d[2]]
print("frames ou le nombre de tirages DIFFERE : %d sur %d" % (len(bad), len(diffs)))
for f, nl, nr in bad:
    print("  frame %d : local=%s  remote=%s  ecart=%s" % (f, nl, nr, (nr-nl) if (nl is not None and nr is not None) else "?"))
print()
print("profil des 12 dernieres frames :")
for f, nl, nr in diffs[-12:]:
    print("  frame %d : local=%-4s remote=%-4s" % (f, nl, nr))
import statistics
vals = [d[1] for d in diffs if d[1] is not None]
print()
print("tirages par frame (local) : min=%d max=%d moyenne=%.1f" % (min(vals), max(vals), statistics.mean(vals)))
