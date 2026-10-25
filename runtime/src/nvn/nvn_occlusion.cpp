// P5R's occlusion culling: ResetCounter(samples passed) before an object, ReportCounter(samples passed, address)
// after it, and later SetRenderEnableConditional(mode, address) around the same object's other draws (PROGRESO.md
// #45: ~413 objects a frame on the 3D title, one draw each in both passes).
// On the GPU, without a frame of delay: each query pass draw runs in a Vulkan occlusion query; at the first
// conditional after new reports the results are copied into the upload ring (outside the pass), and each conditional
// draw becomes an indirect draw whose count is its object's result, at most 1 (VK_KHR_draw_indirect_count: the Adreno
// 610 has no VK_EXT_conditional_rendering). An object whose query is unknown (another submit, no draw or several,
// queries used up) is drawn, as before.
// Off by default: on the 3D title the tablet culls 81% of these objects and saves nothing (75.7 ms of GPU a frame
// off, 78.7 on, 80.6 with every conditional draw skipped: they are cheap; PROGRESO.md #49).
// NX_OCCL=1: on. NX_OCCL_FORCE=0|1: every conditional draw skipped / drawn (the most culling could save).
// NX_OCCL_STATS=1: how many objects were hidden.
// NX_TRACE_OCCL=<lines>[@<first frame>]: the sequence (frame, draw, op, counter/mode, address, targets).
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

#include "nvn_draw_state.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace replay;
using namespace vk;

namespace {

constexpr uint32_t kQueries = 4096;  // per submit
VkQueryPool g_pool = VK_NULL_HANDLE;
bool g_pool_reset = false;  // every query reset once (the pool's first submit)
uint32_t g_used = 0;        // this submit's queries (begun and ended around one draw each)
uint32_t g_copied = 0;      // [0, g_copied): their results are in the upload ring, at g_result
std::vector<Upload> g_result;
bool g_object = false;           // between a ResetCounter of samples passed and its ReportCounter
int32_t g_object_query = -1;     // its draw's query (-1: none yet)
uint32_t g_object_draws = 0;     // draws since the ResetCounter
std::unordered_map<uint64_t, int32_t> g_reports;  // report address -> query (-1: unknown)
int32_t g_conditional = -1;      // the query the draws follow until SetRenderEnable (-1: drawn)
struct {
    uint64_t queries = 0, conditional = 0, unknown = 0, copies = 0, several = 0, used_up = 0;
    uint64_t visible = 0, hidden = 0;  // NX_OCCL_STATS=1: query results
} g_stats_occl;

bool forced() { return std::getenv("NX_OCCL_FORCE") != nullptr; }

bool on() {
    static const bool v = [] {
        const char* e = std::getenv("NX_OCCL");
        bool want = e && e[0] == '1' && !forced();
        // Not with NX_ZERO_DRAWS: its queries wrap every draw (two occlusion queries cannot be active at once).
        bool can = vk_indirect_count() && !zero_draws_on();
        log("nvn: occlusion culling %s", want && can ? "on the GPU (queries and indirect count draws)"
                                         : want ? "off (no VK_KHR_draw_indirect_count)" : "off (NX_OCCL=1: on)");
        return want && can;
    }();
    return v;
}

// NX_TRACE_OCCL=<lines>[@<first frame>]
void trace(const char* what, const Cmd& c, const Targets& t) {
    static const char* env = std::getenv("NX_TRACE_OCCL");
    static long left = env ? std::atol(env) : 0;
    static const uint64_t from = env && std::strchr(env, '@') ? std::strtoull(std::strchr(env, '@') + 1, nullptr, 10) : 0;
    if (left <= 0 || g_frame.load() < from) return;
    left--;
    log("occl: frame %llu draw %llu %s %u %llx (targets %u: %llx, depth %llx), queries %u", (unsigned long long)g_frame.load(),
        (unsigned long long)g_stats.total, what, c.n, (unsigned long long)c.a[0], t.color_count,
        (unsigned long long)t.colors[0], (unsigned long long)t.depth, g_used);
}

// The results not copied yet, into the upload ring: outside the pass, and before the indirect draws read them.
void copy_results(VkCommandBuffer cb) {
    const uint32_t n = g_used - g_copied;
    if (!n) return;
    draw_end_rendering(cb, 5);
    Upload u = upload_alloc(4ull * n, 4);
    p_vkCmdCopyQueryPoolResults(cb, g_pool, g_copied, n, u.buffer, u.offset, 4, VK_QUERY_RESULT_WAIT_BIT);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_result.resize(g_used);
    for (uint32_t k = 0; k < n; k++) g_result[g_copied + k] = {u.buffer, u.offset + 4ull * k, nullptr};
    g_copied = g_used;
    g_stats_occl.copies++;
}

}  // namespace

void occlusion_begin_submit(VkCommandBuffer cb) {
    g_object = false, g_object_query = -1, g_object_draws = 0, g_conditional = -1;
    if (!on()) return;
    if (!g_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = kQueries;
        if (p_vkCreateQueryPool(vk_device(), &qi, nullptr, &g_pool) != VK_SUCCESS) g_pool = VK_NULL_HANDLE;
    }
    // NX_OCCL_STATS=1: the last submit's results, waited for (the replay stalls: for checking, not for timing).
    static const bool stats = std::getenv("NX_OCCL_STATS") != nullptr;
    if (stats && g_pool && g_pool_reset && g_used) {
        std::vector<uint32_t> r(g_used);
        if (p_vkGetQueryPoolResults(vk_device(), g_pool, 0, g_used, r.size() * 4, r.data(), 4,
                                    VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (uint32_t v : r) (v ? g_stats_occl.visible : g_stats_occl.hidden)++;
    }
    // Before this submit uses them again (query commands run in submission order: the last submit's copies came
    // first). Outside any pass: the command buffer was just begun.
    if (g_pool && (!g_pool_reset || g_used)) p_vkCmdResetQueryPool(cb, g_pool, 0, g_pool_reset ? g_used : kQueries);
    g_pool_reset = g_pool != VK_NULL_HANDLE;
    g_used = g_copied = 0;
    g_result.clear();
    g_reports.clear();
}

void occlusion_reset(VkCommandBuffer, const Cmd& c, const Targets& t) {
    trace("reset", c, t);
    if (c.n == 1) g_object = true, g_object_query = -1, g_object_draws = 0;  // samples passed
}

void occlusion_report(VkCommandBuffer, const Cmd& c, const Targets& t) {
    if (c.n != 1) return;
    trace("report", c, t);
    if (!on()) return;
    // One draw since the ResetCounter: its query. Otherwise unknown (drawn).
    const bool one = g_object && g_object_draws == 1 && g_object_query >= 0;
    if (g_object && g_object_draws > 1) g_stats_occl.several++;
    g_reports[c.a[0]] = one ? g_object_query : -1;
    g_object = false;
}

void occlusion_conditional(VkCommandBuffer cb, const Cmd& c, const Targets& t) {
    trace("conditional", c, t);
    g_conditional = -1;
    g_state.cond_on = true, g_state.cond_mode = c.n, g_state.cond_addr = c.a[0];
    if (forced()) {  // NX_OCCL_FORCE=0|1
        g_state.render_enable = std::getenv("NX_OCCL_FORCE")[0] != '0';
        return;
    }
    g_state.render_enable = true;
    if (!on()) return;
    // Mode 1: drawn when the counter at the address differs from the u64 16 bytes on (0 in P5R's slots): when the
    // object's query passed samples. Other modes: drawn.
    auto it = g_reports.find(c.a[0]);
    if (c.n != 1 || it == g_reports.end() || it->second < 0 || !p_vkCmdDrawIndexedIndirectCountKHR) {
        g_stats_occl.unknown++;
        return;
    }
    if (uint32_t(it->second) >= g_copied) copy_results(cb);
    g_conditional = it->second;
}

void occlusion_render_enable() { g_conditional = -1; }  // SetRenderEnable ends the conditional draws

int32_t occlusion_query_begin(VkCommandBuffer cb) {
    if (!g_object || !g_pool) return -1;
    g_object_draws++;
    if (g_used >= kQueries) {
        g_stats_occl.used_up++;
        return -1;
    }
    const int32_t q = int32_t(g_used++);
    p_vkCmdBeginQuery(cb, g_pool, uint32_t(q), 0);  // not precise: any sample passed is enough
    g_object_query = q;
    g_stats_occl.queries++;
    return q;
}

void occlusion_query_end(VkCommandBuffer cb, int32_t q) {
    if (q >= 0) p_vkCmdEndQuery(cb, g_pool, uint32_t(q));
}

bool occlusion_draw(VkCommandBuffer cb, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                    int32_t vertex_offset, uint32_t first_instance) {
    if (g_conditional < 0 || uint32_t(g_conditional) >= g_result.size()) return false;
    if (first_instance && !vk_features().drawIndirectFirstInstance) return false;
    const Upload& r = g_result[size_t(g_conditional)];
    if (indexed) {
        VkDrawIndexedIndirectCommand a{count, instances, first, vertex_offset, first_instance};
        Upload args = upload(&a, sizeof a, 4);
        p_vkCmdDrawIndexedIndirectCountKHR(cb, args.buffer, args.offset, r.buffer, r.offset, 1, sizeof a);
    } else {
        VkDrawIndirectCommand a{count, instances, first, first_instance};
        Upload args = upload(&a, sizeof a, 4);
        p_vkCmdDrawIndirectCountKHR(cb, args.buffer, args.offset, r.buffer, r.offset, 1, sizeof a);
    }
    g_stats_occl.conditional++;
    return true;
}

void occlusion_log() {
    auto& s = g_stats_occl;
    if (s.queries || s.conditional || s.unknown)
        log("nvn: occlusion: %llu queries, %llu conditional draws on their results, %llu drawn unknown, %llu copies, "
            "%llu objects of several draws, %llu past the queries",
            (unsigned long long)s.queries, (unsigned long long)s.conditional, (unsigned long long)s.unknown,
            (unsigned long long)s.copies, (unsigned long long)s.several, (unsigned long long)s.used_up);
    if (s.visible + s.hidden)
        log("nvn: occlusion: %llu objects seen, %llu hidden (%.0f%% culled)", (unsigned long long)s.visible,
            (unsigned long long)s.hidden, 100.0 * double(s.hidden) / double(s.visible + s.hidden));
    s = {};
}

}  // namespace nx::nvn
