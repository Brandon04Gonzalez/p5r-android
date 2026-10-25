#!/usr/bin/env python3
"""Decompress a Switch NSO into a flat memory image (text, ro, data at their offsets; bss not included).

Usage: nso2bin.py <in.nso> <out.bin>
"""
import struct
import sys

import lz4.block


def load_nso(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"NSO0":
        raise ValueError(f"{path}: not an NSO")
    flags = struct.unpack_from("<I", raw, 0x0C)[0]
    segs = []
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
        comp_size = struct.unpack_from("<I", raw, 0x60 + i * 4)[0]
        blob = raw[file_off:file_off + comp_size]
        if flags & (1 << i):
            blob = lz4.block.decompress(blob, uncompressed_size=size)
        segs.append((mem_off, blob))
    bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
    return segs, bss_size


def main():
    segs, bss = load_nso(sys.argv[1])
    end = max(off + len(b) for off, b in segs)
    image = bytearray(end)
    for off, blob in segs:
        image[off:off + len(blob)] = blob
    open(sys.argv[2], "wb").write(image)
    names = ("text", "ro", "data")
    for name, (off, blob) in zip(names, segs):
        print(f"{name:5} 0x{off:08x} size 0x{len(blob):x}")
    print(f"bss   0x{end:08x} size 0x{bss:x}")


if __name__ == "__main__":
    main()
