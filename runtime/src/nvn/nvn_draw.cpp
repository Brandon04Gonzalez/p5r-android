// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <set>

#include "nvn_draw_state.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// The replay state shared with nvn_draw_trace.cpp (nvn_draw_state.h).
namespace replay {

State g_state;
Stats g_stats;
bool g_hud_target = false;
bool g_shadow_target = false;
bool g_low_3d_target = false;

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

}  // namespace replay

using namespace replay;

namespace {

bool g_rendering = false;
bool g_render_pass = false;
VkCommandBuffer g_pass_cb = VK_NULL_HANDLE;  // the command buffer the open pass is recorded in
bool g_vertex_pending = false;  // a narrow pass-end barrier since the last one that covers vertex stages (G4)
uint64_t g_pass_serial = 0, g_vertex_visible_at = 0;  // pass ends so far; the serial a full barrier covered
uint64_t g_vertex_barriers = 0;

// Narrow pass barrier: allows Mali Valhall GPUs to overlap next pass vertex binning with previous pass fragment shading.
bool narrow_pass_barrier() {
#if defined(__ANDROID__)
    static const bool on = [] {
        const char* e = std::getenv("NX_PASS_BARRIER_NARROW");
        return !(e && e[0] == '0');
    }();
#else
    static const bool on = std::getenv("NX_PASS_BARRIER_NARROW") != nullptr;
#endif
    return on;
}

// A vertex shader sampling a rendered image after a narrow pass-end barrier: the pass ends (it cannot take a barrier
// inside) and the writes so far are made visible to the vertex stages.
void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs);  // the open pass is a render pass object (nvn_renderpass.cpp), not dynamic rendering
Targets g_pass;           // what the open pass renders to
GpuImage* g_pass_images[9];  // their images (8: depth), resolved once per pass for gather_textures
uint32_t g_pass_written = 0;  // targets the open pass's draws wrote: bit c colour c, bit 8 depth
uint64_t g_pass_draws = 0;    // draws in the open pass (NX_EXP_RP split)
uint64_t g_pass_ends[8];  // passes ended, by draw_end_rendering reason
uint64_t g_submit_ns = 0;  // when the submit being replayed started
VkExtent2D g_pass_extent{};

// What this submit's command buffer already has bound and set. Vulkan keeps the pipeline, vertex and index buffers
// and dynamic state of a command buffer across its dynamic-rendering passes, and most draws repeated what the draw
// before them set (the vertex streams and the viewport of a whole pass): only what differs is recorded. Forgotten
// at each submit, on another command buffer, and when something else binds a pipeline there (the masked clear's
// fullscreen draw, nvn_mask_clear.cpp). NX_DRAW_SLOW: forgotten before every draw.
struct Bound {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t vertex_set = 0;  // bit s: binding s holds vertex[s]
    Upload vertex[16];
    bool has_index = false, has_dynamic = false;
    Upload index;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    uint32_t stencil[6] = {};  // stencil reference and masks per face
    uint32_t bias[4] = {};     // depth bias factor/units/clamp + equal_slack flag
} g_bound;
uint64_t g_state_set = 0, g_state_kept = 0;  // state commands recorded and left out, for the log
VkPipeline g_last_compute_pipeline = VK_NULL_HANDLE;
bool g_compute_barrier_pending = false;

template <typename T>
bool same_bytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

void bind_vertex(VkCommandBuffer cb, uint32_t binding, const Upload& u) {
    Upload& now = g_bound.vertex[binding];
    if ((g_bound.vertex_set >> binding & 1) && now.buffer == u.buffer && now.offset == u.offset) {
        g_state_kept++;
        return;
    }
    VkDeviceSize off = u.offset;
    p_vkCmdBindVertexBuffers(cb, binding, 1, &u.buffer, &off);
    now = u, g_bound.vertex_set |= 1u << binding, g_state_set++;
}

// Binds active vertex streams (plus binding 15 zero_stream) in contiguous batches.
void bind_vertex_streams(VkCommandBuffer cb, uint32_t used_mask, const Upload* streams) {
    uint32_t active_mask = (used_mask & 0xFFFFu) | (1u << 15);
    uint32_t need_mask = 0;
    const Upload zero = zero_stream();
    for (uint32_t m = active_mask; m; m &= m - 1) {
        const uint32_t s = uint32_t(__builtin_ctz(m));
        const Upload& u = (s == 15) ? zero : streams[s];
        const Upload& now = g_bound.vertex[s];
        if ((g_bound.vertex_set >> s & 1) && now.buffer == u.buffer && now.offset == u.offset) {
            g_state_kept++;
        } else {
            need_mask |= 1u << s;
        }
    }
    if (!need_mask) return;

    while (need_mask) {
        const uint32_t s = uint32_t(__builtin_ctz(need_mask));
        uint32_t n = 1;
        while (s + n < 16 && (need_mask >> (s + n) & 1)) n++;
        VkBuffer bufs[16];
        VkDeviceSize offs[16];
        for (uint32_t i = 0; i < n; i++) {
            const Upload& u = (s + i == 15) ? zero : streams[s + i];
            bufs[i] = u.buffer;
            offs[i] = u.offset;
            g_bound.vertex[s + i] = u;
            g_bound.vertex_set |= 1u << (s + i);
            g_state_set++;
        }
        p_vkCmdBindVertexBuffers(cb, s, n, bufs, offs);
        need_mask &= ~(((1u << n) - 1u) << s);
    }
}

// Viewport, scissor, blend constants and the rasterizer's dynamic state (every pipeline declares them dynamic).
void set_dynamic(VkCommandBuffer cb, const VkViewport& vp, const VkRect2D& sc) {
    Bound& b = g_bound;
    const State& s = g_state;
    bool all = !b.has_dynamic;
    b.has_dynamic = true;
    auto differs = [&](bool d) { return (all || d) ? (g_state_set++, true) : (g_state_kept++, false); };
    if (differs(!same_bytes(b.viewport, vp))) p_vkCmdSetViewport(cb, 0, 1, &vp), b.viewport = vp;
    if (differs(!same_bytes(b.scissor, sc))) p_vkCmdSetScissor(cb, 0, 1, &sc), b.scissor = sc;
    if (differs(std::memcmp(b.blend, s.blend_color, sizeof b.blend) != 0))
        p_vkCmdSetBlendConstants(cb, s.blend_color), std::memcpy(b.blend, s.blend_color, sizeof b.blend);
    uint32_t stencil[6] = {s.stencil_ref[0],          s.stencil_ref[1],
                           s.stencil_compare_mask[0], s.stencil_compare_mask[1],
                           s.stencil_write_mask[0],   s.stencil_write_mask[1]};
    if (differs(std::memcmp(b.stencil, stencil, sizeof stencil) != 0))
        stencil_dynamic(cb), std::memcpy(b.stencil, stencil, sizeof stencil);
    uint32_t bias[4];
    std::memcpy(bias + 0, &s.bias_factor, 4);
    std::memcpy(bias + 1, &s.bias_units, 4);
    std::memcpy(bias + 2, &s.bias_clamp, 4);
    bool slack = s.depth.depth_test && equal_slack_pass(s.depth);
    bias[3] = slack;
    if (differs(std::memcmp(b.bias, bias, sizeof bias) != 0))
        depth_bias_dynamic(cb, slack), std::memcpy(b.bias, bias, sizeof bias);  // (nvn_raster.cpp)
}

// No colour image behind any of the targets' colour slots: P5R binds its shadow maps with two empty colour slots.
bool depth_only(const Targets& t) {
    for (uint32_t k = 0; k < t.color_count; k++)
        if (t.colors[k]) return false;
    return t.depth != 0;
}

// NX_PASS_CENSUS=1: per kind of pass (extent, formats and load operations: L load, C clear, - don't care) how often
// it begins and the bytes its loads and stores move (whole attachments, uncompressed), logged every 300 frames with
// the totals per frame. On a tiler each load and store is a trip to memory (Adreno: the GMEM), whatever the draws.
void pass_census(const Targets& t, const VkRenderingAttachmentInfo* colors, const VkFormat* formats,
                 const GpuImage* dimg, const VkRenderingAttachmentInfo& depth, const VkRenderingAttachmentInfo& stencil,
                 VkExtent2D extent) {
    static const bool on = std::getenv("NX_PASS_CENSUS") != nullptr;
    if (!on) return;
    struct Entry {
        uint64_t passes = 0, load = 0, store = 0;
    };
    static std::map<std::string, Entry> kinds;
    static uint64_t next_log = 0, from = 0;
    const uint64_t frame = g_frame.load();
    if (!next_log) next_log = frame + 300, from = frame;
    if (frame >= next_log && !kinds.empty()) {
        const double n = double(std::max<uint64_t>(1, frame - from));
        std::vector<std::pair<uint64_t, const std::string*>> v;
        Entry all;
        for (auto& [k, e] : kinds) {
            v.push_back({e.load + e.store, &k});
            all.passes += e.passes, all.load += e.load, all.store += e.store;
        }
        std::sort(v.rbegin(), v.rend());
        log("pass census over %.0f frames: %.1f passes, loads %.2f MB, stores %.2f MB per frame", n,
            double(all.passes) / n, double(all.load) / n / 1e6, double(all.store) / n / 1e6);
        for (size_t k = 0; k < v.size() && k < 24; k++) {
            const Entry& e = kinds[*v[k].second];
            log("pass census: %5.1f/frame loads %6.2f MB stores %6.2f MB  %s", double(e.passes) / n,
                double(e.load) / n / 1e6, double(e.store) / n / 1e6, v[k].second->c_str());
        }
        kinds.clear(), from = frame, next_log = frame + 300;
    }
    auto op = [](VkAttachmentLoadOp o) { return o == VK_ATTACHMENT_LOAD_OP_LOAD ? 'L' : o == VK_ATTACHMENT_LOAD_OP_CLEAR ? 'C' : '-'; };
    const uint64_t area = uint64_t(extent.width) * extent.height;
    char key[160];
    int at = std::snprintf(key, sizeof key, "%ux%u", extent.width, extent.height);
    uint64_t load = 0, store = 0;
    for (uint32_t k = 0; k < t.color_count && k < 8; k++) {
        if (!colors[k].imageView) continue;
        const uint64_t bytes = area * std::max(1u, vk_block_bytes(formats[k]));
        if (colors[k].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) load += bytes;
        store += bytes;
        if (at > 0 && at < int(sizeof key) - 16) at += std::snprintf(key + at, sizeof key - at, " c%d%c", int(formats[k]), op(colors[k].loadOp));
    }
    if (dimg) {
        uint32_t b = vk_block_bytes(dimg->format);
        const uint64_t bytes = area * (b ? b : dimg->format == VK_FORMAT_D24_UNORM_S8_UINT ? 4 : 5);
        if (depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) load += bytes;
        store += bytes;
        if (at > 0 && at < int(sizeof key) - 16)
            std::snprintf(key + at, sizeof key - at, " d%d%c%c", int(dimg->format), op(depth.loadOp), op(stencil.loadOp));
    }
    Entry& e = kinds[key];
    e.passes++, e.load += load, e.store += store;
}

void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    if (clears_pending_other(t)) {  // other targets' clears first, in order (NX_CLEARS_V2: only those on its images)
        if (live_clears_why()) clears_log_mismatch("pass", t);
        clears_flush_reason(10), clears_flush_other(cb, t);
    }
    // An image never written (layout UNDEFINED) has nothing to load: loadOp CLEAR to zero, not a LOAD of whatever
    // the memory held (G3, docs/nvn/DEBUG.md #50); a pending clear still wins (clears_apply). Env: bisecting.
    static const bool load_undefined = std::getenv("NX_UNDEFINED_LOAD") != nullptr;
    uint32_t fresh = 0;  // bit c colour c, bit 8 depth: begun cleared to 0 (depth 0, stencil 0)
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    VkFormat formats[8]{};  // the views' (render pass objects)
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        g_pass_images[k] = img;
        if (!img) continue;
        if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << k;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // rendered images stay GENERAL (sampled while rendered)
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        formats[k] = view_format(*img, t.views.format[k]);
        colors[k].imageView = attachment_view(*img, level, t.views.layer[k], formats[k]);
        // NX_TRACE_CUBES=<first frame>: writes into cube images from that frame on. With 400 from the start, the log
        // ran out at the first 3D scene (frame ~171) and never reached a course load.
        static const char* trace_cubes = std::getenv("NX_TRACE_CUBES");
        static const uint64_t cube_from = trace_cubes ? std::strtoull(trace_cubes, nullptr, 10) : 0;
        static int cube_logs = 0;
        if (trace_cubes && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            g_frame >= cube_from && cube_logs++ < 2000)
            log("cubes: render into %llx level %u layer %u (%ux%u, %u levels, %u layers) view %s",
                (unsigned long long)t.colors[k], level, t.views.layer[k], img->width, img->height, img->levels,
                img->layers, colors[k].imageView == img->view ? "WHOLE IMAGE (fallback)" : "ok");
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = fresh >> k & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    g_pass_images[8] = dimg;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        if (dimg->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << 8;
        transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL);
        dimg->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        depth.imageView = attachment_view(*dimg, level, t.views.layer[8]);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = fresh >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = {1.0f, 0};  // far (a fresh depth buffer; the course passes test LESS)
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
    } else {
        dimg = nullptr;
    }
    if (dimg && depth_only(t)) {  // depth-only targets (P5R: its shadow maps), once each
        static std::vector<const GpuImage*> seen;
        if (seen.size() < 32 && std::find(seen.begin(), seen.end(), dimg) == seen.end()) {
            seen.push_back(dimg);
            log("nvn: depth-only pass into %llx: %ux%u, %u layers, format %d, level %u layer %u",
                (unsigned long long)t.depth, dimg->width, dimg->height, dimg->layers, int(dimg->format),
                t.views.level[8], t.views.layer[8]);
        }
    }
    if (extent.width == UINT32_MAX) extent = {1, 1};
    VkRenderingAttachmentInfo stencil = depth;  // same view, its own loadOp
    stencil.storeOp = g_shadow_target ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    uint32_t cleared = clears_apply(t, colors, depth, stencil);  // pending clears of these targets: loadOp CLEAR
    if (exp_render_pass() & 1) {  // NX_EXP_RP=noload: what loading the targets costs (the picture breaks)
        for (uint32_t k = 0; k < t.color_count; k++)
            if (colors[k].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) colors[k].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        if (depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        if (stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) stencil.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    if (gpu_profile_on() && std::getenv("NX_PROFILE_TARGETS")) {  // the targets' formats and load operations (L load, C clear, - don't care)
        auto op = [](VkAttachmentLoadOp o) { return o == VK_ATTACHMENT_LOAD_OP_LOAD ? 'L' : o == VK_ATTACHMENT_LOAD_OP_CLEAR ? 'C' : '-'; };
        std::string s = " [";
        char b[32];
        for (uint32_t k = 0; k < t.color_count; k++)
            if (colors[k].imageView) std::snprintf(b, sizeof b, "c%d%c ", int(formats[k]), op(colors[k].loadOp)), s += b;
        if (dimg) std::snprintf(b, sizeof b, "d%d%c%c", int(dimg->format), op(depth.loadOp), op(stencil.loadOp)), s += b;
        gpu_profile_annotate(s + "]");
    }
    pass_census(t, colors, formats, dimg, depth, stencil, extent);  // NX_PASS_CENSUS
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = t.color_count;
    ri.pColorAttachments = colors;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
    }
    g_render_pass = render_pass_objects();
    if (g_render_pass)
        begin_render_pass(cb, t.color_count, formats, colors, dimg ? dimg->format : VK_FORMAT_UNDEFINED, depth, stencil,
                          extent);
    else
        p_vkCmdBeginRendering(cb, &ri);
    g_rendering = true;
    g_pass_cb = cb;
    g_pass_written = cleared | fresh;  // as a clear inside the pass: sampling it needs it stored first
    g_pass_draws = 0;
    trace_pass("begin: cleared, fresh", t, cleared, fresh);
    g_pass = t;
    g_pass_extent = extent;
}

uint64_t g_dump_draws = 0, g_dump_request = 0;

// The graphics stages bound now: one program for all of them (its own list), or a vertex and a fragment program
// bound separately (their stages gathered in `out`).
uint64_t g_bound_pv = 0, g_bound_pf = 0, g_bound_serial = 0;
const std::vector<const ShaderInfo*>* bound_graphics_shaders(std::vector<const ShaderInfo*>& out) {
    const uint64_t pv = g_state.stage_programs[0], pf = g_state.stage_programs[1];
    if (!pv && !pf) return nullptr;
    if (pv == pf) return program_shaders(pv);
    if (t_replay_serial && g_bound_serial == t_replay_serial && g_bound_pv == pv && g_bound_pf == pf && !out.empty())
        return &out;
    out.clear();
    if (const auto* sh = program_shaders(pv))
        for (const ShaderInfo* s : *sh)
            if (s->stage == ShaderStage::Vertex) out.push_back(s);
    if (const auto* sh = program_shaders(pf))
        for (const ShaderInfo* s : *sh)
            if (s->stage == ShaderStage::Fragment) out.push_back(s);
    g_bound_pv = pv, g_bound_pf = pf, g_bound_serial = t_replay_serial;
    return &out;
}

std::vector<const ShaderInfo*> g_bound_shaders;
const std::vector<const ShaderInfo*>* bound_shaders() {
    return bound_graphics_shaders(g_bound_shaders);
}

// DispatchCompute: the bound program's compute stage over (x, y, z) groups, outside render passes, then a barrier
// so what it wrote is visible to what follows.
void dispatch(VkCommandBuffer cb, const Cmd& c) {
    static const bool off = std::getenv("NX_NO_COMPUTE") != nullptr;  // debug: skip compute dispatches
    if (off) return;
    g_hud_target = false;  // no render target: render scale 1 in its support buffer
    g_low_3d_target = false;
    const auto* shaders = program_shaders(g_state.stage_programs[5]);
    const ShaderInfo* cs = nullptr;
    if (shaders)
        for (const ShaderInfo* s : *shaders)
            if (s->stage == ShaderStage::Compute) cs = s;
    if (!cs) return;
    uint32_t x = uint32_t(c.i[0]), y = uint32_t(c.i[1]), z = uint32_t(c.i[2]);
    const uint8_t* args = c.n == 1 ? guest_bytes(c.a[0]) : nullptr;  // indirect: {x, y, z}
    if (args) {
        flush_compute_barrier(cb);
        frame_guest_read(args, 12);  // deferred write-backs over the arguments land first (nvn_frames.cpp)
    }
    if (c.n == 1 && !args) return;
    // Written by an earlier dispatch of this submit: only the GPU knows them (vkCmdDispatchIndirect, below).
    bool gpu_args = args && !written_separate() && written_overlaps(args, 12);
    if (args && !gpu_args) std::memcpy(&x, args, 4), std::memcpy(&y, args + 4, 4), std::memcpy(&z, args + 8, 4);
    if (!gpu_args && (!x || !y || !z)) return;
    trace_compute_once(cs, x, y, z);  // 0, 0, 0 for GPU arguments
    trace_nan_dispatch(cs);
    static thread_local std::vector<const ShaderInfo*> stages(1);
    static thread_local const ShaderInfo* s_last_cs = nullptr;
    static thread_local const ProgramLayout* s_last_cs_layout = nullptr;
    static thread_local VkPipeline s_last_cs_pipe = VK_NULL_HANDLE;
    stages[0] = cs;
    if (cs != s_last_cs || !s_last_cs_layout || !s_last_cs_pipe) {
        s_last_cs = cs;
        s_last_cs_layout = &program_layout(stages);
        s_last_cs_pipe = compute_pipeline(cs, *s_last_cs_layout);
    }
    const ProgramLayout& layout = *s_last_cs_layout;
    VkPipeline pipeline = s_last_cs_pipe;
    if (!pipeline) return;
    pass_end_only(cb, 2);
    uint32_t gathered = gather_textures(stages);
    if (gathered & kSampledPending) clears_flush_reason(2), clears_flush(cb);
    build_sets(cb, stages, layout);
    if (pipeline != g_last_compute_pipeline) {
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        g_last_compute_pipeline = pipeline;
    }
    bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout);
    if (gpu_profile_on()) gpu_profile_mark(cb, "compute " + cs->hash);
    if (!gpu_args || !written_indirect(cb, args)) p_vkCmdDispatch(cb, x, y, z);
    if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");

    // Defer compute barrier until vertex/graphics consumption, read-after-write, or transfer.
    g_compute_barrier_pending = true;
    g_stats.dispatches++;
}

void vertex_read_barrier(VkCommandBuffer cb, const ShaderInfo* vs) {
    if (!g_vertex_pending || !vs) return;
    bool reads = false;
    for (size_t k = 0; k < g_textures.bindings.size() && !reads; k++)
        if (g_textures.bindings[k].first == vs && k < g_textures.resolved.size() && g_textures.resolved[k].texture)
            if (GpuImage* img = image_of(g_textures.resolved[k].texture))
                reads = img->rendered && img->rendered_at > g_vertex_visible_at;
    if (!reads) return;
    draw_end_rendering(cb, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_vertex_pending = false, g_vertex_barriers++, g_vertex_visible_at = g_pass_serial;
}

}  // namespace

// NX_SHADOWS=0 (the launcher's "Sombras: desactivadas"): no draws into shadow maps, the square depth-only targets of
// 512 or more (P5R: 2048 and 1024, D32). Their clears stay: the maps say "lit" everywhere.
bool shadow_draw_skipped(const Targets& t) {
    static const bool s_shadows_off = [] {
        const char* e = std::getenv("NX_SHADOWS");
        return e && e[0] == '0';
    }();
    if (!s_shadows_off || !depth_only(t)) return false;
    const GpuImage* d = image_of(t.depth);
    return d && d->width == d->height && (d->width >= 256 || d->shadow) && !(d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
}

// NX_DISABLE_BLOOM=1 (the launcher's "Post-procesado: desactivado"): no draws into bloom/blur mip chains.
bool bloom_draw_skipped(const Targets& t) {
    static const bool s_bloom_disabled = [] {
        const char* e = std::getenv("NX_DISABLE_BLOOM");
        return e && (e[0] == '1' || std::strcmp(e, "true") == 0);
    }();
    if (!s_bloom_disabled) return false;
    if (t.color_count > 0 && t.views.level[0] > 0) return true;
    return false;
}

uint64_t current_pass_serial() { return g_pass_serial; }
uint32_t current_pass_written() { return g_pass_written; }

namespace {

// Live experiments while playing (debugging HUD glitches without a restart): a file named live_flags in the data
// folder holds words, read about once a second while it exists (removing it turns them off). ui_always: draws into a
// 1280x720 or larger RGBA8 target that test depth use ALWAYS (draw order alone, as if no depth buffer).
// ui_nowrite: those draws do not write depth. For those draws with plain alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA
// for colour and alpha), the alpha they leave: ui_rgb keeps the target's (no alpha write), ui_amax the larger of the
// two, ui_aone ONE, ONE_MINUS_SRC_ALPHA (the battle portrait's specks, PROGRESO 67).
enum : uint32_t {
    kLiveUiAlways = 1, kLiveUiNoWrite = 2, kLiveUiRgb = 4, kLiveUiAmax = 8, kLiveUiAone = 16,
    kLiveRpNoLoad = 32, kLiveRpNoStore = 64, kLiveRpSplit = 256,  // exp_render_pass
    kLiveNoWb = 512,  // exp_no_write_backs
    kLiveClearsWhy = 128,  // clears_why: the targets of a pending clear and of what flushed it (nvn_clears.cpp)
};

uint32_t live_flags() {
    static const uint32_t s_live_env_flags = [] {
        uint32_t ef = 0;
        if (const char* e = std::getenv("NX_UI_ALWAYS"); e && e[0] == '1') ef |= kLiveUiAlways;
        if (const char* e = std::getenv("NX_UI_NOWRITE"); e && e[0] == '1') ef |= kLiveUiNoWrite;
        if (const char* e = std::getenv("NX_UI_RGB"); e && e[0] == '1') ef |= kLiveUiRgb;
        if (const char* e = std::getenv("NX_UI_AMAX"); e && e[0] == '1') ef |= kLiveUiAmax;
        if (const char* e = std::getenv("NX_UI_AONE"); e && e[0] == '1') ef |= kLiveUiAone;
        return ef;
    }();
    static uint64_t next_check = 0;
    static uint32_t flags = 0;
    const uint64_t f = g_frame.load();
    if (f < next_check) return flags;
    next_check = f + 30;
    uint32_t now = 0;
    if (FILE* file = plat::kDiagnostics ? plat::fopen_utf8(plat::data_root() + "/live_flags", "rb") : nullptr) {
        char words[256] = {};
        std::fread(words, 1, sizeof words - 1, file);
        std::fclose(file);
        if (std::strstr(words, "ui_always")) now |= kLiveUiAlways;
        if (std::strstr(words, "ui_nowrite")) now |= kLiveUiNoWrite;
        if (std::strstr(words, "ui_rgb")) now |= kLiveUiRgb;
        if (std::strstr(words, "ui_amax")) now |= kLiveUiAmax;
        if (std::strstr(words, "ui_aone")) now |= kLiveUiAone;
        if (std::strstr(words, "rp_noload")) now |= kLiveRpNoLoad;
        if (std::strstr(words, "rp_nostore")) now |= kLiveRpNoStore;
        if (std::strstr(words, "rp_split")) now |= kLiveRpSplit;
        if (std::strstr(words, "no_wb")) now |= kLiveNoWb;
        if (std::strstr(words, "clears_why")) now |= kLiveClearsWhy;
    }
    now |= s_live_env_flags;
    if (now != flags) log("live_flags: %x (was %x) at frame %llu", now, flags, (unsigned long long)f);
    flags = now;
    return flags;
}

}  // namespace

bool live_clears_why() { return live_flags() & kLiveClearsWhy; }

bool exp_no_write_backs() {
    static const bool s_exp_no_wb_env = [] {
        const char* e = std::getenv("NX_EXP_NO_WB");
        return e && e[0] != '0';
    }();
    return s_exp_no_wb_env || (live_flags() & kLiveNoWb);
}

int exp_render_pass() {
    static const int s_exp_rp_env = [] {
        const char* e = std::getenv("NX_EXP_RP");
        if (!e) return 0;
        return (std::strstr(e, "noload") ? 1 : 0) | (std::strstr(e, "nostore") ? 2 : 0) | (std::strstr(e, "split") ? 4 : 0);
    }();
    const uint32_t live = live_flags();
    return s_exp_rp_env | (live & kLiveRpNoLoad ? 1 : 0) | (live & kLiveRpNoStore ? 2 : 0) | (live & kLiveRpSplit ? 4 : 0);
}

namespace {

// NX_RESTART_STATS=1: per NVN primitive mode, the indexed draws made while restart is on, how many hold the restart
// index, and of those how many have 32-bit indices (Vulkan would not restart at NVN's 0xFFFF there). Every 600 frames.
// NX_FRAME_DRAWS=1: one line per frame, its depth-tested draws (the 3D) and their vertex shaders' hash
// sum, with the monotonic time (logcat's clock on Android), to line a screen recording up with what the game
// submitted: the Yongen-Jaya pop-ins are ~80 draws the game stops sending (PROGRESO 68).
void frame_draws(const ShaderInfo* vs, uint64_t count) {
    static const bool s_frame_draws_on = std::getenv("NX_FRAME_DRAWS") != nullptr;
    if (!s_frame_draws_on) return;
    static uint64_t frame = 0, draws = 0, verts = 0, sum = 0;
    const uint64_t f = g_frame.load();
    if (f != frame) {
        if (draws) log("frame draws: %llu at %.3f s: %llu 3D draws, %llu indices, vs sum %016llx", (unsigned long long)frame,
                       double(plat::monotonic_ns()) / 1e9, (unsigned long long)draws, (unsigned long long)verts,
                       (unsigned long long)sum);
        frame = f, draws = verts = sum = 0;
    }
    if (!g_state.depth.depth_test) return;
    draws++, verts += count;
    sum += std::strtoull(vs->hash.substr(0, 16).c_str(), nullptr, 16);
}

void restart_stats(uint32_t mode, const Cmd& c) {
    static const bool s_restart_stats_on = std::getenv("NX_RESTART_STATS") != nullptr;
    if (!s_restart_stats_on) return;
    static uint64_t draws[16] = {}, with[16] = {}, wide[16] = {}, logged = 0;
    const uint32_t m = mode & 15, count = uint32_t(c.i[0]), size = index_size(c.mask);
    draws[m]++;
    if (const uint8_t* p = guest_bytes(c.a[0])) {
        bool found = false;
        for (uint32_t k = 0; k < count && !found; k++) {
            uint32_t v = size == 1 ? p[k] : size == 2 ? reinterpret_cast<const uint16_t*>(p)[k] : reinterpret_cast<const uint32_t*>(p)[k];
            found = v == g_state.restart_index;
        }
        if (found) with[m]++, wide[m] += size == 4;
    }
    const uint64_t f = g_frame.load();
    if (f >= logged + 600) {
        logged = f;
        std::string s;
        for (uint32_t k = 0; k < 16; k++)
            if (draws[k]) s += " mode " + std::to_string(k) + ": " + std::to_string(draws[k]) + " (" + std::to_string(with[k]) +
                               " with restart, " + std::to_string(wide[k]) + " of them 32-bit)";
        log("nvn: restart stats, index %#x:%s", g_state.restart_index, s.c_str());
    }
}

// Pitch-linear render targets (a texture with a stride) are read by the CPU in place: P5R draws a depth pyramid
// (R32F, 480x270 down to 15x9, flags 0x44) that nothing samples, and its occlusion culling reads it from guest memory
// (main+fdd6e0); with only the Vulkan image written, the CPU read stale bytes (zeros, another picture's texels) as
// depths, took far buildings for hidden and left them out for whole seconds (the Yongen-Jaya pop-ins, PROGRESO 72).
// The images drawn into in a submit are copied to the upload buffer at its end and written to guest memory once it
// completed, with the other write-backs. NX_LINEAR_WRITE_BACK=0: not (as before).
std::vector<uint64_t> g_linear_targets;  // this submit's (NVNtexture*), drawn into
std::vector<WriteBack> g_late_write_backs;  // their copies: applied when the submit retires, never waited for

bool linear_write_back_on() {
    static const bool s_linear_write_back_on = !(std::getenv("NX_LINEAR_WRITE_BACK") && std::getenv("NX_LINEAR_WRITE_BACK")[0] == '0');
    return s_linear_write_back_on;
}

std::atomic<bool> g_linear_paused{false};  // linear_write_back_pause: the game says nobody reads them now

void note_linear_target(uint64_t texture) {
    if (!texture || !linear_write_back_on() || g_linear_paused.load(std::memory_order_relaxed)) return;
    const Texture* tx = get<Texture>(texture);
    if (!tx || !tx->stride || !tx->memory) return;
    if (std::find(g_linear_targets.begin(), g_linear_targets.end(), texture) == g_linear_targets.end())
        g_linear_targets.push_back(texture);
}

// Runtime configurations initialized lazily on first submit (after android_main / game::renderer_start)
struct SnapFsBeforeConfig {
    std::string str;
    uint64_t from = 0;
    bool active = false;
};
struct UwTraceConfig {
    const char* env = nullptr;
    uint64_t from = 0;
    uint64_t count = 0;
    bool active = false;
};
struct SnapFsConfig {
    std::string str;
    uint64_t from = 0;
    int max = 8;
    bool active = false;
};
struct DumpFsConfig {
    std::string str;
    uint64_t from = 0;
    bool any = false;
    bool active = false;
};

static bool s_draw_env_initialized = false;
static SnapFsBeforeConfig s_snap_fs_before;
static int s_snap_fs_before_taken = 0;
static const char* s_dump_draw_textures_env = nullptr;
static int s_dump_draw_textures_done = 0;
static bool s_dump_draw_textures_said = false;
static bool s_targets_memcmp = false;
static int s_keep_hud_alpha = 0;
static bool s_prim_restart_on = true;
static bool s_uw_cam_patch = true;
static bool s_uw_ui_learn = false;
static UwTraceConfig s_uw_trace;
static int s_exp3d = 0;
static SnapFsConfig s_snap_fs;
static int s_snap_fs_count = 0;
static uint64_t s_dump_target = 0;
static DumpFsConfig s_dump_fs;

void init_draw_env_once() {
    if (__builtin_expect(s_draw_env_initialized, 1)) return;
    s_draw_env_initialized = true;
    if (const char* e = std::getenv("NX_SNAP_FS_BEFORE"); e && e[0]) {
        s_snap_fs_before.str = e;
        size_t at = s_snap_fs_before.str.find('@');
        s_snap_fs_before.from = (at != std::string::npos) ? std::strtoull(s_snap_fs_before.str.c_str() + at + 1, nullptr, 10) : 0;
        s_snap_fs_before.active = true;
    }
    s_dump_draw_textures_env = std::getenv("NX_DUMP_DRAW_TEXTURES");
    s_targets_memcmp = std::getenv("NX_TARGETS_MEMCMP") != nullptr;
    if (const char* keep_env = std::getenv("NX_HUD_KEEP_ALPHA")) {
        s_keep_hud_alpha = keep_env[0] == '1' ? 1 : keep_env[0] == 'a' ? 2 : 0;
    } else {
        s_keep_hud_alpha = 0;
    }
    s_prim_restart_on = !(std::getenv("NX_PRIM_RESTART") && std::getenv("NX_PRIM_RESTART")[0] == '0');
    s_uw_cam_patch = !(std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0');
    if (const char* e = std::getenv("NX_UW_UI")) {
        s_uw_ui_learn = std::strcmp(e, "learn") == 0;
    }
    s_uw_trace.env = std::getenv("NX_UW_TRACE");
    if (s_uw_trace.env && std::sscanf(s_uw_trace.env, "%lu:%lu", &s_uw_trace.from, &s_uw_trace.count) == 2) {
        s_uw_trace.active = true;
    }
    if (const char* e = std::getenv("NX_EXP_3D")) {
        s_exp3d = std::strcmp(e, "scissor") == 0 ? 1 : std::strcmp(e, "tri") == 0 ? 2 : std::strcmp(e, "small") == 0 ? 3 : 0;
    }
    if (const char* e = std::getenv("NX_SNAP_FS"); e && e[0]) {
        s_snap_fs.str = e;
        size_t at = s_snap_fs.str.find('@');
        s_snap_fs.from = (at != std::string::npos) ? std::strtoull(s_snap_fs.str.c_str() + at + 1, nullptr, 10) : 0;
        const char* me = std::getenv("NX_SNAP_FS_MAX");
        s_snap_fs.max = me ? std::atoi(me) : 8;
        s_snap_fs.active = true;
    }
    if (const char* e = std::getenv("NX_DUMP_ON_TARGET")) {
        s_dump_target = std::strtoull(e, nullptr, 16);
    }
    if (const char* e = std::getenv("NX_DUMP_ON_FS"); e && e[0]) {
        s_dump_fs.str = e;
        const char* fe = std::getenv("NX_DUMP_FROM");
        s_dump_fs.from = fe ? std::strtoull(fe, nullptr, 10) : 0;
        s_dump_fs.any = std::getenv("NX_DUMP_ANY") != nullptr;
        s_dump_fs.active = true;
    }
}

// Thread-local state cached for draw()
static thread_local uint64_t s_cached_pv = UINT64_MAX, s_cached_pf = UINT64_MAX;
static thread_local std::vector<const ShaderInfo*> s_combined_shaders;
static thread_local const std::vector<const ShaderInfo*>* s_cached_shaders = nullptr;
static thread_local const ShaderInfo* s_cached_vs = nullptr;
static thread_local const ShaderInfo* s_cached_fs = nullptr;
static thread_local const ProgramLayout* s_cached_layout = nullptr;
static thread_local std::vector<const ShaderInfo*> s_swapped_shaders;

static thread_local uint64_t s_cached_target_c0 = UINT64_MAX, s_cached_target_depth = UINT64_MAX;
static thread_local uint32_t s_cached_target_color_count = UINT32_MAX;
static thread_local bool s_cached_uw_wide = false;
static thread_local bool s_cached_hud_target = false;
static thread_local bool s_cached_shadow_target = false;
static thread_local bool s_cached_low_3d_target = false;
static thread_local VkFormat s_cached_color_formats[8] = {};
static thread_local uint32_t s_cached_ufloat_colors = 0;
static thread_local VkFormat s_cached_depth_format = VK_FORMAT_UNDEFINED;
static thread_local Targets s_cached_format_targets{};
static thread_local bool s_cached_formats_valid = false;

// MRU Pipeline Cache
struct MruEntry {
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
};
static constexpr size_t kMruSize = 8;
static thread_local MruEntry s_mru_cache[kMruSize]{};
static thread_local uint8_t s_mru_order[kMruSize] = {0, 1, 2, 3, 4, 5, 6, 7};
static thread_local size_t s_mru_count = 0;

bool live_ui_draw(const Targets& t) {
    const Texture* t0 = t.color_count && t.colors[0] ? get<Texture>(t.colors[0]) : nullptr;
    return t0 && t0->width >= 1280 && t0->format == 0x25 && t.depth && g_state.depth.depth_test;
}

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    g_stats.total++;
    if (g_state.rasterizer_discard) return false;  // nothing reaches the targets
    // A draw hidden by CPU occlusion culling still marks its textures as sampled (gather_textures below): kept
    // textures nothing sampled for 2 s are released and their slots sample the dummy (black buildings when they
    // came back into view). Everything else about the draw is skipped.
    bool hidden_touch = false;
    if (!g_state.render_enable) {
        if (!occlusion_cpu_culling()) return false;
        hidden_touch = true;
    }
    if (occlusion_object_skipped() && !g_state.depth.depth_write && !g_state.depth.stencil_test) {
        bool writes_color = false;
        for (uint32_t k = 0; k < t.color_count; k++) {
            if (t.colors[k] && (g_state.mask.mask[k] & 0xF)) {
                writes_color = true;
                break;
            }
        }
        if (!writes_color) return false;
    }
    if (shadow_draw_skipped(t)) return false;
    if (bloom_draw_skipped(t)) return false;

    const uint64_t cur_pv = g_state.stage_programs[0];
    const uint64_t cur_pf = g_state.stage_programs[1];
    const std::vector<const ShaderInfo*>* shaders = nullptr;
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    const ProgramLayout* layout_ptr = nullptr;

    if (cur_pv == s_cached_pv && cur_pf == s_cached_pf && s_cached_shaders) {
        shaders = s_cached_shaders;
        vs = s_cached_vs;
        fs = s_cached_fs;
        layout_ptr = s_cached_layout;
    } else {
        shaders = bound_graphics_shaders(s_combined_shaders);
        if (!shaders || shaders->empty()) {
            g_stats.no_program++;
            return false;
        }
        for (const ShaderInfo* s : *shaders) {
            if (s->stage == ShaderStage::Vertex) vs = s;
            if (s->stage == ShaderStage::Fragment) fs = s;
        }
        if (!vs) {
            g_stats.no_vertex++;
            return false;
        }
        if (hidden_touch) {
            gather_textures(*shaders);
            return false;
        }
        layout_ptr = &program_layout(*shaders);
        s_cached_pv = cur_pv;
        s_cached_pf = cur_pf;
        s_cached_shaders = shaders;
        s_cached_vs = vs;
        s_cached_fs = fs;
        s_cached_layout = layout_ptr;
    }
    if (hidden_touch) {
        gather_textures(*shaders);
        return false;
    }
    if (__builtin_expect(debug_shader_overrides_active(), 0)) {
        if (!debug_shader_overrides(shaders, fs, s_swapped_shaders)) return false;
    }
    const ProgramLayout& layout = (shaders == s_cached_shaders) ? *layout_ptr : program_layout(*shaders);

    // NX_SNAP_FS_BEFORE=<hash>[@<first draw>]: snapshot colour target 0 before draws with that fragment shader
    // (what a feedback pass like a tonemap reads), 8 at most.
    if (__builtin_expect(s_snap_fs_before.active, 0)) {
        bool match = (fs && s_snap_fs_before.str.compare(0, 16, fs->hash) == 0) ||
                     s_snap_fs_before.str.compare(0, 16, vs->hash) == 0;
        if (match && g_stats.total >= s_snap_fs_before.from && s_snap_fs_before_taken < 8 &&
            t.color_count && t.colors[0]) {
            if (GpuImage* img = image_of(t.colors[0])) {
                s_snap_fs_before_taken++;
                draw_end_rendering(cb, 6);
                debug_snapshot(cb, *img, 0xB0000000ull + g_stats.total);
            }
        }
    }
    // Textures first: an upload or a layout change cannot happen inside the render pass.
    uint32_t gathered = gather_textures(*shaders);
    if (__builtin_expect(s_dump_draw_textures_env != nullptr, 0)) {   // NX_DUMP_DRAW_TEXTURES=<fs hash>@<draw>: the images the first such draw samples, snapshotted (frames/00000_c00000NN)
        if (!s_dump_draw_textures_said && fs) {
            s_dump_draw_textures_said = true;
            log("nvn: draw-textures watch %s, first fs %s total %llu", s_dump_draw_textures_env, fs->hash.c_str(),
                (unsigned long long)g_stats.total);
        }
        if (s_dump_draw_textures_done < 6 && fs &&
            std::strncmp(s_dump_draw_textures_env, fs->hash.c_str(), 16) == 0 &&
            s_dump_draw_textures_env[16] == '@' &&
            g_stats.total >= std::strtoull(s_dump_draw_textures_env + 17, nullptr, 10)) {
            const uint64_t tag = uint64_t(s_dump_draw_textures_done++) << 8;
            draw_end_rendering(cb, 6);
            for (size_t k = 0; k < g_textures.resolved.size(); k++) {
                const ResolvedTexture& r = g_textures.resolved[k];
                GpuImage* img = r.texture ? image_of(r.texture) : nullptr;
                log("draw textures %zu: texture %llx image %p %ux%u fmt %d levels %u layers %u uploaded %d rendered %d", k,
                    (unsigned long long)r.texture, (void*)img, img ? img->width : 0, img ? img->height : 0,
                    img ? int(img->format) : 0, img ? img->levels : 0, img ? img->layers : 0, img ? int(img->uploaded) : 0,
                    img ? int(img->rendered) : 0);
                if (img)
                    for (uint32_t l = 0; l < (img->layers > 1 ? std::min(img->layers, 12u) : 1u); l++) {
                        g_snap_layer = l, g_snap_level = 0;
                        debug_snapshot(cb, *img, 0xC0000000ull + tag * 4096 + k * 16 + l);
                    }
                g_snap_layer = 0;
            }
        }
    }
    // Feedback lock & pass-continuity optimization:
    // When depth_test, depth_write, and stencil_test are all off:
    // 1. If t.depth != 0 and either dimg is sampled by this draw OR an open color-only pass
    //    is already active on the same color targets (with no pending clears on dimg),
    //    detach t.depth so we don't break the open color-only render pass.
    // 2. Conversely, if t.depth == 0 while an open render pass on the same color targets
    //    already has g_pass.depth != 0 (and g_pass.depth is not sampled by this draw and has
    //    no pending clears), inherit g_pass.depth so the open color+depth pass continues
    //    without a tile flush/reload on Mali TBDR.
    Targets detached;
    const Targets* tp = &t;
    const DepthStencilInfo& d = g_state.depth;
    if (!d.depth_test && !d.depth_write && !d.stencil_test) {
        if (t.depth != 0) {
            GpuImage* dimg = image_of(t.depth);
            bool sampled = false;
            for (const ResolvedTexture& r : g_textures.resolved) {
                if (r.texture && (r.gpu_image ? r.gpu_image : image_of(r.texture)) == dimg) {
                    sampled = true;
                    break;
                }
            }
            bool keep_color_only_pass = g_rendering && g_pass.depth == 0 &&
                                        dimg && !clears_pending_image(dimg);
            if (sampled || keep_color_only_pass) {
                detached = t;
                detached.depth = 0;
                tp = &detached;
            }
        } else if (g_rendering && g_pass.depth != 0 && t.color_count == g_pass.color_count) {
            GpuImage* pimg = image_of(g_pass.depth);
            if (pimg && !clears_pending_image(pimg)) {
                bool sampled = false;
                for (const ResolvedTexture& r : g_textures.resolved) {
                    if (r.texture && (r.gpu_image ? r.gpu_image : image_of(r.texture)) == pimg) {
                        sampled = true;
                        break;
                    }
                }
                if (!sampled) {
                    detached = t;
                    detached.depth = g_pass.depth;
                    detached.views.level[8] = g_pass.views.level[8];
                    detached.views.layer[8] = g_pass.views.layer[8];
                    detached.views.format[8] = g_pass.views.format[8];
                    if (targets_same(g_pass, detached)) {
                        tp = &detached;
                    }
                }
            }
        }
    }
    const Targets& tt = *tp;
    flush_compute_barrier(cb);
    vertex_read_barrier(cb, vs);  // G4

    uint64_t cur_c0 = t.color_count ? t.colors[0] : 0;
    if (cur_c0 != s_cached_target_c0 || t.depth != s_cached_target_depth || t.color_count != s_cached_target_color_count) {
        s_cached_target_c0 = cur_c0;
        s_cached_target_depth = t.depth;
        s_cached_target_color_count = t.color_count;
        s_cached_formats_valid = false;

        s_cached_uw_wide = false;
        if (uw_on()) {
            GpuImage* wi = t.color_count && t.colors[0] ? image_of(t.colors[0]) : (t.depth ? image_of(t.depth) : nullptr);
            s_cached_uw_wide = wi && wi->uw;
        }
        GpuImage* hi = t.color_count && t.colors[0] ? image_of(t.colors[0]) : (t.depth ? image_of(t.depth) : nullptr);
        s_cached_hud_target = hi && hi->hud;
        s_cached_shadow_target = depth_only(t) && t.depth && image_of(t.depth) && image_of(t.depth)->shadow;
        s_cached_low_3d_target = hi && hi->low_3d;
    }
    g_uw_wide = s_cached_uw_wide;
    g_hud_target = s_cached_hud_target;
    g_shadow_target = s_cached_shadow_target;
    g_low_3d_target = s_cached_low_3d_target;
    bool need_break = gathered & kNeedBreak;
    if (gathered & kSampledPending) clears_flush_reason(11), clears_flush(cb);  // cleared (and stored) before it is sampled
    bool targets_changed = !g_rendering || (s_targets_memcmp ? std::memcmp(&g_pass, &tt, sizeof tt) != 0 : !targets_same(g_pass, tt));
    if (g_rendering && (need_break || targets_changed)) {
        (targets_changed ? g_stats.breaks_targets : g_stats.breaks_textures)++;
        // NX_CLEARS_V2: pending clears wait for the pass that takes them, or for one on their images (begin_rendering).
        if (clears_keep_pending() && !need_break)
            pass_end_only(cb, 1);
        else
            draw_end_rendering(cb, 1);
    }

    build_sets(cb, *shaders, layout);
    // Vertex and index data before the pass begins: a range a dispatch of this submit wrote may need a GPU copy
    // (nvn_written.cpp), which cannot be recorded inside a pass.
    DrawInputs in;
    if (!draw_inputs_shared(cb, vs, indexed, c, in)) {
        g_stats.no_indices++;
        return false;
    }
    check_vertex_ranges(vs, fs, indexed, c, in);  // NX_CHECK_VERTS (nvn_written.cpp)
    if (!g_rendering) {
        if (gpu_profile_on()) {
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u L%u fs %s", img ? img->width : 0, img ? img->height : 0,
                          t.views.level[0], fs ? fs->hash.c_str() : "-");
            gpu_profile_mark(cb, label);
        }
        begin_rendering(cb, tt);
    }

    VkPipeline pipeline = VK_NULL_HANDLE;

    // 4-entry intra-pass MRU bypass: consecutive or alternating draws (e.g. character + outline, UI + text)
    // sharing shaders, topology, state version, layout, and render pass reuse the VkPipeline directly without
    // zeroing and populating a 704-byte PipelineKey.
    struct PipeMru {
        const ShaderInfo* vs = nullptr;
        const ShaderInfo* fs = nullptr;
        uint32_t mode = 0;
        uint32_t state_version = 0;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        bool indexed = false;
        bool textures_empty = false;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    static PipeMru s_pipe_mru[4];
    static VkCommandBuffer s_pipe_mru_cb = VK_NULL_HANDLE;
    if (targets_changed || s_pipe_mru_cb != cb) {
        for (auto& e : s_pipe_mru) e.pipeline = VK_NULL_HANDLE;
        s_pipe_mru_cb = cb;
    }

    const bool tex_empty = g_textures.bindings.empty();
    if (!uw_on() && !live_flags()) {
        for (int i = 0; i < 4; i++) {
            const PipeMru& e = s_pipe_mru[i];
            if (e.pipeline != VK_NULL_HANDLE &&
                e.vs == vs &&
                e.fs == fs &&
                e.mode == mode &&
                e.indexed == indexed &&
                e.layout == layout.layout &&
                e.state_version == replay::g_pipeline_state_version &&
                e.textures_empty == tex_empty) {
                pipeline = e.pipeline;
                if (i > 0) {
                    PipeMru hit = e;
                    for (int j = i; j > 0; j--) s_pipe_mru[j] = s_pipe_mru[j - 1];
                    s_pipe_mru[0] = hit;
                }
                break;
            }
        }
    }
    if (pipeline == VK_NULL_HANDLE) {
        // Pipeline.
        PipelineKey key;
        std::memset(&key, 0, sizeof key);  // the key is compared byte for byte
        key.vs = vs, key.fs = fs;
        key.topology = mode;
        key.attrib_count = uint32_t(std::min<size_t>(g_state.attribs.size(), 16));
        std::copy_n(g_state.attribs.begin(), key.attrib_count, key.attribs);
        key.stream_count = uint32_t(std::min<size_t>(g_state.streams.size(), 16));
        std::copy_n(g_state.streams.begin(), key.stream_count, key.streams);
        std::copy_n(g_state.blend, 8, key.blend);
        key.color = g_state.color, key.mask = g_state.mask, key.depth = g_state.depth, key.polygon = g_state.polygon;
        // NX_HUD_KEEP_ALPHA=1 (P5R by default, games/p5r/game.cpp): draws into the HUD/menu targets (RGBA8, 1280 wide or
        // more, depth tested) with plain alpha blending leave the target's alpha alone. P5R builds alpha on purpose with
        // its own states (alpha-only masks, MIN/MAX/REVERSE_SUBTRACT; the panels' drop shadows), sets it to 0 under the
        // art, and a later pass darkens every pixel whose alpha is above 0 and below 1. Its art, drawn with SRC_ALPHA /
        // ONE_MINUS_SRC_ALPHA on all four channels, left the BC3 alphas (246..254 inside the shapes) and the faces'
        // translucent layers there: dark specks on the battle portraits. Kept alpha matches the clean picture
        // (live flag ui_rgb, PROGRESO 70). Only textured draws (the art): the untextured shapes with the same blending
        // build alpha that a later DST_ALPHA fill reads, as in the voice-language notice, whose black panel came out
        // grey when they kept it too (PROGRESO 71). NX_HUD_KEEP_ALPHA=all: untextured ones too; 0: as the game asks.
        if (s_keep_hud_alpha && key.color.blend_enable[0] && (key.mask.mask[0] & 0xF) == 0xF && live_ui_draw(t) &&
            (s_keep_hud_alpha == 2 || !g_textures.bindings.empty())) {
            const BlendTarget& b = key.blend[0];
            if (b.src_rgb == 5 && b.dst_rgb == 6 && b.src_a == 5 && b.dst_a == 6 && b.eq_rgb <= 1 && b.eq_a <= 1)
                key.mask.mask[0] &= 7;
        }
        if (const uint32_t live = live_flags(); live && live_ui_draw(t)) {
            if (live & kLiveUiAlways) key.depth.depth_func = 8;  // NVN ALWAYS
            if (live & kLiveUiNoWrite) key.depth.depth_write = 0;
            BlendTarget& b = key.blend[0];
            if (key.color.blend_enable[0] && b.src_rgb == 5 && b.dst_rgb == 6 && b.src_a == 5 && b.dst_a == 6) {
                if (live & kLiveUiRgb) key.mask.mask[0] &= 7;
                if (live & kLiveUiAmax) b.eq_a = 5;  // NVN MAX
                if (live & kLiveUiAone) b.src_a = 2;  // NVN ONE
            }
        }
        // SetPrimitiveRestart: Vulkan restarts strips and fans at the index type's largest value (NVN's index here is
        // 0xFFFF, the same for 16-bit indices), and point, line and triangle lists where the device has
        // VK_EXT_primitive_topology_list_restart (the Adreno 610 has it); NVN's 0xFFFF in 32-bit indices would need a
        // rewrite (counted by restart_stats). NX_PRIM_RESTART=0: off, as before.
        const bool strip = mode == 3 || mode == 5 || mode == 6, list = mode == 0 || mode == 1 || mode == 4;
        key.restart = s_prim_restart_on && indexed && g_state.restart && (strip || (list && vk_list_restart())) ? 1 : 0;
        if (indexed && g_state.restart) restart_stats(mode, c);
        frame_draws(vs, indexed ? uint64_t(c.i[0]) : uint64_t(c.i[1]));
        key.color_count = t.color_count, key.depth_clamp = g_state.depth_clamp;
        key.alpha_to_coverage = g_state.alpha_to_coverage;
        if (s_cached_formats_valid && std::memcmp(&s_cached_format_targets, &tt, sizeof tt) == 0) {
            std::copy_n(s_cached_color_formats, t.color_count, key.color_formats);
            key.ufloat_colors = s_cached_ufloat_colors;
            key.depth_format = s_cached_depth_format;
        } else {
            uint32_t uf = 0;
            for (uint32_t k = 0; k < t.color_count; k++) {
                GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
                key.color_formats[k] = img ? view_format(*img, t.views.format[k]) : unused_slot_format();
                if (img && (img->nvn_format == 0x3f || t.views.format[k] == 0x3f)) uf |= 1u << k;  // G4
            }
            key.ufloat_colors = uf;
            GpuImage* dimg = tt.depth ? image_of(tt.depth) : nullptr;
            key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
            std::copy_n(key.color_formats, t.color_count, s_cached_color_formats);
            s_cached_ufloat_colors = uf;
            s_cached_depth_format = key.depth_format;
            s_cached_format_targets = tt;
            s_cached_formats_valid = true;
        }
        // NX_ULTRAWIDE. The world is widened by the camera aspect patch (nvn_ultrawide.cpp), so only the 16:9 UI is
        // squeezed by 1/k (centred safe area). The UI is drawn into small BGRA8 layer targets (430x180 for the HUD, never
        // widened) and composited into the scene target by a quad: the vertex shaders seen drawing into such layer targets
        // are the "UI" ones, and a UI quad into a widened target that does not sample a widened texture is squeezed.
        // NX_UW_CAMERA=0 (no camera patch): every geometry draw is squeezed, a world-and-UI shrink.
        if (uw_on() && s_uw_cam_patch && !g_uw_wide && t.color_count && t.colors[0]) {
            const Texture* lt = get<Texture>(t.colors[0]);
            if (lt && lt->format == 0x25 && (lt->flags & 8)) uw_ui_vertex_shader(vs);
        }
        // NX_UW_UI=learn: only the vertex shaders learned above; default: every draw without depth test (screen-space UI).
        const bool ui_pick = !s_uw_cam_patch || (s_uw_ui_learn ? uw_is_ui_vertex_shader(vs) : !g_state.depth.depth_test);
        if (g_uw_wide && uw_vs_scaled(vs) && ui_pick) {
            // A short draw (a triangle or a quad) that samples a widened render target is a screen-space pass (post
            // chain, composite): it already covers the widened target.
            bool samples_wide = false;
            const uint64_t count = indexed ? uint64_t(c.i[0]) : uint64_t(c.i[1]);
            if (count <= 6)
                for (const ResolvedTexture& r : g_textures.resolved)
                    if (r.texture)
                        if (GpuImage* si = image_of(r.texture); si && si->uw) samples_wide = true;
            if (!samples_wide) key.uw_x = 1.0f / uw_factor();
        }
        if (uw_on() && __builtin_expect(s_uw_trace.active, 0)) {  // NX_UW_TRACE=<first draw>:<count>: draws (wide = a widened target)
            if (g_stats.total >= s_uw_trace.from && g_stats.total < s_uw_trace.from + s_uw_trace.count) {
                GpuImage* wi = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
                const Texture* tx = t.color_count && t.colors[0] ? get<Texture>(t.colors[0]) : nullptr;
                std::string smp;
                for (const ResolvedTexture& r : g_textures.resolved) {
                    GpuImage* si = r.texture ? image_of(r.texture) : nullptr;
                    char bb[48];
                    const Texture* st = r.texture ? get<Texture>(r.texture) : nullptr;
                    std::snprintf(bb, sizeof bb, " %ux%u%s(f%x,fmt%x,%llx)", si ? si->width : 0, si ? si->height : 0, si && si->uw ? "w" : "", st ? st->flags : 0, st ? st->format : 0, (unsigned long long)r.texture);
                    smp += bb;
                }
                log("uwsamples:%s", smp.c_str());
                log("uwdraw %llu vs %s fs %s %s count %d tgt %ux%u fl %x fmt %x wide %d scaled %d dt %d dw %d bl %d vp %d %d %d %d", (unsigned long long)g_stats.total,
                    vs->hash.c_str(), fs ? fs->hash.c_str() : "-", indexed ? "idx" : "arr", indexed ? c.i[0] : c.i[1],
                    wi ? wi->width : 0, wi ? wi->height : 0, tx ? tx->flags : 0, wi ? wi->nvn_format : 0, g_uw_wide ? 1 : 0, key.uw_x > 0 ? 1 : 0,
                    g_state.depth.depth_test, g_state.depth.depth_write, g_state.color.blend_enable[0],
                    g_state.viewport[0], g_state.viewport[1], g_state.viewport[2], g_state.viewport[3]);
            }
        }
        VkPipeline found = VK_NULL_HANDLE;
        for (size_t i = 0; i < s_mru_count; i++) {
            uint8_t idx = s_mru_order[i];
            const MruEntry& entry = s_mru_cache[idx];
            if (entry.layout == layout.layout &&
                entry.key.fs == key.fs &&
                entry.key.vs == key.vs &&
                entry.key.topology == key.topology &&
                std::memcmp(&entry.key, &key, sizeof(PipelineKey)) == 0) {
                found = entry.pipeline;
                if (i > 0) {
                    uint8_t hit = s_mru_order[i];
                    std::memmove(&s_mru_order[1], &s_mru_order[0], i);
                    s_mru_order[0] = hit;
                }
                break;
            }
        }
        if (found != VK_NULL_HANDLE) {
            pipeline = found;
        } else {
            pipeline = graphics_pipeline(key, layout);
            if (pipeline != VK_NULL_HANDLE) {
                uint8_t insert_idx;
                if (s_mru_count < kMruSize) {
                    insert_idx = uint8_t(s_mru_count++);
                } else {
                    insert_idx = s_mru_order[kMruSize - 1];
                }
                s_mru_cache[insert_idx] = {key, layout.layout, pipeline};
                size_t shift_len = std::min(s_mru_count - 1, kMruSize - 1);
                if (shift_len > 0) {
                    std::memmove(&s_mru_order[1], &s_mru_order[0], shift_len);
                }
                s_mru_order[0] = insert_idx;
            }
        }
        if (pipeline != VK_NULL_HANDLE && !uw_on() && !live_flags()) {
            for (int j = 3; j > 0; j--) s_pipe_mru[j] = s_pipe_mru[j - 1];
            s_pipe_mru[0] = {vs, fs, mode, replay::g_pipeline_state_version, layout.layout, indexed, tex_empty, pipeline};
        }
    }
    if (!pipeline) {
        g_stats.no_pipeline++;
        return false;
    }

    if (g_bound.cb != cb || draw_slow(kSlowState)) g_bound = Bound{}, g_bound.cb = cb;
    if (g_bound.pipeline != pipeline) {
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_bound.pipeline = pipeline, g_state_set++;
        g_last_compute_pipeline = VK_NULL_HANDLE;
        if (gpu_profile_on()) gpu_profile_count(1);
    } else {
        g_state_kept++;
    }
    bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout);

    // The vertex buffers resolved above, plus the zero stream (binding 15), batched in contiguous runs.
    bind_vertex_streams(cb, in.used, in.vertex);

    VkViewport vp{0, 0, float(g_pass_extent.width), float(g_pass_extent.height), 0, 1};
    if (g_state.has_viewport)
        vp = {float(g_state.viewport[0]), float(g_state.viewport[1]), float(g_state.viewport[2]),
              float(g_state.viewport[3]), g_state.depth_near, g_state.depth_far};
    // NVN's upper-left window origin (what Wonder sets) maps NDC +Y to the top of the render target, like D3D;
    // Vulkan maps it to the bottom. A negative-height viewport gives NVN's mapping, and with it NVN's front faces.
    // (Before this, the front face was inverted instead: triangles were culled right, but every render target was
    // upside down, which the post chain undid in some scenes and not in others: the first course showed a
    // mirrored sky.)
    VkRect2D sc{{0, 0}, g_pass_extent};
    if (g_state.has_scissor)
        sc = {{std::max(0, g_state.scissor[0]), std::max(0, g_state.scissor[1])},
              {uint32_t(std::max(0, g_state.scissor[2])), uint32_t(std::max(0, g_state.scissor[3]))}};
    if (g_uw_wide) {  // NX_ULTRAWIDE: the guest's 16:9 pixel spans, widened
        if (g_state.has_viewport) {
            int32_t x = int32_t(vp.x), w = int32_t(vp.width);
            uw_span(x, w, g_pass_extent.width);
            vp.x = float(x), vp.width = float(w);
        }
        if (g_state.has_scissor) {
            int32_t x = sc.offset.x, w = int32_t(sc.extent.width);
            uw_span(x, w, g_pass_extent.width);
            sc.offset.x = x, sc.extent.width = uint32_t(w);
        }
    }
    if (g_hud_target) {  // NX_HUD_1080: the guest's 1280x720 pixels, 3/2 (or 5/4) each
        const float k = hud_scale();
        if (g_state.has_viewport) vp.x *= k, vp.y *= k, vp.width *= k, vp.height *= k;
        if (g_state.has_scissor) {
            const int32_t x0 = hud_up(sc.offset.x), y0 = hud_up(sc.offset.y);
            const int64_t x1 = std::min<int64_t>(hud_up_end(int32_t(sc.offset.x + int64_t(sc.extent.width))), g_pass_extent.width);
            const int64_t y1 = std::min<int64_t>(hud_up_end(int32_t(sc.offset.y + int64_t(sc.extent.height))), g_pass_extent.height);
            sc = {{x0, y0}, {uint32_t(std::max<int64_t>(0, x1 - x0)), uint32_t(std::max<int64_t>(0, y1 - y0))}};
        }
    }
    if (g_shadow_target) {  // NX_MALI_LOW_SHADOWS: 0.5x resolution shadow depth target
        if (g_state.has_viewport) vp.x *= 0.5f, vp.y *= 0.5f, vp.width *= 0.5f, vp.height *= 0.5f;
        if (g_state.has_scissor) {
            sc.offset.x /= 2, sc.offset.y /= 2;
            sc.extent.width = std::max<uint32_t>(1u, std::min<uint32_t>(sc.extent.width / 2, g_pass_extent.width));
            sc.extent.height = std::max<uint32_t>(1u, std::min<uint32_t>(sc.extent.height / 2, g_pass_extent.height));
        }
    }
    if (g_low_3d_target) {  // NX_LOW_3D: 0.75x resolution 3D render target (720x405)
        if (g_state.has_viewport) vp.x *= 0.75f, vp.y *= 0.75f, vp.width *= 0.75f, vp.height *= 0.75f;
        if (g_state.has_scissor) {
            sc.offset.x = (sc.offset.x * 3) / 4, sc.offset.y = (sc.offset.y * 3) / 4;
            sc.extent.width = std::max<uint32_t>(1u, std::min<uint32_t>((sc.extent.width * 3) / 4, g_pass_extent.width));
            sc.extent.height = std::max<uint32_t>(1u, std::min<uint32_t>((sc.extent.height * 3) / 4, g_pass_extent.height));
        }
    }
    // NX_EXP_3D (an experiment, the picture breaks): on the 3D draws (depth test, a 960-wide pass) "scissor" keeps
    // one pixel (what is left is the vertex and per-draw cost), "tri" draws one triangle (per-draw cost alone),
    // "small" draws into the top-left 3/4 x 3/4 (the GPU work of a 720x405 3D picture).
    const bool exp_draw = s_exp3d && g_state.depth.depth_test && g_pass_extent.width == 960;
    if (exp_draw && s_exp3d == 1) sc = {{0, 0}, {1, 1}};
    if (exp_draw && s_exp3d == 3) {
        vp.x *= 0.75f, vp.y *= 0.75f, vp.width *= 0.75f, vp.height *= 0.75f;
        sc = {{sc.offset.x * 3 / 4, sc.offset.y * 3 / 4}, {sc.extent.width * 3 / 4, sc.extent.height * 3 / 4}};
    }
    vp.y += vp.height, vp.height = -vp.height;
    set_dynamic(cb, vp, sc);

    trace_skin(indexed, c, vs, fs);
    if (indexed) {
        uint32_t count = uint32_t(c.i[0]);
        if (exp_draw && s_exp3d == 2) count = std::min(count, 3u);
        if (g_bound.has_index && g_bound.index.buffer == in.index.buffer && g_bound.index.offset == in.index.offset &&
            g_bound.index_type == in.index_type) {
            g_state_kept++;
        } else {
            p_vkCmdBindIndexBuffer(cb, in.index.buffer, in.index.offset, in.index_type);
            g_bound.has_index = true, g_bound.index = in.index, g_bound.index_type = in.index_type, g_state_set++;
        }
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        const int32_t oq = occlusion_query_begin(cb);  // P5R's occlusion culling (nvn_occlusion.cpp)
        if (!occlusion_draw(cb, true, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2])))
            p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
        occlusion_query_end(cb, oq);
        if (q) zero_draws_end(cb);
    } else {
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        const int32_t oq = occlusion_query_begin(cb);
        const uint32_t vcount = exp_draw && s_exp3d == 2 ? std::min(uint32_t(c.i[1]), 3u) : uint32_t(c.i[1]);
        if (!occlusion_draw(cb, false, vcount, uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), 0, uint32_t(c.i[2])))
            p_vkCmdDraw(cb, vcount, uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
        occlusion_query_end(cb, oq);
        if (q) zero_draws_end(cb);
    }
    g_stats.done++;
    if (g_pass_draws++ == 0 && (exp_render_pass() & 4)) pass_end_only(cb, 9);  // NX_EXP_RP split: one more store and load
    if (gpu_profile_on())
        gpu_profile_count(0, uint64_t(uint32_t(indexed ? c.i[0] : c.i[1])) * uint64_t(std::max(1, c.i[3])));
    g_pass_written |= ((1u << t.color_count) - 1) | (g_state.depth.depth_write || g_state.depth.stencil_test ? 1u << 8 : 0);
    for (uint32_t k = 0; k < t.color_count && k < 8; k++) note_linear_target(t.colors[k]);
    // NX_SNAP_FS=<hash>,<hash>[@<first draw>]: also snapshot after draws with those fragment shaders (8 at most, or
    // NX_SNAP_FS_MAX).
    bool snap_by_fs = s_snap_fs.active && g_stats.done >= s_snap_fs.from && s_snap_fs_count < s_snap_fs.max &&
                      ((fs && s_snap_fs.str.find(fs->hash) != std::string::npos) ||
                       s_snap_fs.str.find(vs->hash) != std::string::npos) &&
                      ++s_snap_fs_count;  // the vertex shader's hash matches too
    if ((snap_by_fs || snap_follow(t, vs, fs, g_textures.handles) || debug_snap_wanted(g_stats.done)) && t.color_count && t.colors[0])
        if (GpuImage* snap = image_of(t.colors[0])) draw_end_rendering(cb, 6), debug_snapshot(cb, *snap, g_stats.done);
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    if (s_dump_target && t.colors[0] == s_dump_target) g_dump_request = ++g_dump_draws;
    // NX_DUMP_ON_FS=<fs hash>[,...]: a draw with one of these fragment shaders dumps its target 0 and what it samples.
    if (s_dump_fs.active && fs && g_frame >= s_dump_fs.from && s_dump_fs.str.find(fs->hash) != std::string::npos && g_dump_draws < 400 &&
        t.colors[0] && image_of(t.colors[0]) && (s_dump_fs.any || image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                                           image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        debug_dump_add(t.colors[0]);
        for (const ResolvedTexture& r : g_textures.resolved) debug_dump_add(r.texture);
        g_dump_request = ++g_dump_draws;
    }
    trace_draw(t, mode, indexed, c, vs, fs, g_textures.bindings, g_textures.handles);
    return true;
}

}  // namespace

// A game that knows nobody reads its linear targets for a while pauses their copies (P5R: during films, whose
// screen hides the 3D its culling serves; ~1.5 ms of the render thread per frame on the Adreno 610).
void linear_write_back_pause(bool paused) { g_linear_paused.store(paused, std::memory_order_relaxed); }

void linear_targets_write_back(VkCommandBuffer cb) {  // before the submit ends, outside any pass
    if (g_linear_targets.empty()) return;
    static uint64_t copies = 0, bytes = 0, next_log = 0;
    struct Copy {
        GpuImage* img;
        const Texture* tx;
        uint32_t texel;
    };
    static thread_local std::vector<Copy> todo;
    todo.clear();
    for (uint64_t texture : g_linear_targets) {
        const Texture* tx = get<Texture>(texture);
        GpuImage* img = tx ? image_of(texture) : nullptr;
        if (!img || !img->image || img->alpha_from_r || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ||
            img->layout == VK_IMAGE_LAYOUT_UNDEFINED || img->width != tx->width || img->height != tx->height ||
            tx->target != 1)
            continue;
        uint32_t bw = 1, bh = 1, texel = 0;
        format_block(tx->format, bw, bh, texel);
        if (bw != 1 || bh != 1 || !texel || tx->stride % texel || tx->stride < tx->width * texel) continue;
        todo.push_back({img, tx, texel});
    }
    g_linear_targets.clear();
    if (todo.empty()) return;
    // One barrier into the copies and one out of them. Stack buffers avoid per-submit heap allocations.
    constexpr size_t kStackBarriers = 16;
    VkImageMemoryBarrier to_stack[kStackBarriers];
    VkImageMemoryBarrier back_stack[kStackBarriers];
    std::vector<VkImageMemoryBarrier> to_heap, back_heap;
    VkImageMemoryBarrier* to = to_stack;
    VkImageMemoryBarrier* back = back_stack;
    if (todo.size() > kStackBarriers) {
        to_heap.resize(todo.size());
        back_heap.resize(todo.size());
        to = to_heap.data();
        back = back_heap.data();
    }
    for (size_t k = 0; k < todo.size(); k++) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = todo[k].img->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = todo[k].img->layout, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to[k] = b;
        b.srcAccessMask = 0, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.newLayout = todo[k].img->layout;
        back[k] = b;
    }
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(todo.size()), to);
    for (const Copy& c : todo) {
        const uint64_t size = uint64_t(c.tx->stride) * (c.tx->height - 1) + uint64_t(c.tx->width) * c.texel;
        Upload up = upload_alloc(size, 16);
        VkBufferImageCopy r{};
        r.bufferOffset = up.offset;
        r.bufferRowLength = c.tx->stride / c.texel;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {c.tx->width, c.tx->height, 1};
        p_vkCmdCopyImageToBuffer(cb, c.img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, up.buffer, 1, &r);
        g_late_write_backs.push_back({up.ptr, reinterpret_cast<uint8_t*>(c.tx->memory), size, nullptr});
        copies++, bytes += size;
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};  // the copies, for the host once the fence signals
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                           0, 1, &mb, 0, nullptr, uint32_t(todo.size()), back);
    if (g_frame >= next_log) {
        next_log = g_frame + 600;
        log("nvn: linear render targets written back: %llu copies, %.2f MB so far", (unsigned long long)copies,
            double(bytes) / 1e6);
    }
}

void draw_end_submit() {  // after the submit's fence: compute results to guest memory, debug snapshots
    debug_snapshots_write();
    for (const WriteBack& w : draw_take_late_write_backs()) apply_write_back(w);  // synchronous submits only
    for (const WriteBack& w : draw_take_write_backs()) {
        trace_nan_write_back(w.from, w.to, w.size);
        apply_write_back(w);
    }
}

// The linear targets' copies (linear_targets_write_back): unlike compute results, which the next submits may read and
// are waited for, the CPU reads them frames later (P5R's culling), so they land when the submit retires on its own
// and the pipeline never waits for them: waited for, they cost the 3D title 11.5 -> 9.8 fps on the Adreno 610.
std::vector<WriteBack> draw_take_late_write_backs() {
    std::vector<WriteBack> v;
    v.swap(g_late_write_backs);
    return v;
}

std::vector<WriteBack> draw_take_write_backs() {  // an asynchronous submit copies them when it is retired
    std::vector<WriteBack> v = written_take();  // one per written range (disjoint), then NX_COMPUTE_SEPARATE's
    v.insert(v.end(), separate_write_backs().begin(), separate_write_backs().end());
    separate_write_backs().clear();
    return v;
}

void draw_begin_submit() {
    init_draw_env_once();
    g_bound_serial = 0;
    g_rendering = false;
    g_pass_cb = VK_NULL_HANDLE;
    g_linear_targets.clear(), g_late_write_backs.clear();
    g_vertex_pending = false, g_vertex_visible_at = g_pass_serial;  // each command buffer begins with a full barrier
    g_bound = Bound{};  // a new command buffer
    g_last_compute_pipeline = VK_NULL_HANDLE;
    g_compute_barrier_pending = false;
    submit_lists_reset();
    sets_begin_submit();
    clears_begin_submit();
    g_submit_ns = plat::monotonic_ns();
    dedup_reset();
    cache_begin_submit();
    pipeline_begin_submit();
}

uint64_t recorded_draws();

void draw_log_stats() {
    if (g_vertex_barriers) log("nvn passes: %llu vertex-read barriers (G4)", (unsigned long long)g_vertex_barriers);
    dedup_log();
    cache_log();
    render_pass_log();
    written_log();
    sampled_ready_log();
    sets_log();
    if (g_state_set + g_state_kept)
        log("nvn: draw state: %llu binds and sets recorded, %llu already in place", (unsigned long long)g_state_set,
            (unsigned long long)g_state_kept);
    g_state_set = g_state_kept = 0;
    clears_log();
    occlusion_log();
    log("nvn passes ended: %llu submit end, %llu draw, %llu dispatch, %llu clear of other targets, %llu clear larger than "
        "the pass, %llu copy, %llu snapshot, %llu SetRenderTargets", (unsigned long long)g_pass_ends[0],
        (unsigned long long)g_pass_ends[1], (unsigned long long)g_pass_ends[2], (unsigned long long)g_pass_ends[3],
        (unsigned long long)g_pass_ends[4], (unsigned long long)g_pass_ends[5], (unsigned long long)g_pass_ends[6],
        (unsigned long long)g_pass_ends[7]);
    Stats& t = g_stats;
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices",
        (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices);
    log("nvn passes: %llu ended by a draw's texture uploads or layout changes, %llu by new targets, %llu dispatches",
        (unsigned long long)t.breaks_textures, (unsigned long long)t.breaks_targets, (unsigned long long)t.dispatches);
}

void pass_end_only(VkCommandBuffer cb, int reason) {
    if (reason != 2 /* Compute */) flush_compute_barrier(cb);
    g_last_compute_pipeline = VK_NULL_HANDLE;
    if (!g_rendering) return;
    g_pass_ends[reason & 7]++, trace_pass("end: reason", g_pass, uint32_t(reason), 0);
    if (g_render_pass)
        p_vkCmdEndRenderPass(cb);
    else
        p_vkCmdEndRendering(cb);
    g_rendering = false;
    g_bound = Bound{};  // each render pass instance requires pipelines and dynamic state to be bound
    // What the pass wrote is read by what follows (sampled, copied, loaded by the next pass), and what it sampled may
    // be rendered over next. The images stay GENERAL, so transition() records no barrier. Metal (MoltenVK) and the
    // Intel driver ordered passes anyway; NVIDIA overlaps them, and later passes read half-written tiles: the course
    // HUD in blocks (DEBUG-2 #71). NX_NO_PASS_BARRIER=1: none, as before (bisecting).
    // Narrow (PHASE4.md G4; NX_PASS_BARRIER_NARROW=1): the later stages only, so the next pass's vertex work and
    // binning may start while this one finishes. A draw whose vertex shader samples a rendered image gets a barrier
    // of its own first (vertex_read_barrier). By default ALL_COMMANDS, as before.
    static const bool no_barrier = std::getenv("NX_NO_PASS_BARRIER") != nullptr;
    if (!no_barrier) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkPipelineStageFlags dst = narrow_pass_barrier()
                                       ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
        g_vertex_pending |= narrow_pass_barrier();
    }
    g_pass_serial++;
    for (uint32_t k = 0; k < 9; k++)  // this pass's targets only (the other slots may name freed images)
        if ((k < g_pass.color_count || (k == 8 && g_pass.depth)) && g_pass_images[k])
            g_pass_images[k]->rendered_at = g_pass_serial;
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
}
void draw_end_rendering(VkCommandBuffer cb, int reason) {  // and the pending clears (nvn_clears.cpp)
    pass_end_only(cb, reason);
    clears_flush_reason(reason >= 0 && reason < 10 ? reason : 14);
    clears_flush(cb);
}
bool pass_open() { return g_rendering; }
// A layout change (and the clear or upload after it) about to be recorded while a pass is open in the same command
// buffer: Vulkan allows neither inside a render pass instance (VUID-vkCmdPipelineBarrier-None-07889,
// VUID-vkCmdClearColorImage-renderpass). Textures made or first filled while the draw's descriptor sets are built
// (sampled_ready said ready, ensure_contents then cleared it; a new dummy) did it on P5R's title, and the PowerVR
// driver crashed at the pass's vkCmdEndRenderPass. The pass ends first; the draw begins a new one.
void pass_end_for_transfer(VkCommandBuffer cb) {
    if (!g_rendering || cb != g_pass_cb) return;
    static uint64_t n = 0;
    if (n++ < 8) log("nvn/vk: a layout change inside a pass (%llu so far): the pass ends first", (unsigned long long)n);
    draw_end_rendering(cb, 5);
}
bool pass_open_on(const Targets& t) { return g_rendering && targets_same(g_pass, t); }
VkExtent2D pass_extent() { return g_pass_extent; }
void pass_begin(VkCommandBuffer cb, const Targets& t) { begin_rendering(cb, t); }
// After a clear inside the pass (nvn_clears.cpp). A masked one is a fullscreen draw with its own pipeline, viewport
// and scissor (nvn_mask_clear.cpp): the next draw sets everything again.
void pass_mark_written(uint32_t bits) { g_pass_written |= bits, g_bound = Bound{}, sets_invalidate_bound(); }

void replay::flush_compute_barrier(VkCommandBuffer cb) {
    if (!g_compute_barrier_pending || cb == VK_NULL_HANDLE) return;
    g_compute_barrier_pending = false;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    const VkPipelineStageFlags dst_stages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                           VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                           VK_PIPELINE_STAGE_TRANSFER_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst_stages, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
}

bool replay::pass_wrote(const GpuImage* img) {
    if (!g_rendering || !img) return false;
    uint32_t hit = g_pass.depth && g_pass_images[8] == img ? 1u << 8 : 0;
    for (uint32_t c = 0; c < g_pass.color_count; c++)
        if (g_pass.colors[c] && g_pass_images[c] == img) hit |= 1u << c;
    return (hit & g_pass_written) != 0;
}

uint64_t submit_start_ns() { return g_submit_ns; }

// Entry points of the draws and dispatches for draw_command (nvn_draw_state.cpp).
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    return draw(cb, t, mode, indexed, c);
}
void dispatch_call(VkCommandBuffer cb, const Cmd& c) { dispatch(cb, c); }

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
