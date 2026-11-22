// Host floating-point mode for Vulkan / Mali shader compiler threads (ported from TOTKDroid v31).
// Guest AArch64 code can set FPCR flush-to-zero (FZ/DN), which Linux inherits on spawned threads.
// Reset FPCR=0 on host Vulkan worker threads and around pipeline/shader compilation calls so
// Mali's shader compiler always runs in standard IEEE-754 mode.
#pragma once

#include <cstdint>

namespace nx::nvn {

inline uint64_t vk_fp_host() {
#if defined(__aarch64__)
    uint64_t cur = 0;
    asm volatile("mrs %0, fpcr" : "=r"(cur));
    if (cur == 0) return 0;
    uint64_t zero = 0;
    asm volatile("msr fpcr, %0" :: "r"(zero));
    return cur;
#else
    return 0;
#endif
}

inline void vk_fp_put(uint64_t saved) {
#if defined(__aarch64__)
    if (saved) asm volatile("msr fpcr, %0" :: "r"(saved));
#else
    (void)saved;
#endif
}

inline void vk_fp_thread() {
#if defined(__aarch64__)
    uint64_t zero = 0;
    asm volatile("msr fpcr, %0" :: "r"(zero));
#endif
}

struct ScopedHostFpcr {
    uint64_t saved;
    ScopedHostFpcr() : saved(vk_fp_host()) {}
    ~ScopedHostFpcr() { vk_fp_put(saved); }
    ScopedHostFpcr(const ScopedHostFpcr&) = delete;
    ScopedHostFpcr& operator=(const ScopedHostFpcr&) = delete;
};

}  // namespace nx::nvn
