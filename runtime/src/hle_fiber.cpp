// nn::os fibers (InitializeFiber, FinalizeFiber, SwitchToFiber) at the API level.
//
// The SDK switches fibers in guest code (nn::os::detail::DispatchFiberContext): it saves the callee-saved
// registers on the current guest stack, loads another guest sp and returns into the other fiber. Recompiled code
// cannot follow that: every guest call is also a host call, so the host stack holds the frames of the fiber that
// was running, and a guest sp switch would return into the wrong host frames. So each guest fiber runs on a host
// fiber of its own (Windows fibers; elsewhere a small context switch below), and a switch saves the guest
// registers of the fiber it leaves and restores them when that fiber runs again.
//
// The guest-visible state is kept exactly as the SDK keeps it (SDK 13, sdk+379220..37947c), so GetCurrentFiber
// and anything else reading it stays the SDK's own code:
//   FiberType +0x20 u8 state (0 none, 1 ready, 2 running, 4 finished), +0x21 u8 stack aliased,
//             +0x28 function, +0x30 argument, +0x38 stack, +0x40 stack as used (alias), +0x48 stack size
//   ThreadType (TLS +0x1F8) +0x70 current fiber, +0x78 the thread's own context as a fiber (a FiberType the SDK
//             puts on the thread's stack at its first switch, until a switch back to it)
//
// Fibers belong to the guest thread that runs them; guest threads are host threads, so the bookkeeping is
// thread_local. A fiber is not moved to another thread (P5R's coroutines all run on the game's main thread).
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace nx {

namespace {

constexpr uint64_t kTlsThread = 0x1F8;    // TLS region: ThreadType*
constexpr uint64_t kThreadFiber = 0x70;   // ThreadType: current fiber
constexpr uint64_t kThreadSelf = 0x78;    // ThreadType: the thread's own context as a fiber
constexpr uint8_t kReady = 1, kRunning = 2, kFinished = 4;
constexpr size_t kHostStack = 32ull << 20;  // reserved, committed as used: recompiled calls nest deeply

}  // namespace
}  // namespace nx

#ifndef _WIN32
// Host context switch: saves the callee-saved registers on the current host stack, stores its sp in *save_sp,
// switches to load_sp and pops the registers saved there. A new stack starts in nx_fiber_trampoline, which calls
// entry(arg) from two of the restored registers.
extern "C" void nx_fiber_switch(void** save_sp, void* load_sp);
extern "C" void nx_fiber_trampoline();
#if defined(__APPLE__)
#define NX_SYM(x) "_" #x
#else
#define NX_SYM(x) #x
#endif
#if defined(__aarch64__)
__asm__(".text\n"
        ".p2align 2\n"
        ".globl " NX_SYM(nx_fiber_switch) "\n" NX_SYM(nx_fiber_switch) ":\n"
        "  sub sp, sp, #0xa0\n"
        "  stp x19, x20, [sp, #0x00]\n"
        "  stp x21, x22, [sp, #0x10]\n"
        "  stp x23, x24, [sp, #0x20]\n"
        "  stp x25, x26, [sp, #0x30]\n"
        "  stp x27, x28, [sp, #0x40]\n"
        "  stp x29, x30, [sp, #0x50]\n"
        "  stp d8, d9, [sp, #0x60]\n"
        "  stp d10, d11, [sp, #0x70]\n"
        "  stp d12, d13, [sp, #0x80]\n"
        "  stp d14, d15, [sp, #0x90]\n"
        "  mov x2, sp\n"
        "  str x2, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0x00]\n"
        "  ldp x21, x22, [sp, #0x10]\n"
        "  ldp x23, x24, [sp, #0x20]\n"
        "  ldp x25, x26, [sp, #0x30]\n"
        "  ldp x27, x28, [sp, #0x40]\n"
        "  ldp x29, x30, [sp, #0x50]\n"
        "  ldp d8, d9, [sp, #0x60]\n"
        "  ldp d10, d11, [sp, #0x70]\n"
        "  ldp d12, d13, [sp, #0x80]\n"
        "  ldp d14, d15, [sp, #0x90]\n"
        "  add sp, sp, #0xa0\n"
        "  ret\n"
        ".p2align 2\n"
        ".globl " NX_SYM(nx_fiber_trampoline) "\n" NX_SYM(nx_fiber_trampoline) ":\n"
        "  hint #34\n"  // bti c
        "  mov x0, x20\n"
        "  blr x19\n"
        "  brk #0\n");
#elif defined(__x86_64__)
__asm__(".text\n"
        ".p2align 4\n"
        ".globl " NX_SYM(nx_fiber_switch) "\n" NX_SYM(nx_fiber_switch) ":\n"
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  subq $8, %rsp\n"
        "  stmxcsr (%rsp)\n"
        "  fnstcw 4(%rsp)\n"
        "  movq %rsp, (%rdi)\n"
        "  movq %rsi, %rsp\n"
        "  ldmxcsr (%rsp)\n"
        "  fldcw 4(%rsp)\n"
        "  addq $8, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n"
        ".p2align 4\n"
        ".globl " NX_SYM(nx_fiber_trampoline) "\n" NX_SYM(nx_fiber_trampoline) ":\n"
        "  movq %r13, %rdi\n"
        "  callq *%r12\n"
        "  ud2\n");
#else
#error "hle_fiber.cpp: no host context switch for this architecture"
#endif
#endif  // !_WIN32

namespace nx {

namespace {

struct HostFiber {
    uint64_t guest = 0;   // the FiberType this host fiber runs
    bool parked = true;   // between runs: the next switch to it starts the guest function anew
    CPU saved;            // the guest registers while it is switched out
#ifdef _WIN32
    void* fiber = nullptr;
#else
    void* sp = nullptr;
    void* stack = nullptr;
#endif
};

struct FiberState {
    HostFiber self;                 // the thread's own context
    uint64_t self_guest = 0;        // its FiberType (ThreadType +0x78) while a fiber runs
    HostFiber* current = nullptr;   // null: the thread's own context
    std::unordered_map<uint64_t, HostFiber*> by_guest;
    std::vector<HostFiber*> pool;   // parked host fibers whose guest fiber was finalized
};

thread_local FiberState t_fibers;

uint64_t guest_thread(const CPU& c) { return ld<uint64_t>(c.tpidrro + kTlsThread); }

void fiber_main(HostFiber* self);

#ifdef _WIN32
void WINAPI win_fiber_proc(void* arg) { fiber_main(static_cast<HostFiber*>(arg)); }
#else
extern "C" void posix_fiber_entry(void* arg) { fiber_main(static_cast<HostFiber*>(arg)); }
#endif

HostFiber* new_host_fiber() {
    auto* h = new HostFiber;
#ifdef _WIN32
    if (!t_fibers.self.fiber)
        t_fibers.self.fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!t_fibers.self.fiber) fatal("fiber: ConvertThreadToFiber failed (%lu)", GetLastError());
    h->fiber = CreateFiberEx(64 << 10, kHostStack, FIBER_FLAG_FLOAT_SWITCH, win_fiber_proc, h);
    if (!h->fiber) fatal("fiber: CreateFiberEx failed (%lu)", GetLastError());
#else
    void* base = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) fatal("fiber: no memory for a %zu MiB host stack", kHostStack >> 20);
    mprotect(base, 64 << 10, PROT_NONE);  // guard
    h->stack = base;
    auto top = (reinterpret_cast<uintptr_t>(base) + kHostStack) & ~uintptr_t(15);
#if defined(__aarch64__)
    auto* frame = reinterpret_cast<uint64_t*>(top - 0xa0);
    std::memset(frame, 0, 0xa0);
    frame[0] = reinterpret_cast<uint64_t>(&posix_fiber_entry);  // x19
    frame[1] = reinterpret_cast<uint64_t>(h);                   // x20
    frame[11] = reinterpret_cast<uint64_t>(&nx_fiber_trampoline);  // x30
#else
    auto* frame = reinterpret_cast<uint64_t*>(top - 64);
    std::memset(frame, 0, 64);
    frame[0] = 0x1F80ull | (0x037Full << 32);                   // mxcsr, x87 control word
    frame[3] = reinterpret_cast<uint64_t>(h);                   // r13
    frame[4] = reinterpret_cast<uint64_t>(&posix_fiber_entry);  // r12
    frame[7] = reinterpret_cast<uint64_t>(&nx_fiber_trampoline);  // return address
#endif
    h->sp = frame;
#endif
    return h;
}

void destroy_host_fiber(HostFiber* h) {
#ifdef _WIN32
    DeleteFiber(h->fiber);
#else
    munmap(h->stack, kHostStack);
#endif
    delete h;
}

void host_switch(HostFiber* from, HostFiber* to) {
    t_fibers.current = to == &t_fibers.self ? nullptr : to;
#ifdef _WIN32
    (void)from;
    SwitchToFiber(to->fiber);
#else
    nx_fiber_switch(&from->sp, to->sp);
#endif
}

HostFiber* host_for(uint64_t guest) {
    FiberState& s = t_fibers;
    if (guest == s.self_guest) return &s.self;
    if (auto it = s.by_guest.find(guest); it != s.by_guest.end()) return it->second;
    HostFiber* h;
    if (!s.pool.empty()) {
        h = s.pool.back();
        s.pool.pop_back();
    } else {
        h = new_host_fiber();
    }
    h->guest = guest;
    s.by_guest[guest] = h;
    return h;
}

// What the SDK does when a fiber function returns its successor: null means the thread's own context.
uint64_t successor(uint64_t thread, uint64_t next) {
    if (!next) return ld<uint64_t>(thread + kThreadSelf);
    if (ld<uint8_t>(next + 0x20) != kReady) fatal("fiber: switch to fiber %llx in state %u", (unsigned long long)next, ld<uint8_t>(next + 0x20));
    return next;
}

void fiber_main(HostFiber* self) {
    for (;;) {
        CPU& c = current_cpu();
        const uint64_t f = self->guest;
        self->parked = false;
        const uint64_t func = ld<uint64_t>(f + 0x28), stack = ld<uint64_t>(f + 0x40), size = ld<uint64_t>(f + 0x48);
        c.x[0] = ld<uint64_t>(f + 0x30);
        c.x[29] = 0;
        c.x[30] = hle_address("__fiber_return");
        c.x[31] = (stack + size) & ~15ull;
        try {
            call(c, func);
        } catch (const LongJmp&) {
            fatal("fiber %llx: longjmp out of the fiber function", (unsigned long long)f);
        }
        const uint64_t thread = guest_thread(c);
        const uint64_t next = successor(thread, c.x[0]);
        st<uint8_t>(f + 0x20, kFinished);
        st<uint8_t>(next + 0x20, kRunning);
        st<uint64_t>(thread + kThreadFiber, next);
        self->parked = true;
        host_switch(self, host_for(next));
        // Switched to again after a new InitializeFiber: run the (new) function.
    }
}

void fiber_return(CPU&) { fatal("fiber: guest returned through x30 of a fiber function"); }

// nn::os::InitializeFiber(FiberType*, FiberFunction, void* arg, void* stack, size_t size, int flags)
void InitializeFiber(CPU& c) {
    const uint64_t f = c.x[0];
    st<uint64_t>(f + 0x48, c.x[4]);
    st<uint64_t>(f + 0x28, c.x[1]);
    st<uint64_t>(f + 0x30, c.x[2]);
    st<uint16_t>(f + 0x20, kReady);  // and +0x21: the stack is never aliased here
    st<uint64_t>(f + 0x38, c.x[3]);
    st<uint64_t>(f + 0x40, c.x[3]);
    FiberState& s = t_fibers;
    if (auto it = s.by_guest.find(f); it != s.by_guest.end() && !it->second->parked && it->second != s.current) {
        destroy_host_fiber(it->second);  // re-initialized while suspended: its host frames are dead
        s.by_guest.erase(it);
    }
}

// nn::os::FinalizeFiber(FiberType*)
void FinalizeFiber(CPU& c) {
    const uint64_t f = c.x[0];
    st<uint8_t>(f + 0x21, 0);
    st<uint64_t>(f + 0x40, ld<uint64_t>(f + 0x38));
    st<uint8_t>(f + 0x20, 0);
    FiberState& s = t_fibers;
    auto it = s.by_guest.find(f);
    if (it == s.by_guest.end()) return;
    HostFiber* h = it->second;
    if (h == s.current) {
        log("fiber: FinalizeFiber(%llx) on the running fiber", (unsigned long long)f);
        return;
    }
    s.by_guest.erase(it);
    if (h->parked) s.pool.push_back(h);
    else destroy_host_fiber(h);
}

// nn::os::SwitchToFiber(FiberType*): null switches back to the thread's own context.
void SwitchToFiber(CPU& c) {
    FiberState& s = t_fibers;
    const uint64_t thread = guest_thread(c);
    uint64_t target = c.x[0];
    if (!target) target = ld<uint64_t>(thread + kThreadSelf);
    else if (ld<uint8_t>(target + 0x20) != kReady)
        fatal("fiber: SwitchToFiber(%llx) in state %u", (unsigned long long)target, ld<uint8_t>(target + 0x20));
    if (!target) return;  // no fiber runs and none was named
    const uint64_t cur = ld<uint64_t>(thread + kThreadFiber);
    if (cur) {
        st<uint8_t>(cur + 0x20, kReady);
    } else {
        // The thread's own context becomes a fiber, on its stack as the SDK has it.
        c.x[31] -= 0x80;
        const uint64_t me = c.x[31] + 8;
        for (uint64_t k = 0; k < 0x60; k += 8) st<uint64_t>(me + k, 0);
        st<uint16_t>(me + 0x20, kReady);
        st<uint64_t>(thread + kThreadSelf, me);
        s.self_guest = me;
    }
    st<uint8_t>(target + 0x20, kRunning);
    st<uint64_t>(thread + kThreadFiber, target);
    HostFiber* from = s.current ? s.current : &s.self;
    HostFiber* to = host_for(target);
    if (to == from) fatal("fiber: switch to the running fiber %llx", (unsigned long long)target);
    from->saved = c;
    host_switch(from, to);
    // Back on this fiber.
    c = from->saved;
    if (!cur) {
        st<uint64_t>(thread + kThreadFiber, 0);
        st<uint64_t>(thread + kThreadSelf, 0);
        s.self_guest = 0;
        c.x[31] += 0x80;
    }
}

}  // namespace

NX_HLE("_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi", InitializeFiber);
NX_HLE("_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", FinalizeFiber);
NX_HLE("_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE", SwitchToFiber);
NX_HLE("__fiber_return", fiber_return);

}  // namespace nx
