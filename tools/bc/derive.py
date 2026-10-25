"""Derive the BC7 partition and anchor tables from Pillow's decoder (the reference), with blocks built so that each
subset decodes to its own colour. Writes tables.json."""
import json

from pil_bc import decode


class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, value, count):
        self.v |= (value & ((1 << count) - 1)) << self.n
        self.n += count

    def block(self):
        assert self.n <= 128, self.n
        return self.v.to_bytes(16, 'little')


def pixels(block):
    px = decode(7, 4, 4, block)
    return [tuple(px[4 * i:4 * i + 3]) for i in range(16)]


def mode1(part, colors, index_bit):
    """Mode 1: 2 subsets, 6-bit endpoints, shared p-bit per subset, 3-bit indices (anchors 2)."""
    b = Bits()
    b.put(0b10, 2)  # mode 1: bit 1 set
    b.put(part, 6)
    for ch in range(3):
        for s in range(2):
            for e in range(2):
                b.put(colors[s][e][ch], 6)
    b.put(0, 2)  # p-bits
    b.put((1 << (128 - b.n)) - 1 if index_bit else 0, 128 - b.n)
    return b.block()


def mode2(part, colors, index_bit):
    """Mode 2: 3 subsets, 5-bit endpoints, 2-bit indices (anchors 1)."""
    b = Bits()
    b.put(0b100, 3)
    b.put(part, 6)
    for ch in range(3):
        for s in range(3):
            for e in range(2):
                b.put(colors[s][e][ch], 5)
    b.put((1 << (128 - b.n)) - 1 if index_bit else 0, 128 - b.n)
    return b.block()


two, three, anchor2, anchor3a, anchor3b = [], [], [], [], []
for p in range(64):
    # subset 0 black, subset 1 white: the partition
    px = pixels(mode1(p, [[(0, 0, 0)] * 2, [(63, 63, 63)] * 2], False))
    two.append([0 if c[0] < 128 else 1 for c in px])
    # subset 1 from black to white, every index bit set: the anchor reads one bit fewer (not the full index)
    px = pixels(mode1(p, [[(0, 0, 0), (0, 0, 0)], [(0, 0, 0), (63, 63, 63)]], True))
    cand = [i for i in range(16) if two[p][i] == 1 and px[i][0] < 250]
    assert len(cand) == 1, (p, cand, px)
    anchor2.append(cand[0])

    px = pixels(mode2(p, [[(0, 0, 0)] * 2, [(31, 31, 31)] * 2, [(31, 0, 0)] * 2], False))
    three.append([0 if c == (0, 0, 0) else 1 if c[1] > 128 else 2 for c in px])
    px = pixels(mode2(p, [[(0, 0, 0)] * 2, [(0, 0, 0), (31, 31, 31)], [(0, 0, 0), (31, 31, 31)]], True))
    c1 = [i for i in range(16) if three[p][i] == 1 and px[i][0] < 250]
    c2 = [i for i in range(16) if three[p][i] == 2 and px[i][0] < 250]
    assert len(c1) == 1 and len(c2) == 1, (p, c1, c2)
    anchor3a.append(c1[0])
    anchor3b.append(c2[0])

json.dump({'two': two, 'three': three, 'anchor2': anchor2, 'anchor3a': anchor3a, 'anchor3b': anchor3b},
          open('tables.json', 'w'))
print('partition 1 (2 subsets):', ''.join(map(str, two[1])))
print('anchors 2:', anchor2[:16])
print('anchors 3a:', anchor3a[:16])
print('anchors 3b:', anchor3b[:16])
