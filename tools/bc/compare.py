"""Our BC decoder (harness) against Pillow on random blocks: max channel difference per format."""
import os
import subprocess

from pil_bc import decode

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 64, 64
# (nvn format, BCn for Pillow, channels compared, block bytes)
CASES = [(0x42, 1, 4, 8), (0x43, 2, 4, 16), (0x44, 3, 4, 16), (0x49, 4, 1, 8), (0x4b, 5, 2, 16), (0x4d, 7, 4, 16)]
for fmt, bc, ch, size in CASES:
    worst, bad = 0, 0
    for trial in range(20):
        data = os.urandom((W // 4) * (H // 4) * size)
        if bc == 7:  # every mode, evenly: a random block's first byte mostly picks mode 0
            data = bytearray(data)
            for k in range(0, len(data), 16):
                data[k] = (data[k] & ~((1 << ((k // 16) % 8 + 1)) - 1) & 0xff) | (1 << ((k // 16) % 8))
            data = bytes(data)
        path = os.path.join(HERE, 'blk.bin')
        open(path, 'wb').write(data)
        ours = subprocess.run([os.path.join(HERE, 'harness'), '%x' % fmt, str(W), str(H), path],
                              capture_output=True, check=True).stdout
        ref = decode(bc, W, H, data)
        n_out = len(ours) // (W * H)
        for i in range(W * H):
            for c in range(ch):
                a = ours[i * n_out + c]
                b = ref[i * 4 + c] if bc not in (4,) else ref[i * 4]
                d = abs(a - b)
                worst = max(worst, d)
                bad += d > 1
    print('BC%d (nvn %#x): max difference %d, %d channel values off by more than 1' % (bc, fmt, worst, bad))
