"""Instructions first needed by SDK 16 (Super Mario Bros. Wonder): AdvSIMD pairwise, reductions and rounding forms,
exclusive pairs, ARMv8.1 LSE atomics and the crypto extension. Mixed into emit.Emitter; the helpers it calls
(vinfo, v_bin, widen, narrow, vldst, mem_ops, regop) live there, the C++ side in runtime/include/nx/cpu.h."""
import re

from capstone import arm64_const as A

from .emit import VAS, Reg, Unhandled, subst, u


class ExtOps:
    # LSE atomics (ARMv8.1): ld<op>/st<op>, swp, cas, casp with a/l/al ordering and b/h sizes. Every ordering is
    # emitted as seq_cst, which is at least as strong.
    LSE = re.compile(r"^(ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|stadd|stclr|steor|stset|stsmax|stsmin|"
                     r"stumax|stumin|swp|casp|cas)(al|a|l)?(b|h)?$")
    LSE_OPS = {"add": "__atomic_fetch_add(p_, v_, __ATOMIC_SEQ_CST)", "clr": "__atomic_fetch_and(p_, T_(~v_), __ATOMIC_SEQ_CST)",
               "eor": "__atomic_fetch_xor(p_, v_, __ATOMIC_SEQ_CST)", "set": "__atomic_fetch_or(p_, v_, __ATOMIC_SEQ_CST)",
               "smax": "nx::amo_minmax<T_, true, true>(p_, v_)", "smin": "nx::amo_minmax<T_, true, false>(p_, v_)",
               "umax": "nx::amo_minmax<T_, false, true>(p_, v_)", "umin": "nx::amo_minmax<T_, false, false>(p_, v_)"}

    def lse(self, i, ops):
        m = self.LSE.match(i.mnemonic)
        if not m:
            raise Unhandled("lse form")
        base, size = m.group(1), m.group(3)
        regs, mem, _ = self.mem_ops(ops)
        rs = [self.regop(i, o) for o in regs]
        addr = f"c.x[{Reg(i.reg_name(mem.mem.base)).n}]"
        bits = {"b": 8, "h": 16}.get(size, rs[0].bits)
        t = u(bits)
        if base == "casp":  # casp x0, x1, x2, x3, [x4]: compare (x0, x1), store (x2, x3), old value to (x0, x1)
            s0, s1, t0, t1 = rs
            if bits == 32:
                return (f"{{ uint64_t e_ = uint64_t(uint32_t({s0.r()})) | uint64_t(uint32_t({s1.r()})) << 32; "
                        f"uint64_t n_ = uint64_t(uint32_t({t0.r()})) | uint64_t(uint32_t({t1.r()})) << 32; "
                        f"__atomic_compare_exchange_n(reinterpret_cast<uint64_t*>({addr}), &e_, n_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                        f"{s0.w('uint32_t(e_)')} {s1.w('uint32_t(e_ >> 32)')} }}")
            return (f"{{ unsigned __int128 e_ = (unsigned __int128){s1.r()} << 64 | {s0.r()}; "
                    f"unsigned __int128 n_ = (unsigned __int128){t1.r()} << 64 | {t0.r()}; "
                    f"__atomic_compare_exchange_n(reinterpret_cast<unsigned __int128*>({addr}), &e_, n_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                    f"{s0.w('uint64_t(e_)')} {s1.w('uint64_t(e_ >> 64)')} }}")
        if base == "cas":  # cas ws, wt, [xn]: compare ws, store wt, old value to ws
            rs_, rt = rs
            return (f"{{ {t} e_ = {t}({rs_.r()}); __atomic_compare_exchange_n(reinterpret_cast<{t}*>({addr}), &e_, {t}({rt.r()}), false, "
                    f"__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); {rs_.w('e_')} }}")
        if base == "swp":
            rs_, rt = rs
            return f"{{ {t} o_ = __atomic_exchange_n(reinterpret_cast<{t}*>({addr}), {t}({rs_.r()}), __ATOMIC_SEQ_CST); {rt.w('o_')} }}"
        op = base[2:]
        rs_ = rs[0]
        rt = rs[1] if len(rs) > 1 and base.startswith("ld") else None
        stmt = f"{{ using T_ = {t}; T_* p_ = reinterpret_cast<T_*>({addr}); T_ v_ = T_({rs_.r()}); T_ o_ = {self.LSE_OPS[op]}; "
        return stmt + (rt.w("o_") if rt else "(void)o_;") + " }"

    def i_xpaclri(self, f, i, ops):  # strip a pointer authentication code from x30: we never sign pointers
        return ";"

    def i_ldaxp(self, f, i, ops):  # ldaxp x0, x1, [x2]
        regs, mem, _ = self.mem_ops(ops)
        lo, hi = self.regop(i, regs[0]), self.regop(i, regs[1])
        t = u(lo.bits)
        return (f"{{ {t} lo_, hi_; nx::ldxp<{t}>(c, c.x[{Reg(i.reg_name(mem.mem.base)).n}], lo_, hi_); "
                f"{lo.w('lo_')} {hi.w('hi_')} }}")

    i_ldxp = i_ldaxp

    def i_stlxp(self, f, i, ops):  # stlxp w0, x1, x2, [x3]
        regs, mem, _ = self.mem_ops(ops)
        st, lo, hi = (self.regop(i, o) for o in regs[:3])
        t = u(lo.bits)
        return st.w(f"nx::stxp<{t}>(c, c.x[{Reg(i.reg_name(mem.mem.base)).n}], {t}({lo.r()}), {t}({hi.r()}))")

    i_stxp = i_stlxp

    def adalp(self, i, ops, signed):
        """SADALP / UADALP: pairwise long add, accumulated into the destination."""
        dbits, n = self.vinfo(ops[0])
        sbits = dbits // 2
        kind = "s" if signed else "u"
        wide = "int64_t" if signed else "uint64_t"
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = c.v[{d.n}]; for (int k = 0; k < {n}; k++) "
                f"r.{kind}{dbits}[k] = {'int' if signed else 'uint'}{dbits}_t({wide}(r.{kind}{dbits}[k]) + {wide}(S.{kind}{sbits}[2 * k]) + S.{kind}{sbits}[2 * k + 1]); "
                f"c.v[{d.n}] = r; }}")

    def i_sadalp(self, f, i, ops):
        return self.adalp(i, ops, True)

    def i_uadalp(self, f, i, ops):
        return self.adalp(i, ops, False)

    def i_rshrn(self, f, i, ops):
        # (a + 2^(n-1)) >> n without the add overflowing 64-bit lanes
        return self.narrow(i, ops, "(({a} >> {n}) + (({a} >> ({n} - 1)) & 1))")

    i_rshrn2 = i_rshrn

    def i_sminv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ int{bits}_t s_ = c.v[{a.n}].s{bits}[0]; for (int k = 1; k < {n}; k++) s_ = c.v[{a.n}].s{bits}[k] < s_ ? c.v[{a.n}].s{bits}[k] : s_; "
                f"nx::V128 r = nx::vzero(); r.s{bits}[0] = s_; c.v[{d.n}] = r; }}")

    def i_uminv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint{bits}_t s_ = c.v[{a.n}].u{bits}[0]; for (int k = 1; k < {n}; k++) s_ = c.v[{a.n}].u{bits}[k] < s_ ? c.v[{a.n}].u{bits}[k] : s_; "
                f"nx::V128 r = nx::vzero(); r.u{bits}[0] = s_; c.v[{d.n}] = r; }}")

    def pairwise(self, i, ops, expr, kind):
        """Pairwise op: the lower half of the result from adjacent pairs of the first source, the upper half
        from the second. Scalar form (fmaxp s0, v1.2s) reduces the two lanes of one source."""
        d = self.regop(i, ops[0])
        if not ops[0].vas:
            a = self.regop(i, ops[1])
            bits = VAS[ops[1].vas][0]
            e = subst(expr, a=f"c.v[{a.n}].{kind}{bits}[0]", b=f"c.v[{a.n}].{kind}{bits}[1]")
            return f"{{ nx::V128 r = nx::vzero(); r.{kind}{bits}[0] = {e}; c.v[{d.n}] = r; }}"
        bits, n = self.vinfo(ops[0])
        a, b = self.regop(i, ops[1]), self.regop(i, ops[2])
        ea = subst(expr, a=f"A_.{kind}{bits}[2 * k]", b=f"A_.{kind}{bits}[2 * k + 1]")
        eb = subst(expr, a=f"B_.{kind}{bits}[2 * k]", b=f"B_.{kind}{bits}[2 * k + 1]")
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n // 2}; k++) "
                f"{{ r.{kind}{bits}[k] = {ea}; r.{kind}{bits}[k + {n // 2}] = {eb}; }} c.v[{d.n}] = r; }}")

    def i_fmaxp(self, f, i, ops):
        return self.pairwise(i, ops, "nx::fmax_(a, b)", "f")

    def i_fminp(self, f, i, ops):
        return self.pairwise(i, ops, "nx::fmin_(a, b)", "f")

    def i_fmaxnmp(self, f, i, ops):
        return self.pairwise(i, ops, "nx::fmaxnm(a, b)", "f")

    def i_fminnmp(self, f, i, ops):
        return self.pairwise(i, ops, "nx::fminnm(a, b)", "f")

    def i_smaxp(self, f, i, ops):
        return self.pairwise(i, ops, "a > b ? a : b", "s")

    def i_sminp(self, f, i, ops):
        return self.pairwise(i, ops, "a < b ? a : b", "s")

    def i_umaxp(self, f, i, ops):
        return self.pairwise(i, ops, "a > b ? a : b", "u")

    def i_uminp(self, f, i, ops):
        return self.pairwise(i, ops, "a < b ? a : b", "u")

    def fac(self, i, ops, op):
        """FACGE / FACGT: compare absolute values, all-ones mask per lane."""
        bits, n = self.vinfo(ops[0])
        d, a, b = (self.regop(i, o) for o in ops[:3])
        return (f"{{ nx::V128 S = c.v[{a.n}], T = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.u{bits}[k] = std::fabs(S.f{bits}[k]) {op} std::fabs(T.f{bits}[k]) ? uint{bits}_t(~0ull) : 0; c.v[{d.n}] = r; }}")

    def i_facge(self, f, i, ops):
        return self.fac(i, ops, ">=")

    def i_facgt(self, f, i, ops):
        return self.fac(i, ops, ">")

    def i_sqrdmulh(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        # 2ab + round overflows int64 only for MIN * MIN (32-bit lanes), which saturates to MAX anyway.
        return self.v_bin(i, ops, f"(int64_t(a) * int64_t(b) == (int64_t(1) << {2 * bits - 2})) ? int{bits}_t(~0ull >> {65 - bits}) : "
                                  f"nx::sat<int{bits}_t>((int64_t(a) * int64_t(b) * 2 + (int64_t(1) << {bits - 1})) >> {bits})",
                          signed=True)

    def i_shsub(self, f, i, ops):
        return self.v_bin(i, ops, "(int64_t(a) - int64_t(b)) >> 1", signed=True)

    def i_uhsub(self, f, i, ops):
        return self.v_bin(i, ops, "(int64_t(a) - int64_t(b)) >> 1")

    def i_srshl(self, f, i, ops):
        # Shift by the signed low byte of b; right shifts round: (a >> n) + bit n-1 of a (no overflow for 64-bit lanes).
        bits = self.vinfo(ops[0])[0]
        sh = f"int8_t(S1.u{bits}[k])"
        return self.v_bin(i, ops, f"{sh} >= 0 ? ({sh} >= {bits} ? 0 : int64_t(uint64_t(int64_t(a)) << {sh})) : "
                                  f"(-{sh} >= {bits} ? 0 : (int64_t(a) >> -{sh}) + ((int64_t(a) >> (-{sh} - 1)) & 1))",
                          signed=True)

    def i_uabdl(self, f, i, ops):
        return self.widen(i, ops, "{a} > {b} ? {a} - {b} : {b} - {a}", False)

    i_uabdl2 = i_uabdl

    def i_sabdl(self, f, i, ops):
        return self.widen(i, ops, "{a} > {b} ? {a} - {b} : {b} - {a}", True)

    i_sabdl2 = i_sabdl

    def i_umlsl(self, f, i, ops):
        return self.widen(i, ops, "{o} - {a} * {b}", False)

    i_umlsl2 = i_umlsl

    def i_smlsl(self, f, i, ops):
        return self.widen(i, ops, "{o} - {a} * {b}", True)

    i_smlsl2 = i_smlsl

    # crypto extension: helpers in nx/cpu.h
    def i_pmull(self, f, i, ops):
        if ops[0].vas != A.ARM64_VAS_1Q:
            raise Unhandled("pmull on bytes")
        d, a, b = (self.regop(i, o) for o in ops[:3])
        lane = 1 if i.mnemonic == "pmull2" else 0
        return f"c.v[{d.n}] = nx::clmul64(c.v[{a.n}].u64[{lane}], c.v[{b.n}].u64[{lane}]);"

    i_pmull2 = i_pmull

    def crypto2(self, i, ops, fn):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return f"c.v[{d.n}] = nx::{fn}(c.v[{d.n}], c.v[{n.n}]);"

    def crypto1(self, i, ops, fn):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return f"c.v[{d.n}] = nx::{fn}(c.v[{n.n}]);"

    def crypto3(self, i, ops, fn):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return f"c.v[{d.n}] = nx::{fn}(c.v[{d.n}], c.v[{n.n}], c.v[{m.n}]);"

    i_aese = lambda self, f, i, ops: self.crypto2(i, ops, "aese")
    i_aesd = lambda self, f, i, ops: self.crypto2(i, ops, "aesd")
    i_aesmc = lambda self, f, i, ops: self.crypto1(i, ops, "aesmc")
    i_aesimc = lambda self, f, i, ops: self.crypto1(i, ops, "aesimc")
    i_sha1su1 = lambda self, f, i, ops: self.crypto2(i, ops, "sha1su1")
    i_sha256su0 = lambda self, f, i, ops: self.crypto2(i, ops, "sha256su0")
    i_sha1su0 = lambda self, f, i, ops: self.crypto3(i, ops, "sha1su0")
    i_sha256h = lambda self, f, i, ops: self.crypto3(i, ops, "sha256h")
    i_sha256h2 = lambda self, f, i, ops: self.crypto3(i, ops, "sha256h2")
    i_sha256su1 = lambda self, f, i, ops: self.crypto3(i, ops, "sha256su1")

    def sha1_rounds(self, i, ops, fn):  # sha1c q0, s1, v2.4s
        d, e, w = (self.regop(i, o) for o in ops[:3])
        return f"c.v[{d.n}] = nx::{fn}(c.v[{d.n}], c.v[{e.n}].u32[0], c.v[{w.n}]);"

    i_sha1c = lambda self, f, i, ops: self.sha1_rounds(i, ops, "sha1c")
    i_sha1p = lambda self, f, i, ops: self.sha1_rounds(i, ops, "sha1p")
    i_sha1m = lambda self, f, i, ops: self.sha1_rounds(i, ops, "sha1m")

    def i_sha1h(self, f, i, ops):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return f"{{ nx::V128 r = nx::vzero(); r.u32[0] = nx::sha1h(c.v[{n.n}].u32[0]); c.v[{d.n}] = r; }}"
