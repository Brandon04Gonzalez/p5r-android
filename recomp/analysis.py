"""Whole-module analysis: functions, PLT stubs, jump tables.

Works on capstone's lite disassembly (mnemonic + operand string), which is fast enough for the whole text.
"""
import bisect
import re
from dataclasses import dataclass, field

import capstone

from . import ehframe, nso

BRANCH_OUT = {"b", "bl", "cbz", "cbnz", "tbz", "tbnz"}
COND_B = re.compile(r"^b\.(\w+)$")
IMM = re.compile(r"#(-?0x[0-9a-f]+|-?\d+)")


def _imm(s):
    return int(s, 0)


def _regno(r):
    """x9 / w9 -> 9; sp/xzr etc -> name."""
    r = r.strip()
    if r in ("fp", "x29", "w29"):
        return 29
    if r in ("lr", "x30", "w30"):
        return 30
    m = re.fullmatch(r"[xw](\d+)", r)
    return int(m.group(1)) if m else r


def _ops(op_str):
    """Split an operand string on top-level commas (keeps [..] and {..} together)."""
    out, depth, cur = [], 0, ""
    for ch in op_str:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


# Mnemonics whose first operand is not a destination register.
NO_DEST = {"str", "strb", "strh", "stur", "sturb", "sturh", "stp", "stlr", "stlrb", "stlrh", "st1", "st2", "st3", "st4",
           "cmp", "cmn", "tst", "fcmp", "fcmpe", "ccmp", "ccmn", "fccmp", "b", "bl", "br", "blr", "ret", "cbz", "cbnz",
           "tbz", "tbnz", "prfm", "nop", "udf", "brk", "svc", "msr", "dmb", "dsb", "isb", "clrex", "hint"}


@dataclass
class Function:
    start: int
    end: int
    fde: bool = False
    name: str = ""


@dataclass
class Analysis:
    module: nso.Module
    insns: dict  # addr -> (mnemonic, op_str)
    functions: list = field(default_factory=list)  # sorted by start
    plt: dict = field(default_factory=dict)  # stub addr -> import name
    got_imports: dict = field(default_factory=dict)  # got slot -> import name
    jump_tables: dict = field(default_factory=dict)  # br addr -> sorted target list
    names: dict = field(default_factory=dict)  # addr -> symbol name
    unresolved_br: list = field(default_factory=list)

    def function_at(self, addr):
        starts = [f.start for f in self.functions]
        i = bisect.bisect_right(starts, addr) - 1
        return self.functions[i] if i >= 0 and addr < self.functions[i].end else None


def analyze(m, log=print):
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.skipdata = True
    t0, tn = m.text
    insns = {}
    for a, _, mn, op in cs.disasm_lite(bytes(m.image[t0:t0 + tn]), t0):
        if mn != ".byte":
            insns[a] = (mn, op)
    an = Analysis(m, insns)
    text_end = t0 + tn

    an.got_imports = nso.plt_imports(m)
    for s in m.symbols:
        if s.name and not s.imported and m.in_text(s.value):
            an.names[s.value] = s.name

    # PLT stubs: adrp x16, page / ldr x17, [x16, #off] / add x16, x16, #off / br x17
    for a, (mn, op) in insns.items():
        if mn == "adrp" and op.startswith("x16,"):
            nxt = [insns.get(a + 4 * k, ("", "")) for k in (1, 2, 3)]
            if nxt[0][0] == "ldr" and nxt[0][1].startswith("x17, [x16") and nxt[2] == ("br", "x17"):
                page = _imm(IMM.search(op).group(1))
                off = IMM.search(nxt[0][1])
                got = page + (_imm(off.group(1)) if off else 0)
                an.plt[a] = an.got_imports.get(got, f"got_{got:x}")

    # Seeds.
    fdes = [f for f in ehframe.parse(m) if t0 <= f.start < text_end]
    entries = {f.start for f in fdes} | set(an.plt) | set(an.names)
    entries |= {a for a in m.init_array + m.fini_array if t0 <= a < text_end}
    for r in m.relocs:
        if r.type == nso.R_AARCH64_RELATIVE and t0 <= r.addend < text_end and r.addend in insns:
            entries.add(r.addend)
    for a, (mn, op) in insns.items():
        if mn == "bl":
            entries.add(_imm(IMM.search(op).group(1)))
        elif mn == "adr":
            t = _imm(IMM.search(op).group(1))
            if t0 <= t < text_end and t in insns:
                entries.add(t) if _looks_like_code_pointer(an, a, t) else None
    entries.add(t0) if t0 in insns else None
    fde_bounds = {f.start: f.start + f.size for f in fdes}
    log(f"  seeds: {len(fdes)} FDE, {len(an.plt)} PLT stubs, {len(entries)} entries")

    # Fixpoint: build ranges, resolve jump tables, promote out-of-range branch targets to entries.
    for it in range(20):
        an.functions = _ranges(an, sorted(entries), fdes, fde_bounds)
        an.jump_tables = {}
        new = set()
        for f in an.functions:
            if f.start in an.plt:
                continue
            for a in range(f.start, f.end, 4):
                ins = insns.get(a)
                if not ins:
                    continue
                mn, op = ins
                if mn in BRANCH_OUT or COND_B.match(mn):
                    if mn == "bl":
                        continue
                    t = _imm(IMM.findall(op)[-1])
                    if not (f.start <= t < f.end) and t in insns and t not in entries:
                        new.add(t)
                elif mn == "br":
                    tg = _jump_table(an, f, a)
                    if tg is not None:
                        an.jump_tables[a] = tg
        log(f"  pass {it}: {len(an.functions)} functions, {len(an.jump_tables)} jump tables, +{len(new)} entries")
        if not new:
            break
        entries |= new

    an.unresolved_br = [a for a, (mn, _) in insns.items()
                        if mn == "br" and a not in an.jump_tables and not _in_plt(an, a)]
    return an


def _in_plt(an, a):
    return any(s <= a < s + 16 for s in (a - 12, a - 8, a - 4, a) if s in an.plt)


def _looks_like_code_pointer(an, a, t):
    # adr used for jump-table anchors is followed by an add ..., lsl #2 into a br; those are not functions.
    for k in range(1, 4):
        mn, op = an.insns.get(a + 4 * k, ("", ""))
        if mn == "add" and "lsl #2" in op:
            return False
    return True


def _ranges(an, entries, fdes, fde_bounds):
    """Function ranges: FDE bounds when known, otherwise up to the next entry. Entries inside an FDE function
    become separate functions that run to that FDE's end (shared tails)."""
    t0, tn = an.module.text
    text_end = t0 + tn
    fde_starts = [f.start for f in fdes]
    funcs = []
    for i, e in enumerate(entries):
        if e in an.plt:
            funcs.append(Function(e, e + 16))
            continue
        if e in fde_bounds:
            funcs.append(Function(e, fde_bounds[e], fde=True))
            continue
        # Inside an FDE function?
        j = bisect.bisect_right(fde_starts, e) - 1
        if j >= 0 and e < fdes[j].start + fdes[j].size:
            funcs.append(Function(e, fdes[j].start + fdes[j].size))
            continue
        nxt = entries[i + 1] if i + 1 < len(entries) else text_end
        k = bisect.bisect_right(fde_starts, e)
        if k < len(fde_starts):
            nxt = min(nxt, fde_starts[k])
        funcs.append(Function(e, nxt))
    for f in funcs:
        f.name = an.names.get(f.start, "")
    return funcs


def _find_def(an, f, before, reg):
    """Last instruction in f before `before` (linear, backwards) that writes register number `reg`."""
    a = before - 4
    while a >= f.start:
        mn, op = an.insns.get(a, ("", ""))
        if mn and mn not in NO_DEST:
            ops = _ops(op)
            if ops and _regno(ops[0]) == reg:
                return a, mn, ops
            # ldp writes two registers.
            if mn in ("ldp", "ldpsw") and len(ops) > 1 and _regno(ops[1]) == reg:
                return a, mn, ops
        a -= 4
    return None


def _const(an, f, before, reg, depth=0):
    """Constant value of `reg` at `before` if it comes from adrp[+add] / adr / mov."""
    d = _find_def(an, f, before, reg)
    if not d or depth > 4:
        return None
    a, mn, ops = d
    if mn in ("adr", "adrp"):
        return _imm(IMM.search(ops[1]).group(1))
    if mn == "add" and len(ops) == 3 and ops[2].startswith("#") and _regno(ops[1]) != "sp":
        base = _const(an, f, a, _regno(ops[1]), depth + 1)
        return None if base is None else base + _imm(ops[2][1:])
    if mn == "mov" and ops[1].startswith("#"):
        return _imm(ops[1][1:])
    if mn == "mov":
        return _const(an, f, a, _regno(ops[1]), depth + 1)
    return None


def _jump_table(an, f, br):
    """Targets of a compressed clang jump table ending at `br`, or None."""
    m = an.module
    reg = _regno(an.insns[br][1])
    d = _find_def(an, f, br, reg)
    if not d or d[1] != "add" or len(d[2]) < 3:
        return None
    a_add, _, ops = d
    shift = 0
    mshift = re.search(r"(lsl|sxtw|uxtw) #(\d+)", ",".join(ops[2:]))
    if mshift:
        shift = int(mshift.group(2))
    # Either operand can be the loaded entry; the other one is the anchor (anchor + entry << shift).
    ld = anchor = None
    for ent_op, anc_op in ((ops[2], ops[1]), (ops[1], ops[2])):
        cand = _find_def(an, f, a_add, _regno(ent_op))
        if cand and cand[1].startswith("ldr") and "[" in ",".join(cand[2]):
            ld = cand
            anchor = _const(an, f, a_add, _regno(anc_op))
            if anc_op is ops[2] and shift:
                return None  # shifted anchor: not a jump table shape we know
            break
    if ld is None or anchor is None:
        return None
    a_ld, ld_mn, ld_ops = ld
    size = {"ldrb": 1, "ldrh": 2, "ldrsb": 1, "ldrsh": 2, "ldrsw": 4, "ldr": 4}.get(ld_mn)
    signed = ld_mn in ("ldrsb", "ldrsh", "ldrsw")
    mem = re.match(r"\[(\w+)(?:, (\w+))?", ld_ops[1])
    if not size or not mem or not mem.group(2):
        return None
    base_reg = _regno(mem.group(1))
    idx = _regno(mem.group(2))
    count = _bound(an, f, a_ld, idx)

    def entries(table):
        """(targets, misses) of the table at `table`."""
        # Without a bound, take every entry that lands on an instruction of this function (up to 256): a superset
        # only adds labels, while a missing target would be a call into the middle of the function at run time.
        limit = count if count is not None else 256
        out = []
        misses = 0
        for k in range(limit):
            p = table + k * size
            if p + size > len(m.image):
                break
            v = int.from_bytes(m.image[p:p + size], "little", signed=signed)
            t = anchor + (v << shift)
            if not (f.start <= t < f.end) or t not in an.insns:
                misses += 1
                if count is None and misses > 4:
                    break
                continue
            out.append(t)
        return out, misses

    table = _const(an, f, a_ld, base_reg)
    if table is not None and m.in_image(table):
        out, _ = entries(table)
        return sorted(set(out)) if out else None
    # The base register's last definition in address order is not a constant (control flow reaches the load from
    # another path: Wonder's main+28b3bc takes x24 from an adrp/add 0x5000 bytes earlier, the linear search found an
    # unrelated movk). With a known bound, try every constant the function puts in that register and keep one whose
    # entries all land on instructions of this function.
    if count is None:
        return None
    candidates = []
    a = f.start
    while a < f.end:
        mn, op = an.insns.get(a, ("", ""))
        ops = _ops(op)
        if mn in ("adrp", "adr", "add") and ops and _regno(ops[0]) == base_reg:
            c = _const(an, f, a + 4, base_reg)
            if c is not None and m.in_image(c) and c not in candidates:
                candidates.append(c)
        a += 4
    # Several constants can pass that test (main+28b3bc: an add x24, x23, #0x90 as well as the real adrp/add), and
    # extra labels are harmless (a case only fires on its exact target), so the union of all of them is taken.
    found = set()
    for c in candidates:
        out, misses = entries(c)
        if out and misses == 0:
            found.update(out)
    return sorted(found) if found else None


def _bound(an, f, before, idx):
    """Number of table entries from `cmp idx, #N` + b.hi/b.hs before the load. The index may be a zero/sign
    extended copy of the compared register (and x8, x22, #0xffffffff / mov / uxtw / sxtw)."""
    regs = {idx}
    a = before - 4
    while a >= max(f.start, before - 4 * 40):
        mn, op = an.insns.get(a, ("", ""))
        ops = _ops(op)
        copy = (len(ops) >= 2 and ops[1][0] in "xw" and
                (mn in ("mov", "uxtw", "sxtw") or
                 (mn == "and" and len(ops) == 3 and ops[2] in ("#0xffffffff", "#4294967295"))))
        if copy and _regno(ops[0]) in regs:
            # Before this point the index lives in the source register.
            regs.discard(_regno(ops[0]))
            regs.add(_regno(ops[1]))
            a -= 4
            continue
        if mn == "cmp" and len(ops) == 2 and _regno(ops[0]) in regs and ops[1].startswith("#"):
            n = _imm(ops[1][1:])
            # Find the branch that uses these flags; a cmp guarding anything else (b.ne, ...) is no bound.
            for k in range(1, 6):
                bm = an.insns.get(a + 4 * k, ("", ""))[0]
                if bm == "b.hi":
                    return n + 1
                if bm == "b.hs":
                    return n
            return None
        if mn and mn not in NO_DEST and ops and _regno(ops[0]) in regs:
            # The index was produced here (e.g. ldrb w8, [x27, #3]) with no bounds check after it: an older cmp of
            # the same register guards another value. Wonder's main+22e5f8 lost a case to exactly that (the cmp
            # w8, #2 of a previous branch capped the table at 3 entries).
            return None
        a -= 4
    return None
