"""Flags frames with the flat lavender bands and stripes seen on the PC (docs/nvn/pc-quality/E1-baseline.md; the
lavender of DEBUG-2 #77): pixels close to (143, 177, 245) in perfectly flat 3x3 neighbourhoods. The near-black
detector (tools/find_glitches.py) misses them; this one misses black triangles. Use both.

    python tools/find_lavender.py <frames dir> [threshold per mille, default 1.0]

Frame NNNNN.png is taken as (NNNNN-1)/2 s into the video (tools/record_run.ps1 extracts at 2 fps).
"""
import sys
from pathlib import Path

from PIL import Image

REF = (143, 177, 245)
TOL = 7


def score(path):
    im = Image.open(path).convert("RGB").resize((480, 270))
    px = im.load()
    w, h = im.size
    hit = flat = 0
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            r, g, b = px[x, y]
            if abs(r - REF[0]) > TOL or abs(g - REF[1]) > TOL or abs(b - REF[2]) > TOL:
                continue
            hit += 1
            if all(abs(q[0] - r) <= 3 and abs(q[1] - g) <= 3 and abs(q[2] - b) <= 3
                   for q in (px[x + dx, y + dy] for dy in (-1, 0, 1) for dx in (-1, 0, 1))):
                flat += 1
    n = (w - 2) * (h - 2)
    return 1000.0 * hit / n, 1000.0 * flat / n


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    limit = float(argv[1]) if len(argv) > 1 else 1.0
    flagged = 0
    for f in sorted(Path(argv[0]).glob("*.png")):
        near, flat = score(f)
        mark = "  <-- " if flat > limit else ""
        flagged += bool(mark)
        t = (int(f.stem) - 1) / 2 if f.stem.isdigit() else 0
        print(f"{f.name} ({t:6.1f} s): lavender {near:6.1f}, flat {flat:6.1f} per mille{mark}")
    print(f"{flagged} frames flagged")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
