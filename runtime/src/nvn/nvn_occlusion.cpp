// P5R's occlusion culling: ResetCounter(samples passed) before an object, ReportCounter(samples passed, address)
// after it, and later SetRenderEnableConditional(mode, address) around the same object's other draws.
// NX_TRACE_OCCL=<lines>: the sequence (frame, draw, op, counter/mode, address, targets).
#include <cstdlib>
#include <cstring>

#include <nx/runtime.h>

#include "nvn_draw_state.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace replay;

namespace {

// NX_TRACE_OCCL=<lines>[@<first frame>]
void trace(const char* what, const Cmd& c, const Targets& t) {
    static const char* env = std::getenv("NX_TRACE_OCCL");
    static long left = env ? std::atol(env) : 0;
    static const uint64_t from = env && std::strchr(env, '@') ? std::strtoull(std::strchr(env, '@') + 1, nullptr, 10) : 0;
    if (left <= 0 || g_frame.load() < from) return;
    left--;
    log("occl: frame %llu draw %llu %s %u %llx (targets %u: %llx, depth %llx)", (unsigned long long)g_frame.load(),
        (unsigned long long)g_stats.total, what, c.n, (unsigned long long)c.a[0], t.color_count,
        (unsigned long long)t.colors[0], (unsigned long long)t.depth);
}

}  // namespace

void occlusion_reset(VkCommandBuffer, const Cmd& c, const Targets& t) { trace("reset", c, t); }

void occlusion_report(VkCommandBuffer, const Cmd& c, const Targets& t) {
    if (c.n == 1) trace("report", c, t);
}

// NX_OCCL_FORCE=0|1: every conditional draw skipped / drawn (measuring the most culling could save).
void occlusion_conditional(VkCommandBuffer, const Cmd& c, const Targets& t) {
    trace("conditional", c, t);
    static const char* force = std::getenv("NX_OCCL_FORCE");
    g_state.render_enable = !(force && force[0] == '0');
}

}  // namespace nx::nvn
