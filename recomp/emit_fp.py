"""Scalar floating point: arithmetic, fused multiply-add, rounding, conversions, compares and moves. Mixed into
emit.Emitter (split from emit.py, 700-line rule)."""

from capstone import arm64_const as A

from .emit import VAS, Unhandled, subst, u, s  # noqa: F401


class FpOps:
    # ---- scalar floating point ----------------------------------------------------------------------------

    def fr(self, r):
        """Read a scalar FP register as float/double."""
        if r.kind == "s":
            return f"c.v[{r.n}].f32[0]"
        if r.kind == "d":
            return f"c.v[{r.n}].f64[0]"
        if r.kind == "h":
            return f"nx::f16_to_f32(c.v[{r.n}].u16[0])"
        raise Unhandled(f"fp read {r.name}")

    def fw(self, r, expr):
        """Write a scalar FP register (upper bits cleared)."""
        if r.kind == "h":
            return f"{{ uint16_t h_ = nx::f32_to_f16(float({expr})); nx::V128 t_ = nx::vzero(); t_.u16[0] = h_; c.v[{r.n}] = t_; }}"
        field = {"s": "f32", "d": "f64"}.get(r.kind)
        if not field:
            raise Unhandled(f"fp write {r.name}")
        ty = "float" if r.kind == "s" else "double"
        return f"{{ {ty} r_ = {ty}({expr}); nx::V128 t_ = nx::vzero(); t_.{field}[0] = r_; c.v[{r.n}] = t_; }}"

    def is_vec(self, i, o):
        return o.type == A.ARM64_OP_REG and (o.vas or o.vector_index != -1) and not self.regop(i, o).gp

    def fbin(self, i, ops, expr):
        if self.is_vec(i, ops[0]):
            return self.v_bin(i, ops, expr, fp=True)
        d, n, m = (self.regop(i, o) for o in ops[:3])
        if self.is_vec(i, ops[2]):  # scalar by element: fmul s0, s1, v2.s[1]
            m_expr = self.velem(i, ops[2], "f", like=d)
        else:
            m_expr = self.fr(m)
        return self.fw(d, subst(expr, a=f"({self.fr(n)})", b=f"({m_expr})"))

    def velem(self, i, o, kind, like=None):
        """Element o.vector_index of a vector register. Without an arrangement the size comes from the register's
        own kind, or from `like` (the scalar destination) when Capstone names the whole vector (v2 for v2.s[1])."""
        r = self.regop(i, o)
        if o.vas:
            bits = VAS[o.vas][0]
        else:
            k = like.kind if r.kind in ("v", "q") and like is not None else r.kind
            if k not in ("b", "h", "s", "d"):
                raise Unhandled(f"element size of {r.name}")
            bits = {"b": 8, "h": 16, "s": 32, "d": 64}[k]
        return f"c.v[{r.n}].{kind}{bits}[{o.vector_index}]"

    def i_fadd(self, f, i, ops):
        return self.fbin(i, ops, "a + b")

    def i_fsub(self, f, i, ops):
        return self.fbin(i, ops, "a - b")

    def i_fmul(self, f, i, ops):
        return self.fbin(i, ops, "a * b")

    def i_fdiv(self, f, i, ops):
        return self.fbin(i, ops, "a / b")

    def i_fnmul(self, f, i, ops):
        return self.fbin(i, ops, "-(a * b)")

    def i_fabd(self, f, i, ops):
        return self.fbin(i, ops, "std::fabs(a - b)")

    def i_fmaxnm(self, f, i, ops):
        return self.fbin(i, ops, "nx::fmaxnm(a, b)")

    def i_fminnm(self, f, i, ops):
        return self.fbin(i, ops, "nx::fminnm(a, b)")

    def i_fmax(self, f, i, ops):
        return self.fbin(i, ops, "nx::fmax_(a, b)")

    def i_fmin(self, f, i, ops):
        return self.fbin(i, ops, "nx::fmin_(a, b)")

    def fma(self, i, ops, kind):
        d, n, m, a = (self.regop(i, o) for o in ops[:4])
        N, M, Aa = self.fr(n), self.fr(m), self.fr(a)
        e = {"fmadd": f"std::fma({N}, {M}, {Aa})", "fmsub": f"std::fma(-({N}), {M}, {Aa})",
             "fnmadd": f"std::fma(-({N}), {M}, -({Aa}))", "fnmsub": f"std::fma({N}, {M}, -({Aa}))"}[kind]
        return self.fw(d, e)

    def i_fmadd(self, f, i, ops):
        return self.fma(i, ops, "fmadd")

    def i_fmsub(self, f, i, ops):
        return self.fma(i, ops, "fmsub")

    def i_fnmadd(self, f, i, ops):
        return self.fma(i, ops, "fnmadd")

    def i_fnmsub(self, f, i, ops):
        return self.fma(i, ops, "fnmsub")

    def fun(self, i, ops, expr):
        if self.is_vec(i, ops[0]):
            return self.v_un(i, ops, expr, fp=True)
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return self.fw(d, subst(expr, a=f"({self.fr(n)})"))

    def i_fneg(self, f, i, ops):
        return self.fun(i, ops, "-a")

    def i_fabs(self, f, i, ops):
        return self.fun(i, ops, "std::fabs(a)")

    def i_fsqrt(self, f, i, ops):
        return self.fun(i, ops, "std::sqrt(a)")

    def i_frintm(self, f, i, ops):
        return self.fun(i, ops, "std::floor(a)")

    def i_frintp(self, f, i, ops):
        return self.fun(i, ops, "std::ceil(a)")

    def i_frintz(self, f, i, ops):
        return self.fun(i, ops, "std::trunc(a)")

    def i_frinta(self, f, i, ops):
        return self.fun(i, ops, "nx::round_away(a)")

    def i_frintn(self, f, i, ops):
        return self.fun(i, ops, "nx::round_even(a)")

    i_frintx = i_frintn
    i_frinti = i_frintn

    def i_fcmp(self, f, i, ops):
        n = self.regop(i, ops[0])
        b = "0.0" if ops[1].type == A.ARM64_OP_FP or (ops[1].type == A.ARM64_OP_IMM) else self.fr(self.regop(i, ops[1]))
        ty = "float" if n.kind in ("s", "h") else "double"
        return f"nx::fcmp<{ty}>(c, {self.fr(n)}, {ty}({b}));"

    i_fcmpe = i_fcmp

    def i_fccmp(self, f, i, ops):
        n, m = self.regop(i, ops[0]), self.regop(i, ops[1])
        ty = "float" if n.kind in ("s", "h") else "double"
        return f"if ({self.cond(i)}) nx::fcmp<{ty}>(c, {self.fr(n)}, {self.fr(m)}); else nx::set_nzcv(c, {ops[2].imm});"

    i_fccmpe = i_fccmp

    def i_fcsel(self, f, i, ops):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return self.fw(d, f"({self.cond(i)}) ? {self.fr(n)} : {self.fr(m)}")

    def i_fmov(self, f, i, ops):
        d = self.regop(i, ops[0])
        o = ops[1]
        if self.is_vec(i, ops[0]) and ops[0].vector_index != -1:  # fmov v0.d[1], x1
            return f"c.v[{d.n}].u64[{ops[0].vector_index}] = {self.regop(i, o).r()};"
        if o.type == A.ARM64_OP_FP:
            if self.is_vec(i, ops[0]):
                bits, lanes = VAS[ops[0].vas]
                t = "f32" if bits == 32 else "f64"
                return f"{{ nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.{t}[k] = {o.fp!r}; c.v[{d.n}] = r; }}"
            return self.fw(d, repr(o.fp))
        if o.type == A.ARM64_OP_REG:
            n = self.regop(i, o)
            if self.is_vec(i, o) and o.vector_index != -1:  # fmov x0, v1.d[1]
                return d.w(f"c.v[{n.n}].u64[{o.vector_index}]")
            if d.gp and not n.gp:
                return d.w(f"c.v[{n.n}].u{d.bits}[0]")
            if not d.gp and n.gp:
                return f"{{ nx::V128 t_ = nx::vzero(); t_.u{d.bits}[0] = {u(d.bits)}({n.r()}); c.v[{d.n}] = t_; }}"
            return f"{{ nx::V128 t_ = nx::vzero(); t_.u{d.bits}[0] = c.v[{n.n}].u{d.bits}[0]; c.v[{d.n}] = t_; }}"
        if o.type == A.ARM64_OP_IMM:  # fmov s0, #0? capstone sometimes gives imm
            return self.fw(d, "0.0")
        raise Unhandled("fmov form")

    def i_fcvt(self, f, i, ops):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return self.fw(d, self.fr(n))

    def cvt_to_int(self, i, ops, rnd, signed):
        if self.is_vec(i, ops[0]):
            return self.v_cvt_to_int(i, ops, rnd, signed)
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        if not d.gp and d.kind not in ("s", "d"):
            raise Unhandled("fcvt to simd scalar")
        v = self.fr(n)
        if len(ops) > 2:  # fixed point
            v = f"({v}) * {float(2 ** ops[2].imm)!r}"
        v = {"z": f"std::trunc({v})", "m": f"std::floor({v})", "p": f"std::ceil({v})",
             "a": f"nx::round_away({v})", "n": f"nx::round_even({v})"}[rnd]
        t = f"{s(d.bits) if signed else u(d.bits)}"
        if not d.gp:  # fcvtzs s0, s1: the integer stays in the SIMD register
            return (f"{{ nx::V128 t_ = nx::vzero(); t_.u{d.bits}[0] = {u(d.bits)}(nx::fcvt_sat<{t}>({v})); "
                    f"c.v[{d.n}] = t_; }}")
        return d.w(f"{u(d.bits)}(nx::fcvt_sat<{t}>({v}))")

    def i_fcvtzs(self, f, i, ops):
        return self.cvt_to_int(i, ops, "z", True)

    def i_fcvtzu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "z", False)

    def i_fcvtms(self, f, i, ops):
        return self.cvt_to_int(i, ops, "m", True)

    def i_fcvtmu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "m", False)

    def i_fcvtps(self, f, i, ops):
        return self.cvt_to_int(i, ops, "p", True)

    def i_fcvtpu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "p", False)

    def i_fcvtas(self, f, i, ops):
        return self.cvt_to_int(i, ops, "a", True)

    def i_fcvtau(self, f, i, ops):
        return self.cvt_to_int(i, ops, "a", False)

    def i_fcvtns(self, f, i, ops):
        return self.cvt_to_int(i, ops, "n", True)

    def i_fcvtnu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "n", False)

    def cvt_from_int(self, i, ops, signed):
        if self.is_vec(i, ops[0]):
            if len(ops) > 2 and ops[2].type == A.ARM64_OP_IMM:  # scvtf v0.4s, v0.4s, #fbits
                ty = "float" if self.vinfo(ops[0])[0] == 32 else "double"
                return self.v_un(i, ops, f"{ty}(a) / {ty}({float(2 ** ops[2].imm)!r})", fp_from_int=("s" if signed else "u"))
            return self.v_un(i, ops, "a", fp_from_int=("s" if signed else "u"))
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        ty = "float" if d.kind in ("s", "h") else "double"
        if n.gp:
            v = f"{s(n.bits) if signed else u(n.bits)}({n.r()})"
        else:  # scvtf s0, s1 (integer in simd register)
            v = f"c.v[{n.n}].{'s' if signed else 'u'}{n.bits}[0]"
        v = f"{ty}({v})"
        if len(ops) > 2:
            v = f"{v} / {ty}({float(2 ** ops[2].imm)!r})"
        return self.fw(d, v)

    def i_scvtf(self, f, i, ops):
        return self.cvt_from_int(i, ops, True)

    def i_ucvtf(self, f, i, ops):
        return self.cvt_from_int(i, ops, False)
