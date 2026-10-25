# Draws that come and go between consecutive frames of a multi-frame draw trace (a trace_draws file holding a frame
# count; lines "td: frame F draw D tgt ... vs V fs S idx N | t0 TEX ... | a0(...): (x y z w) ..."). A draw is keyed by
# its target, shaders, index count, first texture and first vertex (model space: the same object gives the same key
# every frame); the presence of each key over the frames shows objects the game stops submitting (an 0 in a row of
# 1s) as opposed to objects it submits but the GPU shows wrong.
#   python td_toggles.py nx.log [--min-frames N]
import re
import sys
from collections import OrderedDict, defaultdict

LINE = re.compile(r"td: frame (\d+) draw (\d+) tgt (\w+) (\d+x\d+).*? vs (\w+) fs (\S+) (?:idx|arr) (\d+)(.*)")


def key_of(m):
    rest = m.group(8)
    tex = re.search(r"\| t0 (\w+) (\d+x\d+)", rest)
    vert = re.search(r"a0\(\w+\+\d+\): \(([-\d.e+na]+) ([-\d.e+na]+) ([-\d.e+na]+)", rest)
    v = tuple(vert.groups()) if vert else ()
    # The target by its size: the game alternates two scene targets frame by frame (ping-pong).
    return (m.group(4), m.group(5)[:8], m.group(6)[:8], int(m.group(7)), tex.group(2) if tex else "-", v)


def main():
    path = sys.argv[1]
    frames = OrderedDict()
    for line in open(path, errors="replace"):
        m = LINE.search(line)
        if m:
            frames.setdefault(int(m.group(1)), []).append(key_of(m))
    ids = list(frames)
    if len(ids) < 3:
        print(f"{len(ids)} frames traced: not enough")
        return
    print(f"{len(ids)} frames ({ids[0]}..{ids[-1]}), draws per frame: {[len(frames[f]) for f in ids]}")
    presence = defaultdict(lambda: [0] * len(ids))
    for i, f in enumerate(ids):
        for k in set(frames[f]):
            presence[k][i] = 1
    toggles = []
    for k, p in presence.items():
        s = "".join(map(str, p))
        flips = sum(1 for a, b in zip(s, s[1:]) if a != b)
        # A hole: present before and after but missing in between (not an object entering or leaving the view).
        holes = len(re.findall(r"(?<=1)0+(?=1)", s))
        if holes:
            toggles.append((holes, flips, s, k))
    toggles.sort(key=lambda t: (-t[0], -t[1]))
    by_target = defaultdict(int)
    for t in toggles:
        by_target[t[3][0]] += 1
    print(f"{len(presence)} distinct draws, {len(toggles)} with holes; by target: {dict(by_target)}")
    for holes, flips, s, k in toggles[:40]:
        print(f"{s}  tgt {k[0]} vs {k[1]} fs {k[2]} n {k[3]} tex {k[4]} v {' '.join(k[5])}")


if __name__ == "__main__":
    main()
