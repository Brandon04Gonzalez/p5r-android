"""Module overview and instruction histogram.

Usage: python -m recomp.stats <nso> [out.txt]
"""
import collections
import sys

import capstone

from . import nso


def main():
    m = nso.load(sys.argv[1])
    out = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) > 2 else sys.stdout
    p = lambda *a: print(*a, file=out)
    p(f"text 0x{m.text[0]:x}+0x{m.text[1]:x}  ro 0x{m.ro[0]:x}+0x{m.ro[1]:x}  "
      f"data 0x{m.data[0]:x}+0x{m.data[1]:x}  bss 0x{m.bss[0]:x}+0x{m.bss[1]:x}")
    imports = [s for s in m.symbols if s.imported and s.name]
    exports = [s for s in m.symbols if not s.imported and s.name]
    p(f"symbols {len(m.symbols)}: imported {len(imports)}, defined {len(exports)}")
    kinds = collections.Counter(r.type for r in m.relocs)
    p("relocs", dict(kinds))
    p(f"init_array {len(m.init_array)}  fini_array {len(m.fini_array)}  eh_frame_hdr 0x{m.eh_frame_hdr[0]:x}")
    p(f"plt/got imports {len(nso.plt_imports(m))}")

    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.skipdata = True
    hist = collections.Counter()
    invalid = 0
    start, size = m.text
    code = bytes(m.image[start:start + size])
    for addr, sz, mnem, ops in cs.disasm_lite(code, start):
        if mnem == ".byte":
            invalid += 1
            continue
        # Vector forms get their own bucket: "add.v", "fmul.v" etc.
        if "v" in ops and any(t in ops for t in (".4s", ".2s", ".8b", ".16b", ".4h", ".8h", ".2d", ".1d", ".s[", ".d[", ".h[", ".b[")):
            mnem += ".v"
        hist[mnem] += 1
    total = sum(hist.values())
    p(f"instructions {total}, undecodable words {invalid}, distinct mnemonics {len(hist)}")
    for mnem, n in hist.most_common():
        p(f"{n:9} {mnem}")


if __name__ == "__main__":
    main()
