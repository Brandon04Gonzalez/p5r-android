#!/usr/bin/env python3
"""Persona 5 Royal's NVN shaders: extract them for the SPIR-V harness (tools/maxwell/spirv_harness --batch).

P5R (GFD engine) keeps its compiled shaders in shader caches inside CPK/ALL_USEU.CPK: BASE/GFDNXHHPRESET{1..8}.GSC and
BASE/GFDNXHHPRESET_INIT.GSC. A GSC is "GFS0" + "GSC0" (big endian) and a list of entries ("SVSV" vertex, "__SP" pixel,
...), each with the shader's source path and an "HNSH V002" record that holds a standard NVN control section
(magic 0x98761234, docs/nvn/SHADERS.md) and its code block (0x12345678 + 0x2c zeros, then the SPH and Maxwell code).

HNSH V002 record (the 0x24-byte head is big endian, the rest little endian; offsets from the record):
  +0x00 "HNSHV002", +0x08 u32 BE size of what follows, ...
  +0x50 u32 count (1, or 0 for an empty slot that still carries a program), +0x54 control offset, +0x58 code offset,
  +0x5c code size, +0x60 control size; the offsets count from record +0x24.
All 30,418 records of the nine caches follow it (control magic, code magic and control +0x704 == code size), for
29,387 distinct code blocks. The magic-pairing search of tools/maxwell/exefs_shaders.py (still used for main.bin)
paired a third of the GSC blocks with the wrong control section, so the game asked for hashes it never wrote.
One harness group per file; each distinct code block is kept once across all groups.

Usage:
  python games/p5r/tools/gsc_shaders.py --cpk private/p5r/extract/romfs/CPK/ALL_USEU.CPK \
      --main private/p5r/analysis/main.bin --out private/p5r/shaders_extracted
"""
import argparse
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "maxwell"))
sys.path.insert(0, HERE)
import exefs_shaders  # noqa: E402
import cpk  # noqa: E402

HEADER = ("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
          "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")


def hnsh_records(data):
    """(code offset, control offset) of every HNSH V002 record of a GSC."""
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    found = []
    for m in re.finditer(b"HNSHV002", data):
        h = m.start()
        ctrl, code, size = h + 0x24 + u32(h + 0x54), h + 0x24 + u32(h + 0x58), u32(h + 0x5c)
        if u32(ctrl) != 0x98761234 or u32(code) not in (0x12345678, 0x12345679) or u32(ctrl + 0x704) != size:
            print(f"HNSH at {h:#x}: unexpected layout, skipped", file=sys.stderr)
            continue
        found.append((code, ctrl))
    return found, u32


def extract(group, data, out, seen, gsc=False):
    found, u32 = hnsh_records(data) if gsc else exefs_shaders.find(data)
    gdir = os.path.join(out, group)
    os.makedirs(gdir, exist_ok=True)
    n = 0
    with open(os.path.join(gdir, "manifest.tsv"), "w", newline="\n") as m:
        m.write(HEADER)
        for p, c in found:
            s = u32(c + 0x704)
            code, ctrl = data[p:p + s], data[c:c + 0x880]
            h = hashlib.sha1(code).hexdigest()[:16]
            if h in seen:
                continue
            seen.add(h)
            stage = exefs_shaders.stage_of(u32, p, c)
            cf = f"ctrl_{hashlib.sha1(ctrl).hexdigest()[:16]}.bin"
            with open(os.path.join(gdir, f"code_{h}.bin"), "wb") as f:
                f.write(code)
            with open(os.path.join(gdir, cf), "wb") as f:
                f.write(ctrl)
            m.write(f"{group}/{p:#x}/{stage}\t{stage}\t{p:#x}\t{s:#x}\t{c:#x}\t{0x880:#x}\t{u32(c + 0x708):#x}\t"
                    f"{u32(c + 0x6f8):#x}\t{u32(c + 0x700):#x}\t{u32(c + 0x6fc):#x}\t{h}\tcode_{h}.bin\t{cf}\n")
            n += 1
    print(f"{group}: {len(found)} stages found, {n} new code blocks", file=sys.stderr)
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpk", required=True)
    ap.add_argument("--main")
    ap.add_argument("--out", required=True)
    ap.add_argument("--grep", default=r"\.GSC$", help="CPK paths to scan (regex)")
    a = ap.parse_args()
    out = os.path.normpath(a.out)
    if not os.path.realpath(out).startswith(os.path.realpath(os.path.join(ROOT, "private")) + os.sep):
        sys.exit(f"refusing to write outside private/: {out}")
    seen = set()
    total = 0
    arc = cpk.Cpk(a.cpk)
    for e in arc.files:
        if re.search(a.grep, e["path"], re.I):
            group = re.sub(r"[^A-Za-z0-9_]+", "_", os.path.splitext(e["path"])[0]).strip("_").lower()
            total += extract(group, arc.read(e), out, seen, gsc=True)
    if a.main:
        total += extract("exefs", open(a.main, "rb").read(), out, seen)
    print(f"{total} distinct code blocks in {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
