#!/usr/bin/env python3
"""P4AU keeps one BNSH (nn::gfx shader file) per stage in romfs data/shader/*.bnsh. Extract all of them into one
group, like containers.py --extract --unique, with a single manifest.tsv.

Usage: python games/p4au/tools/bnsh_shaders.py <romfs>/data/shader private/p4au/shaders_extracted/bnsh
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools", "maxwell"))
import containers as C  # noqa: E402


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    written, n_stages, bad = set(), 0, 0
    with open(os.path.join(out, "manifest.tsv"), "w", newline="\n") as m:
        m.write("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
                "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")
        for name in sorted(os.listdir(src)):
            if not name.endswith(".bnsh"):
                continue
            data = C.read_file(os.path.join(src, name))
            try:
                c = C.open_blob(data, 0, len(data), name)
            except Exception as e:
                print(f"{name}: {e}")
                bad += 1
                continue
            if not c:
                print(f"{name}: no container")
                bad += 1
                continue
            for s in C.all_stages(c):
                code = data[s.code_off:s.code_off + s.code_size]
                ctrl = data[s.ctrl_off:s.ctrl_off + s.ctrl_size]
                code_file = f"code_{s.hash}.bin"
                ctrl_file = f"ctrl_{hashlib.sha1(bytes(ctrl)).hexdigest()[:16]}.bin"
                for fn, blob in ((code_file, code), (ctrl_file, ctrl)):
                    if fn not in written:
                        written.add(fn)
                        open(os.path.join(out, fn), "wb").write(blob)
                m.write(f"{s.id}\t{s.kind}\t{s.code_off:#x}\t{s.code_size:#x}\t{s.ctrl_off:#x}\t{s.ctrl_size:#x}\t"
                        f"{s.start:#x}\t{s.length:#x}\t{s.const_off:#x}\t{s.const_size:#x}\t{s.hash}\t{code_file}\t"
                        f"{ctrl_file}\n")
                n_stages += 1
    print(f"{n_stages} stages, {len(written)} files, {bad} files failed -> {out}")


if __name__ == "__main__":
    main()
