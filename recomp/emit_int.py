"""Integer data processing: moves, arithmetic and logic, shifts, bitfields, extends, conditional selects and compares,
multiplies and divides, bit and byte reversal. Mixed into emit.Emitter (split from emit.py, 700-line rule)."""

from capstone import arm64_const as A

from .emit import B, Unhandled, u, s  # noqa: F401


class IntOps:
    # ---- integer data processing --------------------------------------------------------------------------

    def i_mov(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp or (ops[1].type == A.ARM64_OP_REG and not self.regop(i, ops[1]).gp):
            return self.v_mov(f, i, ops)
        if ops[1].type == A.ARM64_OP_IMM:
            return d.w(f"{ops[1].imm & ((1 << d.bits) - 1):#x}ull")
        return d.w(self.regop(i, ops[1]).r())

    def i_movz(self, f, i, ops):
        d = self.regop(i, ops[0])
        v = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
        return d.w(f"{v & ((1 << d.bits) - 1):#x}ull")

    def i_movn(self, f, i, ops):
        d = self.regop(i, ops[0])
        v = ~(ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0))
        return d.w(f"{v & ((1 << d.bits) - 1):#x}ull")

    def i_movk(self, f, i, ops):
        d = self.regop(i, ops[0])
        sh = ops[1].shift.value if ops[1].shift.type else 0
        mask = (0xFFFF << sh) & ((1 << d.bits) - 1)
        return d.w(f"({d.r()} & {~mask & ((1 << d.bits) - 1):#x}ull) | {(ops[1].imm & 0xFFFF) << sh:#x}ull")

    def i_adrp(self, f, i, ops):
        return self.regop(i, ops[0]).w(f"{B} + {ops[1].imm:#x}ull")

    i_adr = i_adrp

    def arith(self, i, ops, op, setflags):
        d = self.regop(i, ops[0])
        bits = 64 if d.bits == 64 else 32
        n = self.regop(i, ops[1])
        a = n.r()
        b = self.shifted(i, ops[2], bits)
        if setflags:
            fn = "adds" if op == "+" else "subs"
            return d.w(f"nx::{fn}{bits}(c, {a}, {b})" if op == "-" else f"nx::adds{bits}(c, {a}, {b}, 0)")
        return d.w(f"{u(bits)}({a}) {op} {b}")

    def i_add(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a + b")
        return self.arith(i, ops, "+", False)

    def i_sub(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a - b")
        return self.arith(i, ops, "-", False)

    def i_adds(self, f, i, ops):
        return self.arith(i, ops, "+", True)

    def i_subs(self, f, i, ops):
        return self.arith(i, ops, "-", True)

    def i_cmp(self, f, i, ops):
        n = self.regop(i, ops[0])
        bits = 64 if n.bits == 64 else 32
        return f"nx::subs{bits}(c, {n.r()}, {self.shifted(i, ops[1], bits)});"

    def i_cmn(self, f, i, ops):
        n = self.regop(i, ops[0])
        bits = 64 if n.bits == 64 else 32
        return f"nx::adds{bits}(c, {n.r()}, {self.shifted(i, ops[1], bits)}, 0);"

    def i_neg(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_un(i, ops, "-a", signed=True)
        bits = d.bits
        return d.w(f"{u(bits)}(0) - {self.shifted(i, ops[1], bits)}")

    def i_negs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::subs{d.bits}(c, 0, {self.shifted(i, ops[1], d.bits)})")

    def i_adc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({self.regop(i, ops[1]).r()} + {self.regop(i, ops[2]).r()} + c.cf)")

    def i_adcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, {self.regop(i, ops[1]).r()}, {self.regop(i, ops[2]).r()}, c.cf)")

    def i_sbc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({self.regop(i, ops[1]).r()} + {u(d.bits)}(~{self.regop(i, ops[2]).r()}) + c.cf)")

    def i_sbcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, {self.regop(i, ops[1]).r()}, {u(d.bits)}(~{self.regop(i, ops[2]).r()}), c.cf)")

    def logic(self, i, ops, expr, setflags=False, invert=False):
        d = self.regop(i, ops[0])
        if not d.gp:
            raise Unhandled("vector logic here")
        bits = d.bits
        a = self.regop(i, ops[1]).r()
        b = self.shifted(i, ops[2], bits)
        if invert:
            b = f"{u(bits)}(~{b})"
        v = f"{u(bits)}({expr.format(a=a, b=b)})"
        if setflags:
            return f"{{ {u(bits)} r = {v}; nx::logic{bits}(c, r); {d.w('r')} }}"
        return d.w(v)

    def i_and(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a & b", lanes="bytes")
        return self.logic(i, ops, "{a} & {b}")

    def i_ands(self, f, i, ops):
        return self.logic(i, ops, "{a} & {b}", setflags=True)

    def i_orr(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            if ops[1].type == A.ARM64_OP_IMM:
                return self.v_imm_logic(i, ops, "|")
            return self.v_bin(i, ops, "a | b", lanes="bytes")
        return self.logic(i, ops, "{a} | {b}")

    def i_eor(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a ^ b", lanes="bytes")
        return self.logic(i, ops, "{a} ^ {b}")

    def i_bic(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            if ops[1].type == A.ARM64_OP_IMM:
                return self.v_imm_logic(i, ops, "&~")
            return self.v_bin(i, ops, "a & ~b", lanes="bytes")
        return self.logic(i, ops, "{a} & {b}", invert=True)

    def i_bics(self, f, i, ops):
        return self.logic(i, ops, "{a} & {b}", setflags=True, invert=True)

    def i_orn(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a | ~b", lanes="bytes")
        return self.logic(i, ops, "{a} | {b}", invert=True)

    def i_eon(self, f, i, ops):
        return self.logic(i, ops, "{a} ^ {b}", invert=True)

    def i_tst(self, f, i, ops):
        n = self.regop(i, ops[0])
        bits = n.bits
        return f"nx::logic{bits}(c, {u(bits)}({n.r()} & {self.shifted(i, ops[1], bits)}));"

    def i_mvn(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_un(i, ops, "~a", lanes="bytes")
        return d.w(f"{u(d.bits)}(~{self.shifted(i, ops[1], d.bits)})")

    def shift_op(self, i, ops, kind):
        d = self.regop(i, ops[0])
        bits = d.bits
        a = self.regop(i, ops[1]).r()
        if ops[2].type == A.ARM64_OP_IMM:
            n = str(ops[2].imm)
        else:
            n = f"({self.regop(i, ops[2]).r()} & {bits - 1})"
        return d.w({"lsl": f"{u(bits)}({a}) << {n}", "lsr": f"{u(bits)}({a}) >> {n}",
                    "asr": f"nx::asr{bits}({a}, unsigned({n}))", "ror": f"nx::ror{bits}({a}, unsigned({n}))"}[kind])

    def i_lsl(self, f, i, ops):
        return self.shift_op(i, ops, "lsl")

    def i_lsr(self, f, i, ops):
        return self.shift_op(i, ops, "lsr")

    def i_asr(self, f, i, ops):
        return self.shift_op(i, ops, "asr")

    def i_ror(self, f, i, ops):
        return self.shift_op(i, ops, "ror")

    def i_extr(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits = d.bits
        hi, lo, n = self.regop(i, ops[1]).r(), self.regop(i, ops[2]).r(), ops[3].imm
        if n == 0:
            return d.w(lo)
        return d.w(f"({u(bits)}({lo}) >> {n}) | ({u(bits)}({hi}) << {bits - n})")

    def bitfield(self, i, ops, lsb, width, signed, insert=False, to_pos=None):
        """Extract `width` bits at `lsb` from source, place at `to_pos` (0 = extract)."""
        d = self.regop(i, ops[0])
        bits = d.bits
        src = self.regop(i, ops[1]).r()
        field_mask = (1 << width) - 1
        if signed:
            v = f"{u(bits)}({s(bits)}({u(bits)}({src}) << {bits - lsb - width}) >> {bits - width})"
        else:
            v = f"(({u(bits)}({src}) >> {lsb}) & {field_mask:#x}ull)"
        pos = to_pos or 0
        if pos:
            v = f"{u(bits)}({v} << {pos})"
        if insert:
            keep = ~(field_mask << pos) & ((1 << bits) - 1)
            v = f"({d.r()} & {keep:#x}ull) | ({v} & {(field_mask << pos) & ((1 << bits) - 1):#x}ull)"
        return d.w(v)

    def i_ubfx(self, f, i, ops):
        return self.bitfield(i, ops, ops[2].imm, ops[3].imm, False)

    def i_sbfx(self, f, i, ops):
        return self.bitfield(i, ops, ops[2].imm, ops[3].imm, True)

    def i_ubfiz(self, f, i, ops):
        return self.bitfield(i, ops, 0, ops[3].imm, False, to_pos=ops[2].imm)

    def i_sbfiz(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits, lsb, width = d.bits, ops[2].imm, ops[3].imm
        src = self.regop(i, ops[1]).r()
        return d.w(f"{u(bits)}({s(bits)}({u(bits)}({src}) << {bits - width}) >> {bits - width - lsb})")

    def i_bfi(self, f, i, ops):
        return self.bitfield(i, ops, 0, ops[3].imm, False, insert=True, to_pos=ops[2].imm)

    def i_bfxil(self, f, i, ops):
        return self.bitfield(i, ops, ops[2].imm, ops[3].imm, False, insert=True)

    def i_ubfm(self, f, i, ops):
        raise Unhandled("raw ubfm")

    def ext(self, i, ops, t):
        d = self.regop(i, ops[0])
        src = self.regop(i, ops[1]).r()
        return d.w(f"{u(d.bits)}({s(d.bits)}({t}({src})))" if t.startswith("int") else f"{u(d.bits)}({t}({src}))")

    def i_sxtb(self, f, i, ops):
        return self.ext(i, ops, "int8_t")

    def i_sxth(self, f, i, ops):
        return self.ext(i, ops, "int16_t")

    def i_sxtw(self, f, i, ops):
        return self.ext(i, ops, "int32_t")

    def i_uxtb(self, f, i, ops):
        return self.ext(i, ops, "uint8_t")

    def i_uxth(self, f, i, ops):
        return self.ext(i, ops, "uint16_t")

    def i_csel(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {self.regop(i, ops[1]).r()} : {self.regop(i, ops[2]).r()}")

    def i_csinc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {u(d.bits)}({self.regop(i, ops[1]).r()}) : {u(d.bits)}({self.regop(i, ops[2]).r()} + 1)")

    def i_csinv(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {u(d.bits)}({self.regop(i, ops[1]).r()}) : {u(d.bits)}(~{self.regop(i, ops[2]).r()})")

    def i_csneg(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {u(d.bits)}({self.regop(i, ops[1]).r()}) : {u(d.bits)}(0 - {self.regop(i, ops[2]).r()})")

    def i_cset(self, f, i, ops):
        return self.regop(i, ops[0]).w(f"({self.cond(i)}) ? 1 : 0")

    def i_csetm(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}(~{u(d.bits)}(0)) : {u(d.bits)}(0)")

    def i_cinc(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}({n} + 1) : {u(d.bits)}({n})")

    def i_cinv(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}(~{n}) : {u(d.bits)}({n})")

    def i_cneg(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}(0 - {n}) : {u(d.bits)}({n})")

    def ccmp(self, i, ops, fn):
        n = self.regop(i, ops[0])
        bits = n.bits
        b = self.shifted(i, ops[1], bits)
        return f"if ({self.cond(i)}) nx::{fn}{bits}(c, {n.r()}, {b}{', 0' if fn == 'adds' else ''}); else nx::set_nzcv(c, {ops[2].imm});"

    def i_ccmp(self, f, i, ops):
        return self.ccmp(i, ops, "subs")

    def i_ccmn(self, f, i, ops):
        return self.ccmp(i, ops, "adds")

    def i_madd(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits = d.bits
        n, m, a = (self.regop(i, o).r() for o in ops[1:4])
        return d.w(f"{u(bits)}({a} + {u(bits)}({n}) * {u(bits)}({m}))")

    def i_msub(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits = d.bits
        n, m, a = (self.regop(i, o).r() for o in ops[1:4])
        return d.w(f"{u(bits)}({a} - {u(bits)}({n}) * {u(bits)}({m}))")

    def i_mul(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_bin(i, ops, "a * b")
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"{u(d.bits)}({u(d.bits)}({n}) * {u(d.bits)}({m}))")

    def i_mneg(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"{u(d.bits)}(0 - {u(d.bits)}({n}) * {u(d.bits)}({m}))")

    def long_mul(self, i, ops, signed, acc=None):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        t = "int64_t(int32_t" if signed else "uint64_t(uint32_t"
        prod = f"uint64_t({t}({n})) * {t}({m})))"
        if acc is None:
            return d.w(prod)
        a = self.regop(i, ops[3]).r()
        return d.w(f"{a} {acc} {prod}")

    def i_smull(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_long(i, ops, "a * b", signed=True, high=False)
        return self.long_mul(i, ops, True)

    def i_umull(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_long(i, ops, "a * b", signed=False, high=False)
        return self.long_mul(i, ops, False)

    def i_smaddl(self, f, i, ops):
        return self.long_mul(i, ops, True, "+")

    def i_umaddl(self, f, i, ops):
        return self.long_mul(i, ops, False, "+")

    def i_smsubl(self, f, i, ops):
        return self.long_mul(i, ops, True, "-")

    def i_umsubl(self, f, i, ops):
        return self.long_mul(i, ops, False, "-")

    def i_smulh(self, f, i, ops):
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return self.regop(i, ops[0]).w(f"uint64_t(nx::smulh(int64_t({n}), int64_t({m})))")

    def i_umulh(self, f, i, ops):
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return self.regop(i, ops[0]).w(f"nx::umulh({n}, {m})")

    def i_udiv(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"nx::udiv{d.bits}({n}, {m})")

    def i_sdiv(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"{u(d.bits)}(nx::sdiv{d.bits}({s(d.bits)}({n}), {s(d.bits)}({m})))")

    def i_clz(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:  # clz v0.4s, v1.4s
            bits = self.vinfo(ops[0])[0]
            return self.v_un(i, ops, f"std::countl_zero(uint{bits}_t(a))")
        return d.w(f"std::countl_zero({u(d.bits)}({self.regop(i, ops[1]).r()}))")

    def i_cls(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:  # cls v0.4s, v1.4s
            bits = self.vinfo(ops[0])[0]
            return self.v_un(i, ops, f"std::countl_zero(uint{bits}_t((uint{bits}_t(a) ^ (uint{bits}_t(a) << 1)) | 1))")
        n = self.regop(i, ops[1]).r()
        return d.w(f"std::countl_zero({u(d.bits)}(({u(d.bits)}({n}) ^ ({u(d.bits)}({n}) << 1)) | 1)) ")

    def i_rbit(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::rbit{d.bits}({self.regop(i, ops[1]).r()})")

    def i_rev(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::bswap({u(d.bits)}({self.regop(i, ops[1]).r()}))")

    def i_rev16(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_rev(i, ops, 16)
        return d.w(f"nx::rev16_{d.bits}({self.regop(i, ops[1]).r()})")

    def i_rev32(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_rev(i, ops, 32)
        return d.w(f"nx::rev32_64({self.regop(i, ops[1]).r()})")

    def i_rev64(self, f, i, ops):
        return self.v_rev(i, ops, 64)
