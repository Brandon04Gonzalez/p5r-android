#!/usr/bin/env python3
"""Flags frame dumps showing the exploded-mesh glitch (issue #43): large flat brown areas where the course has sky.

Course 1-1's sky and hills have almost no saturated brown in their upper two thirds; the exploded tree trunk fills it
with long brown triangles. A frame is flagged when more than 1.5% of the upper two thirds is trunk brown.

Usage: detect_explode.py <frames dir>...   (prints flagged frames and a count per directory)
Known false positives: the title (frames 600 and 1200 of the course script), the character select screen and the course's
tree (11400). Look at what it flags.
"""
import glob
import os
import sys

from PIL import Image


def brown_fraction(path):
    im = Image.open(path).convert('RGB').resize((192, 108)).crop((0, 0, 192, 72))
    raw = im.tobytes()
    px = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    n = sum(1 for r, g, b in px if 110 < r < 200 and 60 < g < 130 and 30 < b < 100 and r - b > 50 and r - g > 25)
    return n / len(px)


def main():
    for d in sys.argv[1:]:
        flagged = []
        frames = sorted(glob.glob(os.path.join(d, '[0-9]*.ppm')))
        for f in frames:
            if brown_fraction(f) > 0.015:
                flagged.append(os.path.basename(f))
        print(f'{d}: {len(flagged)} of {len(frames)} frames flagged {" ".join(flagged)}')


if __name__ == '__main__':
    main()
