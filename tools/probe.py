#!/usr/bin/env python3
"""What a Switch game is made of: modules, SDK and middleware, graphics API, imports per nn:: service.

Usage: python tools/probe.py <exefs dir | module files...>

Takes NSOs (main, sdk, subsdk*) or flat images from tools/nso2bin.py; main.npdm, when present, gives the program id.
The graphics API is read from what the modules import and export: `nvnBootstrapLoader` (NVN), `vk*` (Vulkan),
`egl*`/`gl*` (OpenGL).
"""
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from recomp import nso  # noqa: E402

MODULE_ORDER = ("rtld", "main", "sdk") + tuple(f"subsdk{k}" for k in range(10))
# Middleware and SDK libraries leave "SDK MW+<vendor>+<library>" markers in their read-only data.
MW = re.compile(rb"SDK MW\+([\x21-\x7e]+?)\+([\x21-\x7e]+)")


def load(path):
    raw = open(path, "rb").read(4)
    if raw == b"NSO0":
        return nso.load(path, os.path.basename(path)), "nso"
    # Flat image (tools/nso2bin.py): only the MOD0 data is needed here.
    image = bytearray(open(path, "rb").read())
    m = nso.Module(name=os.path.splitext(os.path.basename(path))[0], image=image, text=(0, len(image)), ro=(0, 0), data=(0, 0),
                   bss=(0, 0))
    nso._parse_mod0(m)
    return m, "flat"


def program_id(npdm):
    raw = open(npdm, "rb").read()
    k = raw.find(b"ACI0")
    return struct.unpack_from("<Q", raw, k + 0x10)[0] if k >= 0 else None


def namespace(name):
    """Group an imported symbol: nn::<service>, nv::, vk, nvn, gl/egl, or libc / C++."""
    if name.startswith("_ZN2nn"):
        m = re.match(r"_ZN2nn(\d+)", name)
        if m:
            n = int(m.group(1))
            return "nn::" + name[len(m.group(0)):len(m.group(0)) + n]
        return "nn"
    if name.startswith("_ZN2nv"):
        return "nv::"
    if name.startswith("nvn"):
        return "nvn"
    if re.match(r"vk[A-Z]", name):
        return "vulkan"
    if re.match(r"(egl|gl)[A-Z]", name):
        return "opengl"
    if name.startswith("_Z"):
        return "c++"
    return "libc"


def graphics(imports):
    found = []
    if "nvnBootstrapLoader" in imports:
        found.append("NVN")
    if any(re.match(r"vk[A-Z]", s) for s in imports):
        found.append("Vulkan")
    if any(re.match(r"(egl|gl)[A-Z]", s) for s in imports):
        found.append("OpenGL")
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    paths = []
    npdm = None
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            for f in sorted(os.listdir(arg)):
                p = os.path.join(arg, f)
                if f == "main.npdm":
                    npdm = p
                elif os.path.isfile(p) and os.path.splitext(f)[0] in MODULE_ORDER:
                    paths.append(p)
        else:
            paths.append(arg)
    paths.sort(key=lambda p: MODULE_ORDER.index(os.path.splitext(os.path.basename(p))[0])
               if os.path.splitext(os.path.basename(p))[0] in MODULE_ORDER else 99)

    if npdm:
        pid = program_id(npdm)
        print(f"program id  {pid:016X}" if pid is not None else "program id  ? (no ACI0 in main.npdm)")

    per_module = []
    for p in paths:
        m, kind = load(p)
        imports = {s.name for s in m.symbols if s.imported and s.name}
        exports = {s.name for s in m.symbols if not s.imported and s.name}
        mw = sorted({(v.decode(), lib.decode()) for v, lib in MW.findall(bytes(m.image))})
        per_module.append((m, kind, imports, exports, mw))

    print(f"\n{'module':10} {'kind':5} {'size':>10} {'imports':>8} {'exports':>8}")
    for m, kind, imports, exports, _ in per_module:
        print(f"{m.name:10} {kind:5} {len(m.image):#10x} {len(imports):8} {len(exports):8}")

    print("\nSDK and middleware")
    for m, _, _, _, mw in per_module:
        for vendor, lib in mw:
            print(f"  {m.name:10} {vendor:12} {lib}")

    # The game's own modules: what they need from outside (the SDK and system modules provide the rest).
    game = [x for x in per_module if not x[0].name.startswith(("sdk", "rtld"))] or per_module
    imports = set().union(*(x[2] for x in game))
    api = graphics(imports)
    print(f"\ngraphics    {', '.join(api) or 'none found (check subsdk modules)'}")

    groups = collections.Counter(namespace(s) for s in imports)
    print(f"\nimports of {', '.join(x[0].name for x in game)}: {len(imports)}")
    for ns, n in groups.most_common():
        print(f"  {n:6} {ns}")


if __name__ == "__main__":
    main()
