// Native code execution: see nce.h.
#include "nce.h"

#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "platform.h"

#if !defined(__aarch64__)
#error "NX_NCE runs the game's AArch64 code natively: AArch64 hosts only"
#endif

namespace nx {
void hle_invoke(CPU& c, size_t index);  // loader.cpp: the HLE function of thunk `index`
}

namespace nx::nce {

namespace {

// The thread's block. TPIDR_EL0 points at `slots` while guest code runs; the fields the stubs use sit at fixed
// offsets from there (the asm below has them as TP_*).
struct Block {
    uint64_t below[2];   // host tp-16, tp-8: bionic's TLS slots -2 and -1
    uint64_t slots[32];  // host tp+0..: slots 0..8 copied, the rest zero
    uint64_t magic;      // tp+0x100
    uint64_t host_tp;    // tp+0x108  the host's TPIDR_EL0
    uint64_t guest_tpidr;
    uint64_t guest_tpidrro;
    CPU* ctx;            // tp+0x120  the guest registers while the runtime runs
    uint64_t host_sp;    // tp+0x128  host stack for the runtime's code (the frame of the latest nce_enter)
    uint64_t host_x18;
    uint64_t target;     // tp+0x138  where nce_enter starts
    uint64_t x16, x17;   // tp+0x140  an svc's x16/x17, for its resume stub
    uint64_t dispatch;   // tp+0x150  nce_dispatch
    uint64_t host_fpcr;
};
#define NCE_TP(f) (offsetof(Block, f) - offsetof(Block, slots))
static_assert(NCE_TP(magic) == 0x100 && NCE_TP(host_tp) == 0x108 && NCE_TP(guest_tpidr) == 0x110 &&
              NCE_TP(guest_tpidrro) == 0x118 && NCE_TP(ctx) == 0x120 && NCE_TP(host_sp) == 0x128 &&
              NCE_TP(host_x18) == 0x130 && NCE_TP(target) == 0x138 && NCE_TP(x16) == 0x140 &&
              NCE_TP(x17) == 0x148 && NCE_TP(dispatch) == 0x150 && NCE_TP(host_fpcr) == 0x158);
static_assert(offsetof(CPU, x) == 0 && offsetof(CPU, v) == 256 && offsetof(CPU, nf) == 768 &&
              offsetof(CPU, zf) == 769 && offsetof(CPU, cf) == 770 && offsetof(CPU, vf) == 771 &&
              offsetof(CPU, fpcr) == 776 && offsetof(CPU, fpsr) == 784 && offsetof(CPU, tpidr) == 792 &&
              offsetof(CPU, tpidrro) == 800);

constexpr uint64_t kMagic = 0x314B4C425F45434Eull;  // "NCE_BLK1"
constexpr uint64_t kThunkExtra = 0x10000;  // after the thunk stubs: their copy of the common code
constexpr uint64_t kRegionSize = 0x40000;  // stubs next to each module

}  // namespace

}  // namespace nx::nce

// ---- the transitions ------------------------------------------------------------------------------------------
// nce_region_start..nce_region_end is copied next to every module and after the thunk stubs (a B reaches +-128 MiB):
//   nce_common  every stub branches here with [sp] = the guest's x16, x17 (pushed), w16 = what to run (bit 31: svc
//               site index, bit 30: hook index, else thunk index) and x17 = the svc's resume stub. Saves the guest
//               registers into the thread's CPU, runs nce_dispatch on the host stack and returns into the guest: an
//               HLE function or hook returns to the x30 it was called with, an svc resumes at its stub (which reloads
//               x16/x17 from the block and branches back after the svc).
//   nce_return  the x30 of every guest call made by nce_enter: saves the guest registers and returns from nce_enter.
// nce_enter(tp) itself is ordinary host code: it saves the host's callee-saved registers on the host stack, makes
// that frame the host stack for nested runtime calls and starts the guest at TP_TARGET with the CPU's registers.
// Register use on the host side: x19 = block (tp), x20 = CPU, x21 = code, x22 = resume stub, x23 = the guest's x30
// on entry, x24 = the host stack (all callee-saved in the host ABI, so they survive nce_dispatch, even across a
// host fiber switch).
__asm__(R"(
.text
.equ TP_HOST_TP, 0x108
.equ TP_GUEST_TPIDR, 0x110
.equ TP_GUEST_TPIDRRO, 0x118
.equ TP_CTX, 0x120
.equ TP_HOST_SP, 0x128
.equ TP_HOST_X18, 0x130
.equ TP_TARGET, 0x138
.equ TP_X16, 0x140
.equ TP_DISPATCH, 0x150
.equ TP_HOST_FPCR, 0x158

// Guest x2..x15 and x18..x30 into the CPU at \b.
.macro NCE_SAVE_GPRS b
    stp x2, x3, [\b, #16]
    stp x4, x5, [\b, #32]
    stp x6, x7, [\b, #48]
    stp x8, x9, [\b, #64]
    stp x10, x11, [\b, #80]
    stp x12, x13, [\b, #96]
    stp x14, x15, [\b, #112]
    stp x18, x19, [\b, #144]
    stp x20, x21, [\b, #160]
    stp x22, x23, [\b, #176]
    stp x24, x25, [\b, #192]
    stp x26, x27, [\b, #208]
    stp x28, x29, [\b, #224]
    str x30, [\b, #240]
.endm

// x19 = block, x20 = CPU: the guest's vector registers, flags, FPCR/FPSR and TLS registers into the CPU, then the
// host's FPCR, TPIDR_EL0 and x18.
.macro NCE_SAVE_REST
    add x2, x20, #256
    stp q0, q1, [x2, #0]
    stp q2, q3, [x2, #32]
    stp q4, q5, [x2, #64]
    stp q6, q7, [x2, #96]
    stp q8, q9, [x2, #128]
    stp q10, q11, [x2, #160]
    stp q12, q13, [x2, #192]
    stp q14, q15, [x2, #224]
    stp q16, q17, [x2, #256]
    stp q18, q19, [x2, #288]
    stp q20, q21, [x2, #320]
    stp q22, q23, [x2, #352]
    stp q24, q25, [x2, #384]
    stp q26, q27, [x2, #416]
    stp q28, q29, [x2, #448]
    stp q30, q31, [x2, #480]
    mrs x3, nzcv
    ubfx w4, w3, #31, #1
    strb w4, [x20, #768]
    ubfx w4, w3, #30, #1
    strb w4, [x20, #769]
    ubfx w4, w3, #29, #1
    strb w4, [x20, #770]
    ubfx w4, w3, #28, #1
    strb w4, [x20, #771]
    mrs x4, fpsr
    str x4, [x20, #784]
    mrs x3, fpcr
    str x3, [x20, #776]
    ldr x4, [x19, #TP_GUEST_TPIDR]
    str x4, [x20, #792]
    ldr x4, [x19, #TP_GUEST_TPIDRRO]
    str x4, [x20, #800]
    ldr x4, [x19, #TP_HOST_FPCR]
    cmp x3, x4
    b.eq 1f
    msr fpcr, x4
1:
    ldr x4, [x19, #TP_HOST_TP]
    msr tpidr_el0, x4
    ldr x18, [x19, #TP_HOST_X18]
.endm

// x19 = block, x20 = CPU: back to the guest's TLS registers, FPCR/FPSR, flags, vector registers and stack.
.macro NCE_LOAD_REST
    ldr x3, [x20, #792]
    str x3, [x19, #TP_GUEST_TPIDR]
    ldr x3, [x20, #800]
    str x3, [x19, #TP_GUEST_TPIDRRO]
    mrs x3, fpcr
    ldr x4, [x20, #776]
    cmp x3, x4
    b.eq 1f
    msr fpcr, x4
1:
    ldr x4, [x20, #784]
    msr fpsr, x4
    ldrb w3, [x20, #768]
    ldrb w4, [x20, #769]
    orr w3, w4, w3, lsl #1
    ldrb w4, [x20, #770]
    orr w3, w4, w3, lsl #1
    ldrb w4, [x20, #771]
    orr w3, w4, w3, lsl #1
    lsl w3, w3, #28
    msr nzcv, x3
    add x2, x20, #256
    ldp q0, q1, [x2, #0]
    ldp q2, q3, [x2, #32]
    ldp q4, q5, [x2, #64]
    ldp q6, q7, [x2, #96]
    ldp q8, q9, [x2, #128]
    ldp q10, q11, [x2, #160]
    ldp q12, q13, [x2, #192]
    ldp q14, q15, [x2, #224]
    ldp q16, q17, [x2, #256]
    ldp q18, q19, [x2, #288]
    ldp q20, q21, [x2, #320]
    ldp q22, q23, [x2, #352]
    ldp q24, q25, [x2, #384]
    ldp q26, q27, [x2, #416]
    ldp q28, q29, [x2, #448]
    ldp q30, q31, [x2, #480]
    msr tpidr_el0, x19
    ldr x2, [x20, #248]
    mov sp, x2
.endm

// The guest's general registers from the CPU at x20 (x20 itself last); x16/x17 only with \with16.
.macro NCE_LOAD_GPRS with16
    ldp x0, x1, [x20, #0]
    ldp x2, x3, [x20, #16]
    ldp x4, x5, [x20, #32]
    ldp x6, x7, [x20, #48]
    ldp x8, x9, [x20, #64]
    ldp x10, x11, [x20, #80]
    ldp x12, x13, [x20, #96]
    ldp x14, x15, [x20, #112]
.if \with16
    ldp x16, x17, [x20, #128]
.endif
    ldr x18, [x20, #144]
    ldp x21, x22, [x20, #168]
    ldp x23, x24, [x20, #184]
    ldp x25, x26, [x20, #200]
    ldp x27, x28, [x20, #216]
    ldp x29, x30, [x20, #232]
    ldp x19, x20, [x20, #152]
.endm

.p2align 4
.globl nce_region_start
.hidden nce_region_start
nce_region_start:

.globl nce_common
.hidden nce_common
nce_common:
    stp x0, x1, [sp, #-16]!
    mrs x0, tpidr_el0
    ldr x1, [x0, #TP_CTX]
    NCE_SAVE_GPRS x1
    ldp x2, x3, [sp], #16
    stp x2, x3, [x1, #0]
    ldp x2, x3, [sp], #16
    stp x2, x3, [x1, #128]
    mov x2, sp
    str x2, [x1, #248]
    mov x19, x0
    mov x20, x1
    mov w21, w16
    mov x22, x17
    ldr x23, [x20, #240]
    NCE_SAVE_REST
    ldr x24, [x19, #TP_HOST_SP]
    mov sp, x24
    mov x0, x19
    mov w1, w21
    ldr x9, [x19, #TP_DISPATCH]
    blr x9
    str x24, [x19, #TP_HOST_SP]
    NCE_LOAD_REST
    tbnz w21, #31, 2f
    str x23, [x20, #240]
    NCE_LOAD_GPRS 1
    ret
2:
    ldp x2, x3, [x20, #128]
    stp x2, x3, [x19, #TP_X16]
    mov x17, x22
    NCE_LOAD_GPRS 0
    br x17

.p2align 4
.globl nce_return
.hidden nce_return
nce_return:
    stp x0, x1, [sp, #-16]!
    mrs x0, tpidr_el0
    ldr x1, [x0, #TP_CTX]
    NCE_SAVE_GPRS x1
    stp x16, x17, [x1, #128]
    ldp x2, x3, [sp], #16
    stp x2, x3, [x1, #0]
    mov x2, sp
    str x2, [x1, #248]
    mov x19, x0
    mov x20, x1
    NCE_SAVE_REST
    ldr x2, [x19, #TP_HOST_SP]
    mov sp, x2
    ldr x3, [sp, #0xa0]
    str x3, [x19, #TP_HOST_SP]
    ldp x19, x20, [sp, #0x00]
    ldp x21, x22, [sp, #0x10]
    ldp x23, x24, [sp, #0x20]
    ldp x25, x26, [sp, #0x30]
    ldp x27, x28, [sp, #0x40]
    ldp x29, x30, [sp, #0x50]
    ldp d8, d9, [sp, #0x60]
    ldp d10, d11, [sp, #0x70]
    ldp d12, d13, [sp, #0x80]
    ldp d14, d15, [sp, #0x90]
    add sp, sp, #0xb0
    ret

.p2align 4
.globl nce_region_end
.hidden nce_region_end
nce_region_end:

.p2align 4
.globl nce_enter
.hidden nce_enter
nce_enter:
    hint #34
    sub sp, sp, #0xb0
    stp x19, x20, [sp, #0x00]
    stp x21, x22, [sp, #0x10]
    stp x23, x24, [sp, #0x20]
    stp x25, x26, [sp, #0x30]
    stp x27, x28, [sp, #0x40]
    stp x29, x30, [sp, #0x50]
    stp d8, d9, [sp, #0x60]
    stp d10, d11, [sp, #0x70]
    stp d12, d13, [sp, #0x80]
    stp d14, d15, [sp, #0x90]
    ldr x2, [x0, #TP_HOST_SP]
    str x2, [sp, #0xa0]
    mov x2, sp
    str x2, [x0, #TP_HOST_SP]
    mrs x2, tpidr_el0
    str x2, [x0, #TP_HOST_TP]
    str x18, [x0, #TP_HOST_X18]
    mrs x2, fpcr
    str x2, [x0, #TP_HOST_FPCR]
    mov x19, x0
    ldr x20, [x19, #TP_CTX]
    NCE_LOAD_REST
    ldr x16, [x19, #TP_TARGET]
    ldr x17, [x20, #136]
    NCE_LOAD_GPRS 0
    br x16
)");

extern "C" {
extern const uint8_t nce_region_start[], nce_common[], nce_return[], nce_region_end[];
void nce_enter(uint64_t tp);
void nce_dispatch(uint64_t tp, uint32_t code);
}

namespace nx::nce {

namespace {

uint64_t page_size() {
    static const uint64_t p = uint64_t(sysconf(_SC_PAGESIZE));
    return p;
}

uint64_t tp_of(Block* b) { return reinterpret_cast<uint64_t>(&b->slots[0]); }
Block* block_of(uint64_t tp) { return reinterpret_cast<Block*>(tp - offsetof(Block, slots)); }

uint64_t host_tp() {
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

// The host thread's TLS slots into the block (from host code only: TPIDR_EL0 is the host's).
void refresh(Block* b) {
    const uint64_t tp = host_tp();
    std::memcpy(b->below, reinterpret_cast<const void*>(tp - 16), sizeof b->below);
    std::memcpy(b->slots, reinterpret_cast<const void*>(tp), 9 * sizeof(uint64_t));
}

thread_local Block* t_block = nullptr;

Block* block() {
    if (Block* b = t_block) return b;
    auto* b = static_cast<Block*>(std::aligned_alloc(64, (sizeof(Block) + 63) & ~size_t(63)));
    std::memset(b, 0, sizeof *b);
    b->magic = kMagic;
    b->dispatch = reinterpret_cast<uint64_t>(&nce_dispatch);
    t_block = b;
    return b;
}

// ---- encodings ----

uint32_t b_to(uint64_t from, uint64_t to) {
    const int64_t d = int64_t(to - from);
    if ((d & 3) || d < -(int64_t(1) << 27) || d >= (int64_t(1) << 27))
        fatal("nce: branch from %llx to %llx out of range", (unsigned long long)from, (unsigned long long)to);
    return 0x14000000u | (uint32_t(d >> 2) & 0x3FFFFFFu);
}

uint32_t adr_to(uint64_t from, uint64_t to, uint32_t rd) {
    const int64_t d = int64_t(to - from);
    if (d < -(int64_t(1) << 20) || d >= (int64_t(1) << 20)) fatal("nce: adr out of range");
    return 0x10000000u | (uint32_t(d & 3) << 29) | ((uint32_t(d >> 2) & 0x7FFFFu) << 5) | rd;
}

uint32_t ldr_literal(uint64_t from, uint64_t to, uint32_t rt) {
    const int64_t d = int64_t(to - from);
    if ((d & 3) || d < -(int64_t(1) << 20) || d >= (int64_t(1) << 20)) fatal("nce: literal out of range");
    return 0x58000000u | ((uint32_t(d >> 2) & 0x7FFFFu) << 5) | rt;
}

constexpr uint32_t kPushX16X17 = 0xA9BF47F0;  // stp x16, x17, [sp, #-16]!
constexpr uint32_t kNop = 0xD503201F;
uint32_t movz_w16(uint32_t imm) { return 0x52800000u | (imm & 0xFFFF) << 5 | 16; }
uint32_t movk_w16_hi(uint32_t imm) { return 0x72A00000u | (imm & 0xFFFF) << 5 | 16; }
uint32_t mrs_tpidr(uint32_t rt) { return 0xD53BD040u | rt; }        // mrs xT, tpidr_el0
uint32_t mrs_cntvct(uint32_t rt) { return 0xD53BE040u | rt; }       // mrs xT, cntvct_el0
uint32_t ldr_off(uint32_t rt, uint32_t rn, uint32_t off) { return 0xF9400000u | (off / 8) << 10 | rn << 5 | rt; }
uint32_t str_off(uint32_t rt, uint32_t rn, uint32_t off) { return 0xF9000000u | (off / 8) << 10 | rn << 5 | rt; }

// Instructions whose meaning depends on where they are: not movable to a hook's trampoline as they are.
bool pc_relative(uint32_t w) {
    return (w & 0x1F000000u) == 0x10000000u     // adr, adrp
           || (w & 0x7C000000u) == 0x14000000u  // b, bl
           || (w & 0xFF000010u) == 0x54000000u  // b.cond
           || (w & 0x7E000000u) == 0x34000000u  // cbz, cbnz
           || (w & 0x7E000000u) == 0x36000000u  // tbz, tbnz
           || (w & 0x3B000000u) == 0x18000000u; // ldr (literal), prfm (literal)
}

// ---- code regions ----

struct Region {
    uint64_t base = 0, cur = 0, end = 0;
    uint64_t common = 0, ret = 0;  // this region's copy of nce_common / nce_return

    uint64_t emit(uint32_t w) {
        if (cur + 4 > end) fatal("nce: stub region at %llx is full", (unsigned long long)base);
        st<uint32_t>(cur, w);
        cur += 4;
        return cur - 4;
    }
    uint64_t emit64(uint64_t v) {
        while (cur & 7) emit(kNop);
        const uint64_t a = cur;
        emit(uint32_t(v));
        emit(uint32_t(v >> 32));
        return a;
    }
    void copy_common() {
        const size_t n = size_t(nce_region_end - nce_region_start);
        if (cur + n > end) fatal("nce: stub region too small");
        std::memcpy(reinterpret_cast<void*>(cur), nce_region_start, n);
        common = cur + uint64_t(nce_common - nce_region_start);
        ret = cur + uint64_t(nce_return - nce_region_start);
        cur += (n + 15) & ~size_t(15);
    }
};

// A B instruction reaches +/-128 MiB. Keep stub pools near the code they patch.
bool branch_in_range(uint64_t from, uint64_t to) {
    const int64_t d = int64_t(to - from);
    return !(d & 3) && d >= -(int64_t(1) << 27) && d < (int64_t(1) << 27);
}

Region* region_for_site(std::vector<Region>& regions, uint64_t pc, size_t reserve_bytes = 0x100) {
    Region* best = nullptr;
    uint64_t best_distance = UINT64_MAX;
    for (Region& r : regions) {
        if (r.cur + reserve_bytes > r.end || !branch_in_range(pc, r.cur) ||
            !branch_in_range(r.cur + reserve_bytes, pc + 4))
            continue;
        const uint64_t d = pc > r.cur ? pc - r.cur : r.cur - pc;
        if (d < best_distance) best = &r, best_distance = d;
    }
    return best;
}

std::map<uint64_t, std::vector<Region>> g_regions;  // module base -> nearby stub pools
std::mutex g_regions_lock;
uint64_t g_return = 0;  // nce_return (the thunk region's copy)

struct SvcSite {
    uint32_t imm;
    uint64_t pc;
};
std::vector<SvcSite> g_svc_sites;  // every module's, made before any guest code runs

constexpr uint32_t kMaxHooks = 4096;
HleFn g_hooks[kMaxHooks];
std::atomic<uint32_t> g_hook_count{0};

// The counter: guest ticks = (CNTVCT * mul) >> 32, minus an offset that lines them up with plat::monotonic_ns().
uint64_t g_tick_mul = uint64_t(1) << 32, g_tick_off = 0;

uint64_t cntvct() {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

uint64_t scaled(uint64_t raw) { return uint64_t((unsigned __int128)raw * g_tick_mul >> 32); }

void clear_cache(uint64_t a, uint64_t size) {
    __builtin___clear_cache(reinterpret_cast<char*>(a), reinterpret_cast<char*>(a + size));
}

// Page-aligned protection change of [a, a + size).
bool protect(uint64_t a, uint64_t size, int prot) {
    const uint64_t ps = page_size(), lo = a & ~(ps - 1), hi = (a + size + ps - 1) & ~(ps - 1);
    return mprotect(reinterpret_cast<void*>(lo), hi - lo, prot) == 0;
}

// Write code that may be running on other threads: read-write-execute while writing where the system allows it,
// else briefly read-write.
void write_code(uint64_t a, const void* src, size_t size) {
    const bool rwx = protect(a, size, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!rwx) protect(a, size, PROT_READ | PROT_WRITE);
    std::memcpy(reinterpret_cast<void*>(a), src, size);
    protect(a, size, PROT_READ | PROT_EXEC);
    clear_cache(a, size);
}

// The stub for one svc site: push x16/x17, the site's index, its resume stub, into nce_common; the resume stub reloads
// x16/x17 from the block and returns after the svc.
uint64_t svc_stub(Region& r, uint32_t index, uint64_t site) {
    const uint64_t start = r.emit(kPushX16X17);
    r.emit(movz_w16(index));
    r.emit(movk_w16_hi(0x8000));
    const uint64_t adr = r.cur;
    r.emit(0);
    r.emit(b_to(r.cur, r.common));
    const uint64_t resume = r.cur;
    st<uint32_t>(adr, adr_to(adr, resume, 17));
    r.emit(mrs_tpidr(17));
    r.emit(0xA9544630);  // ldp x16, x17, [x17, #TP_X16]
    r.emit(b_to(r.cur, site + 4));
    return start;
}

// mrs xN, CNTPCT_EL0 (or CNTVCT_EL0): the virtual counter at 19.2 MHz, as tick().
uint64_t counter_stub(Region& r, uint32_t n, uint64_t site) {
    const uint32_t a = n == 0 ? 1 : 0, b = n == 2 ? 1 : (n == 1 || a == 1 ? 2 : 1);
    const uint64_t start = r.emit(0xA9BF03E0u | b << 10 | a);  // stp xA, xB, [sp, #-16]!
    r.emit(mrs_cntvct(a));
    const uint64_t ldr_mul = r.emit(0);
    r.emit(0x9B007C00u | b << 16 | a << 5 | n);  // mul xN, xA, xB
    r.emit(0x9BC07C00u | b << 16 | a << 5 | a);  // umulh xA, xA, xB
    r.emit(0x93C08000u | n << 16 | a << 5 | n);  // extr xN, xA, xN, #32
    const uint64_t ldr_sub = r.emit(0);
    r.emit(0xCB000000u | b << 16 | n << 5 | n);  // sub xN, xN, xB
    r.emit(0xA8C103E0u | b << 10 | a);           // ldp xA, xB, [sp], #16
    r.emit(b_to(r.cur, site + 4));
    const uint64_t mul = r.emit64(g_tick_mul), off = r.emit64(g_tick_off);
    st<uint32_t>(ldr_mul, ldr_literal(ldr_mul, mul, b));
    st<uint32_t>(ldr_sub, ldr_literal(ldr_sub, off, b));
    return start;
}

}  // namespace

void init() {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) freq = 19200000;
    g_tick_mul = uint64_t(((unsigned __int128)19200000 << 32) / freq);
    g_tick_off = scaled(cntvct()) - plat::monotonic_ns() * 192 / 10000;

    // The thunks: kThunkBase + 16 * i runs HLE function i; the common code right after them.
    if (!commit(kThunkBase, kThunkSize + kThunkExtra))
        fatal("nce: the HLE range %llx is not available on this device", (unsigned long long)kThunkBase);
    Region r;
    r.base = r.cur = kThunkBase + kThunkSize;
    r.end = r.base + kThunkExtra;
    r.copy_common();
    g_return = r.ret;
    for (uint64_t i = 0; i < kThunkSize / 16; i++) {
        const uint64_t a = kThunkBase + i * 16;
        st<uint32_t>(a, kPushX16X17);
        st<uint32_t>(a + 4, movz_w16(uint32_t(i)));
        st<uint32_t>(a + 8, b_to(a + 8, r.common));
        st<uint32_t>(a + 12, 0xD4200000);  // brk #0
    }
    if (!protect(kThunkBase, kThunkSize + kThunkExtra, PROT_READ | PROT_EXEC))
        fatal("nce: this system does not let the app run code it loaded (mprotect PROT_EXEC failed)");
    clear_cache(kThunkBase, kThunkSize + kThunkExtra);
    log("nce: native execution, counter %llu Hz (x%.6f to the Switch's 19.2 MHz), page size %llu",
        (unsigned long long)freq, double(g_tick_mul) / double(uint64_t(1) << 32), (unsigned long long)page_size());
}

void prepare_module(const LoadedModule& m) {
    const ModuleInfo* mi = m.info;
    std::vector<Region> regions;

    // Keep a pool after the module for late code and add one before it for early code.
    // A single pool at the end fails once a large NSO exceeds the +/-128 MiB B range.
    Region end_region;
    end_region.base = end_region.cur = m.base + ((m.size + 0xFFFF) & ~uint64_t(0xFFFF));
    end_region.end = end_region.base + kRegionSize;
    if (!commit(end_region.base, kRegionSize))
        fatal("nce: no room for %s's end stub pool at %llx", mi->name, (unsigned long long)end_region.base);
    end_region.copy_common();
    regions.push_back(end_region);

    const uint64_t near_base = m.base - kRegionSize;
    bool near_ready = false;
    if (m.base > kModuleBase) {
        // The pool before sdk usually sits in the gap after main, inside the already-reserved module arena.
        bool overlaps = module_at(near_base) != nullptr;
        for (const auto& [module_base, pools] : g_regions)
            for (const Region& pool : pools)
                if (near_base < pool.end && near_base + kRegionSize > pool.base) overlaps = true;
        if (!overlaps) near_ready = commit(near_base, kRegionSize);
    } else {
        // main begins at kModuleBase, so reserve its early pool just outside the module arena.
        near_ready = plat::reserve(near_base, kRegionSize) && commit(near_base, kRegionSize);
    }
    if (near_ready) {
        Region near_region;
        near_region.base = near_region.cur = near_base;
        near_region.end = near_base + kRegionSize;
        near_region.copy_common();
        regions.push_back(near_region);
    } else {
        log("nce: %s early stub pool unavailable", mi->name);
    }

    const uint64_t text = m.base + mi->text, text_end = text + mi->text_size;
    size_t svcs = 0, tls = 0, counters = 0, others = 0;
    for (uint64_t pc = text; pc < text_end; pc += 4) {
        const uint32_t w = ld<uint32_t>(pc);
        uint64_t stub = 0;
        Region* r = nullptr;
        const bool is_svc = (w & 0xFFE0001Fu) == 0xD4000001u;
        const bool is_sysreg = (w & 0xFFD00000u) == 0xD5100000u;
        if (is_svc || is_sysreg) r = region_for_site(regions, pc);
        if (is_svc && !r)
            fatal("nce: no in-range stub pool for %s svc at %llx", mi->name, (unsigned long long)pc);
        if ((w & 0xFFE0001Fu) == 0xD4000001u) {  // svc #imm
            if (g_svc_sites.size() >= 0x10000) fatal("nce: too many svc sites");
            g_svc_sites.push_back({(w >> 5) & 0xFFFF, pc});
            stub = svc_stub(*r, uint32_t(g_svc_sites.size() - 1), pc);
            svcs++;
        } else if ((w & 0xFFD00000u) == 0xD5100000u) {  // mrs / msr (register)
            const bool read = w & (1u << 21);
            const uint32_t sysreg = ((w >> 5) & 0x7FFF) | 0x8000, n = w & 31;
            if (n == 31 && read && (sysreg == 0xDE82 || sysreg == 0xDE83 || sysreg == 0xDF01 || sysreg == 0xDF02)) {
                st<uint32_t>(pc, kNop);  // a read into xzr
                continue;
            }
            const bool needs_stub = (read && (sysreg == 0xDE83 || sysreg == 0xDE82 || sysreg == 0xDF01 ||
                                               sysreg == 0xDF02 || sysreg == 0xDF00)) ||
                                    (!read && sysreg == 0xDE82);
            if (needs_stub && !r)
                fatal("nce: no in-range stub pool for %s system-register site at %llx", mi->name,
                      (unsigned long long)pc);
            if (read && (sysreg == 0xDE83 || sysreg == 0xDE82)) {  // TPIDRRO_EL0 / TPIDR_EL0
                stub = r->emit(mrs_tpidr(n));
                r->emit(ldr_off(n, n, sysreg == 0xDE83 ? 0x118 : 0x110));
                r->emit(b_to(r->cur, pc + 4));
                tls++;
            } else if (!read && sysreg == 0xDE82) {  // msr TPIDR_EL0, xN
                const uint32_t s = n == 0 ? 1 : 0;
                stub = r->emit(0xF81F0FE0u | s);  // str xS, [sp, #-16]!
                r->emit(mrs_tpidr(s));
                r->emit(str_off(n, s, 0x110));
                r->emit(0xF84107E0u | s);  // ldr xS, [sp], #16
                r->emit(b_to(r->cur, pc + 4));
                tls++;
            } else if (read && (sysreg == 0xDF01 || sysreg == 0xDF02)) {  // CNTPCT_EL0 / CNTVCT_EL0
                stub = counter_stub(*r, n, pc);
                counters++;
            } else if (read && sysreg == 0xDF00) {  // CNTFRQ_EL0
                stub = r->cur;
                const uint64_t ldr = r->emit(0);
                r->emit(b_to(r->cur, pc + 4));
                st<uint32_t>(ldr, ldr_literal(ldr, r->emit64(19200000), n));
                counters++;
            } else if (sysreg != 0xDA20 && sysreg != 0xDA21 && sysreg != 0xDA10 && sysreg != 0xD801 &&
                       sysreg != 0xD807) {  // FPCR, FPSR, NZCV, CTR_EL0, DCZID_EL0 run as they are
                if (others++ < 16)
                    log("nce: %s %#x at %s left as is", read ? "mrs" : "msr", sysreg, describe(pc).c_str());
            }
        }
        if (stub) {
            if (!r || !branch_in_range(pc, stub))
                fatal("nce: selected stub for %s at %llx is out of range (%llx)", mi->name,
                      (unsigned long long)pc, (unsigned long long)stub);
            st<uint32_t>(pc, b_to(pc, stub));
        }
    }
    if (!protect(text, mi->text_size, PROT_READ | PROT_EXEC))
        fatal("nce: this system does not let the app run code it loaded (mprotect text PROT_EXEC failed)");
    uint64_t total_stub_bytes = 0;
    for (Region& pool : regions) {
        if (!protect(pool.base, kRegionSize, PROT_READ | PROT_EXEC))
            fatal("nce: this system does not let the app run stub code at %llx (mprotect PROT_EXEC failed)",
                  (unsigned long long)pool.base);
        clear_cache(pool.base, pool.cur - pool.base);
        total_stub_bytes += pool.cur - pool.base;
    }
    clear_cache(text, mi->text_size);
    log("nce: %s ready: %zu svc, %zu TLS and %zu counter sites patched, %llu stub bytes in %zu pools",
        mi->name, svcs, tls, counters, (unsigned long long)total_stub_bytes, regions.size());
    std::lock_guard lk(g_regions_lock);
    g_regions[m.base] = std::move(regions);
}

void enter(CPU& c, uint64_t target) {
    Block* b = block();
    CPU* prev = b->ctx;
    b->ctx = &c;
    const uint64_t lr = c.x[30];
    c.x[30] = g_return;
    b->target = target;
    refresh(b);
    nce_enter(tp_of(b));
    c.x[30] = lr;
    b->ctx = prev;
}

uint64_t hook(uint64_t addr, HleFn fn) {
    std::lock_guard lk(g_regions_lock);
    const LoadedModule* m = module_at(addr);
    auto it = m ? g_regions.find(m->base) : g_regions.end();
    if (it == g_regions.end() || addr - (m->base + m->info->text) >= m->info->text_size) {
        log("nce: hook at %llx: not in a module's code", (unsigned long long)addr);
        return 0;
    }
    const uint32_t first = ld<uint32_t>(addr);
    if (pc_relative(first)) {
        log("nce: hook at %s: its first instruction %08x depends on its address", describe(addr).c_str(), first);
        return 0;
    }
    const uint32_t index = g_hook_count.load();
    if (index >= kMaxHooks) return 0;
    g_hooks[index] = fn;
    g_hook_count.store(index + 1);

    Region* selected = region_for_site(it->second, addr);
    if (!selected) {
        log("nce: hook at %s: no in-range stub pool", describe(addr).c_str());
        return 0;
    }
    Region& r = *selected;
    const uint64_t from = r.cur;
    if (!protect(r.base, kRegionSize, PROT_READ | PROT_WRITE | PROT_EXEC))
        protect(r.base, kRegionSize, PROT_READ | PROT_WRITE);
    const uint64_t original = r.emit(first);
    r.emit(b_to(r.cur, addr + 4));
    const uint64_t stub = r.emit(kPushX16X17);
    r.emit(movz_w16(index));
    r.emit(movk_w16_hi(0x4000));
    r.emit(b_to(r.cur, r.common));
    protect(r.base, kRegionSize, PROT_READ | PROT_EXEC);
    clear_cache(from, r.cur - from);
    const uint32_t branch = b_to(addr, stub);
    write_code(addr, &branch, 4);
    return original;
}

uint64_t tick() { return scaled(cntvct()) - g_tick_off; }

bool guest_fault_context(void* ucontext, CPU& c, uint64_t& pc) {
    const auto* uc = static_cast<const ucontext_t*>(ucontext);
    pc = uc->uc_mcontext.pc;
    bool guest = module_at(pc) != nullptr || pc - kThunkBase < kThunkSize + kThunkExtra;
    for (auto& [base, pools] : g_regions)
        for (const Region& r : pools) guest = guest || (pc >= r.base && pc < r.end);
    if (!guest) return false;
    for (int k = 0; k < 31; k++) c.x[k] = uc->uc_mcontext.regs[k];
    c.x[31] = uc->uc_mcontext.sp;
    return true;
}

}  // namespace nx::nce

// Every stub ends up here, on the host stack with the host's TLS: an svc, a hook or an HLE function on the thread's
// guest registers.
extern "C" void nce_dispatch(uint64_t tp, uint32_t code) {
    using namespace nx;
    using namespace nx::nce;
    Block* b = block_of(tp);
    CPU& c = *b->ctx;
    if (code & 0x80000000u) {
        const SvcSite& s = g_svc_sites[code & 0xFFFF];
        svc(c, s.imm, s.pc);
    } else if (code & 0x40000000u) {
        g_hooks[code & 0xFFFF](c);
    } else {
        hle_invoke(c, code);
    }
    refresh(b);
}
