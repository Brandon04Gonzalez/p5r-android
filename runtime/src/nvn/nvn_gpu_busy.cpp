// The GPU's busy time per frame at the real load (NX_GPU_PROFILE=3; docs/nvn/android-perf/PHASE4.md G0). The per-pass
// profile (NX_GPU_PROFILE=1, nvn_debug.cpp) waits for every submit, so the GPU idles between frames and its clock
// drops: it measured a slower GPU than the game runs on. Here each queued part of a submit or present gets a timestamp
// at its start and its end in its slot's own query pool, and the results are read when the slot is retired (its
// fences passed: no wait); synchronous submits (NX_PIPELINED=0) are one part each, read after their fence. Logged
// every 60 presents: busy = the parts' summed lengths (presents: those of submits that replay nothing), span = first
// start to last end of each slot, and the GPU clock from sysfs when readable.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr uint32_t kSlots = 8, kParts = 8;
VkQueryPool g_pools[kSlots] = {};
double g_period_ns = 1.0;
double g_busy_ns = 0, g_span_ns = 0, g_present_ns = 0;  // present: the parts of submits that replay nothing
uint64_t g_parts = 0, g_not_ready = 0;

uint64_t read_sysfs(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (std::fscanf(f, "%llu", &v) != 1) v = 0;
    std::fclose(f);
    return v;
}

}  // namespace

bool gpu_busy_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_GPU_PROFILE");
        return e && std::atoi(e) == 3;
    }();
    return on;
}

void gpu_busy_part_begin(uint32_t slot, uint32_t part, VkCommandBuffer cb) {
    if (!gpu_busy_on() || slot >= kSlots || part >= kParts) return;
    if (!g_pools[slot]) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kParts;
        p_vkCreateQueryPool(vk_device(), &qi, nullptr, &g_pools[slot]);
        static bool once = false;
        if (!once) {
            once = true;
            VkPhysicalDeviceProperties props{};
            p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
            g_period_ns = props.limits.timestampPeriod;
            log("gpu busy: timestamps on (NX_GPU_PROFILE=3), period %.2f ns", g_period_ns);
        }
    }
    p_vkCmdResetQueryPool(cb, g_pools[slot], 2 * part, 2);
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_pools[slot], 2 * part);
}

void gpu_busy_part_end(uint32_t slot, uint32_t part, VkCommandBuffer cb) {
    if (!gpu_busy_on() || slot >= kSlots || part >= kParts || !g_pools[slot]) return;
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pools[slot], 2 * part + 1);
}

void gpu_busy_retired(uint32_t slot, uint32_t queued, bool replay) {
    if (!gpu_busy_on() || slot >= kSlots || !g_pools[slot] || !queued) return;
    uint64_t first = UINT64_MAX, last = 0;
    for (uint32_t p = 0; p < kParts; p++) {
        if (!(queued >> p & 1)) continue;
        uint64_t t[2] = {};
        if (p_vkGetQueryPoolResults(vk_device(), g_pools[slot], 2 * p, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) !=
            VK_SUCCESS) {
            g_not_ready++;
            continue;
        }
        if (t[1] < t[0]) continue;
        g_busy_ns += double(t[1] - t[0]) * g_period_ns, g_parts++;
        if (!replay) g_present_ns += double(t[1] - t[0]) * g_period_ns;
        first = std::min(first, t[0]), last = std::max(last, t[1]);
    }
    if (last > first) g_span_ns += double(last - first) * g_period_ns;
}

void gpu_busy_log(uint64_t frames) {
    if (!gpu_busy_on() || !frames) return;
    uint64_t clk = read_sysfs("/sys/class/kgsl/kgsl-3d0/gpuclk");
    uint64_t busy_pct = read_sysfs("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
    log("gpu busy: %.2f ms/frame (%.1f parts/frame, presents %.2f ms), span %.2f ms/frame, %llu not ready; clock %llu "
        "MHz, busy %llu%%",
        g_busy_ns / 1e6 / double(frames), double(g_parts) / double(frames), g_present_ns / 1e6 / double(frames),
        g_span_ns / 1e6 / double(frames),
        (unsigned long long)g_not_ready, (unsigned long long)(clk / 1000000), (unsigned long long)busy_pct);
    g_busy_ns = g_span_ns = g_present_ns = 0, g_parts = g_not_ready = 0;
}

}  // namespace nx::nvn
