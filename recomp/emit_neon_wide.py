"""AdvSIMD (NEON): widening and narrowing forms, permutes and reductions. Mixed into emit.Emitter (split from
emit.py, 700-line rule)."""

from capstone import arm64_const as A

from .emit import VAS, u, s  # noqa: F401


class NeonWideOps:
    # widening / narrowing
    def widen(self, i, ops, expr, signed, second_wide=False, acc=False):
        """Destination lanes are 2x the source lanes. `2` variants read the upper half of narrow sources."""
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        sbits = dbits // 2
        kind = "s" if signed else "u"
        d = self.regop(i, ops[0])
        srcs = []
        pre = []
        for k, o in enumerate(ops[1:3] if len(ops) > 2 and ops[2].type == A.ARM64_OP_REG else ops[1:2]):
            r = self.regop(i, o)
            pre.append(f"nx::V128 S{k} = c.v[{r.n}];")
            wide = second_wide and k == 0
            if o.vector_index != -1:
                srcs.append(f"int64_t(S{k}.{kind}{sbits}[{o.vector_index}])" if signed else f"uint64_t(S{k}.{kind}{sbits}[{o.vector_index}])")
            elif wide:
                srcs.append(f"{'int64_t' if signed else 'uint64_t'}(S{k}.{kind}{dbits}[k])")
            else:
                idx = f"k + {n}" if hi else "k"
                srcs.append(f"{'int64_t' if signed else 'uint64_t'}(S{k}.{kind}{sbits}[{idx}])")
        imm = ops[2].imm if len(ops) > 2 and ops[2].type == A.ARM64_OP_IMM else 0
        e = expr.format(a=srcs[0], b=srcs[1] if len(srcs) > 1 else "0", n=imm, o=f"O.{kind}{dbits}[k]")
        return (f"{{ {' '.join(pre)} nx::V128 O = c.v[{d.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{dbits}[k] = {'int' if signed else 'uint'}{dbits}_t({e}); c.v[{d.n}] = r; }}")

    def i_ushll(self, f, i, ops):
        return self.widen(i, ops, "{a} << {n}", False)

    i_ushll2 = i_ushll

    def i_sshll(self, f, i, ops):
        return self.widen(i, ops, "{a} << {n}", True)

    i_sshll2 = i_sshll

    def i_uxtl(self, f, i, ops):
        return self.widen(i, ops, "{a}", False)

    i_uxtl2 = i_uxtl

    def i_sxtl(self, f, i, ops):
        return self.widen(i, ops, "{a}", True)

    i_sxtl2 = i_sxtl

    def i_shll(self, f, i, ops):
        bits = self.vinfo(ops[0])[0] // 2
        return self.widen(i, ops, "{a} << " + str(bits), False)

    i_shll2 = i_shll

    def i_uaddw(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", False, second_wide=True)

    i_uaddw2 = i_uaddw

    def i_saddw(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", True, second_wide=True)

    i_saddw2 = i_saddw

    def i_usubw(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", False, second_wide=True)

    i_usubw2 = i_usubw

    def i_ssubw(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", True, second_wide=True)

    i_ssubw2 = i_ssubw

    def i_uaddl(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", False)

    i_uaddl2 = i_uaddl

    def i_saddl(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", True)

    i_saddl2 = i_saddl

    def i_usubl(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", False)

    i_usubl2 = i_usubl

    def i_ssubl(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", True)

    i_ssubl2 = i_ssubl

    def v_long(self, i, ops, expr, signed, high):
        return self.widen(i, ops, "{a} * {b}", signed)

    i_umull2 = lambda self, f, i, ops: self.widen(i, ops, "{a} * {b}", False)
    i_smull2 = lambda self, f, i, ops: self.widen(i, ops, "{a} * {b}", True)

    def i_umlal(self, f, i, ops):
        return self.widen(i, ops, "{o} + {a} * {b}", False)

    i_umlal2 = i_umlal

    def i_smlal(self, f, i, ops):
        return self.widen(i, ops, "{o} + {a} * {b}", True)

    i_smlal2 = i_smlal

    def pairwise_long(self, i, ops, signed):
        dbits, n = self.vinfo(ops[0])
        sbits = dbits // 2
        kind = "s" if signed else "u"
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{dbits}[k] = {'int' if signed else 'uint'}{dbits}_t({'int64_t' if signed else 'uint64_t'}(S.{kind}{sbits}[2 * k]) + S.{kind}{sbits}[2 * k + 1]); c.v[{d.n}] = r; }}")

    def i_saddlp(self, f, i, ops):
        return self.pairwise_long(i, ops, True)

    def i_uaddlp(self, f, i, ops):
        return self.pairwise_long(i, ops, False)

    def narrow(self, i, ops, expr, src_kind="u", dst_kind="u"):
        """Destination lanes are half the source lanes; `2` variants write the upper half and keep the lower."""
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        sbits = dbits * 2
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        cnt = n // 2 if hi else n
        base = n // 2 if hi else 0
        imm = ops[2].imm if len(ops) > 2 and ops[2].type == A.ARM64_OP_IMM else 0
        sty = "int64_t" if src_kind == "s" else "uint64_t"
        e = expr.format(a=f"{sty}(S.{src_kind}{sbits}[k])", n=imm, dbits=dbits)
        init = f"c.v[{d.n}]" if hi else "nx::vzero()"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = {init}; for (int k = 0; k < {cnt}; k++) "
                f"r.{dst_kind}{dbits}[{base} + k] = {'int' if dst_kind == 's' else 'uint'}{dbits}_t({e}); c.v[{d.n}] = r; }}")

    def i_xtn(self, f, i, ops):
        return self.narrow(i, ops, "{a}")

    i_xtn2 = i_xtn

    def i_shrn(self, f, i, ops):
        return self.narrow(i, ops, "{a} >> {n}")

    i_shrn2 = i_shrn

    def i_sqshrn(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<int{dbits}_t>({a} >> {n})", src_kind="s", dst_kind="s")

    i_sqshrn2 = i_sqshrn

    def i_sqrshrn(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<int{dbits}_t>(({a} + (int64_t(1) << ({n} - 1))) >> {n})",
                           src_kind="s", dst_kind="s")

    i_sqrshrn2 = i_sqrshrn

    def i_sqadd(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"nx::sat<int{bits}_t>(int64_t(a) + int64_t(b))", signed=True)

    def i_sqsub(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"nx::sat<int{bits}_t>(int64_t(a) - int64_t(b))", signed=True)

    def i_uqadd(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"(uint64_t(a) + uint64_t(b)) > uint{bits}_t(~0ull) ? uint{bits}_t(~0ull) : a + b")

    def i_uqsub(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a - b : 0")

    def i_uaddlv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint64_t s_ = 0; for (int k = 0; k < {n}; k++) s_ += c.v[{a.n}].u{bits}[k]; "
                f"nx::V128 r = nx::vzero(); r.u{bits * 2}[0] = uint{bits * 2}_t(s_); c.v[{d.n}] = r; }}")

    def i_smaxv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ int{bits}_t s_ = c.v[{a.n}].s{bits}[0]; for (int k = 1; k < {n}; k++) s_ = c.v[{a.n}].s{bits}[k] > s_ ? c.v[{a.n}].s{bits}[k] : s_; "
                f"nx::V128 r = nx::vzero(); r.s{bits}[0] = s_; c.v[{d.n}] = r; }}")

    def i_frsqrts(self, f, i, ops):
        return self.fbin(i, ops, "nx::frsqrts(a, b)")

    def i_frecps(self, f, i, ops):
        return self.fbin(i, ops, "nx::frecps(a, b)")

    def i_sqxtun(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<uint{dbits}_t>({a})", src_kind="s")

    i_sqxtun2 = i_sqxtun

    def i_sqxtn(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<int{dbits}_t>({a})", src_kind="s", dst_kind="s")

    i_sqxtn2 = i_sqxtn

    def i_uqxtn(self, f, i, ops):
        return self.narrow(i, ops, "{a} > uint{dbits}_t(~0ull) ? uint{dbits}_t(~0ull) : {a}")

    i_uqxtn2 = i_uqxtn

    def i_sqshrun(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<uint{dbits}_t>({a} >> {n})", src_kind="s")

    i_sqshrun2 = i_sqshrun

    def i_sqrshrun(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<uint{dbits}_t>(({a} + (int64_t(1) << ({n} - 1))) >> {n})", src_kind="s")

    i_sqrshrun2 = i_sqrshrun

    def i_fcvtn(self, f, i, ops):
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        cnt = n // 2 if hi else n
        base = n // 2 if hi else 0
        init = f"c.v[{d.n}]" if hi else "nx::vzero()"
        if dbits == 32:
            body = f"r.f32[{base} + k] = float(S.f64[k]);"
        else:
            body = f"r.u16[{base} + k] = nx::f32_to_f16(S.f32[k]);"
        return f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = {init}; for (int k = 0; k < {cnt}; k++) {body} c.v[{d.n}] = r; }}"

    i_fcvtn2 = i_fcvtn

    def i_fcvtl(self, f, i, ops):
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        off = f"k + {n}" if hi else "k"
        body = f"r.f64[k] = double(S.f32[{off}]);" if dbits == 64 else f"r.f32[k] = nx::f16_to_f32(S.u16[{off}]);"
        return f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) {body} c.v[{d.n}] = r; }}"

    i_fcvtl2 = i_fcvtl

    def v_cvt_to_int(self, i, ops, rnd, signed):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        v = f"S.f{bits}[k]"
        if len(ops) > 2:
            v = f"({v}) * {float(2 ** ops[2].imm)!r}"
        v = {"z": f"std::trunc({v})", "m": f"std::floor({v})", "p": f"std::ceil({v})",
             "a": f"nx::round_away({v})", "n": f"nx::round_even({v})"}[rnd]
        t = f"{'int' if signed else 'uint'}{bits}_t"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{'s' if signed else 'u'}{bits}[k] = nx::fcvt_sat<{t}>({v}); c.v[{d.n}] = r; }}")

    # permutes
    def i_ext(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        nbytes = n * bits // 8
        d, a, b = (self.regop(i, o) for o in ops[:3])
        imm = ops[3].imm
        return (f"{{ uint8_t t_[32]; std::memcpy(t_, &c.v[{a.n}], {nbytes}); std::memcpy(t_ + {nbytes}, &c.v[{b.n}], {nbytes}); "
                f"nx::V128 r = nx::vzero(); std::memcpy(&r, t_ + {imm}, {nbytes}); c.v[{d.n}] = r; }}")

    def permute(self, i, ops, body):
        bits, n = self.vinfo(ops[0])
        d, a, b = (self.regop(i, o) for o in ops[:3])
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n // 2}; k++) {{ {body.format(t=f'u{bits}', h=n // 2)} }} c.v[{d.n}] = r; }}")

    def i_zip1(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[k]; r.{t}[2 * k + 1] = B_.{t}[k];")

    def i_zip2(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[k + {h}]; r.{t}[2 * k + 1] = B_.{t}[k + {h}];")

    def i_uzp1(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[k] = A_.{t}[2 * k]; r.{t}[k + {h}] = B_.{t}[2 * k];")

    def i_uzp2(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[k] = A_.{t}[2 * k + 1]; r.{t}[k + {h}] = B_.{t}[2 * k + 1];")

    def i_trn1(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[2 * k]; r.{t}[2 * k + 1] = B_.{t}[2 * k];")

    def i_trn2(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[2 * k + 1]; r.{t}[2 * k + 1] = B_.{t}[2 * k + 1];")

    def v_rev(self, i, ops, container):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        per = container // bits
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.u{bits}[k] = S.u{bits}[(k / {per}) * {per} + ({per} - 1 - k % {per})]; c.v[{d.n}] = r; }}")

    def i_tbl(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        d = self.regop(i, ops[0])
        tables = [self.regop(i, o) for o in ops[1:-1]]
        idx = self.regop(i, ops[-1])
        cp = " ".join(f"std::memcpy(t_ + {16 * k}, &c.v[{r.n}], 16);" for k, r in enumerate(tables))
        return (f"{{ uint8_t t_[64]; {cp} nx::V128 I_ = c.v[{idx.n}]; nx::V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n}; k++) r.u8[k] = I_.u8[k] < {16 * len(tables)} ? t_[I_.u8[k]] : 0; c.v[{d.n}] = r; }}")

    # reductions
    def i_addv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint64_t s_ = 0; for (int k = 0; k < {n}; k++) s_ += c.v[{a.n}].u{bits}[k]; "
                f"nx::V128 r = nx::vzero(); r.u{bits}[0] = uint{bits}_t(s_); c.v[{d.n}] = r; }}")

    def i_umaxv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint{bits}_t s_ = 0; for (int k = 0; k < {n}; k++) s_ = c.v[{a.n}].u{bits}[k] > s_ ? c.v[{a.n}].u{bits}[k] : s_; "
                f"nx::V128 r = nx::vzero(); r.u{bits}[0] = s_; c.v[{d.n}] = r; }}")

    def i_fmaxnmv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; float s_ = nx::fmaxnm(nx::fmaxnm(S.f32[0], S.f32[1]), nx::fmaxnm(S.f32[2], S.f32[3])); "
                f"nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[{d.n}] = r; }}")

    def i_fminnmv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; float s_ = nx::fminnm(nx::fminnm(S.f32[0], S.f32[1]), nx::fminnm(S.f32[2], S.f32[3])); "
                f"nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[{d.n}] = r; }}")

    def i_faddp(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not ops[0].vas:  # faddp s0, v1.2s
            a = self.regop(i, ops[1])
            bits = VAS[ops[1].vas][0]
            return self.fw(d, f"c.v[{a.n}].f{bits}[0] + c.v[{a.n}].f{bits}[1]")
        bits, n = self.vinfo(ops[0])
        a, b = self.regop(i, ops[1]), self.regop(i, ops[2])
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n // 2}; k++) "
                f"{{ r.f{bits}[k] = A_.f{bits}[2 * k] + A_.f{bits}[2 * k + 1]; r.f{bits}[k + {n // 2}] = B_.f{bits}[2 * k] + B_.f{bits}[2 * k + 1]; }} c.v[{d.n}] = r; }}")

    def i_addp(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not ops[0].vas:  # addp d0, v1.2d
            a = self.regop(i, ops[1])
            return f"{{ nx::V128 r = nx::vzero(); r.u64[0] = c.v[{a.n}].u64[0] + c.v[{a.n}].u64[1]; c.v[{d.n}] = r; }}"
        bits, n = self.vinfo(ops[0])
        a, b = self.regop(i, ops[1]), self.regop(i, ops[2])
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n // 2}; k++) "
                f"{{ r.u{bits}[k] = A_.u{bits}[2 * k] + A_.u{bits}[2 * k + 1]; r.u{bits}[k + {n // 2}] = B_.u{bits}[2 * k] + B_.u{bits}[2 * k + 1]; }} c.v[{d.n}] = r; }}")

    def i_cnt(self, f, i, ops):
        return self.v_un(i, ops, "std::popcount(a)")

    def i_not(self, f, i, ops):
        return self.v_un(i, ops, "~a", lanes="bytes")

    def i_abs(self, f, i, ops):
        return self.v_un(i, ops, "a < 0 ? -a : a", signed=True)

    def i_frecpe(self, f, i, ops):
        return self.fun(i, ops, "1.0f / a")

    def i_frsqrte(self, f, i, ops):
        return self.fun(i, ops, "1.0f / std::sqrt(a)")
