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

}  // namespace

void memory_init() {
    reserve(kModuleBase, 0x20000000, "modules");
    reserve(kHeapBase, kHeapMax, "heap");
    reserve(kAliasBase, kAliasSize, "alias");
    reserve(kStackBase, kStackSize, "stack");
    reserve(kKernelBase, kKernelSize, "kernel");
    // The thunk range only names HLE functions: calls to it are dispatched by address (loader.cpp) and nothing is ever
    // mapped, read or run there, so a host mapping already in it is harmless. On a POCO F6 (Android 15, Adreno 735,
    // Xiaomi 24069PC21G) something sat at 0x6F00000000 and this was fatal: the app closed as soon as the game started.
#if NX_NCE
    // Native execution: the thunks are real code there (a stub per 16 bytes, nce.cpp), and their common code after.
    if (!plat::reserve(kThunkBase, kThunkSize + 0x10000))
        fatal("cannot reserve the HLE range at %llx: the range is in use on this device", (unsigned long long)kThunkBase);
#else
    if (!plat::reserve(kThunkBase, kThunkSize))
        log("memory: thunk range %llx (+%llx) already in use on this device: not reserved (only an address range for "
            "HLE calls, never mapped)", (unsigned long long)kThunkBase, (unsigned long long)kThunkSize);
#endif
    // Only the alias and stack regions are free for the guest to pick addresses in (nn::os maps shared memory,
    // transfer memory and thread stacks at random free addresses); everything else is reported inaccessible.
    uint64_t end = kAddressSpaceBase + kAddressSpaceSize;
    vm_set(kAddressSpaceBase, kAliasBase - kAddressSpaceBase, kMemInaccessible, 0);
    if (kAliasBase + kAliasSize < kStackBase)  // Android: the alias sits below the modules (runtime.h)
        vm_set(kAliasBase + kAliasSize, kStackBase - (kAliasBase + kAliasSize), kMemInaccessible, 0);
    vm_set(kStackBase + kStackSize, end - (kStackBase + kStackSize), kMemInaccessible, 0);
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

}  // namespace nx
