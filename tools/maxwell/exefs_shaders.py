#!/usr/bin/env python3
"""NVN shaders built into the executable (not in any RomFS container): find them and extract them like
containers.py --extract --unique, so the SPIR-V harness translates them with the rest (group `exefs`).

Wonder's main carries 34 such code blocks (dialog glyph shaders among them, docs/nvn/TRANSLATOR.md). Code blocks
and control sections are found by their magics (0x12345678/9 + 0x2c zero bytes; 0x98761234) in the flat image
that tools/nso2bin.py writes. A block has no pointer to its control section here, so each block is paired with a
control section whose fields describe it exactly (docs/nvn/SHADERS.md, "The code block"): code size +0x704,
program start +0x708 (0x30 / 0x100 compute), program length +0x6f8 (the program fills it, zeros after it),
constant data +0x700/+0x6fc at the next 0x100 boundary, block size = constant data end rounded to 0x100. Several
control sections can fit one block; the fields the translator reads (+0x6f8..+0x708, +0x748..+0x758) must then
agree, or the block is reported and skipped.

Blocks that a RomFS container also holds (main embeds a few Ui2d shaders of the layouts) are left out: every
manifest.tsv next to the output directory is read first, so run this after containers.py. In Wonder 1.0.0 that
leaves exactly the 34 of the reference set (5 found blocks are layout shaders; 1 ambiguous one is skipped).

Usage: python tools/maxwell/exefs_shaders.py <main.bin> --extract private/wonder/shaders_extracted/exefs
"""
import hashlib
import os
import re
import struct
import sys

EXIT = struct.pack("<Q", 0xe30000000007000f)


def up(x):
    return (x + 0xff) & ~0xff


def find(d):
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    ctrls = [m.start() for m in re.finditer(re.escape(struct.pack("<I", 0x98761234)), d)
             if m.start() % 4 == 0 and m.start() + 0x880 <= len(d) and 0 < u32(m.start() + 0x704) < 1 << 24]
    codes = [m.start() for m in re.finditer(b"(?:\x78|\x79)\x56\x34\x12" + b"\0" * 0x2c, d)]

    def fits(p, c):
        s, start, plen, csize, coff = (u32(c + o) for o in (0x704, 0x708, 0x6f8, 0x6fc, 0x700))
        compute = u32(p) == 0x12345679
        if p + s > len(d) or start != (0x100 if compute else 0x30) or start + plen > s:
            return False
        if EXIT not in d[p + start:p + start + plen] or d[p + start + plen - 8:p + start + plen] == bytes(8):
            return False
        if csize:
            if coff != up(start + plen) or s != up(coff + csize) or d[p + coff + csize:p + s].strip(b"\0"):
                return False
        elif s != up(start + plen):
            return False
        if d[p + start + plen:p + up(start + plen)].strip(b"\0"):
            return False
        return compute or (u32(p + start) >> 10) & 0xf in (1, 5)

    found = []
    for i, p in enumerate(codes):
        room = (codes[i + 1] if i + 1 < len(codes) else len(d)) - p   # a block ends before the next one starts
        cands = [c for c in ctrls if u32(c + 0x704) <= room and fits(p, c)]
        # P5R's main keeps each control section right before its code block (0x850 bytes: the block starts where the
        # section's 0x880-byte window still runs). When that one fits, it is the block's own: other sections can fit
        # too (a longer size over zero bytes, or other fields), and they made 146 of P5R's blocks ambiguous or wrong.
        adjacent = [c for c in cands if p - c == 0x850]
        if len(adjacent) == 1:
            cands = adjacent
        if cands:  # a shorter size can also fit when the constant data happens to be zeros: take the longest
            size = max(u32(c + 0x704) for c in cands)
            cands = [c for c in cands if u32(c + 0x704) == size]
        keys = {(d[c + 0x6f8:c + 0x70c], d[c + 0x748:c + 0x758]) for c in cands}
        if len(keys) > 1:
            print(f"code at {p:#x}: {len(cands)} control sections that disagree, skipped", file=sys.stderr)
        elif cands:
            found.append((p, cands[0]))
    # Compute blocks with no NVN header at all (P5R): no code magic to find, but their control section says program
    # start 0 with a work-group size (+0x748), as the harness tells them (spirv_harness Program.cs), and the code
    # follows the section 0x850 bytes on, as above.
    paired = {p for p, _ in found}
    for c in ctrls:
        p, s, plen = c + 0x850, u32(c + 0x704), u32(c + 0x6f8)
        if u32(c + 0x708) or not u32(c + 0x748) or p in paired or p + s > len(d) or not 0 < plen <= s:
            continue
        if u32(p) in (0x12345678, 0x12345679) or EXIT not in d[p:p + plen]:
            continue
        found.append((p, c))
    return found, u32


def stage_of(u32, p, c):
    """compute (NVN header 0x12345679, or none and program start 0), else the SPH's: vertex or pixel."""
    if u32(p) == 0x12345679 or u32(c + 0x708) == 0:
        return "compute"
    return {1: "vertex", 5: "pixel"}[(u32(p + 0x30) >> 10) & 0xf]


def main():
    if len(sys.argv) != 4 or sys.argv[2] != "--extract":
        sys.exit(__doc__.strip().splitlines()[-1])
    d = open(sys.argv[1], "rb").read()
    out = os.path.normpath(sys.argv[3])
    repo_private = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "private"))
    if not os.path.realpath(out).startswith(repo_private + os.sep):
        sys.exit(f"refusing to write outside private/: {out}")
    known = set()  # blocks already extracted from RomFS containers (sibling directories)
    parent = os.path.dirname(os.path.abspath(out))
    for sub in os.listdir(parent) if os.path.isdir(parent) else []:
        manifest = os.path.join(parent, sub, "manifest.tsv")
        if sub != os.path.basename(out) and os.path.isfile(manifest):
            known |= {l.split("\t")[10] for l in open(manifest).read().split("\n")[1:] if l}
    found, u32 = find(d)
    os.makedirs(out, exist_ok=True)
    seen = set()
    with open(os.path.join(out, "manifest.tsv"), "w", newline="\n") as m:
        m.write("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
                "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")
        for p, c in found:
            s = u32(c + 0x704)
            code, ctrl = d[p:p + s], d[c:c + 0x880]
            h = hashlib.sha1(code).hexdigest()[:16]
            if h in seen or h in known:
                continue
            seen.add(h)
            stage = stage_of(u32, p, c)
            cf = f"ctrl_{hashlib.sha1(ctrl).hexdigest()[:16]}.bin"
            open(os.path.join(out, f"code_{h}.bin"), "wb").write(code)
            open(os.path.join(out, cf), "wb").write(ctrl)
            m.write(f"main/{p:#x}/{stage}\t{stage}\t{p:#x}\t{s:#x}\t{c:#x}\t{0x880:#x}\t{u32(c + 0x708):#x}\t"
                    f"{u32(c + 0x6f8):#x}\t{u32(c + 0x700):#x}\t{u32(c + 0x6fc):#x}\t{h}\tcode_{h}.bin\t{cf}\n")
    print(f"{len(seen)} code blocks extracted to {out}")


if __name__ == "__main__":
    main()
