"""Flags single frames where course 1-1's distant background loses its aerial-perspective haze (DEBUG-2 #92): the far
rock towers and castle drawn saturated brown/orange for a frame instead of pale pink, then hazy again.

Score per frame: the per mille of pixels in the upper part of the picture (above the ground) that are warm (hue 5-50
degrees), saturated (S > 0.42) and not dark: hazy towers have almost none, unhazed ones many. A frame is flagged when
its score jumps above both neighbours' by more than the threshold (and by a factor), i.e. a one- or two-frame spike.

    python tools/find_haze_flicker.py <video.mp4 | frames dir> [threshold per mille, default 25] [--fps N] [--from S]
    python tools/find_haze_flicker.py --score a.png b.png       # just the scores

A video is read at its full frame rate (ffmpeg, scaled to 128x72), so a one-frame flicker is not missed; frames are
named by time. Output: one line per flagged frame, "t=<s> score <s> neighbours <a> <b> <--", and a summary line.
"""
import colorsys
import shutil
import subprocess
import sys
from pathlib import Path

W, H = 128, 72
TOP = 0.55  # the part of the picture scored: the sky and the far background


def score_rgb(data, w=W, h=H):
    hit = 0
    n = 0
    for y in range(int(h * 0.04), int(h * TOP)):
        row = y * w * 3
        for x in range(int(w * 0.15), w):  # skips the HUD's coin and lives counters at the top left
            i = row + x * 3
            r, g, b = data[i] / 255, data[i + 1] / 255, data[i + 2] / 255
            n += 1
            hh, s, v = colorsys.rgb_to_hsv(r, g, b)
            if 5 / 360 <= hh <= 50 / 360 and s > 0.42 and 0.22 < v < 0.95:
                hit += 1
    return 1000 * hit / max(n, 1)


def score_image(path):
    from PIL import Image

    im = Image.open(path).convert("RGB").resize((W, H))
    return score_rgb(im.tobytes())


def ffmpeg():
    f = shutil.which("ffmpeg")
    if f:
        return f
    import glob
    import os

    c = glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin\ffmpeg.exe"))
    return c[0] if c else "ffmpeg"


def video_scores(path, fps=None, start=0.0):
    vf = f"scale={W}:{H}" + (f",fps={fps}" if fps else "")
    args = [ffmpeg(), "-hide_banner", "-loglevel", "error", "-ss", str(start), "-i", str(path), "-vf", vf,
            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    probe = subprocess.run([ffmpeg(), "-hide_banner", "-i", str(path)], capture_output=True, text=True).stderr
    rate = fps
    if not rate:
        import re

        m = re.search(r"(\d+(?:\.\d+)?) fps", probe)
        rate = float(m.group(1)) if m else 30.0
    p = subprocess.Popen(args, stdout=subprocess.PIPE)
    size = W * H * 3
    out = []
    k = 0
    while True:
        buf = p.stdout.read(size)
        if len(buf) < size:
            break
        out.append((start + k / rate, score_rgb(buf)))
        k += 1
    p.wait()
    return out


def flag(scores, threshold):
    hits = []
    for k in range(1, len(scores) - 1):
        a, s, b = scores[k - 1][1], scores[k][1], scores[k + 1][1]
        # a spike of one frame, or the first of two (k+2 back down)
        nb = b
        if k + 2 < len(scores) and s - b < threshold:
            nb = scores[k + 2][1]
        base = max(a, nb)
        if s - base > threshold and s > 2 * base + 5:
            hits.append((scores[k][0], s, a, nb))
    return hits


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--score":
        for p in argv[1:]:
            print(f"{p}: {score_image(p):.1f}")
        return
    fps = None
    start = 0.0
    if "--fps" in argv:
        i = argv.index("--fps")
        fps = float(argv[i + 1])
        del argv[i:i + 2]
    if "--from" in argv:
        i = argv.index("--from")
        start = float(argv[i + 1])
        del argv[i:i + 2]
    src = Path(argv[0])
    threshold = float(argv[1]) if len(argv) > 1 else 25.0
    if src.is_dir():
        files = sorted(src.glob("*.png"))
        scores = [(float(f.stem) if f.stem.isdigit() else k, score_image(f)) for k, f in enumerate(files)]
    else:
        scores = video_scores(src, fps, start)
    hits = flag(scores, threshold)
    for t, s, a, b in hits:
        print(f"t={t:.3f} score {s:.1f} neighbours {a:.1f} {b:.1f} <--")
    vals = sorted(s for _, s in scores)
    med = vals[len(vals) // 2] if vals else 0
    print(f"{len(scores)} frames, median score {med:.1f}, {len(hits)} haze flicker frames")


if __name__ == "__main__":
    main()
