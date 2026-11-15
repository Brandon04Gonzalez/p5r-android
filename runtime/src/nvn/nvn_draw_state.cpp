// The replay's state commands (split from nvn_draw.cpp): binds, viewports, uniform updates, counters; draws and
// dispatches go back to nvn_draw.cpp.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn {

using namespace replay;

namespace replay {
uint32_t g_pipeline_state_version = 1;
uint32_t g_attrib_state_version = 1;
uint32_t g_tex_state_version = 1;
}

void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets) {
    State& s = g_state;
    auto blob = [&](size_t size) -> const uint8_t* {
        return list.blob_ptr(c.blob, size);
    };
    switch (c.op) {
        case Cmd::SetViewport:
            if (!s.has_viewport || std::memcmp(s.viewport, c.i, sizeof s.viewport) != 0) {
                std::memcpy(s.viewport, c.i, sizeof s.viewport);
                s.has_viewport = true;
            }
            break;
        case Cmd::SetRasterizerDiscard: s.rasterizer_discard = c.n != 0; break;
        case Cmd::SetPrimitiveRestart: {
            bool rst = c.n != 0;
            uint32_t idx = uint32_t(c.a[0]);
            if (s.restart != rst || s.restart_index != idx) {
                s.restart = rst, s.restart_index = idx;
                g_pipeline_state_version++;
            }
            break;
        }
        case Cmd::BindImage:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) {
                if (s.images[c.i[0]][c.i[1]] != c.a[0]) {
                    s.images[c.i[0]][c.i[1]] = c.a[0];
                    g_tex_state_version++;
                }
            }
            break;
        case Cmd::DispatchCompute:
            dispatch_call(cb, c);
            break;
        case Cmd::SetRenderEnable:
            s.render_enable = c.n != 0;
            s.cond_on = false;
            occlusion_render_enable();
            break;
        case Cmd::SetScissor:
            if (!s.has_scissor || std::memcmp(s.scissor, c.i, sizeof s.scissor) != 0) {
                std::memcpy(s.scissor, c.i, sizeof s.scissor);
                s.has_scissor = true;
            }
            break;
        case Cmd::SetDepthRange:
            s.depth_near = c.f[0], s.depth_far = c.f[1];
            break;
        case Cmd::BindProgram: {
            s.program = c.a[0];
            const uint32_t bits = c.mask ? c.mask : (1u << kStages) - 1;  // NVN_SHADER_STAGE_*_BIT, 1 << stage
            for (int k = 0; k < kStages; k++)
                if (bits & (1u << k)) s.stage_programs[k] = c.a[0];
            break;
        }
        case Cmd::BindUniformBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 18) {
                s.uniforms[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            }
            break;
        case Cmd::BindStorageBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) {
                s.storage[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            }
            break;
        case Cmd::BindDeferred: {
            const DeferredValues* dv = current_deferred();
            auto it = dv ? dv->find(c.a[0]) : DeferredValues::const_iterator{};
            uint32_t words = c.mask >= 2 ? 2 : 1;
            int st = c.i[0];
            if (st < 0 || st >= kStages) break;
            for (uint32_t k = 0; k < c.n; k++) {
                auto value = [&](uint32_t w) {  // the submit's snapshot, else the memory now
                    size_t at = size_t(k) * words + w;
                    return dv && it != dv->end() && at < it->second.size() ? it->second[at]
                                                                            : ld<uint64_t>(c.a[0] + 8 * at);
                };
                int slot = c.i[1] + int(k);
                if (c.mask == 0 && slot >= 0 && slot < 32) {
                    uint64_t v0 = value(0);
                    if (s.textures[st][slot] != v0) s.textures[st][slot] = v0, g_tex_state_version++;
                }
                if (c.mask == 1 && slot >= 0 && slot < 16) {
                    uint64_t v0 = value(0);
                    if (s.images[st][slot] != v0) s.images[st][slot] = v0, g_tex_state_version++;
                }
                if (c.mask == 2 && slot >= 0 && slot < 18) s.uniforms[st][slot] = {value(0), value(1)};
                if (c.mask == 3 && slot >= 0 && slot < 16) s.storage[st][slot] = {value(0), value(1)};
            }
            break;
        }
        case Cmd::BindTexture:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 32) {
                if (s.textures[c.i[0]][c.i[1]] != c.a[0]) {
                    s.textures[c.i[0]][c.i[1]] = c.a[0];
                    g_tex_state_version++;
                }
            }
            break;
        case Cmd::BindVertexBuffer:
            if (c.i[0] >= 0 && c.i[0] < 16) {
                s.vertex_buffers[c.i[0]] = {c.a[0], c.a[1]};
            }
            break;
        case Cmd::SetTexturePool:
            if (s.texture_pool != c.a[0]) s.texture_pool = c.a[0], g_tex_state_version++;
            break;
        case Cmd::SetSamplerPool:
            if (s.sampler_pool != c.a[0]) s.sampler_pool = c.a[0], g_tex_state_version++;
            break;
        case Cmd::BindVertexAttribs:
            if (auto* p = blob(c.n * sizeof(VertexAttrib))) {
                s.attribs.assign(reinterpret_cast<const VertexAttrib*>(p), reinterpret_cast<const VertexAttrib*>(p) + c.n);
                g_attrib_state_version++;
                g_pipeline_state_version++;
            }
            break;
        case Cmd::BindVertexStreams:
            if (auto* p = blob(c.n * sizeof(VertexStream))) {
                s.streams.assign(reinterpret_cast<const VertexStream*>(p), reinterpret_cast<const VertexStream*>(p) + c.n);
                g_pipeline_state_version++;
            }
            break;
        case Cmd::BindBlendState:
            if (auto* p = blob(sizeof(BlendTarget))) {
                BlendTarget b;
                std::memcpy(&b, p, sizeof b);
                if (b.target < 8 && std::memcmp(&s.blend[b.target], &b, sizeof b) != 0) {
                    s.blend[b.target] = b;
                    g_pipeline_state_version++;
                }
            }
            break;
        case Cmd::BindColorState:
            if (auto* p = blob(sizeof(ColorStateInfo))) {
                if (std::memcmp(&s.color, p, sizeof s.color) != 0) {
                    std::memcpy(&s.color, p, sizeof s.color);
                    g_pipeline_state_version++;
                }
            }
            break;
        case Cmd::BindChannelMaskState:
            if (auto* p = blob(sizeof(ChannelMaskInfo))) {
                if (std::memcmp(&s.mask, p, sizeof s.mask) != 0) {
                    std::memcpy(&s.mask, p, sizeof s.mask);
                    g_pipeline_state_version++;
                }
            }
            break;
        case Cmd::BindDepthStencilState:
            if (auto* p = blob(sizeof(DepthStencilInfo))) {
                if (std::memcmp(&s.depth, p, sizeof s.depth) != 0) {
                    std::memcpy(&s.depth, p, sizeof s.depth);
                    g_pipeline_state_version++;
                }
            }
            break;
        case Cmd::BindPolygonState:
            if (auto* p = blob(sizeof(PolygonInfo))) {
                if (std::memcmp(&s.polygon, p, sizeof s.polygon) != 0) {
                    std::memcpy(&s.polygon, p, sizeof s.polygon);
                    g_pipeline_state_version++;
                }
            }
            break;
        case Cmd::BindMultisampleState: {
            bool a2c = c.n != 0;
            if (s.alpha_to_coverage != a2c) {
                s.alpha_to_coverage = a2c;
                g_pipeline_state_version++;
            }
            break;
        }
        case Cmd::SetBlendColor: std::memcpy(s.blend_color, c.f, sizeof s.blend_color); break;
        case Cmd::UpdateUniformBuffer: {  // (buffer address, size, offset, data): written in order, like the GPU
            uint64_t off = c.a[2];
            // NX_TRACE_UUB=<first draw>: logs 40 updates from that draw on.
            static const char* uub = std::getenv("NX_TRACE_UUB");
            static const uint64_t uub_from = uub ? std::strtoull(uub, nullptr, 10) : 0;
            static int uub_n = 0;
            if (uub && g_stats.done >= uub_from && uub_n++ < 1500 && c.blob < list.blobs.size()) {
                const uint8_t* b_ptr = list.blob_ptr(c.blob);
                const size_t b_sz = list.blob_size(c.blob);
                float f[8] = {};
                if (b_ptr) std::memcpy(f, b_ptr, std::min<size_t>(b_sz, sizeof f));
                log("uub before draw %llu: buffer %llx size %llu offset %llu data %zu bytes: %g %g %g %g | %g %g %g %g",
                    (unsigned long long)g_stats.done, (unsigned long long)c.a[0], (unsigned long long)c.a[1],
                    (unsigned long long)off, b_sz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
            if (c.blob < list.blobs.size())
                if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0] + off))) {
                    const uint8_t* b_ptr = list.blob_ptr(c.blob);
                    const size_t b_sz = list.blob_size(c.blob);
                    if (b_ptr && b_sz) {
                        std::memcpy(p, b_ptr, b_sz);
                        dedup_invalidate(p, b_sz);
                        written_cpu_write(p, b_sz);
                    }
                }
            break;
        }
        case Cmd::ResetCounter: occlusion_reset(cb, c, targets); break;
        case Cmd::RenderConditional: occlusion_conditional(cb, c, targets); break;
        case Cmd::ReportCounter: {  // {value, timestamp}: the work before it is done when the replay gets here
            occlusion_report(cb, c, targets);
            static uint64_t reports = 0, types_seen = 0;
            if (c.n < 64 && !(types_seen >> c.n & 1)) types_seen |= 1ull << c.n, log("nvn: first counter report of type %u", c.n);
            if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0]))) {
                // NX_FAKE_GPU_TIME=1: every report of a submit carries the submit's start (a GPU with no cost):
                // the game picks its present interval from the timings it measures (bisecting its 30 fps mode).
                static const bool fake = std::getenv("NX_FAKE_GPU_TIME") != nullptr;
                const uint64_t ts = (fake || c.n == 1) ? submit_start_ns() : plat::monotonic_ns();
                uint64_t v[2] = {++reports, ts};
                std::memcpy(p, v, sizeof v);
                if (c.n != 1) {
                    dedup_invalidate(p, sizeof v);
                    written_cpu_write(p, sizeof v);
                }
            }
            break;
        }
        case Cmd::DrawArrays: draw_call(cb, targets, c.n, false, c); break;
        case Cmd::DrawElements: draw_call(cb, targets, c.n, true, c); break;
        default: raster_command(c); break;
    }
}

}  // namespace nx::nvn
