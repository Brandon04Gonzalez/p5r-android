"""Decode raw BC blocks with Pillow (the reference): wraps them in a DDS (DX10 header) and returns RGBA bytes."""
import io
import struct

from PIL import Image

DXGI = {1: 71, 2: 74, 3: 77, 4: 80, 5: 83, 6: 95, 7: 98}  # BCn UNORM (BC6H: UF16)
DXGI_SIGNED = {4: 81, 5: 84, 6: 96}


def dds(bc, w, h, data, signed=False):
    fmt = (DXGI_SIGNED if signed else DXGI)[bc]
    header = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, len(data), 0, 1)
    pf = struct.pack('<II4sIIIII', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    caps = struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack('<IIIII', fmt, 3, 0, 1, 0)
    return header + pf + caps + dx10 + data


def decode(bc, w, h, data, signed=False):
    im = Image.open(io.BytesIO(dds(bc, w, h, data, signed)))
    im.load()
    return im.convert('RGBA').tobytes() if bc != 6 else im.tobytes()


if __name__ == '__main__':
    import os
    for bc in (1, 2, 3, 4, 5, 6, 7):
        size = 8 if bc in (1, 4) else 16
        try:
            px = decode(bc, 4, 4, os.urandom(size))
            print('BC%d ok' % bc, len(px))
        except Exception as e:
            print('BC%d fail' % bc, e)
