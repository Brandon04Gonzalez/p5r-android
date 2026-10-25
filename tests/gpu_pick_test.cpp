// The GPU choice rule (runtime/src/nvn/nvn_gpu_pick.cpp, NX_GPU). Standalone:
//   clang-cl /std:c++20 /EHsc -DNX_GPU_PICK_STANDALONE -Iruntime/src/nvn tests/gpu_pick_test.cpp
//       runtime/src/nvn/nvn_gpu_pick.cpp
// or through CMake (-DNX_TESTS=ON, ctest). Exit code 0: every case passed.
#include <cstdio>

#include "nvn_gpu_pick.h"

using nx::nvn::GpuCandidate;
using nx::nvn::pick_gpu;

namespace {

int g_failed = 0;

void expect(const std::vector<GpuCandidate>& gpus, const char* want, size_t index, bool matched, int line) {
    bool m = false;
    size_t k = pick_gpu(gpus, want, m);
    if (k != index || m != matched) {
        std::printf("FAIL line %d: NX_GPU=%s -> %zu (matched %d), expected %zu (matched %d)\n", line,
                    want ? want : "(unset)", k, int(m), index, int(matched));
        g_failed++;
    }
}

}  // namespace

#define EXPECT(gpus, want, index, matched) expect(gpus, want, index, matched, __LINE__)

int main() {
    // The laptop this was written on: Intel first, NVIDIA second, Intel again through a second driver entry.
    const std::vector<GpuCandidate> laptop = {
        {"Intel(R) UHD Graphics 770", 1}, {"NVIDIA GeForce RTX 3050 6GB Laptop GPU", 2}, {"Intel(R) UHD Graphics 770", 1}};
    EXPECT(laptop, nullptr, 1, true);  // default: discrete
    EXPECT(laptop, "", 1, true);
    EXPECT(laptop, "discrete", 1, true);
    EXPECT(laptop, "DISCRETE", 1, true);
    EXPECT(laptop, "integrated", 0, true);
    EXPECT(laptop, "0", 0, true);
    EXPECT(laptop, "2", 2, true);
    EXPECT(laptop, "3", 1, false);   // out of range: default
    EXPECT(laptop, "1x", 1, false);  // not a number
    EXPECT(laptop, "nvidia", 1, true);
    EXPECT(laptop, "Intel", 0, true);
    EXPECT(laptop, "3050", 1, true);
    EXPECT(laptop, "amd", 1, false);

    // A desktop with only an integrated GPU, and a software rasterizer.
    const std::vector<GpuCandidate> igpu = {{"llvmpipe", 4}, {"Intel(R) Iris(R) Xe Graphics", 1}};
    EXPECT(igpu, nullptr, 1, true);  // no discrete: the integrated one, not the CPU renderer
    EXPECT(igpu, "discrete", 1, false);
    EXPECT(igpu, "llvm", 0, true);

    // Only unknown types: device 0. No device: 0 (the caller checks for an empty list).
    const std::vector<GpuCandidate> other = {{"x", 0}, {"y", 3}}, none;
    EXPECT(other, nullptr, 0, true);
    EXPECT(none, "discrete", 0, true);

    std::printf("gpu_pick_test: %s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
