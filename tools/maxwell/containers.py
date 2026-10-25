#!/usr/bin/env python3
"""Nintendo shader containers: find every Maxwell shader in a file, describe it, optionally extract it.

Usage:
  python tools/maxwell/containers.py <file> [--extract <outdir> [--unique]] [--list] [--reflect]
  python tools/maxwell/containers.py --inventory <file|dir>...

Understood formats (docs/nvn/SHADERS.md has the layouts and what is verified vs. inferred):
  SARC   archive; every member is opened again (nesting is followed)
  FSHA   NintendoWare G3d2 shader archive (.bfsha): shading models, programs, their BNSH
  BNSH   nn::gfx shader file: variations -> program -> per-stage code + NVN control section
  VFXB   NintendoWare Vfx2 effect binary: its GRSR/GRSC sections hold BNSH files
  BAHS   agl shader archive binary (.sharcb): programs, variations, per-stage records
  BYML   (.esetb.byml) not parsed: the VFXB inside is found by its magic
Files ending in .zs are zstd and are decompressed first (zstandard module or the zstd tool).

Every stage gets a stable id (container path / program / stage) and a content hash of its code block. --extract writes
<id>.code.bin (the code block as stored: 0x30-byte NVN header, SPH for graphics stages, Maxwell instructions,
constant data) and <id>.ctrl.bin (the NVN control section) plus manifest.tsv; with --unique each distinct block is
written once, named by its hash. The output directory must be under private/.
"""
import collections
import hashlib
import os
import re
import struct
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

STAGES = ("vertex", "hull", "domain", "geometry", "pixel", "compute")  # nn::gfx ShaderStage order
# Shader Program Header, word 0 bits 10..13: which graphics stage the code was compiled for.
SPH_TYPE = {1: "vertex", 2: "hull", 3: "domain", 4: "geometry", 5: "pixel"}

CODE_MAGIC_GRAPHICS = 0x12345678   # first word of the 0x30-byte header in front of vertex..pixel code
CODE_MAGIC_COMPUTE = 0x12345679    # same header in front of compute code
CONTROL_MAGIC = 0x98761234         # first word of an NVN control section


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def s32(d, o):
    return struct.unpack_from("<i", d, o)[0]


def u64(d, o):
    return struct.unpack_from("<Q", d, o)[0]


def binstr(d, o):
    """nn::util BinString: u16 length, then the characters (NUL terminated). Pointers point at the length."""
    n = u16(d, o)
    return bytes(d[o + 2:o + 2 + n]).decode("utf-8", "replace")


def cstr(d, o):
    end = bytes(d[o:o + 256]).index(b"\0")
    return bytes(d[o:o + end]).decode("utf-8", "replace")


def resdic(d, base, off):
    """nn::util ResDic (a Patricia tree): the keys in entry order. Signature is "_DIC" in BNSH, 0 in FSHA.

    Layout: u32 signature, s32 count, then count+1 nodes of 0x10 bytes (s32 ref bit, u16 left, u16 right,
    u64 key pointer). Node 0 is the root with an empty key; nodes 1..count are the entries in index order.
    """
    if not off:
        return []
    o = base + off
    if bytes(d[o:o + 4]) not in (b"_DIC", b"\0\0\0\0") or s32(d, o + 8) != -1:
        raise ValueError(f"no dictionary at {o:#x}")
    return [binstr(d, base + u64(d, o + 8 + k * 0x10 + 8)) for k in range(1, u32(d, o + 4) + 1)]


def rlt(d, base, off):
    """nn::util RelocationTable ("_RLT"): which 8-byte fields of the file are pointers (file-relative offsets).

    u32 "_RLT", u32 own offset, u32 section count, u32 0; sections of 0x18 (u64 0, u32 position, u32 size,
    u32 first entry, u32 entry count); entries of 8 (u32 position, u16 struct count, u8 pointers, u8 skipped):
    starting at position, struct count times: `pointers` consecutive pointers, then skip `skipped` 8-byte words.
    Returns (sections, list of pointer positions).
    """
    o = base + off
    if bytes(d[o:o + 4]) != b"_RLT":
        raise ValueError(f"no _RLT at {o:#x}")
    nsec = u32(d, o + 8)
    sections = [struct.unpack_from("<QIIII", d, o + 0x10 + k * 0x18)[1:] for k in range(nsec)]
    entries = o + 0x10 + nsec * 0x18
    ptrs = []
    for _, _, first, count in sections:
        for k in range(first, first + count):
            pos, structs, n, skip = struct.unpack_from("<IHBB", d, entries + k * 8)
            for s in range(structs):
                for p in range(n):
                    ptrs.append(pos + (s * (n + skip) + p) * 8)
    return sections, ptrs


class Stage:
    """One shader stage of one program: where its code and control section are, in the top-level file."""

    def __init__(self, sid, kind, code_off, code_size, ctrl_off, ctrl_size, data):
        self.id = sid
        self.kind = kind
        self.code_off = code_off
        self.code_size = code_size
        self.ctrl_off = ctrl_off
        self.ctrl_size = ctrl_size
        code = bytes(data[code_off:code_off + code_size])
        self.hash = hashlib.sha1(code).hexdigest()[:16]
        ctrl = data[ctrl_off:ctrl_off + ctrl_size]
        # Control section words matched against the code (docs/nvn/SHADERS.md): where the program starts inside
        # the code block (0x30 for graphics, where the SPH is; 0x100 for compute), how many bytes it has from
        # there, and where the compiler's constant data (appended after the program) is and how big.
        full = ctrl_size >= 0x710
        self.start = u32(ctrl, 0x708) if full else 0
        self.length = u32(ctrl, 0x6F8) if full else 0
        self.const_off = u32(ctrl, 0x700) if full else 0
        self.const_size = u32(ctrl, 0x6FC) if full else 0
        self.problems = check_stage(self, data)


def check_stage(st, data):
    """What does not match the layout we expect. Empty list = the stage looks right."""
    bad = []
    if st.code_off + st.code_size > len(data) or st.ctrl_off + st.ctrl_size > len(data):
        return ["out of file"]
    magic = u32(data, st.code_off)
    want = CODE_MAGIC_COMPUTE if st.kind == "compute" else CODE_MAGIC_GRAPHICS
    if magic != want:
        bad.append(f"code magic {magic:#x}")
    if u32(data, st.ctrl_off) != CONTROL_MAGIC:
        bad.append("control magic")
    elif st.ctrl_size >= 0x710:
        c = st.code_off
        end = st.start + st.length
        if u32(data, st.ctrl_off + 0x704) != st.code_size:
            bad.append("ctrl+0x704 != code size")
        if any(data[c + 4:c + st.start]):
            bad.append("header not zero")
        if st.const_off != (end + 0xFF) & ~0xFF or st.code_size != (st.const_off + st.const_size + 0xFF) & ~0xFF:
            bad.append("program / constants / size do not line up")
        elif any(data[c + end:c + st.const_off]) or any(data[c + st.const_off + st.const_size:c + st.code_size]):
            bad.append("padding not zero")
    if st.kind != "compute":
        sph = u32(data, st.code_off + 0x30)
        if SPH_TYPE.get((sph >> 10) & 0xF) != st.kind:
            bad.append(f"SPH type {(sph >> 10) & 0xF}")
    return bad


class Container:
    """A shader-bearing file (or part of one). `programs` is a list of (name, [Stage...])."""

    def __init__(self, kind, path, off, size):
        self.kind = kind
        self.path = path
        self.off = off
        self.size = size
        self.programs = []
        self.variations = 0      # distinct compiled variations (programs with code)
        self.info = []           # human-readable lines
        self.children = []
        self.named = 0           # programs as the engine names them (FSHA shading-model programs, BAHS programs)


# ---------------------------------------------------------------------------------------------------------------
# BNSH: nn::gfx ResShaderFile

def parse_bnsh(d, base, path, reflect=False):
    """BNSH at `base`. All pointers inside are offsets from `base` (the file is relocatable on its own)."""
    if bytes(d[base:base + 8]) != b"BNSH\0\0\0\0":
        raise ValueError(f"no BNSH at {base:#x}")
    ver = u32(d, base + 8)
    first_block = u16(d, base + 0x16)
    rlt_off = u32(d, base + 0x18)
    size = u32(d, base + 0x1C)
    c = Container("BNSH", path, base, size)
    g = base + first_block                                  # "grsc": ResShaderContainerData
    if bytes(d[g:g + 4]) != b"grsc":
        raise ValueError(f"no grsc block at {g:#x}")
    api, api_ver = u16(d, g + 0x10), u16(d, g + 0x12)
    code_type = d[g + 0x14]
    compiler = u32(d, g + 0x18)
    nvar = u32(d, g + 0x1C)
    var_array = u64(d, g + 0x20)
    pool = u64(d, g + 0x28)                                 # ResMemoryPoolData of the code pool
    pool_prop, pool_size, pool_off = u32(d, base + pool), u32(d, base + pool + 4), u64(d, base + pool + 8)
    sections, ptrs = rlt(d, base, rlt_off)
    c.info.append(f"version {ver >> 16}.{(ver >> 8) & 0xFF}.{ver & 0xFF}, api {api} (4 = NVN) v{api_ver}, "
                  f"code type {code_type}, compiler {compiler:#x}, {nvar} variations")
    c.info.append(f"code pool {pool_off:#x}..{pool_off + pool_size:#x} ({pool_size:#x} bytes, property {pool_prop:#x}); "
                  f"_RLT at {rlt_off:#x}: {len(sections)} sections, {len(ptrs)} pointers")
    for k in range(nvar):
        # ResShaderVariationData: source, intermediate language and binary program pointers (only the binary is
        # ever present in this game), the parent container, reserved.
        binary = u64(d, base + var_array + k * 0x40 + 0x10)
        if not binary:
            c.programs.append((f"v{k:05d}", []))
            continue
        c.variations += 1
        p = base + binary                                   # ResShaderProgramData, starts with ShaderInfoData
        stages = []
        for s, kind in enumerate(STAGES):
            sc = u64(d, p + 8 + s * 8)
            if not sc:
                continue
            sc += base                                      # per-stage code descriptor, 0x40 bytes
            ctrl, code = u64(d, sc + 8), u64(d, sc + 0x10)
            code_size, ctrl_size = u32(d, sc + 0x18), u32(d, sc + 0x1C)
            stages.append(Stage(f"{path}/v{k:05d}/{kind}", kind, base + code, code_size, base + ctrl, ctrl_size, d))
        c.programs.append((f"v{k:05d}", stages))
        if reflect:
            refl = u64(d, p + 0x78)
            if refl:
                for s, kind in enumerate(STAGES):
                    r = u64(d, base + refl + s * 8)
                    if r:
                        c.info.append(f"  v{k:05d} {kind:8} " + reflection(d, base, r))
    # Code blocks are shared between variations. The distinct blocks should lie back to back from the start of the
    # code pool, and every control section should sit outside it (with the metadata).
    blocks = sorted({(s.code_off - base, s.code_size) for _, ss in c.programs for s in ss})
    ctrls = {s.ctrl_off - base for _, ss in c.programs for s in ss}
    at, gaps = pool_off, 0
    for off, n in blocks:
        gaps += off != at
        at = max(at, off + n)
    c.info.append(f"{len(blocks)} distinct code blocks, {sum(n for _, n in blocks):#x} bytes, "
                  f"{'back to back from the pool start' if not gaps else f'{gaps} gaps'} to {at:#x} "
                  f"({'inside' if at <= pool_off + pool_size else 'PAST'} the pool); "
                  f"{len(ctrls)} distinct control sections, "
                  f"{sum(pool_off <= x < pool_off + pool_size for x in ctrls)} inside the pool")
    return c


def reflection(d, base, r):
    """ResShaderReflectionStageData (0x60): dictionaries of names and the slot (binding) of each one."""
    o = base + r
    dics = [resdic(d, base, u64(d, o + k * 8)) for k in range(5)] + [resdic(d, base, u64(d, o + 0x50))]
    offs = [0, u32(d, o + 0x28), u32(d, o + 0x2C), u32(d, o + 0x30), u32(d, o + 0x34), u32(d, o + 0x4C)]
    slots = u64(d, o + 0x38)
    wg = struct.unpack_from("<3I", d, o + 0x40)
    parts = []
    for label, names, first in zip(("in", "out", "sampler", "cbuf", "ssbo", "image"), dics, offs):
        if names:
            parts.append(label + "[" + ", ".join(f"{n}@{s32(d, base + slots + (first + i) * 4)}"
                                                 for i, n in enumerate(names)) + "]")
    if any(wg):
        parts.append(f"workgroup {wg}")
    return " ".join(parts)


# ---------------------------------------------------------------------------------------------------------------
# FSHA: NintendoWare G3d2 ResShaderArchive (.bfsha)

def parse_fsha(d, base, path, reflect=False):
    if bytes(d[base:base + 8]) != b"FSHA    ":
        raise ValueError(f"no FSHA at {base:#x}")
    ver = u32(d, base + 8)
    size = u32(d, base + 0x1C)
    c = Container("FSHA", path, base, size)
    arc = base + u64(d, base + 0x20)                        # ResShaderArchiveData
    name = binstr(d, base + u64(d, arc))
    models, nmodels = u64(d, arc + 0x10), u16(d, arc + 0x40)
    c.info.append(f"archive '{name}', version {ver >> 16}.{(ver >> 8) & 0xFF}.{ver & 0xFF}, {nmodels} shading models")
    for m in range(nmodels):
        o = base + models + m * 0x100                       # ResShadingModelData, 0x100 bytes
        mname = binstr(d, base + u64(d, o))
        dic = {label: resdic(d, base, u64(d, o + k)) for label, k in
               (("static options", 0x10), ("dynamic options", 0x20), ("attributes", 0x30), ("samplers", 0x40),
                ("images", 0x50), ("uniform blocks", 0x60), ("storage buffers", 0x78))}
        progs, keys, bnsh = u64(d, o + 0x88), u64(d, o + 0x90), u64(d, o + 0xA8)
        nstatic, ndynamic, nprog = u16(d, o + 0xE4), u16(d, o + 0xE6), u16(d, o + 0xE8)
        key_static, key_dynamic = d[o + 0xEA], d[o + 0xEB]
        c.named += nprog
        c.info.append(f"model {m} '{mname}': {nprog} programs, {nstatic} static + {ndynamic} dynamic options, "
                      f"key {key_static}+{key_dynamic} words at {keys:#x}, BNSH at {bnsh:#x}")
        c.info.append("  " + ", ".join(f"{len(v)} {k}" for k, v in dic.items()))
        c.info.append("  uniform blocks: " + " ".join(dic["uniform blocks"]))
        sub = parse_bnsh(d, base + bnsh, f"{path}/{mname}", reflect)
        # Each program (0x40 bytes) points at one variation of the model's BNSH (+0x20, file offset).
        var_array = u64(d, base + bnsh + u16(d, base + bnsh + 0x16) + 0x20)
        index = []
        for k in range(nprog):
            v = u64(d, base + progs + k * 0x40 + 0x20) - bnsh - var_array
            if v % 0x40 or not 0 <= v // 0x40 < len(sub.programs):
                raise ValueError(f"model {mname} program {k} points outside its BNSH")
            index.append(v // 0x40)
        c.info.append(f"  programs -> variations: {len(set(index))} distinct of {len(sub.programs)}, "
                      f"{'in order' if index == sorted(index) else 'not in order'}")
        c.children.append(sub)
    return c


# ---------------------------------------------------------------------------------------------------------------
# VFXB: NintendoWare Vfx2 effect binary

def parse_vfxb(d, base, path, reflect=False):
    """Sections of 0x20-byte headers: magic, u32 size, u32 child offset, u32 next offset (both relative to the
    section, 0xffffffff = none), u32 attribute offset, u32 data offset, u32 0, u16 child count.
    GRSR (graphics) and GRSC (compute) hold a BNSH at their data offset."""
    if bytes(d[base:base + 8]) != b"VFXB    ":
        raise ValueError(f"no VFXB at {base:#x}")
    size = u32(d, base + 0x1C)
    c = Container("VFXB", path, base, size)
    names = []
    todo = [base + u16(d, base + 0x16)]
    seen = set()
    while todo:
        s = todo.pop()
        # The last child's "next" does not always say none (GTNT, G3NT): a chain ends at the first entry
        # whose magic is not four letters/digits.
        while s not in seen and s + 0x20 <= base + size and bytes(d[s:s + 4]).isalnum():
            seen.add(s)
            magic = bytes(d[s:s + 4]).decode("ascii")
            child, nxt, data_off = u32(d, s + 8), u32(d, s + 12), u32(d, s + 20)
            names.append(magic)
            if magic in ("GRSR", "GRSC") and bytes(d[s + data_off:s + data_off + 4]) == b"BNSH":
                c.children.append(parse_bnsh(d, s + data_off, f"{path}/{magic}", reflect))
            if child != 0xFFFFFFFF:
                todo.append(s + child)
            if nxt == 0xFFFFFFFF:
                break
            s += nxt
    count = collections.Counter(names)
    c.info.append("sections: " + ", ".join(f"{n} {k}" for k, n in sorted(count.items())))
    return c


# ---------------------------------------------------------------------------------------------------------------
# BAHS: agl ShaderArchive binary (.sharcb), version 9. No public layout; everything here is read from our file.

BAHS_STAGES = {0: "vertex", 1: "pixel", 2: "geometry", 3: "compute"}   # record stage = bit in the program mask


def parse_bahs(d, base, path, reflect=False):
    """Header: "BAHS", u32 version, u32 size, u32 ?, u32 0, u32 records offset, u32 code size, u32 code offset.
    Records area (in every file seen, 0x150 bytes after the records offset): u32 size, u32 count, then `count`
    records of
      u32 size, u32 stage (0 vertex, 1 fragment, 3 compute), u32 0, u32 size - 0x10,
      u32 code offset (from the code area), u32 0, u32 control size, s32 control offset (from record + 0x10;
      negative when the control section is shared with an earlier record), resource lists...
    followed by the program table: u32 size, u32 count, programs of
      u32 size, u32 name length, u32 stage mask (bit n = record stage n), u32 first record, name, then three
      lists (u32 size, u32 count, entries); entries of the first list are the variation macros:
      u32 size, u32 name length, u32 value count, u32 ?, name, values.
    A program has product(value counts) variations, each one record per stage in its mask, in record order.
    """
    if bytes(d[base:base + 4]) != b"BAHS":
        raise ValueError(f"no BAHS at {base:#x}")
    ver, size = u32(d, base + 4), u32(d, base + 8)
    rec_area, code_size, code_area = u32(d, base + 0x14), u32(d, base + 0x18), u32(d, base + 0x1C)
    c = Container("BAHS", path, base, size)
    o = base + rec_area
    while not u32(d, o):                                    # skip the zeros before the (size, count) header
        o += 4
    area_size, count = u32(d, o), u32(d, o + 4)
    end = o + area_size
    r = o + 8
    records = []
    for k in range(count):
        rsize, stage, code, ctrl_size, ctrl_rel = (u32(d, r), u32(d, r + 4), u32(d, r + 0x10), u32(d, r + 0x18),
                                                   s32(d, r + 0x1C))
        records.append((stage, base + code_area + code, r + 0x10 + ctrl_rel, ctrl_size))
        r += rsize
    if r != end:
        raise ValueError(f"BAHS records end at {r:#x}, expected {end:#x}")
    pcount = u32(d, end + 4)                                # program table right after the records
    p = end + 8
    programs = []
    for k in range(pcount):
        esize, nlen, mask, first = u32(d, p), u32(d, p + 4), u32(d, p + 8), u32(d, p + 12)
        name = cstr(d, p + 16)
        q = p + 16 + nlen
        variations = 1
        macros = []
        e = q + 8
        for _ in range(u32(d, q + 4)):                      # first list: variation macros
            msize, nvals = u32(d, e), u32(d, e + 8)
            variations *= nvals
            macros.append(f"{cstr(d, e + 16)}x{nvals}")
            e += msize
        programs.append((name, mask, first, variations, macros))
        p += esize
    c.named = pcount
    c.info.append(f"version {ver}, {count} stage records, {len(set(x[2] for x in records))} distinct control "
                  f"sections, code area {code_area:#x} ({code_size:#x} bytes), {pcount} programs")
    for i, (name, mask, first, nvar, macros) in enumerate(programs):
        last = programs[i + 1][2] if i + 1 < len(programs) else count
        want = [b for b in range(8) if mask >> b & 1]
        c.info.append(f"  {name}: {'+'.join(BAHS_STAGES.get(b, str(b)) for b in want)}, records {first}..{last - 1}, "
                      f"{nvar} variations ({' '.join(macros) or 'no macros'})")
        if last - first != len(want) * nvar:
            c.info.append(f"  !! {name}: {last - first} records, expected {len(want) * nvar}")
        for v in range(nvar):
            stages = []
            for j, b in enumerate(want):
                k = first + v * len(want) + j
                if k >= last:
                    break
                stage, code, ctrl, ctrl_size = records[k]
                kind = BAHS_STAGES.get(stage, f"stage{stage}")
                if stage != b:
                    c.info.append(f"  !! record {k}: stage {stage}, program mask says {b}")
                csize = u32(d, ctrl + 0x704)                # code block size, from the control section (see docs)
                stages.append(Stage(f"{path}/{name}/v{v:03d}/{kind}", kind, code, csize, ctrl, ctrl_size, d))
            c.programs.append((f"{name}/v{v:03d}", stages))
            c.variations += 1
    # As in BNSH, the distinct code blocks (sized from their control sections) should lie back to back.
    blocks = sorted({(s.code_off, s.code_size) for _, ss in c.programs for s in ss})
    at, gaps = base + code_area, 0
    for off, n in blocks:
        gaps += off != at
        at = off + n
    c.info.append(f"{len(blocks)} distinct code blocks, {'back to back' if not gaps else f'{gaps} gaps'}, "
                  f"{'inside' if at <= base + code_area + code_size else 'PAST'} the code area")
    return c


# ---------------------------------------------------------------------------------------------------------------
# SARC and dispatch

def sarc_files(d, base):
    """SARC: header 0x14 (u32 data offset at +0xc), SFAT (node count at +0x1a; nodes of u32 hash, u32 attribute
    (name offset / 4 in the low 16 bits when bit 24 is set), u32 start, u32 end), SFNT names after the nodes."""
    data = base + u32(d, base + 0xC)
    n = u16(d, base + 0x1A)
    names = base + 0x20 + n * 0x10 + 8
    out = []
    for k in range(n):
        _, attr, start, end = struct.unpack_from("<IIII", d, base + 0x20 + k * 0x10)
        name = cstr(d, names + (attr & 0xFFFF) * 4) if attr >> 24 else f"{k:04d}.bin"
        out.append((name, data + start, end - start))
    return out


def open_blob(d, base, size, path, reflect=False):
    """Recognise what is at `base` and return a Container (with children), or None."""
    magic = bytes(d[base:base + 8])
    if magic[:4] == b"SARC":
        c = Container("SARC", path, base, size)
        for name, off, n in sarc_files(d, base):
            sub = open_blob(d, off, n, f"{path}/{name}", reflect)
            if sub:
                c.children.append(sub)
        return c if c.children else None
    if magic == b"FSHA    ":
        return parse_fsha(d, base, path, reflect)
    if magic == b"BNSH\0\0\0\0":
        return parse_bnsh(d, base, path, reflect)
    if magic == b"VFXB    ":
        return parse_vfxb(d, base, path, reflect)
    if magic[:4] == b"BAHS":
        return parse_bahs(d, base, path, reflect)
    if magic[:2] in (b"YB", b"BY"):
        # BYML: the effect binary is stored as a binary value; find it by magic and trust its own header.
        c = Container("BYML", path, base, size)
        for m in re.finditer(re.escape(b"VFXB    "), bytes(d[base:base + size])):
            c.children.append(parse_vfxb(d, base + m.start(), f"{path}/VFXB@{m.start():#x}", reflect))
        return c if c.children else None
    return None


def read_file(path):
    raw = open(path, "rb").read()
    if path.endswith(".zs"):
        try:
            import zstandard
            raw = zstandard.ZstdDecompressor().decompress(raw, max_output_size=1 << 31)
        except ImportError:
            raw = subprocess.run(["zstd", "-dc", path], capture_output=True, check=True).stdout
    return raw


def walk(c):
    yield c
    for sub in c.children:
        yield from walk(sub)


def all_stages(c):
    for x in walk(c):
        for _, stages in x.programs:
            yield from stages


def describe(c, depth=0):
    pad = "  " * depth
    print(f"{pad}{c.kind} {c.path} @ {c.off:#x} ({c.size:#x} bytes)")
    for line in c.info:
        print(f"{pad}  {line}")
    if c.programs:
        stages = [s for _, ss in c.programs for s in ss]
        kinds = collections.Counter(s.kind for s in stages)
        print(f"{pad}  {len(c.programs)} variations, {c.variations} with code, stages: "
              + ", ".join(f"{n} {k}" for k, n in kinds.items())
              + f"; code {sum(s.code_size for s in stages):#x} bytes, "
              f"{len({s.code_off for s in stages})} distinct code blocks, {len({s.hash for s in stages})} distinct by content")
        bad = [s for s in stages if s.problems]
        print(f"{pad}  layout check: {'all stages OK' if not bad else f'{len(bad)} stages FAIL'}")
        for s in bad[:10]:
            print(f"{pad}    {s.id}: {', '.join(s.problems)}")
    for sub in c.children:
        describe(sub, depth + 1)


def safe_name(sid):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", sid).strip("_")


def extract(c, outdir, data, unique=False):
    """Write every stage's code block and control section, and manifest.tsv (one line per stage).

    Default: <id>.code.bin / <id>.ctrl.bin per stage. --unique: code_<hash>.bin / ctrl_<hash>.bin, each written
    once (variations share code heavily: the uber shader has 17,160 stages but 4,857 distinct code blocks).
    """
    real = os.path.realpath(outdir)
    if not real.startswith(os.path.realpath(os.path.join(REPO, "private")) + os.sep):
        sys.exit(f"refusing to write outside private/: {outdir}")
    os.makedirs(real, exist_ok=True)
    written = set()
    with open(os.path.join(real, "manifest.tsv"), "w", newline="\n") as m:  # LF on Windows too
        m.write("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
                "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")
        for s in all_stages(c):
            code = data[s.code_off:s.code_off + s.code_size]
            ctrl = data[s.ctrl_off:s.ctrl_off + s.ctrl_size]
            if unique:
                code_file = f"code_{s.hash}.bin"
                ctrl_file = f"ctrl_{hashlib.sha1(bytes(ctrl)).hexdigest()[:16]}.bin"
            else:
                code_file = safe_name(s.id) + ".code.bin"
                ctrl_file = safe_name(s.id) + ".ctrl.bin"
            for name, blob in ((code_file, code), (ctrl_file, ctrl)):
                if name not in written:
                    written.add(name)
                    open(os.path.join(real, name), "wb").write(blob)
            m.write(f"{s.id}\t{s.kind}\t{s.code_off:#x}\t{s.code_size:#x}\t{s.ctrl_off:#x}\t{s.ctrl_size:#x}\t"
                    f"{s.start:#x}\t{s.length:#x}\t{s.const_off:#x}\t{s.const_size:#x}\t{s.hash}\t{code_file}\t"
                    f"{ctrl_file}\n")
    return len(written)


def inventory(paths):
    """Totals over many files: containers, programs, variations, stages by kind, code bytes.

    programs   = what the engine names: FSHA shading-model programs and BAHS programs
    variations = compiled programs with code: BNSH variations and BAHS variations
    code bytes = sum over stages (a shared block counts once per stage using it); distinct = each block once
    """
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if os.path.isfile(os.path.join(p, f))]
        else:
            files.append(p)
    total = collections.Counter()
    kinds = collections.Counter()
    blocks, ctrls = {}, {}
    print(f"{'file':56} {'containers':>10} {'programs':>8} {'variations':>10} {'stages':>7} {'code bytes':>11}")
    for f in files:
        data = read_file(f)
        c = open_blob(data, 0, len(data), os.path.basename(f))
        if not c:
            total["files without shaders"] += 1
            continue
        conts = collections.Counter(x.kind for x in walk(c))
        named = sum(x.named for x in walk(c))
        variations = sum(x.variations for x in walk(c))
        stages = list(all_stages(c))
        code = sum(s.code_size for s in stages)
        for s in stages:
            kinds[s.kind] += 1
            total["layout check failures"] += bool(s.problems)
            blocks[s.hash] = s.code_size
            ctrls[hashlib.sha1(bytes(data[s.ctrl_off:s.ctrl_off + s.ctrl_size])).hexdigest()] = s.ctrl_size
        for k, n in conts.items():
            total["container " + k] += n
        total["files with shaders"] += 1
        total["programs"] += named
        total["variations"] += variations
        total["stages"] += len(stages)
        total["code bytes"] += code
        print(f"{os.path.basename(f)[:56]:56} {sum(conts.values()):10} {named:8} {variations:10} {len(stages):7} {code:11}")
    print()
    for k in sorted(total):
        print(f"  {k:28} {total[k]}")
    for k, n in kinds.most_common():
        print(f"  {'stages ' + k:28} {n}")
    print(f"  {'distinct code blocks':28} {len(blocks)} ({sum(blocks.values())} bytes, by content)")
    print(f"  {'distinct control sections':28} {len(ctrls)} ({sum(ctrls.values())} bytes, by content)")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--inventory":
        inventory(args[1:])
        return
    path, outdir = args[0], None
    if "--extract" in args:
        outdir = args[args.index("--extract") + 1]
    data = read_file(path)
    c = open_blob(data, 0, len(data), os.path.basename(path), reflect="--reflect" in args)
    if not c:
        sys.exit(f"{path}: no shader container recognised")
    describe(c)
    if "--list" in args:
        for s in all_stages(c):
            print(f"{s.id}\t{s.kind}\tcode {s.code_off:#x}+{s.code_size:#x}\tctrl {s.ctrl_off:#x}+{s.ctrl_size:#x}\t{s.hash}")
    if outdir:
        files = extract(c, outdir, data, unique="--unique" in args)
        print(f"extracted {sum(1 for _ in all_stages(c))} stages ({files} files) to {outdir}")


if __name__ == "__main__":
    main()
