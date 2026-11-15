// Guest address space on the host: fixed reserved ranges, commit on demand, and a Horizon-style memory map
// for svcQueryMemory.
#include <map>
#include <mutex>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

std::mutex g_vm_lock;
std::map<uint64_t, VmInfo> g_vm;  // start -> block (non-overlapping; gaps are free)
uint64_t g_kernel_next = kKernelBase;

void reserve(uint64_t addr, uint64_t size, const char* what) {
    if (!plat::reserve(addr, size))
        fatal("cannot reserve %s at %llx (+%llx): the range is in use", what, (unsigned long long)addr,
              (unsigned long long)size);
}

#if defined(__ANDROID__)
void reserve_flex(uint64_t& base, uint64_t size, const char* what, uint64_t step = 0x100000000ull) {
    if (plat::reserve(base, size)) return;
    for (uint64_t cand = 0x1000000000ull; cand + size <= 0x7000000000ull; cand += step) {
        if (cand < kModuleBase + 0x40000000ull && cand + size > kModuleBase - 0x10000000ull) continue;
        if (plat::reserve(cand, size)) {
            log("memory: %s range %llx (+%llx) is in use on this device: using %llx", what,
                (unsigned long long)base, (unsigned long long)size, (unsigned long long)cand);
            base = cand;
            return;
        }
    }
    fatal("cannot reserve %s at %llx (+%llx): the range is in use", what, (unsigned long long)base,
          (unsigned long long)size);
}
#endif

}  // namespace

void memory_init() {
#if defined(__ANDROID__)
    if (!plat::reserve(kModuleBase, 0x40000000ull) && !plat::reserve(kModuleBase, 0x20000000ull)) {
        reserve_flex(kModuleBase, 0x20000000ull, "modules", 0x40000000ull);
    }
    reserve_flex(kHeapBase, kHeapMax, "heap");
    if (!plat::reserve(kAliasBase, kAliasSize)) {
        // If a contiguous 64 GiB range is fragmented by ASLR on this kernel, try 16 GiB (4x the heap) or 8 GiB.
        bool ok = false;
        for (uint64_t sz : {kAliasSize, uint64_t(0x400000000ull), uint64_t(0x200000000ull)}) {
            for (uint64_t cand = 0x1000000000ull; cand + sz <= 0x7000000000ull; cand += 0x100000000ull) {
                if (cand < kModuleBase + 0x40000000ull && cand + sz > kModuleBase - 0x10000000ull) continue;
                if (plat::reserve(cand, sz)) {
                    log("memory: alias range %llx (+%llx) is in use on this device: using %llx (+%llx)",
                        (unsigned long long)kAliasBase, (unsigned long long)kAliasSize,
                        (unsigned long long)cand, (unsigned long long)sz);
                    kAliasBase = cand;
                    kAliasSize = sz;
                    ok = true;
                    break;
                }
            }
            if (ok) break;
        }
        if (!ok) fatal("cannot reserve alias at %llx (+%llx): the range is in use",
                       (unsigned long long)kAliasBase, (unsigned long long)kAliasSize);
    }
    reserve_flex(kStackBase, kStackSize, "stack");
    reserve_flex(kKernelBase, kKernelSize, "kernel");
    g_kernel_next = kKernelBase;
#else
    reserve(kModuleBase, 0x20000000, "modules");
    reserve(kHeapBase, kHeapMax, "heap");
    reserve(kAliasBase, kAliasSize, "alias");
    reserve(kStackBase, kStackSize, "stack");
    reserve(kKernelBase, kKernelSize, "kernel");
#endif
    // The thunk range only names HLE functions: calls to it are dispatched by address (loader.cpp) and nothing is ever
    // mapped, read or run there, so a host mapping already in it is harmless. On a POCO F6 (Android 15, Adreno 735,
    // Xiaomi 24069PC21G) something sat at 0x6F00000000 and this was fatal: the app closed as soon as the game started.
#if NX_NCE
    // Native execution: the thunks are real code there (a stub per 16 bytes, nce.cpp), and their common code after.
    bool thunks_ok = plat::reserve(kThunkBase, kThunkSize + 0x10000);
#if defined(__ANDROID__)
    // If that address is taken on this device, use another free one (the thunks are called by absolute address from
    // the GOT and from hooks far from the modules, so any range far from them works). All before the first thunk is handed out.
    static const uint64_t kThunkFallback[] = {0x4800000000ull, 0x3800000000ull, 0x6E00000000ull,
                                              0x6D00000000ull, 0x6C00000000ull, 0x6F00000000ull};
    for (size_t i = 0; !thunks_ok && i < sizeof kThunkFallback / sizeof *kThunkFallback; i++) {
        if (!plat::reserve(kThunkFallback[i], kThunkSize + 0x10000)) continue;
        log("memory: the HLE range %llx is in use on this device: using %llx", (unsigned long long)kThunkBase,
            (unsigned long long)kThunkFallback[i]);
        kThunkBase = kThunkFallback[i];
        thunks_ok = true;
    }
    for (uint64_t cand = 0x1000000000ull; !thunks_ok && cand < 0x7000000000ull; cand += 0x80000000ull) {
        if (!plat::reserve(cand, kThunkSize + 0x10000)) continue;
        log("memory: the HLE range %llx is in use on this device: using %llx", (unsigned long long)kThunkBase,
            (unsigned long long)cand);
        kThunkBase = cand;
        thunks_ok = true;
    }
#endif
    if (!thunks_ok)
        fatal("cannot reserve the HLE range at %llx: the range is in use on this device", (unsigned long long)kThunkBase);
#else
    if (!plat::reserve(kThunkBase, kThunkSize))
        log("memory: thunk range %llx (+%llx) already in use on this device: not reserved (only an address range for "
            "HLE calls, never mapped)", (unsigned long long)kThunkBase, (unsigned long long)kThunkSize);
#endif
    // Only the alias and stack regions are free for the guest to pick addresses in (nn::os maps shared memory,
    // transfer memory and thread stacks at random free addresses); everything else is reported inaccessible.
    vm_set(kAddressSpaceBase, kAddressSpaceSize, kMemInaccessible, 0);
    vm_set(kAliasBase, kAliasSize, kMemFree, 0);
    vm_set(kStackBase, kStackSize, kMemFree, 0);
}

bool commit(uint64_t addr, uint64_t size) {
    return plat::commit(addr, size);
}

void decommit(uint64_t addr, uint64_t size) {
    plat::decommit(addr, size);
}

uint64_t kernel_alloc(uint64_t size, uint64_t align) {
    std::lock_guard lk(g_vm_lock);
    uint64_t a = (g_kernel_next + align - 1) & ~(align - 1);
    size = (size + 0xFFF) & ~0xFFFull;
    if (a + size > kKernelBase + kKernelSize) fatal("kernel region exhausted");
    g_kernel_next = a + size;
    if (!commit(a, size)) fatal("kernel_alloc commit failed");
    return a;
}

void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    std::lock_guard lk(g_vm_lock);
    uint64_t end = addr + size;
    // Cut overlapping blocks.
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (p->second.addr + p->second.size > addr) it = p;
    }
    while (it != g_vm.end() && it->second.addr < end) {
        VmInfo b = it->second;
        it = g_vm.erase(it);
        if (b.addr < addr) g_vm[b.addr] = {b.addr, addr - b.addr, b.state, b.perm};
        if (b.addr + b.size > end) g_vm[end] = {end, b.addr + b.size - end, b.state, b.perm};
    }
    if (state != kMemFree) g_vm[addr] = {addr, size, state, perm};
}

VmInfo vm_query(uint64_t addr) {
    std::lock_guard lk(g_vm_lock);
    auto it = g_vm.upper_bound(addr);
    uint64_t gap_end = it == g_vm.end() ? kAddressSpaceBase + kAddressSpaceSize : it->first;
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (addr < p->second.addr + p->second.size) return p->second;
        uint64_t gap_start = p->second.addr + p->second.size;
        return {gap_start, gap_end - gap_start, kMemFree, 0};
    }
    uint64_t gap_start = addr < kAddressSpaceBase ? 0 : kAddressSpaceBase;
    if (addr < kAddressSpaceBase) gap_end = kAddressSpaceBase;
    return {gap_start, gap_end - gap_start, kMemFree, 0};
}

namespace plat {

void fast_memcpy(void* __restrict dst, const void* __restrict src, size_t n) {
    if (n > 0) __builtin_memcpy(dst, src, n);
}

}  // namespace plat

}  // namespace nx
