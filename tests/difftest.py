"""Differential test of the emitter against Unicorn (reference AArch64).

Samples every instruction *shape* used by a module (mnemonic x operand kinds x arrangement x addressing mode),
emits each sample with the recompiler, runs it natively on random states and on Unicorn, and compares
registers, flags and memory.

Usage: python -m tests.difftest <module.nso> <work dir> [--per-shape N] [--states N] [--filter mnem,...]
(Windows: run from a VS dev shell, needs clang-cl. macOS / Linux: clang++ and `pip install unicorn`.)
"""
import argparse
import collections
import math
import os
import random
import struct
import subprocess
import sys

import capstone
from capstone import arm64_const as A
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UcError
from unicorn import arm64_const as U

from recomp import analysis, emit, nso

NX_BASE = 0x0000004000000000
# Scratch memory for loads and stores, at the same address natively and in Unicorn. macOS arm64 reserves the
# low 4 GiB (__PAGEZERO), so POSIX hosts use a higher address.
BUF = 0x10000000 if os.name == "nt" else 0x300000000
BUF_SIZE = 0x1000
BUF_MID = 0x600

SKIP = {"b", "bl", "blr", "br", "ret", "cbz", "cbnz", "tbz", "tbnz", "udf", "brk", "svc", "hint", "nop", "prfm",
        "dmb", "dsb", "isb", "clrex", "mrs", "msr", "stlxr", "stxr", "stlxp", "stxp", "stlxrb", "stxrb", "stlxrh", "yield"}
# Results that are allowed to differ (estimates).
APPROX = {"frecpe", "frsqrte"}

STATE = struct.Struct("<I I 32Q 512s I")  # test id, has_mem, x[32], v[32]x16, nzcv


def shape(i):
    parts = [i.mnemonic, "wb" if i.writeback else ""]
    for o in i.operands:
        if o.type == A.ARM64_OP_REG:
            n = i.reg_name(o.reg)
            cls = n[0] if n[0] in "xwbhsdqv" and n[1:].isdigit() else n
            parts.append(f"R{cls}{o.vas}{'i' if o.vector_index != -1 else ''}{o.shift.type}{o.ext}")
        elif o.type == A.ARM64_OP_MEM:
            parts.append(f"M{'i' if o.mem.index else ''}{o.shift.type}{o.ext}")
        else:
            parts.append(f"T{o.type}{o.shift.type}")
    return "|".join(parts)


def collect(an, per_shape):
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.detail = True
    m = an.module
    shapes = collections.defaultdict(list)
    seen = collections.Counter()
    rng = random.Random(1)
    for f in an.functions:
        code = bytes(m.image[f.start:f.end])
        for i in cs.disasm(code, f.start):
            if i.mnemonic in SKIP or i.mnemonic.startswith("b."):
                continue
            k = shape(i)
            seen[k] += 1
            # Reservoir sample per shape.
            if len(shapes[k]) < per_shape:
                shapes[k].append(i.address)
            elif rng.random() < per_shape / seen[k]:
                shapes[k][rng.randrange(per_shape)] = i.address
    return shapes, seen


class Fake:
    def __init__(self, a):
        self.start, self.end = a, a + 4


def fvals(rng):
    return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 1e-40, -1e-40, math.inf, -math.inf, 3.4e38, 65504.0,
                       2147483648.0, -2147483649.0, 4294967296.5, 1.5, 2.5, -2.5,
                       rng.uniform(-10, 10), rng.uniform(-1e6, 1e6), rng.uniform(-1e20, 1e20)])


def gvals(rng):
    return rng.choice([0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF, 0x7FFFFFFF,
                       0x80000000, 0x100000000, rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(32),
                       rng.getrandbits(8), rng.randrange(64)])


def vreg(rng):
    kind = rng.randrange(4)
    if kind == 0:
        return rng.getrandbits(128).to_bytes(16, "little")
    if kind == 1:
        return b"".join(struct.pack("<f", fvals(rng)) for _ in range(4))
    if kind == 2:
        return b"".join(struct.pack("<d", fvals(rng)) for _ in range(2))
    return bytes(rng.choice([0, 1, 0x7F, 0x80, 0xFF, rng.getrandbits(8)]) for _ in range(16))


def mem_plan(i):
    """(base reg number, index reg number or None, disp, post-index) of the memory operand, or None."""
    for k, o in enumerate(i.operands):
        if o.type == A.ARM64_OP_MEM:
            base = emit.Reg(i.reg_name(o.mem.base)).n
            idx = emit.Reg(i.reg_name(o.mem.index)).n if o.mem.index else None
            post = i.writeback and k + 1 < len(i.operands)
            return base, idx, o.mem.disp, post
    return None


def make_state(rng, i, tid):
    x = [gvals(rng) for _ in range(31)] + [BUF + BUF_MID]
    v = b"".join(vreg(rng) for _ in range(32))
    nzcv = rng.randrange(16) << 28
    plan = mem_plan(i)
    if plan:
        base, idx, disp, post = plan
        x[base] = BUF + BUF_MID - (0 if post else disp)
        if idx is not None:
            x[idx] = rng.randrange(8)
            if idx == base:
                return None
    return STATE.pack(tid, 1 if plan else 0, *x, v, nzcv)


RUNNER_HEAD = r"""
#include <nx/cpu.h>
#define NXB 0x4000000000ull
#ifdef _WIN32
#include <windows.h>
#else
#include <csetjmp>
#include <csignal>
#include <sys/mman.h>
#endif
#include <cstdio>
#include <cstdlib>
namespace nx {
void call(CPU&, uint64_t t) { std::printf("call %llx\n", (unsigned long long)t); std::exit(3); }
void unimpl(CPU&, uint64_t pc, const char* w) { std::printf("unimpl %llx %s\n", (unsigned long long)pc, w); std::exit(4); }
void svc(CPU&, uint32_t, uint64_t) {}
uint64_t mrs(CPU&, uint32_t) { return 0; }
void msr(CPU&, uint32_t, uint64_t) {}
}
"""

RUNNER_MAIN = r"""
#pragma pack(push, 1)
struct State { uint32_t id, has_mem; uint64_t x[32]; uint8_t v[512]; uint32_t nzcv; };
#pragma pack(pop)
#ifdef _WIN32
static int run(nx::Fn f, nx::CPU& c) {
    __try { f(c); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
static uint8_t* map_buf() {
    return (uint8_t*)VirtualAlloc((void*)BUF_ADDR, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
#else
static sigjmp_buf fault_jmp;
static void on_fault(int) { siglongjmp(fault_jmp, 1); }
static int run(nx::Fn f, nx::CPU& c) {
    if (sigsetjmp(fault_jmp, 1)) return 1;
    f(c);
    return 0;
}
static uint8_t* map_buf() {
    std::signal(SIGSEGV, on_fault);
    std::signal(SIGBUS, on_fault);
    void* p = mmap((void*)BUF_ADDR, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}
#endif
int main(int argc, char** argv) {
    uint8_t* buf = map_buf();
    if (!buf) { std::printf("no buffer\n"); return 2; }
    FILE* in = std::fopen(argv[1], "rb");
    FILE* out = std::fopen(argv[2], "wb");
    State s;
    uint8_t mem[BUF_SIZE];
    while (std::fread(&s, sizeof s, 1, in) == 1) {
        if (s.has_mem) std::fread(mem, sizeof mem, 1, in);
        for (int k = 0; k < 0x10000; k++) buf[k] = uint8_t(k * 131 + 7);
        if (s.has_mem) std::memcpy(buf, mem, sizeof mem);
        nx::CPU c{};
        std::memcpy(c.x, s.x, sizeof c.x);
        std::memcpy(c.v, s.v, sizeof c.v);
        nx::set_nzcv(c, s.nzcv >> 28);
        int crashed = run(tests[s.id], c);
        std::memcpy(s.x, c.x, sizeof c.x);
        std::memcpy(s.v, c.v, sizeof c.v);
        s.nzcv = uint32_t(nx::get_nzcv(c));
        if (crashed) s.has_mem = 2;
        std::fwrite(&s, sizeof s, 1, out);
        if (s.has_mem) std::fwrite(buf, BUF_SIZE, 1, out);
    }
    return 0;
}
"""


def build_runner(work, stmts):
    src = os.path.join(work, "runner.cpp")
    with open(src, "w") as f:
        f.write(RUNNER_HEAD)
        f.write(f"#define BUF_SIZE {BUF_SIZE}\n#define BUF_ADDR {BUF:#x}ull\n")
        for k, s_ in enumerate(stmts):
            f.write(f"static void t{k}(nx::CPU& c) {{ {s_} }}\n")
        f.write("static const nx::Fn tests[] = {" + ", ".join(f"t{k}" for k in range(len(stmts))) + "};\n")
        f.write(RUNNER_MAIN)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.name == "nt":
        exe = os.path.join(work, "runner.exe")
        cmd = ["clang-cl", "/nologo", "/std:c++20", "/O1", "/arch:AVX2", "/EHa", f"/I{root}/runtime/include",
               "-Wno-everything", src, f"/Fe{exe}", f"/Fo{work}/"]
    else:
        exe = os.path.join(work, "runner")
        cmd = ["clang++", "-std=c++20", "-O1", f"-I{root}/runtime/include", "-Wno-everything", src, "-o", exe]
        cmd += os.environ.get("NX_DIFFTEST_CFLAGS", "").split()
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-4000:], r.stderr[-4000:])
        sys.exit(1)
    return exe


def uc_run(uc, m, addr, st, mem):
    tid, has_mem, *rest = STATE.unpack(st)
    x, v, nzcv = rest[:32], rest[32], rest[33]
    for k in range(29):
        uc.reg_write(U.UC_ARM64_REG_X0 + k, x[k])
    uc.reg_write(U.UC_ARM64_REG_X29, x[29])
    uc.reg_write(U.UC_ARM64_REG_X30, x[30])
    uc.reg_write(U.UC_ARM64_REG_SP, x[31])
    for k in range(32):
        uc.reg_write(U.UC_ARM64_REG_Q0 + k, int.from_bytes(v[16 * k:16 * k + 16], "little"))
    uc.reg_write(U.UC_ARM64_REG_NZCV, nzcv)
    uc.reg_write(U.UC_ARM64_REG_FPCR, 0)
    pattern = bytes((k * 131 + 7) & 0xFF for k in range(0x10000))
    uc.mem_write(BUF, pattern)
    if has_mem:
        uc.mem_write(BUF, mem)
    try:
        uc.emu_start(NX_BASE + addr, NX_BASE + addr + 4, count=1)
        crashed = False
    except UcError:
        crashed = True
    xs = [uc.reg_read(U.UC_ARM64_REG_X0 + k) for k in range(29)]
    xs += [uc.reg_read(U.UC_ARM64_REG_X29), uc.reg_read(U.UC_ARM64_REG_X30), uc.reg_read(U.UC_ARM64_REG_SP)]
    vs = b"".join(uc.reg_read(U.UC_ARM64_REG_Q0 + k).to_bytes(16, "little") for k in range(32))
    n = uc.reg_read(U.UC_ARM64_REG_NZCV) & 0xF0000000
    out_mem = bytes(uc.mem_read(BUF, BUF_SIZE)) if has_mem else b""
    return xs, vs, n, out_mem, crashed


def same_v(a, b):
    if a == b:
        return True
    for k in range(0, 16, 4):
        x, y = a[k:k + 4], b[k:k + 4]
        if x != y:
            fx, fy = struct.unpack("<f", x)[0], struct.unpack("<f", y)[0]
            if not (math.isnan(fx) and math.isnan(fy)):
                # maybe a double lane
                dk = k & ~7
                dx, dy = struct.unpack("<d", a[dk:dk + 8])[0], struct.unpack("<d", b[dk:dk + 8])[0]
                if not (math.isnan(dx) and math.isnan(dy)):
                    return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("work")
    ap.add_argument("--per-shape", type=int, default=2)
    ap.add_argument("--states", type=int, default=12)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)

    m = nso.load(args.nso)
    an = analysis.analyze(m, log=lambda *a: None)
    shapes, seen = collect(an, args.per_shape)
    want = set(args.filter.split(",")) if args.filter else None
    print(f"{len(shapes)} shapes")

    e = emit.Emitter(an)
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.detail = True
    tests = []  # (shape, addr, insn text, stmt)
    skipped = collections.Counter()
    for k, addrs in sorted(shapes.items()):
        for a in addrs:
            i = next(cs.disasm(bytes(m.image[a:a + 4]), a))
            if want and i.mnemonic not in want:
                continue
            if mem_plan(i) is None and any(o.type == A.ARM64_OP_MEM for o in i.operands):
                continue
            if i.mnemonic.startswith("ld") and not any(o.type == A.ARM64_OP_MEM for o in i.operands):
                skipped["literal"] += 1
                continue
            e.labels = set()
            try:
                stmt = e.insn(Fake(a), i, {})
            except emit.Unhandled as ex:
                skipped[f"unhandled {i.mnemonic}"] += 1
                continue
            if "nx::call" in stmt or "unimpl" in stmt:
                skipped["control"] += 1
                continue
            tests.append((k, a, f"{i.mnemonic} {i.op_str}", stmt))
    print(f"{len(tests)} tests; skipped {dict(skipped)}")

    exe = build_runner(args.work, [t[3] for t in tests])
    rng = random.Random(42)
    states = []
    with open(os.path.join(args.work, "in.bin"), "wb") as f:
        for tid, (k, a, txt, _) in enumerate(tests):
            i = next(cs.disasm(bytes(m.image[a:a + 4]), a))
            for _ in range(args.states):
                st = make_state(rng, i, tid)
                if st is None:
                    continue
                mem = rng.getrandbits(8 * BUF_SIZE).to_bytes(BUF_SIZE, "little") if STATE.unpack(st)[1] else b""
                states.append((tid, st, mem))
                f.write(st + mem)
    r = subprocess.run([exe, os.path.join(args.work, "in.bin"), os.path.join(args.work, "out.bin")],
                       capture_output=True, text=True)
    if r.returncode:
        print("runner failed", r.returncode, r.stdout[-2000:])
        sys.exit(1)

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(U.UC_CPU_ARM64_MAX)  # ARMv8.1+ (LSE atomics) and the crypto extension
    t0, tn = m.text
    uc.mem_map(NX_BASE, (tn + 0xFFF) & ~0xFFF)
    uc.mem_write(NX_BASE, bytes(m.image[t0:t0 + tn]))
    uc.mem_map(BUF, 0x10000)

    fails = collections.defaultdict(list)
    out = open(os.path.join(args.work, "out.bin"), "rb")
    for tid, st, mem in states:
        rec = out.read(STATE.size)
        _, has_mem, *rest = STATE.unpack(rec)
        hmem = out.read(BUF_SIZE) if has_mem else b""
        hx, hv, hn = rest[:32], rest[32], rest[33]
        k, a, txt, stmt = tests[tid]
        ux, uv, un, umem, ucrash = uc_run(uc, m, a, st, mem)
        if has_mem == 2 or ucrash:
            if not (has_mem == 2 and ucrash):
                fails[k].append((txt, a, f"crash host={has_mem == 2} uc={ucrash}"))
            continue
        diffs = []
        for r_ in range(32):
            if hx[r_] != ux[r_]:
                diffs.append(f"x{r_}: host {hx[r_]:#x} uc {ux[r_]:#x} (in {STATE.unpack(st)[2 + r_]:#x})")
        for r_ in range(32):
            if not same_v(hv[16 * r_:16 * r_ + 16], uv[16 * r_:16 * r_ + 16]):
                diffs.append(f"v{r_}: host {hv[16 * r_:16 * r_ + 16].hex()} uc {uv[16 * r_:16 * r_ + 16].hex()}")
        if hn != un:
            diffs.append(f"nzcv host {hn >> 28:04b} uc {un >> 28:04b}")
        if has_mem and hmem != umem:
            diffs.append("memory")
        if diffs and txt.split()[0] not in APPROX:
            fails[k].append((txt, a, "; ".join(diffs[:3])))

    print(f"\n{len(tests)} tests, {len(states)} runs, {len(fails)} failing shapes "
          f"({sum(seen[k] for k in fails)} instructions in the module)")
    rep = open(os.path.join(args.work, "report.txt"), "w")
    for k, lst in sorted(fails.items(), key=lambda kv: -seen[kv[0]]):
        txt, a, d = lst[0]
        line = f"[{seen[k]:6}x] {a:#x} {txt}: {d}"
        print(line[:300])
        rep.write(line + "\n    " + tests[[t[0] for t in tests].index(k)][3] + "\n")


if __name__ == "__main__":
    main()
