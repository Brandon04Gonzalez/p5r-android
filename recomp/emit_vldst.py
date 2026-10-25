"""AdvSIMD structured loads and stores (ld1-ld4, st1-st4, single lanes, replicating ldNr). Mixed into
emit.Emitter."""
import re

from .emit import VAS, Reg


class VecLdSt:
    def vlist(self, i, ops):
        regs, mem, after = self.mem_ops(ops)
        return [self.regop(i, o) for o in regs], regs, mem, after

    def vldst(self, i, ops, nstruct, store, replicate=False):
        rs, regs, mem, after = self.vlist(i, ops)
        o0 = regs[0]
        bits, lanes = VAS[o0.vas]
        a, wb = self.address(i, mem, after)
        if i.writeback and after is None:
            # Immediate post-index: capstone drops the operand; it always equals the bytes transferred.
            per_reg = bits // 8 if (o0.vector_index != -1 or replicate) else bits * lanes // 8
            wb = f"c.x[{Reg(i.reg_name(mem.mem.base)).n}] += {per_reg * len(rs)};"
            a = f"c.x[{Reg(i.reg_name(mem.mem.base)).n}]"
        parts = [f"uint64_t a = {a};"]
        esz = bits // 8
        lane_m = re.search(r"\}\[(\d+)\]", i.op_str)  # capstone omits the lane index on multi-register lists
        if o0.vector_index != -1 or lane_m:  # single lane
            lane = o0.vector_index if o0.vector_index != -1 else int(lane_m.group(1))
            for k, r in enumerate(rs):
                if store:
                    parts.append(f"nx::st<uint{bits}_t>(a + {k * esz}, c.v[{r.n}].u{bits}[{lane}]);")
                else:
                    parts.append(f"c.v[{r.n}].u{bits}[{lane}] = nx::ld<uint{bits}_t>(a + {k * esz});")
        elif replicate:
            for k, r in enumerate(rs):
                parts.append(f"{{ uint{bits}_t e_ = nx::ld<uint{bits}_t>(a + {k * esz}); nx::V128 r_ = nx::vzero(); "
                             f"for (int j = 0; j < {lanes}; j++) r_.u{bits}[j] = e_; c.v[{r.n}] = r_; }}")
        elif nstruct == 1:
            size = bits * lanes // 8
            for k, r in enumerate(rs):
                if store:
                    parts.append(f"std::memcpy(reinterpret_cast<void*>(a + {k * size}), &c.v[{r.n}], {size});")
                else:
                    parts.append(f"{{ nx::V128 r_ = nx::vzero(); std::memcpy(&r_, reinterpret_cast<const void*>(a + {k * size}), {size}); c.v[{r.n}] = r_; }}")
        else:
            # Interleaved: element j of structure s is at a + (j * nstruct + s) * esz.
            if store:
                for s_, r in enumerate(rs):
                    parts.append(f"for (int j = 0; j < {lanes}; j++) nx::st<uint{bits}_t>(a + (j * {nstruct} + {s_}) * {esz}, c.v[{r.n}].u{bits}[j]);")
            else:
                tmp = []
                for s_, r in enumerate(rs):
                    parts.append(f"nx::V128 t{s_} = nx::vzero(); for (int j = 0; j < {lanes}; j++) t{s_}.u{bits}[j] = nx::ld<uint{bits}_t>(a + (j * {nstruct} + {s_}) * {esz});")
                    tmp.append(f"c.v[{r.n}] = t{s_};")
                parts += tmp
        parts.append(wb)
        return "{ " + " ".join(p for p in parts if p) + " }"

    def i_ld1(self, f, i, ops):
        return self.vldst(i, ops, 1, False)

    def i_st1(self, f, i, ops):
        return self.vldst(i, ops, 1, True)

    def i_ld1r(self, f, i, ops):
        return self.vldst(i, ops, 1, False, replicate=True)

    def i_ld2(self, f, i, ops):
        return self.vldst(i, ops, 2, False)

    def i_st2(self, f, i, ops):
        return self.vldst(i, ops, 2, True)

    def i_ld3(self, f, i, ops):
        return self.vldst(i, ops, 3, False)

    def i_st3(self, f, i, ops):
        return self.vldst(i, ops, 3, True)

    def i_ld4(self, f, i, ops):
        return self.vldst(i, ops, 4, False)

    def i_st4(self, f, i, ops):
        return self.vldst(i, ops, 4, True)

    def i_ld2r(self, f, i, ops):
        return self.vldst(i, ops, 2, False, replicate=True)

    def i_ld3r(self, f, i, ops):
        return self.vldst(i, ops, 3, False, replicate=True)

    def i_ld4r(self, f, i, ops):
        return self.vldst(i, ops, 4, False, replicate=True)
