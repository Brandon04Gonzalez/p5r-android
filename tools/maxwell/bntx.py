#!/usr/bin/env python3
"""BNTX (nn::gfx texture file): list the textures in a file and extract one texture's tiled data for tests.

Usage:
  python tools/maxwell/bntx.py <file>...                      list every texture (SARC / BFRES members are searched)
  python tools/maxwell/bntx.py <file> --extract <name> <outdir> [--index N]
  python tools/maxwell/bntx.py --survey <file|dir>...         block height and size formula over many files

BNTX is how Nintendo games ship textures: the image data is already in the GPU's block-linear layout, so the game
copies it into an NVN memory pool as is (docs/nvn/TILING.md). Layout (little endian, pointers are file offsets):

  0x00  "BNTX\\0\\0\\0\\0", u32 version, u16 BOM, u8 alignment log2, u8 address size, u32 file name,
        u16 flags, u16 first block, u32 relocation table (_RLT), u32 file size
  0x20  "NX  ", u32 texture count, u64 -> array of u64 -> BRTI, u64 -> BRTD (data block), u64 -> _DIC of names,
        u64 memory pool info
  BRTI  (texture info, one per texture)
        +0x00 "BRTI", u32 next, u32 size, u32 0
        +0x10 u8 flags, u8 dimension, u16 tile mode (0 block linear, 1 pitch), u16 swizzle, u16 mip count,
              u16 sample count, u16 0
        +0x1c u32 image format (nn::gfx: channel format << 8 | type)
        +0x20 u32 GPU access flags, s32 width, s32 height, s32 depth, s32 array length
        +0x34 u32 texture layout: bits 0..2 = log2 of the block height in GOBs (the "sizeRange" of some tools)
        +0x38 u32 texture layout 2 (0x10007 seen everywhere so far)
        +0x50 u32 image size (all levels and layers), u32 alignment, u32 channel swizzle, u8 image dimension
        +0x60 u64 -> name, u64 -> NX header, u64 -> array of u64 mip offsets (file offsets, level 0 = data start)
  BRTD  "BRTD", u32 0, u64 size; the data follows at +0x10.

Sizes are checked against runtime/src/nvn/nvn_texture_layout.cpp's formula (nvn_storage_size below is the same
computation, with an optional forced block height). Output of --extract: <name>.tiled.bin and <name>.txt, the
parameter line tests/tiling_test.cpp takes. Output must go under private/.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import containers  # noqa: E402  (SARC parser, zstd reading)

REPO = containers.REPO

# nn::gfx ChannelFormat (high byte of the image format): name, block width, block height, bytes per block.
CHANNEL = {
    0x01: ("R4G4", 1, 1, 1), 0x02: ("R8", 1, 1, 1), 0x03: ("R4G4B4A4", 1, 1, 2), 0x04: ("A4B4G4R4", 1, 1, 2),
    0x05: ("R5G5B5A1", 1, 1, 2), 0x06: ("A1B5G5R5", 1, 1, 2), 0x07: ("R5G6B5", 1, 1, 2), 0x08: ("B5G6R5", 1, 1, 2),
    0x09: ("R8G8", 1, 1, 2), 0x0A: ("R16", 1, 1, 2), 0x0B: ("R8G8B8A8", 1, 1, 4), 0x0C: ("B8G8R8A8", 1, 1, 4),
    0x0D: ("R9G9B9E5", 1, 1, 4), 0x0E: ("R10G10B10A2", 1, 1, 4), 0x0F: ("R11G11B10", 1, 1, 4),
    0x10: ("B10G11R11", 1, 1, 4), 0x11: ("R10G11B11", 1, 1, 4), 0x12: ("R16G16", 1, 1, 4), 0x13: ("R24G8", 1, 1, 4),
    0x14: ("R32", 1, 1, 4), 0x15: ("R16G16B16A16", 1, 1, 8), 0x16: ("R32G8X24", 1, 1, 8), 0x17: ("R32G32", 1, 1, 8),
    0x18: ("R32G32B32", 1, 1, 12), 0x19: ("R32G32B32A32", 1, 1, 16),
    0x1A: ("BC1", 4, 4, 8), 0x1B: ("BC2", 4, 4, 16), 0x1C: ("BC3", 4, 4, 16), 0x1D: ("BC4", 4, 4, 8),
    0x1E: ("BC5", 4, 4, 16), 0x1F: ("BC6H", 4, 4, 16), 0x20: ("BC7", 4, 4, 16),
    0x2D: ("ASTC4x4", 4, 4, 16), 0x2E: ("ASTC5x4", 5, 4, 16), 0x2F: ("ASTC5x5", 5, 5, 16),
    0x30: ("ASTC6x5", 6, 5, 16), 0x31: ("ASTC6x6", 6, 6, 16), 0x32: ("ASTC8x5", 8, 5, 16),
    0x33: ("ASTC8x6", 8, 6, 16), 0x34: ("ASTC8x8", 8, 8, 16), 0x35: ("ASTC10x5", 10, 5, 16),
    0x36: ("ASTC10x6", 10, 6, 16), 0x37: ("ASTC10x8", 10, 8, 16), 0x38: ("ASTC10x10", 10, 10, 16),
    0x39: ("ASTC12x10", 12, 10, 16), 0x3A: ("ASTC12x12", 12, 12, 16), 0x3B: ("B5G5R5A1", 1, 1, 2),
}
TYPE = {1: "UNORM", 2: "SNORM", 3: "UINT", 4: "SINT", 5: "FLOAT", 6: "SRGB", 7: "DEPTH", 8: "UFLOAT"}

u16, u32, u64 = containers.u16, containers.u32, containers.u64


class Texture:
    """One BRTI. `data` is the whole file buffer; mip_offsets are absolute offsets into it."""

    def __init__(self, d, base, info):
        self.path = ""
        self.flags, self.dim = d[info + 0x10], d[info + 0x11]
        self.tile_mode, self.swizzle, self.levels, self.samples = struct.unpack_from("<HHHH", d, info + 0x12)
        self.format = u32(d, info + 0x1C)
        self.width, self.height, self.depth, self.layers = struct.unpack_from("<iiii", d, info + 0x24)
        self.layout, self.layout2 = u32(d, info + 0x34), u32(d, info + 0x38)
        self.image_size, self.alignment = u32(d, info + 0x50), u32(d, info + 0x54)
        self.image_dim = d[info + 0x5C]
        self.name = containers.binstr(d, base + u64(d, info + 0x60))
        mips = base + u64(d, info + 0x70)
        self.mip_offsets = [base + u64(d, mips + 8 * k) for k in range(self.levels)]
        self.data = d
        ch = CHANNEL.get(self.format >> 8)
        self.format_name = f"{ch[0] if ch else hex(self.format >> 8)}_{TYPE.get(self.format & 0xFF, '?')}"
        self.block_w, self.block_h, self.bpb = (ch[1], ch[2], ch[3]) if ch else (0, 0, 0)

    @property
    def block_height_log2(self):
        return self.layout & 7

    def tiled(self):
        start = self.mip_offsets[0]
        return bytes(self.data[start:start + self.image_size])


def parse_bntx(d, base=0):
    if bytes(d[base:base + 8]) != b"BNTX\0\0\0\0" or bytes(d[base + 0x20:base + 0x24]) != b"NX  ":
        raise ValueError(f"no BNTX at {base:#x}")
    count, infos = u32(d, base + 0x24), base + u64(d, base + 0x28)
    out = []
    for k in range(count):
        info = base + u64(d, infos + 8 * k)
        if bytes(d[info:info + 4]) != b"BRTI":
            raise ValueError(f"no BRTI at {info:#x}")
        out.append(Texture(d, base, info))
    return out


def find_textures(path):
    """Every texture in a file: a BNTX, a SARC (members opened recursively) or anything embedding BNTX (BFRES)."""
    d = containers.read_file(path)

    def scan(base, size, where):
        if bytes(d[base:base + 4]) == b"SARC":
            for name, off, n in containers.sarc_files(d, base):
                yield from scan(off, n, f"{where}/{name}")
            return
        pos = base
        while True:
            pos = bytes(d[base:base + size]).find(b"BNTX\0\0\0\0", pos - base)
            if pos < 0:
                return
            pos += base
            try:
                for t in parse_bntx(d, pos):
                    t.path = where
                    yield t
            except (ValueError, struct.error, IndexError):
                pass
            pos += 8

    return list(scan(0, len(d), os.path.relpath(path, REPO)))


def div_up(a, b):
    return (a + b - 1) // b


def block_height_gobs(rows):
    """nvn_texture_layout.cpp: the smallest power of two >= rows / 8, at most 16."""
    gobs, h = div_up(rows, 8), 1
    while h < gobs and h < 16:
        h <<= 1
    return h


def nvn_storage_size(width, height, layers, levels, bw, bh, bpb, bh0=None):
    """nvn_texture_layout.cpp's size (block linear, not multisampled). bh0 forces the level 0 block height."""
    if bh0 is None:
        bh0 = block_height_gobs(div_up(height, bh))
    layer = 0
    for level in range(levels):
        w, h = max(1, width >> level), max(1, height >> level)
        cols, rows = div_up(w, bw), div_up(h, bh)
        gobs = min(bh0, block_height_gobs(rows))
        pitch = (cols * bpb + 63) & ~63
        layer += pitch * (div_up(rows, 8 * gobs) * 8 * gobs)
    block = 512 * bh0
    return div_up(layer, block) * block * layers


def describe(t):
    rows = div_up(t.height, t.block_h) if t.block_h else 0
    nvn = nvn_storage_size(t.width, t.height, t.layers * t.depth, t.levels, t.block_w, t.block_h, t.bpb) \
        if t.bpb else 0
    forced = nvn_storage_size(t.width, t.height, t.layers * t.depth, t.levels, t.block_w, t.block_h, t.bpb,
                              1 << t.block_height_log2) if t.bpb else 0
    return (f"{t.name:32s} {t.format_name:16s} {t.width}x{t.height}x{t.depth} layers={t.layers} "
            f"levels={t.levels} tile={t.tile_mode} bh_log2={t.block_height_log2} (nvn picks "
            f"{block_height_gobs(rows) if rows else 0}) size={t.image_size:#x} nvn={nvn:#x} "
            f"nvn_with_file_bh={forced:#x}")


def survey(paths):
    """Compare every texture's block height and size with the nvn_texture_layout.cpp formula."""
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in names if ".bntx" in n or ".blarc" in n or ".bfres" in n]
        else:
            files.append(p)
    total = bh_same = size_same = size_same_file_bh = linear = unknown = 0
    mismatches = []
    for f in sorted(files):
        try:
            texs = find_textures(f)
        except Exception as e:  # a damaged or unexpected file must not stop the survey
            print(f"skip {f}: {e}", file=sys.stderr)
            continue
        for t in texs:
            if not t.bpb:
                unknown += 1
                continue
            if t.tile_mode != 0:
                linear += 1
                continue
            total += 1
            rows = div_up(t.height, t.block_h)
            layers = t.layers * t.depth
            nvn = nvn_storage_size(t.width, t.height, layers, t.levels, t.block_w, t.block_h, t.bpb)
            nvn_f = nvn_storage_size(t.width, t.height, layers, t.levels, t.block_w, t.block_h, t.bpb,
                                     1 << t.block_height_log2)
            bh_same += block_height_gobs(rows) == 1 << t.block_height_log2
            size_same += nvn == t.image_size
            size_same_file_bh += nvn_f == t.image_size
            if nvn != t.image_size or block_height_gobs(rows) != 1 << t.block_height_log2:
                mismatches.append(f"{t.path}:{describe(t)}")
    print(f"block-linear textures {total}, pitch {linear}, unknown format {unknown}")
    print(f"  block height == nvn_texture_layout's choice: {bh_same}")
    print(f"  image size == nvn formula: {size_same}; == nvn formula with the file's block height: "
          f"{size_same_file_bh}")
    for m in mismatches[:60]:
        print("  " + m)
    if len(mismatches) > 60:
        print(f"  ... {len(mismatches) - 60} more")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    if args[0] == "--survey":
        survey(args[1:])
        return 0
    if "--extract" in args:
        i = args.index("--extract")
        path, name, outdir = args[0], args[i + 1], args[i + 2]
        index = int(args[args.index("--index") + 1]) if "--index" in args else 0
        if not os.path.abspath(outdir).startswith(os.path.join(REPO, "private") + os.sep):
            print("the output directory must be under private/", file=sys.stderr)
            return 1
        texs = [t for t in find_textures(path) if t.name == name]
        if len(texs) <= index:
            print(f"no texture {name}", file=sys.stderr)
            return 1
        t = texs[index]
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, f"{name}.tiled.bin"), "wb") as f:
            f.write(t.tiled())
        # The parameter line tests/tiling_test.cpp reads: everything it needs to walk the mip chain.
        with open(os.path.join(outdir, f"{name}.txt"), "w") as f:
            f.write(f"{t.width} {t.height} {t.layers * t.depth} {t.levels} {t.block_w} {t.block_h} {t.bpb} "
                    f"{1 << t.block_height_log2} {t.format_name}\n")
            f.write(" ".join(str(o - t.mip_offsets[0]) for o in t.mip_offsets) + "\n")
        print(describe(t))
        return 0
    for p in args:
        for t in find_textures(p):
            print(f"{t.path}: {describe(t)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
