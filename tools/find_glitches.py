"""Flags frames with exploded geometry in bright scenes (docs/nvn/DEBUG-2.md #76): stretched triangles show as
near-black pixels where a course has none (the HUD, top left, is left out).

    python tools/find_glitches.py <frames dir> [threshold per mille]

Prints, per frame, the share of near-black pixels in the play area; frames over the threshold (default 4 per
mille) are marked. Dark scenes (loading screens, the castle) have a high share everywhere: read the marks with the
contact sheet (tools/frames.py sheet).
"""
import sys
from pathlib import Path

from PIL import Image


def dark_share(path):
    img = Image.open(path).convert("RGB").resize((480, 270))
    w, h = img.size
    px = img.load()
    dark = total = 0
    for y in range(int(h * 0.05), int(h * 0.85)):
        for x in range(w):
            if x < w * 0.2 and y < h * 0.3:  # the HUD
                continue
            r, g, b = px[x, y]
            total += 1
            dark += max(r, g, b) < 24
    return 1000.0 * dark / total


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    limit = float(argv[1]) if len(argv) > 1 else 4.0
    flagged = 0
    for f in sorted(list(Path(argv[0]).glob("*.png")) + list(Path(argv[0]).glob("*.jpg"))):
        s = dark_share(f)
        mark = "  <-- " if 0 < s and s > limit and s < 500 else ""
        flagged += bool(mark)
        print(f"{f.name}: {s:6.1f} per mille near-black{mark}")
    print(f"{flagged} frames flagged")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
