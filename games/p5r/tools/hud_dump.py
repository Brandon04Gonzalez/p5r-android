# Decodes the dump_hud files (runtime nvn_debug.cpp: <data>/frames/hud_<frame>_<texture>.bin, header width, height,
# Vulkan format, bytes) to PNG: block-compressed textures through a DDS header (Pillow decodes BC1-BC5 and BC7),
# RGBA8 as is. Each gets <name>.png (with alpha) and <name>_a.png (the alpha alone).
# Usage: python hud_dump.py FILE.bin [...]   (or a folder)
import io
import os
import struct
import sys

from PIL import Image

# Vulkan format -> (DDS FourCC or DX10 DXGI format, DX10?)
BC = {
    131: (b"DXT1", False), 132: (b"DXT1", False), 133: (b"DXT1", False), 134: (b"DXT1", False),
    135: (b"DXT3", False), 136: (b"DXT3", False), 137: (b"DXT5", False), 138: (b"DXT5", False),
    139: (b"ATI1", False), 140: (b"ATI1", False), 141: (b"ATI2", False), 142: (b"ATI2", False),
    145: (98, True), 146: (99, True),  # BC7 UNORM / SRGB (DXGI)
}


def dds(w, h, fmt, data):
    fourcc, dx10 = BC[fmt]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    head = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, h, w, len(data), 0, 1) + b"\0" * 44
    pf = struct.pack("<II4s5I", 32, 0x4, b"DX10" if dx10 else fourcc, 0, 0, 0, 0, 0)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    ext = struct.pack("<5I", fourcc, 3, 0, 1, 0) if dx10 else b""
    return Image.open(io.BytesIO(head + pf + caps + ext + data))


def decode(path):
    raw = open(path, "rb").read()
    w, h, fmt, n = struct.unpack_from("<4I", raw, 0)
    data = raw[16:16 + n]
    if fmt in BC:
        im = dds(w, h, fmt, data).convert("RGBA")
    elif fmt in (37, 43):  # R8G8B8A8_UNORM / _SRGB
        im = Image.frombytes("RGBA", (w, h), data)
    else:
        print(f"{path}: Vulkan format {fmt} not handled")
        return
    base = os.path.splitext(path)[0]
    im.save(base + ".png")
    im.getchannel("A").save(base + "_a.png")
    print(f"{path}: {w}x{h} format {fmt} -> {base}.png")


for arg in sys.argv[1:]:
    paths = [os.path.join(arg, f) for f in sorted(os.listdir(arg)) if f.endswith(".bin")] if os.path.isdir(arg) else [arg]
    for p in paths:
        decode(p)
