"""AdvSIMD (NEON): lane helpers, element-wise arithmetic and logic, compares, shifts by immediate. Mixed into
emit.Emitter (split from emit.py, 700-line rule)."""

from capstone import arm64_const as A

from .emit import VAS, Unhandled, subst, u, s  # noqa: F401


class NeonOps:
    # ---- NEON ---------------------------------------------------------------------------------------------

    def vinfo(self, o):
        if not o.vas:
            # AdvSIMD scalar form (sqadd s0, s0, s1; sub d0, d0, d1): one lane of the register's size.
            name = self.cs.reg_name(o.reg) if o.type == A.ARM64_OP_REG else None
            if name and name[0] in "bhsd" and name[1:].isdigit() and o.vector_index == -1:
                return {"b": 8, "h": 16, "s": 32, "d": 64}[name[0]], 1
            raise Unhandled("vector without arrangement")
        return VAS[o.vas]

    def lane_t(self, bits, kind):
        return f"{kind}{bits}"

    def v_src(self, i, o, bits, kind, k):
        """Lane k of a vector source operand; by-element operands broadcast their lane."""
        r = self.regop(i, o)
        if o.vector_index != -1:
            return f"c.v[{r.n}].{kind}{bits}[{o.vector_index}]"
        return f"c.v[{r.n}].{kind}{bits}[{k}]"

    def v_loop(self, i, dst, lanes, bits, body_fn, keep_dst=False):
        d = self.regop(i, dst)
        init = f"c.v[{d.n}]" if keep_dst else "nx::vzero()"
        return (f"{{ nx::V128 r = {init}; for (int k = 0; k < {lanes}; k++) {{ {body_fn()} }} c.v[{d.n}] = r; }}")

    def v_bin(self, i, ops, expr, fp=False, signed=False, lanes=None, acc=None):
        """Lanewise binary op; `acc` means the destination is also an input (o)."""
        bits, n = self.vinfo(ops[0])
        if lanes == "bytes":
            n = n * bits // 8
            bits = 8
        kind = "f" if fp else ("s" if signed else "u")
        d = self.regop(i, ops[0])
        srcs = [f"A[{k}]" for k in range(len(ops) - 1)]
        pre = []
        for k, o in enumerate(ops[1:3]):
            r = self.regop(i, o)
            pre.append(f"nx::V128 S{k} = c.v[{r.n}];")
        a = "S0.{t}[k]".format(t=f"{kind}{bits}")
        if ops[2].vector_index != -1:
            b = f"S1.{kind}{bits}[{ops[2].vector_index}]"
        else:
            b = f"S1.{kind}{bits}[k]"
        o_ = f"O.{kind}{bits}[k]"
        e = subst(expr, a=a, b=b, o=o_)
        ty = {("f", 32): "float", ("f", 64): "double"}.get((kind, bits), f"{'int' if kind == 's' else 'uint'}{bits}_t")
        pre.append(f"nx::V128 O = c.v[{d.n}];")
        return (f"{{ {' '.join(pre)} nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) r.{kind}{bits}[k] = {ty}({e}); "
                f"c.v[{d.n}] = r; }}")

    def v_un(self, i, ops, expr, fp=False, signed=False, lanes=None, fp_from_int=None):
        bits, n = self.vinfo(ops[0])
        if lanes == "bytes":
            n = n * bits // 8
            bits = 8
        kind = "f" if fp or fp_from_int else ("s" if signed else "u")
        skind = fp_from_int or kind
        d, m = self.regop(i, ops[0]), self.regop(i, ops[1])
        ty = {("f", 32): "float", ("f", 64): "double"}.get((kind, bits), f"{'int' if kind == 's' else 'uint'}{bits}_t")
        src = f"S.{skind}{bits}[k]" if ops[1].vector_index == -1 else f"S.{skind}{bits}[{ops[1].vector_index}]"
        e = subst(expr, a=src)
        return (f"{{ nx::V128 S = c.v[{m.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{bits}[k] = {ty}({e}); c.v[{d.n}] = r; }}")

    def v_mov(self, f, i, ops):
        d, n = ops[0], ops[1]
        dr = self.regop(i, d)
        if n.type == A.ARM64_OP_REG:
            nr = self.regop(i, n)
            if d.vector_index != -1 and nr.gp:  # mov v0.s[1], w8  (ins)
                bits = VAS[d.vas][0] if d.vas else 32
                return f"c.v[{dr.n}].u{bits}[{d.vector_index}] = uint{bits}_t({nr.r()});"
            if d.vector_index != -1 and n.vector_index != -1:  # mov v0.s[1], v1.s[0]
                bits = VAS[d.vas][0] if d.vas else 32
                return f"c.v[{dr.n}].u{bits}[{d.vector_index}] = c.v[{nr.n}].u{bits}[{n.vector_index}];"
            if dr.gp and n.vector_index != -1:  # mov w0, v0.s[1]  (umov)
                bits = VAS[n.vas][0] if n.vas else dr.bits
                return dr.w(f"c.v[{nr.n}].u{bits}[{n.vector_index}]")
            if not dr.gp and n.vector_index != -1:  # mov s0, v1.s[1]  (dup scalar)
                bits = VAS[n.vas][0] if n.vas else dr.bits
                return f"{{ nx::V128 t_ = nx::vzero(); t_.u{bits}[0] = c.v[{nr.n}].u{bits}[{n.vector_index}]; c.v[{dr.n}] = t_; }}"
            if d.vas and n.vas:  # mov v0.16b, v1.16b
                bits, lanes = VAS[d.vas]
                if bits * lanes == 64:
                    return f"{{ nx::V128 t_ = nx::vzero(); t_.u64[0] = c.v[{nr.n}].u64[0]; c.v[{dr.n}] = t_; }}"
                return f"c.v[{dr.n}] = c.v[{nr.n}];"
        raise Unhandled("mov vector form")

    def i_umov(self, f, i, ops):
        return self.v_mov(f, i, ops)

    def i_smov(self, f, i, ops):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits = VAS[ops[1].vas][0]
        return d.w(f"{u(d.bits)}({s(d.bits)}(c.v[{n.n}].s{bits}[{ops[1].vector_index}]))")

    def i_ins(self, f, i, ops):
        return self.v_mov(f, i, ops)

    def i_dup(self, f, i, ops):
        bits, lanes = self.vinfo(ops[0])
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        if n.gp:
            v = f"uint{bits}_t({n.r()})"
        else:
            v = f"c.v[{n.n}].u{bits}[{ops[1].vector_index}]"
        return f"{{ uint{bits}_t e_ = {v}; nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = e_; c.v[{d.n}] = r; }}"

    def i_movi(self, f, i, ops):
        d = self.regop(i, ops[0])
        imm = ops[1].imm
        if ops[1].shift.type == A.ARM64_SFT_LSL:
            imm <<= ops[1].shift.value
        elif ops[1].shift.type == A.ARM64_SFT_MSL:
            imm = (imm << ops[1].shift.value) | ((1 << ops[1].shift.value) - 1)
        if not ops[0].vas:  # movi d0, #imm  (64-bit scalar, byte mask)
            return f"{{ nx::V128 r = nx::vzero(); r.u64[0] = {imm & 0xFFFFFFFFFFFFFFFF:#x}ull; c.v[{d.n}] = r; }}"
        bits, lanes = self.vinfo(ops[0])
        return (f"{{ nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = "
                f"uint{bits}_t({imm & ((1 << bits) - 1):#x}ull); c.v[{d.n}] = r; }}")

    def i_mvni(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits, lanes = self.vinfo(ops[0])
        imm = ops[1].imm
        if ops[1].shift.type == A.ARM64_SFT_LSL:
            imm <<= ops[1].shift.value
        elif ops[1].shift.type == A.ARM64_SFT_MSL:
            imm = (imm << ops[1].shift.value) | ((1 << ops[1].shift.value) - 1)
        return (f"{{ nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = "
                f"uint{bits}_t(~{imm:#x}ull); c.v[{d.n}] = r; }}")

    def v_imm_logic(self, i, ops, op):
        d = self.regop(i, ops[0])
        bits, lanes = self.vinfo(ops[0])
        imm = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
        e = f"r.u{bits}[k] | uint{bits}_t({imm:#x}ull)" if op == "|" else f"r.u{bits}[k] & uint{bits}_t(~{imm:#x}ull)"
        clear = "" if bits * lanes == 128 else " r.u64[1] = 0;"
        return f"{{ nx::V128 r = c.v[{d.n}]; for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = {e};{clear} c.v[{d.n}] = r; }}"

    def i_fmla(self, f, i, ops):
        if not ops[0].vas:  # scalar by element: fmla s0, s1, v2.s[1]
            d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
            return self.fw(d, f"std::fma({self.fr(n)}, {self.velem(i, ops[2], 'f')}, {self.fr(d)})")
        return self.v_bin(i, ops, "std::fma(a, b, o)", fp=True)

    def i_fmls(self, f, i, ops):
        if not ops[0].vas:
            d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
            return self.fw(d, f"std::fma(-({self.fr(n)}), {self.velem(i, ops[2], 'f')}, {self.fr(d)})")
        return self.v_bin(i, ops, "std::fma(-a, b, o)", fp=True)

    def i_mla(self, f, i, ops):
        return self.v_bin(i, ops, "o + a * b")

    def i_mls(self, f, i, ops):
        return self.v_bin(i, ops, "o - a * b")

    def i_bsl(self, f, i, ops):
        return self.v_bin(i, ops, "(o & a) | (~o & b)", lanes="bytes")

    def i_bit(self, f, i, ops):
        return self.v_bin(i, ops, "(o & ~b) | (a & b)", lanes="bytes")

    def i_bif(self, f, i, ops):
        return self.v_bin(i, ops, "(o & b) | (a & ~b)", lanes="bytes")

    def i_smax(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a : b", signed=True)

    def i_smin(self, f, i, ops):
        return self.v_bin(i, ops, "a < b ? a : b", signed=True)

    def i_umax(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a : b")

    def i_umin(self, f, i, ops):
        return self.v_bin(i, ops, "a < b ? a : b")

    def i_urhadd(self, f, i, ops):
        return self.v_bin(i, ops, "(uint64_t(a) + uint64_t(b) + 1) >> 1")

    def i_srhadd(self, f, i, ops):
        return self.v_bin(i, ops, "(int64_t(a) + int64_t(b) + 1) >> 1", signed=True)

    def i_uhadd(self, f, i, ops):
        return self.v_bin(i, ops, "(uint64_t(a) + uint64_t(b)) >> 1")

    def i_shadd(self, f, i, ops):
        return self.v_bin(i, ops, "(int64_t(a) + int64_t(b)) >> 1", signed=True)

    def i_sabd(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a - b : b - a", signed=True)

    def i_uabd(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a - b : b - a")

    def i_sqdmulh(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"nx::sat<int{bits}_t>((int64_t(a) * int64_t(b) * 2) >> {bits})", signed=True)

    def i_sshl(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"int8_t(S1.u{bits}[k]) >= 0 ? (int8_t(S1.u{bits}[k]) >= {bits} ? 0 : int64_t(a) << int8_t(S1.u{bits}[k])) : (int8_t(S1.u{bits}[k]) <= -{bits} ? (a < 0 ? -1 : 0) : int64_t(a) >> -int8_t(S1.u{bits}[k]))", signed=True)

    def i_ushl(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"int8_t(S1.u{bits}[k]) >= 0 ? (int8_t(S1.u{bits}[k]) >= {bits} ? 0 : uint64_t(a) << int8_t(S1.u{bits}[k])) : (int8_t(S1.u{bits}[k]) <= -{bits} ? 0 : uint64_t(a) >> -int8_t(S1.u{bits}[k]))")

    def cmp_mask(self, i, ops, op, kind):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        if ops[2].type in (A.ARM64_OP_IMM, A.ARM64_OP_FP):
            b = "0"
        else:
            b = f"c.v[{self.regop(i, ops[2]).n}].{kind}{bits}[k]"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 T = c.v[{self.regop(i, ops[2]).n if ops[2].type == A.ARM64_OP_REG else a.n}]; nx::V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n}; k++) r.u{bits}[k] = (S.{kind}{bits}[k] {op} {b.replace(f'c.v[{self.regop(i, ops[2]).n if ops[2].type == A.ARM64_OP_REG else a.n}]', 'T')}) ? uint{bits}_t(~0ull) : 0; c.v[{d.n}] = r; }}")

    def i_cmeq(self, f, i, ops):
        if not ops[0].vas:  # cmeq d0, d1, d2 / cmeq d0, d1, #0
            d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
            b = "0" if ops[2].type != A.ARM64_OP_REG else f"c.v[{self.regop(i, ops[2]).n}].u64[0]"
            return (f"{{ uint64_t r_ = c.v[{n.n}].u64[0] == {b} ? ~0ull : 0; nx::V128 t_ = nx::vzero(); "
                    f"t_.u64[0] = r_; c.v[{d.n}] = t_; }}")
        return self.cmp_mask(i, ops, "==", "u")

    def i_cmgt(self, f, i, ops):
        return self.cmp_mask(i, ops, ">", "s")

    def i_cmge(self, f, i, ops):
        return self.cmp_mask(i, ops, ">=", "s")

    def i_cmhi(self, f, i, ops):
        return self.cmp_mask(i, ops, ">", "u")

    def i_cmhs(self, f, i, ops):
        return self.cmp_mask(i, ops, ">=", "u")

    def i_cmlt(self, f, i, ops):
        return self.cmp_mask(i, ops, "<", "s")

    def i_cmle(self, f, i, ops):
        return self.cmp_mask(i, ops, "<=", "s")

    def i_cmtst(self, f, i, ops):
        return self.v_bin(i, ops, "(a & b) ? ~0ull : 0")

    def i_fcmeq(self, f, i, ops):
        return self.cmp_mask(i, ops, "==", "f")

    def i_fcmgt(self, f, i, ops):
        return self.cmp_mask(i, ops, ">", "f")

    def i_fcmge(self, f, i, ops):
        return self.cmp_mask(i, ops, ">=", "f")

    def i_fcmlt(self, f, i, ops):
        return self.cmp_mask(i, ops, "<", "f")

    def i_fcmle(self, f, i, ops):
        return self.cmp_mask(i, ops, "<=", "f")

    # shifts by immediate
    def v_shift_imm(self, i, ops, expr, signed=False, acc=False):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        sh = ops[2].imm
        kind = "s" if signed else "u"
        e = expr.format(a=f"int64_t(S.{kind}{bits}[k])" if signed else f"uint64_t(S.{kind}{bits}[k])", n=sh,
                        bits=bits, o=f"r.{kind}{bits}[k]")
        init = f"c.v[{d.n}]" if acc else "nx::vzero()"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = {init}; for (int k = 0; k < {n}; k++) "
                f"r.{kind}{bits}[k] = {'int' if signed else 'uint'}{bits}_t({e}); "
                + ("" if bits * n == 128 else "r.u64[1] = 0; ") + f"c.v[{d.n}] = r; }}")

    def i_shl(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{a} << {n}")

    def i_ushr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "({n} >= {bits}) ? 0 : ({a} >> {n})")

    def i_sshr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{a} >> ({n} >= {bits} ? {bits} - 1 : {n})", signed=True)

    def i_srshr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "({a} + (int64_t(1) << ({n} - 1))) >> {n}", signed=True)

    def i_urshr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "({a} + (uint64_t(1) << ({n} - 1))) >> {n}")

    def i_ssra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + ({a} >> ({n} >= {bits} ? {bits} - 1 : {n}))", signed=True, acc=True)

    def i_usra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + (({n} >= {bits}) ? 0 : ({a} >> {n}))", acc=True)

    def i_srsra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + (({a} + (int64_t(1) << ({n} - 1))) >> {n})", signed=True, acc=True)

    def i_ursra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + (({a} + (uint64_t(1) << ({n} - 1))) >> {n})", acc=True)
