"""Frame tools for the Wonder test runs (docs/nvn/WINDOWS-PLAN.md, "Test plan").

    python tools/frames.py png <ppm dir> <png dir>        every NNNNN.ppm (NX_DUMP_FRAMES) as NNNNN.png
    python tools/frames.py diff <a.png> <b.png> [out.png] mean and 99th-percentile channel difference (0-255), the
                                                          share of pixels off by more than 24, and a heat map
    python tools/frames.py sheet <png dir> <out.png>      a contact sheet of every frame (thumbnails, 6 across)
    python tools/frames.py stats <png>                    mean brightness and the share of near-black / magenta
                                                          pixels (a black or dummy-texture frame shows at once)

Needs Pillow (private/venv: pip install pillow).
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


def to_png(src: Path, dst: Path) -> int:
    dst.mkdir(parents=True, exist_ok=True)
    n = 0
    for ppm in sorted(src.glob("*.ppm")):
        try:
            Image.open(ppm).convert("RGB").save(dst / (ppm.stem + ".png"))
            n += 1
        except OSError:  # the last one, cut short when the run was stopped
            print(f"skipped {ppm.name} (truncated)")
    print(f"{n} frames -> {dst}")
    return n


def diff(a: Path, b: Path, out: Path | None) -> dict:
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        ib = ib.resize(ia.size, Image.BILINEAR)
    d = ImageChops.difference(ia, ib)
    gray = d.convert("L")
    hist = gray.histogram()
    total = sum(hist)
    mean = sum(k * v for k, v in enumerate(hist)) / total
    acc, p99 = 0, 0
    for k, v in enumerate(hist):
        acc += v
        if acc >= total * 0.99:
            p99 = k
            break
    off = sum(hist[25:]) / total
    if out:
        gray.point(lambda v: min(255, v * 4)).save(out)
    r = {"mean": round(mean, 2), "p99": p99, "off": round(off, 4)}
    print(f"{a.name} vs {b.name}: mean {r['mean']}, p99 {r['p99']}, pixels off {100 * r['off']:.2f}%")
    return r


def sheet(src: Path, out: Path) -> None:
    pngs = sorted(src.glob("*.png"))
    if not pngs:
        print("no frames")
        return
    tw, th, across = 320, 180, 6
    rows = (len(pngs) + across - 1) // across
    img = Image.new("RGB", (tw * across, (th + 14) * rows), (32, 32, 32))
    draw = ImageDraw.Draw(img)
    for k, p in enumerate(pngs):
        x, y = (k % across) * tw, (k // across) * (th + 14)
        img.paste(Image.open(p).convert("RGB").resize((tw, th), Image.BILINEAR), (x, y + 14))
        draw.text((x + 4, y + 1), p.stem, fill=(255, 255, 255))
    img.save(out)
    print(f"{len(pngs)} frames -> {out}")


def stats(p: Path) -> dict:
    img = Image.open(p).convert("RGB")
    small = img.resize((320, 180))
    px = list(small.getdata())
    n = len(px)
    mean = sum(sum(c) for c in px) / (3 * n)
    black = sum(1 for c in px if max(c) < 12) / n
    magenta = sum(1 for c in px if c[0] > 200 and c[2] > 200 and c[1] < 60) / n
    r = {"mean": round(mean, 1), "black": round(black, 3), "magenta": round(magenta, 3)}
    print(f"{p.name}: brightness {r['mean']}, near-black {100 * r['black']:.1f}%, magenta {100 * r['magenta']:.1f}%")
    return r


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[0] == "png":
        to_png(Path(argv[1]), Path(argv[2]))
    elif len(argv) >= 3 and argv[0] == "diff":
        diff(Path(argv[1]), Path(argv[2]), Path(argv[3]) if len(argv) > 3 else None)
    elif len(argv) >= 3 and argv[0] == "sheet":
        sheet(Path(argv[1]), Path(argv[2]))
    elif len(argv) >= 2 and argv[0] == "stats":
        for a in argv[1:]:
            stats(Path(a))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
