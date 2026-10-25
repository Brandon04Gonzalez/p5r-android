"""Recompile an NSO module to C++.

Usage: python -m recomp <module.nso> <out dir> --name main --base 0x4000000000 [--only 0xADDR ...]

Writes (all symbols are prefixed with the module name, so several modules link into one program):
  funcs.h         NXB (the module's load address) and declarations of every recompiled function
  table.cpp       nx::ModuleInfo: guest offset -> function, relocations, symbols, init arrays, segments
  code_NNN.cpp    the functions, split into translation units
  report.txt      coverage: unhandled instructions, unresolved indirect branches
"""
import argparse
import contextlib
import io
import os
import time

from . import analysis, emit, nso

INSNS_PER_FILE = 12000


@contextlib.contextmanager
def open_if_changed(path):
    """Like open(path, "w"), but leaves the file (and its timestamp) alone when the text is the same, so a rebuild
    after regenerating compiles only the files that changed."""
    buf = io.StringIO()
    yield buf
    text = buf.getvalue()
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("out")
    ap.add_argument("--name", default="main")
    ap.add_argument("--base", default="0x4000000000")
    ap.add_argument("--only", nargs="*", default=None, help="only these function addresses (debug)")
    args = ap.parse_args()
    base = int(args.base, 0)
    name = args.name

    t = time.time()
    m = nso.load(args.nso, name)
    an = analysis.analyze(m)
    print(f"analysis: {len(an.functions)} functions, {len(an.jump_tables)} jump tables ({time.time() - t:.1f}s)")

    os.makedirs(args.out, exist_ok=True)
    e = emit.Emitter(an, prefix=name)
    funcs = an.functions
    if args.only:
        want = {int(x, 16) for x in args.only}
        funcs = [f for f in funcs if f.start in want]

    with open_if_changed(os.path.join(args.out, "funcs.h")) as h:
        # --base is an address in the reference layout (modules at 0x40_0000_0000); the build's NX_BASE (nx/cpu.h,
        # per host) moves all modules together, so the same generated code serves every platform.
        h.write(f"#pragma once\n#include <nx/cpu.h>\n\n#undef NXB\n#define NXB (NX_BASE + {base - REF_BASE:#x}ull)\n\n")
        for f in an.functions:
            h.write(f"void {e.fn(f.start)}(nx::CPU& c);\n")

    files = []
    cur, cur_n = [], 0
    for k, f in enumerate(funcs):
        cur.append(e.function(f))
        cur_n += (f.end - f.start) // 4
        if cur_n >= INSNS_PER_FILE or k == len(funcs) - 1:
            fname = f"code_{len(files):03d}.cpp"
            with open_if_changed(os.path.join(args.out, fname)) as out:
                out.write('#include "funcs.h"\n\n')
                out.write("\n\n".join(cur))
                out.write("\n")
            files.append(fname)
            cur, cur_n = [], 0
        if k % 4000 == 0:
            print(f"  emitted {k}/{len(funcs)} functions ({time.time() - t:.0f}s)")

    write_table(os.path.join(args.out, "table.cpp"), m, an, e, name, base)

    with open(os.path.join(args.out, "report.txt"), "w") as out:
        out.write(f"functions {len(funcs)}, handled {e.handled}, unhandled {sum(e.unhandled.values())}\n")
        out.write(f"jump tables {len(an.jump_tables)}, unresolved br (indirect tail calls) {len(an.unresolved_br)}\n\n")
        for mn, n in e.unhandled.most_common():
            out.write(f"{n:8} {mn}\n")
    print(f"done: {len(files)} files, handled {e.handled}, unhandled {sum(e.unhandled.values())} "
          f"({time.time() - t:.0f}s)")
    for mn, n in e.unhandled.most_common(30):
        print(f"  {n:7} {mn}")


REF_BASE = 0x4000000000  # where --base addresses are measured from (NX_BASE of the reference layout)


def write_table(path, m, an, e, name, base):
    with open_if_changed(path) as out:
        out.write('#include "funcs.h"\n#include <nx/module.h>\n\n')
        out.write("static const nx::FuncEntry funcs[] = {\n")
        for f in an.functions:
            out.write(f"    {{{f.start:#x}, {e.fn(f.start)}}},\n")
        out.write("};\n\n")
        out.write("static const nx::Reloc relocs[] = {\n")
        for r in m.relocs:
            out.write(f"    {{{r.offset:#x}, {r.type}, {r.sym}, {r.addend}ll}},\n")
        out.write("};\n\n")
        out.write("static const nx::Symbol symbols[] = {\n")
        for s in m.symbols:
            out.write(f"    {{{cstr(s.name)}, {s.value:#x}, {s.size}, {s.type}, {s.bind}, {int(s.imported)}}},\n")
        out.write("};\n\n")
        ia = ", ".join(f"{a:#x}" for a in m.init_array + [0])
        fa = ", ".join(f"{a:#x}" for a in m.fini_array + [0])
        out.write(f"static const uint64_t init_array[] = {{{ia}}};\n")
        out.write(f"static const uint64_t fini_array[] = {{{fa}}};\n\n")
        out.write(f"extern const nx::ModuleInfo nx_module_{name} = {{\n")
        out.write(f"    {cstr(name)}, NX_BASE + {base - REF_BASE:#x}ull,\n")
        out.write(f"    {m.text[0]:#x}, {m.text[1]:#x}, {m.ro[0]:#x}, {m.ro[1]:#x}, "
                  f"{m.data[0]:#x}, {m.data[1]:#x}, {m.bss[0]:#x}, {m.bss[1]:#x},\n")
        out.write(f"    funcs, {len(an.functions)},\n")
        out.write(f"    relocs, {len(m.relocs)},\n")
        out.write(f"    symbols, {len(m.symbols)},\n")
        out.write(f"    init_array, {len(m.init_array)}, fini_array, {len(m.fini_array)},\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
