"""Switch NSO loader: segments, MOD0, .dynamic, symbols and relocations.

Addresses are module-relative (the module is linked at 0 and relocated by rtld).
"""
import struct
from dataclasses import dataclass, field

import lz4.block

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_PLTGOT, DT_HASH, DT_STRTAB, DT_SYMTAB = 0, 1, 2, 3, 4, 5, 6
DT_RELA, DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT = 7, 8, 9, 10, 11
DT_INIT, DT_FINI, DT_REL, DT_JMPREL = 12, 13, 17, 23
DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28
DT_GNU_HASH = 0x6FFFFEF5

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027

STT_FUNC = 2
SHN_UNDEF = 0


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int
    bind: int
    shndx: int

    @property
    def imported(self):
        return self.shndx == SHN_UNDEF


@dataclass
class Reloc:
    offset: int
    type: int
    sym: int
    addend: int


@dataclass
class Module:
    name: str
    image: bytearray
    text: tuple  # (start, size)
    ro: tuple
    data: tuple
    bss: tuple
    symbols: list = field(default_factory=list)
    relocs: list = field(default_factory=list)
    dynamic: dict = field(default_factory=dict)
    init_array: list = field(default_factory=list)
    fini_array: list = field(default_factory=list)
    eh_frame_hdr: tuple = (0, 0)

    def u32(self, addr):
        return struct.unpack_from("<I", self.image, addr)[0]

    def u64(self, addr):
        return struct.unpack_from("<Q", self.image, addr)[0]

    def in_text(self, addr):
        return self.text[0] <= addr < self.text[0] + self.text[1]

    def in_image(self, addr):
        return 0 <= addr < len(self.image)

    def cstr(self, addr):
        end = self.image.index(b"\0", addr)
        return self.image[addr:end].decode("utf-8", "replace")


def load(path, name=None):
    raw = open(path, "rb").read()
    if raw[:4] != b"NSO0":
        raise ValueError(f"{path}: not an NSO")
    flags = struct.unpack_from("<I", raw, 0x0C)[0]
    segs = []
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
        comp = struct.unpack_from("<I", raw, 0x60 + i * 4)[0]
        blob = raw[file_off:file_off + comp]
        if flags & (1 << i):
            blob = lz4.block.decompress(blob, uncompressed_size=size)
        segs.append((mem_off, blob))
    bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
    data_end = segs[2][0] + len(segs[2][1])
    image = bytearray(data_end)
    for off, blob in segs:
        image[off:off + len(blob)] = blob
    m = Module(
        name=name or path,
        image=image,
        text=(segs[0][0], len(segs[0][1])),
        ro=(segs[1][0], len(segs[1][1])),
        data=(segs[2][0], len(segs[2][1])),
        bss=(data_end, bss_size),
    )
    _parse_mod0(m)
    return m


def _parse_mod0(m):
    mod0 = m.u32(4)
    if m.image[mod0:mod0 + 4] != b"MOD0":
        raise ValueError("MOD0 not found")
    dyn_off, bss_start, bss_end, eh_start, eh_end = (
        struct.unpack_from("<i", m.image, mod0 + 4 + i * 4)[0] + mod0 for i in range(5)
    )
    m.bss = (bss_start, bss_end - bss_start)
    m.eh_frame_hdr = (eh_start, eh_end - eh_start)

    dyn = {}
    addr = dyn_off
    while True:
        tag, val = struct.unpack_from("<qQ", m.image, addr)
        addr += 16
        if tag == DT_NULL:
            break
        dyn.setdefault(tag, val)
    m.dynamic = dyn

    strtab, symtab = dyn[DT_STRTAB], dyn[DT_SYMTAB]
    syment = dyn.get(DT_SYMENT, 24)
    # nsyms: symtab normally sits right before strtab.
    nsyms = (strtab - symtab) // syment
    for i in range(nsyms):
        st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", m.image, symtab + i * syment)
        m.symbols.append(Symbol(m.cstr(strtab + st_name), st_value, st_size,
                                st_info & 0xF, st_info >> 4, st_shndx))

    def relocs(start, size):
        for a in range(start, start + size, 24):
            off, info, addend = struct.unpack_from("<QQq", m.image, a)
            m.relocs.append(Reloc(off, info & 0xFFFFFFFF, info >> 32, addend))

    if DT_RELA in dyn:
        relocs(dyn[DT_RELA], dyn[DT_RELASZ])
    if DT_JMPREL in dyn:
        relocs(dyn[DT_JMPREL], dyn[DT_PLTRELSZ])

    # init/fini arrays hold RELATIVE-relocated pointers: read the addends.
    rel_at = {r.offset: r for r in m.relocs}

    def ptr_array(start, size):
        out = []
        for a in range(start, start + size, 8):
            r = rel_at.get(a)
            out.append(r.addend if r and r.type == R_AARCH64_RELATIVE else m.u64(a))
        return out

    if DT_INIT_ARRAY in dyn:
        m.init_array = ptr_array(dyn[DT_INIT_ARRAY], dyn[DT_INIT_ARRAYSZ])
    if DT_FINI_ARRAY in dyn:
        m.fini_array = ptr_array(dyn[DT_FINI_ARRAY], dyn[DT_FINI_ARRAYSZ])


def apply_relative(m):
    """Write RELATIVE addends into the image (module base 0) so data pointers are readable statically."""
    for r in m.relocs:
        if r.type == R_AARCH64_RELATIVE:
            struct.pack_into("<Q", m.image, r.offset, r.addend & 0xFFFFFFFFFFFFFFFF) if r.offset + 8 <= len(m.image) else None


def plt_imports(m):
    """GOT slot address -> imported symbol name (JUMP_SLOT and GLOB_DAT)."""
    out = {}
    for r in m.relocs:
        if r.type in (R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT, R_AARCH64_ABS64) and r.sym:
            s = m.symbols[r.sym]
            if s.imported:
                out[r.offset] = s.name
    return out
