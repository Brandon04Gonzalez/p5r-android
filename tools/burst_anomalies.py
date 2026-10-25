#!/usr/bin/env python3
"""Finds one- or two-frame glitches in a frame burst (tools/wonder_phone_course.sh BURST=...): exploded vertices
(dark shards across the screen) and flicker (a region that changes in one frame only).

For consecutive dumped frames a, b, c (same step), a pixel of b is anomalous when b differs from both a and c by more
than --threshold while a and c agree (a moving object changes a then c; a one-frame glitch changes b alone). It also
counts "dark" anomalies (b much darker than both). Frames are compared at 320x180.

usage: burst_anomalies.py <frames dir> [--threshold 60] [--top 12] [--out <dir for heat maps>]
Prints per frame: anomalous %, dark %, and the bounding box of the anomaly; writes heat maps for the top frames.
"""
import argparse
import glob
import os

from PIL import Image


def load(path):
    return Image.open(path).convert('L').crop((0, 0, 1280, 720)).resize((320, 180))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--threshold', type=int, default=60)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--out')
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, '[0-9]*.ppm')), key=lambda p: int(os.path.basename(p)[:-4]))
    nums = [int(os.path.basename(p)[:-4]) for p in files]
    rows = []
    for i in range(1, len(files) - 1):
        if nums[i] - nums[i - 1] != nums[i + 1] - nums[i] or nums[i] - nums[i - 1] > 4:
            continue  # not a burst triple
        pa, pb, pc = (load(files[k]).tobytes() for k in (i - 1, i, i + 1))
        n = dark = 0
        xs, ys = [], []
        for k in range(len(pb)):
            x, y, z = pa[k], pb[k], pc[k]
            if abs(x - z) < a.threshold // 2 and abs(y - x) > a.threshold and abs(y - z) > a.threshold:
                n += 1
                if y + a.threshold < min(x, z):
                    dark += 1
                xs.append(k % 320), ys.append(k // 320)
        if n:
            rows.append((n, dark, nums[i], (min(xs), min(ys), max(xs), max(ys)), files[i - 1:i + 2]))
    rows.sort(reverse=True)
    total = 320 * 180
    print(f'{a.dir}: {len(files)} frames, {len(rows)} with one-frame anomalies')
    for n, dark, num, box, _ in rows[:a.top]:
        print(f'  frame {num:6d}: {100 * n / total:5.2f}% anomalous, {100 * dark / total:5.2f}% dark, box {box}')
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        for n, dark, num, box, trio in rows[:a.top]:
            ims = [Image.open(p).crop((0, 0, 1280, 720)).resize((480, 270)) for p in trio]
            w = Image.new('RGB', (1440, 270))
            for k, im in enumerate(ims):
                w.paste(im, (480 * k, 0))
            w.save(os.path.join(a.out, f'{num:06d}.png'))


if __name__ == '__main__':
    main()
