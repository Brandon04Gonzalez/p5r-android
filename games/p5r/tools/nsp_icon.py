# The game's own icon for the APK's launcher icon, from the player's NSP: its Control NCA holds
# icon_AmericanEnglish.dat (a 256x256 JPEG). Needs the player's prod.keys (and the ticket in the NSP for
# titlekey-encrypted games). Writes <out>/icon.jpg and the launcher sizes under <out>/res/ (mipmap-*/ic_launcher.png
# and an adaptive icon), which android/build_apk.ps1 adds when private/<game>/res exists. Game-derived: keep <out> in
# private/. Usage: nsp_icon.py <game.nsp> <prod.keys> <out dir>
import io
import os
import struct
import sys

from Crypto.Cipher import AES
from PIL import Image


def load_keys(path):
    keys = {}
    for line in open(path, encoding="utf-8"):
        if "=" in line:
            name, value = line.split("=", 1)
            keys[name.strip()] = bytes.fromhex(value.strip())
    return keys


def pfs0(f):  # name -> (absolute offset, size)
    f.seek(0)
    magic, count, strings = struct.unpack("<4sII", f.read(12))
    assert magic == b"PFS0", "not an NSP (PFS0)"
    f.seek(16)
    entries = [struct.unpack("<QQII", f.read(24)) for _ in range(count)]
    table = f.read(strings)
    base = 16 + 24 * count + strings
    out = {}
    for offset, size, name_at, _ in entries:
        name = table[name_at:table.index(b"\0", name_at)].decode()
        out[name] = (base + offset, size)
    return out


def xts_decrypt(key, data, sector, size=0x200):  # Nintendo's AES-XTS: the tweak is the sector number, big-endian
    k1, k2 = AES.new(key[:16], AES.MODE_ECB), AES.new(key[16:], AES.MODE_ECB)
    out = bytearray()
    for s in range(len(data) // size):
        t = int.from_bytes(k2.encrypt((sector + s).to_bytes(16, "big")), "little")
        for b in range(s * size, (s + 1) * size, 16):
            tb = t.to_bytes(16, "little")
            block = k1.decrypt(bytes(x ^ y for x, y in zip(data[b:b + 16], tb)))
            out += bytes(x ^ y for x, y in zip(block, tb))
            t = ((t << 1) & ((1 << 128) - 1)) ^ (0x87 if t >> 127 else 0)
    return bytes(out)


def control_icon(nsp_path, keys):
    with open(nsp_path, "rb") as f:
        files = pfs0(f)
        for name, (at, size) in files.items():
            if not name.endswith(".nca") or name.endswith(".cnmt.nca"):
                continue
            f.seek(at)
            header = xts_decrypt(keys["header_key"], f.read(0xC00), 0)
            if header[0x200:0x204] != b"NCA3" or header[0x205] != 2:  # 2: Control
                continue
            generation = max(header[0x206], header[0x220])
            rev = "%02x" % max(generation - 1, 0)
            rights = header[0x230:0x240]
            if any(rights):  # titlekey crypto: the key from the NSP's ticket
                tik = files[rights.hex() + ".tik"]
                f.seek(tik[0] + 0x180)
                key = AES.new(keys["titlekek_" + rev], AES.MODE_ECB).decrypt(f.read(16))
            else:  # the key area's AES-CTR key (application key area)
                key = AES.new(keys["key_area_key_application_" + rev], AES.MODE_ECB).decrypt(header[0x320:0x330])
            start = struct.unpack_from("<I", header, 0x240)[0] * 0x200
            fs = header[0x400:0x600]  # section 0: the RomFS
            levels = fs[0x8 + 0x10:]
            data_at, data_size = struct.unpack_from("<QQ", levels, 5 * 0x18)  # the last IVFC level is the data
            ctr_hi = fs[0x140:0x148][::-1]

            def read(offset, n):  # decrypted bytes of the RomFS (offsets within it)
                absolute = start + data_at + offset
                aligned = absolute & ~0xF
                f.seek(at + aligned)
                raw = f.read(n + (absolute - aligned))
                ctr = ctr_hi + (aligned >> 4).to_bytes(8, "big")
                plain = AES.new(key, AES.MODE_CTR, nonce=b"", initial_value=ctr).decrypt(raw)
                return plain[absolute - aligned:]

            romfs = struct.unpack("<10Q", read(0, 0x50))
            meta_at, meta_size, files_at = romfs[7], romfs[8], romfs[9]
            meta = read(meta_at, meta_size)
            pos = 0
            while pos + 0x20 <= len(meta):
                _, _, offset, length, _, name_len = struct.unpack_from("<IIQQII", meta, pos)
                entry = meta[pos + 0x20:pos + 0x20 + name_len].decode(errors="replace")
                if entry == "icon_AmericanEnglish.dat":
                    return read(files_at + offset, length)
                pos += 0x20 + (name_len + 3) // 4 * 4
    raise SystemExit("no Control NCA with icon_AmericanEnglish.dat in " + nsp_path)


def main():
    nsp, keys_path, out = sys.argv[1:4]
    jpeg = control_icon(nsp, load_keys(keys_path))
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "icon.jpg"), "wb").write(jpeg)
    art = Image.open(io.BytesIO(jpeg)).convert("RGB")
    res = os.path.join(out, "res")
    # Legacy square icons, and the adaptive icon's layer (108 dp, the art inset to the 72 dp that every mask shows).
    for folder, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(res, "mipmap-" + folder)
        os.makedirs(d, exist_ok=True)
        art.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
        layer = round(px * 108 / 48)
        inner = round(layer * 72 / 108)
        fg = Image.new("RGB", (layer, layer), art.getpixel((2, 2)))  # the art's edge colour around it
        fg.paste(art.resize((inner, inner), Image.LANCZOS), ((layer - inner) // 2, (layer - inner) // 2))
        fg.save(os.path.join(d, "ic_launcher_layer.png"))
    d = os.path.join(res, "mipmap-anydpi-v26")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "ic_launcher.xml"), "w", encoding="utf-8") as x:
        x.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@mipmap/ic_launcher_layer" />\n'
                '    <foreground android:drawable="@android:color/transparent" />\n'
                '</adaptive-icon>\n')
    print("icon: %s (%dx%d), launcher sizes in %s" % (os.path.join(out, "icon.jpg"), art.width, art.height, res))


if __name__ == "__main__":
    main()
