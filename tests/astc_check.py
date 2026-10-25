"""Compares the runtime's ASTC decoder (runtime/src/nvn/nvn_astc.cpp, through astc_test) with an independent decoder
(texture2ddecoder, MIT, test-only: pip install texture2ddecoder) on the game's own ASTC textures.

    python tests/astc_check.py <build dir with astc_test and tiling_test> <RomFS file> [<RomFS file> ...]

Every ASTC texture in the files (SARC/BNTX members, tools/maxwell/bntx.py) is extracted, untiled (tiling_test),
decoded by both, and compared on level 0 in linear mode (the reference has no sRGB decode mode). Exit code 0 when
every texture matches within 1 per channel (rounding of the 16-bit interpolation) on 99.9% of texels or more.
Work files go to private/wonder/astc_check/.
"""
import re
import subprocess
import sys
from pathlib import Path

import texture2ddecoder

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable
WORK = ROOT / "private" / "wonder" / "astc_check"


def astc_textures(romfs_file):
    out = subprocess.run([PY, str(ROOT / "tools/maxwell/bntx.py"), romfs_file], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.search(r": (\S+)\s+ASTC(\d+)x(\d+)(_SRGB|_UNORM)?\s+(\d+)x(\d+)x1 layers=1", line)
        if m:
            yield m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(5)), int(m.group(6))


def check(build, romfs_file, name, bw, bh, w, h):
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
    d = WORK / safe
    d.mkdir(parents=True, exist_ok=True)
    subprocess.run([PY, str(ROOT / "tools/maxwell/bntx.py"), romfs_file, "--extract", name, str(d)],
                   check=True, capture_output=True)
    tiled = next(d.glob("*.tiled.bin"))
    txt = tiled.with_name(tiled.name.replace(".tiled.bin", ".txt"))
    subprocess.run([str(build / "tiling_test"), str(tiled), str(txt), str(d)], check=True, capture_output=True)
    linear = next(d.glob("*.linear.bin")).read_bytes()
    levels = [list(map(int, line.split())) for line in next(d.glob("*.levels.txt")).read_text().splitlines()]
    exact = close = total = worst = 0
    for lv, (offset, layer_size, lw, lh, _, _) in enumerate(levels):  # every mip level, layer 0
        data = linear[offset:offset + layer_size]
        (d / "level.bin").write_bytes(data)
        ours_path = d / "ours.rgba"
        subprocess.run([str(build / "astc_test"), str(d / "level.bin"), str(lw), str(lh), str(bw), str(bh), "0",
                        str(ours_path)], check=True, capture_output=True)
        ours = ours_path.read_bytes()
        ref = texture2ddecoder.decode_astc(data, lw, lh, bw, bh)  # BGRA
        for k in range(lw * lh):
            r = (ref[4 * k + 2], ref[4 * k + 1], ref[4 * k], ref[4 * k + 3])
            diff = max(abs(a - b) for a, b in zip(r, ours[4 * k:4 * k + 4]))
            worst = max(worst, diff)
            exact += diff == 0
            close += diff <= 1
        total += lw * lh
    ok = close >= 0.999 * total
    print(f"{'ok  ' if ok else 'FAIL'} {name:32s} ASTC {bw}x{bh} {w}x{h} ({len(levels)} levels): exact "
          f"{100 * exact / total:6.2f}%, within 1 {100 * close / total:6.2f}%, worst {worst}")
    return ok


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    build = Path(argv[0])
    failed = total = 0
    for f in argv[1:]:
        for name, bw, bh, w, h in astc_textures(f):
            total += 1
            failed += not check(build, f, name, bw, bh, w, h)
    print(f"astc_check: {total - failed} of {total} textures match")
    return 1 if failed or not total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
