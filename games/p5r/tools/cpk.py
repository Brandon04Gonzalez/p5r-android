"""CRI CPK archives (Persona 5 Royal keeps its whole RomFS in two of them: CPK/ALL_USEU.CPK, CPK/PATCH1.CPK).

Usage:
  python games/p5r/tools/cpk.py <file.cpk>                         # header summary
  python games/p5r/tools/cpk.py <file.cpk> --list [--grep REGEX]   # one line per file: offset, sizes, path
  python games/p5r/tools/cpk.py <file.cpk> --extract <out dir> [--grep REGEX]
  python games/p5r/tools/cpk.py <file.cpk> --cat <path>             # one file to stdout (decompressed)

The format (public: CriPakTools, YACpkTool): a "CPK " block holding an @UTF table with the archive's header row,
then "TOC " (names, sizes, offsets), optionally "ITOC"/"ETOC"/"GTOC". @UTF tables are big endian and may be XOR
obfuscated. Files may be CRILAYLA compressed (backwards LZ, decompressed here). Output stays under private/.
"""
import argparse
import os
import re
import struct
import sys

# ---- @UTF tables ---------------------------------------------------------------------------------------------

def _unmask(data):
    if data[:4] == b"@UTF":
        return data
    out = bytearray(len(data))
    m = 0x655F
    for i, b in enumerate(data):
        out[i] = b ^ (m & 0xFF)
        m = (m * 0x4115) & 0xFFFFFFFF
    if out[:4] != b"@UTF":
        raise ValueError("not an @UTF table")
    return bytes(out)


def read_utf(data):
    """Rows of an @UTF table as a list of dicts (column name -> value)."""
    data = _unmask(data)
    size, = struct.unpack_from(">I", data, 4)
    base = 8
    _, rows_off, strings_off, data_off, name_off, ncols, row_len, nrows = struct.unpack_from(">HHIIIHHI", data, 8)
    rows_off += base
    strings_off += base
    data_off += base

    def cstr(off):
        p = strings_off + off
        e = data.index(b"\0", p)
        return data[p:e].decode("utf-8", "replace")

    fmt = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f"}

    def value(t, pos):
        if t in fmt:
            return struct.unpack_from(fmt[t], data, pos)[0], struct.calcsize(fmt[t])
        if t == 0xA:  # string
            o, = struct.unpack_from(">I", data, pos)
            return cstr(o), 4
        if t == 0xB:  # data: offset, size
            o, n = struct.unpack_from(">II", data, pos)
            return data[data_off + o:data_off + o + n], 8
        if t == 9:  # double
            return struct.unpack_from(">d", data, pos)[0], 8
        raise ValueError(f"@UTF column type {t:#x}")

    cols = []
    pos = 0x20
    for _ in range(ncols):
        flags = data[pos]
        nameo, = struct.unpack_from(">I", data, pos + 1)
        pos += 5
        storage, t = flags & 0xF0, flags & 0x0F
        const = None
        if storage == 0x30:  # constant
            const, n = value(t, pos)
            pos += n
        cols.append((cstr(nameo), storage, t, const))

    out = []
    for r in range(nrows):
        p = rows_off + r * row_len
        row = {}
        for name, storage, t, const in cols:
            if storage == 0x50:  # per row
                row[name], n = value(t, p)
                p += n
            elif storage == 0x30:
                row[name] = const
            else:  # 0x10: zero / none
                row[name] = None
        out.append(row)
    return out


def _block(f, off, magic):
    f.seek(off)
    head = f.read(16)
    if head[:4] != magic:
        raise ValueError(f"expected {magic!r} at {off:#x}, found {head[:4]!r}")
    n, = struct.unpack_from("<Q", head, 8)
    return f.read(n)


# ---- CRILAYLA --------------------------------------------------------------------------------------------------

def crilayla(src):
    if src[:8] != b"CRILAYLA":
        return src
    usize, hoff = struct.unpack_from("<II", src, 8)
    prefix = src[0x10 + hoff:0x10 + hoff + 0x100]
    out = bytearray(usize + 0x100)
    out[:0x100] = prefix
    # the compressed stream is read backwards, bit by bit, from the byte before the 0x100-byte raw prefix
    pos = len(src) - 0x100 - 1
    bits = 0
    nbits = 0

    def get(n):
        nonlocal pos, bits, nbits
        v = 0
        while n:
            if nbits == 0:
                bits = src[pos]
                pos -= 1
                nbits = 8
            take = min(n, nbits)
            v = (v << take) | ((bits >> (nbits - take)) & ((1 << take) - 1))
            nbits -= take
            n -= take
        return v

    w = usize + 0x100 - 1
    end = 0x100
    while w >= end:
        if get(1):
            back = get(13) + 3
            length = 3
            for lb in (2, 3, 5, 8):
                v = get(lb)
                length += v
                if v != (1 << lb) - 1:
                    break
            else:
                while True:
                    v = get(8)
                    length += v
                    if v != 0xFF:
                        break
            for _ in range(length):
                out[w] = out[w + back]
                w -= 1
        else:
            out[w] = get(8)
            w -= 1
    return bytes(out)


# ---- the archive -----------------------------------------------------------------------------------------------

class Cpk:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.header = read_utf(_block(self.f, 0, b"CPK "))[0]
        self.files = []
        toc_off = self.header.get("TocOffset")
        content_off = self.header.get("ContentOffset") or 0
        if toc_off:
            base = min(toc_off, content_off) if content_off else toc_off
            for r in read_utf(_block(self.f, toc_off, b"TOC ")):
                d = r.get("DirName") or ""
                name = (d + "/" if d else "") + (r.get("FileName") or "")
                self.files.append({"path": name, "offset": base + r["FileOffset"], "size": r["FileSize"],
                                   "extract": r.get("ExtractSize") or r["FileSize"], "id": r.get("ID")})
        elif self.header.get("ItocOffset"):
            raise NotImplementedError("ITOC-only archive (no names)")

    def read(self, ent):
        self.f.seek(ent["offset"])
        data = self.f.read(ent["size"])
        if ent["extract"] != ent["size"]:
            data = crilayla(data)
        return data


def _safe_out(out):
    priv = os.path.abspath("private")
    if not os.path.abspath(out).startswith(priv):
        sys.exit(f"refusing to write outside {priv}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cpk")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--grep")
    ap.add_argument("--extract")
    ap.add_argument("--cat")
    a = ap.parse_args()
    c = Cpk(a.cpk)
    sel = [e for e in c.files if not a.grep or re.search(a.grep, e["path"], re.I)]
    if a.cat:
        e = next(e for e in c.files if e["path"].lower() == a.cat.lower())
        sys.stdout.buffer.write(c.read(e))
    elif a.extract:
        _safe_out(a.extract)
        for i, e in enumerate(sel):
            p = os.path.join(a.extract, e["path"])
            os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
            with open(p, "wb") as o:
                o.write(c.read(e))
            if i % 500 == 0:
                print(f"{i}/{len(sel)} {e['path']}", file=sys.stderr)
        print(f"extracted {len(sel)} files", file=sys.stderr)
    elif a.list:
        for e in sel:
            print(f"{e['offset']:#012x} {e['size']:>10} {e['extract']:>10} {e['path']}")
    else:
        h = {k: v for k, v in c.header.items() if not isinstance(v, bytes)}
        print(h)
        tot = sum(e["extract"] for e in c.files)
        comp = sum(1 for e in c.files if e["extract"] != e["size"])
        print(f"{len(c.files)} files, {tot / 1e9:.2f} GB extracted, {comp} compressed")
        exts = {}
        for e in c.files:
            x = os.path.splitext(e["path"])[1].lower()
            n, s = exts.get(x, (0, 0))
            exts[x] = (n + 1, s + e["extract"])
        for x, (n, s) in sorted(exts.items(), key=lambda kv: -kv[1][1])[:40]:
            print(f"  {x or '(none)':10} {n:7} files {s / 1e6:10.1f} MB")


if __name__ == "__main__":
    main()
