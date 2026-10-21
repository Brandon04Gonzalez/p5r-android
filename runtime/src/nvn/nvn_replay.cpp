// The replay of recorded NVN command lists into a Vulkan command buffer (split from nvn_vk.cpp): render targets,
// clears, calls into other lists, copies; draws and state go to nvn_draw.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "nvn_draw_state.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {
// The lists called during one submit, by handle (PHASE4.md C5): command_list() takes a mutex and walks a map, and a
// course frame calls hundreds of lists, each looked up twice (the dispatch count, then the replay). The first lookup
// of a handle in a submit is kept for the rest of it: a list re-recorded meanwhile on the game's recording thread
// is not mixed into a submit already being replayed. NX_CALL_TABLE=0: every call looks up.
std::unordered_map<uint64_t, std::shared_ptr<CmdList>> g_called;
}  // namespace

void submit_lists_reset() { g_called.clear(); }

std::shared_ptr<CmdList> submit_command_list(uint64_t handle) {
    static const bool off = [] {
        const char* e = std::getenv("NX_CALL_TABLE");
        return e && e[0] == '0';
    }();
    if (off) return command_list(handle);
    auto it = g_called.find(handle);
    if (it != g_called.end()) return it->second;
    auto l = command_list(handle);
    if (l) g_called.emplace(handle, l);
    return l;
}

void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth) {
    if (skin_lists_on()) skin_list_replayed(list, depth);
    for (const Cmd& c : list.cmds) {
        cb = frame_chunk(cb, replay::g_stats.done, pass_open());  // part of the submit queued early (PLAN.md R1)
        // NX_TRACE_CMDS=<first draw>: the ops replayed from that draw on (20000), with n, mask and a[0].
        static const char* tc = std::getenv("NX_TRACE_CMDS");
        static const uint64_t tc_from = tc ? std::strtoull(tc, nullptr, 10) : 0;
        static int tc_n = 0;
        if (tc && replay::g_stats.done >= tc_from && tc_n++ < 20000)
            log("cmd after draw %llu: op %u n %u mask %x a0 %llx a1 %llx i %d %d %d %d f %g %g %g %g depth %d",
                (unsigned long long)replay::g_stats.done, unsigned(c.op), c.n, c.mask, (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], c.i[0], c.i[1], c.i[2], c.i[3], c.f[0], c.f[1], c.f[2], c.f[3], depth);
        switch (c.op) {
            case Cmd::SetRenderTargets: {  // the pass ends only when a draw or clear needs other targets
                static const bool always = std::getenv("NX_TARGETS_BREAK") != nullptr;  // bisecting: the old way
                if (always) draw_end_rendering(cb, 7);
                st.color_count = c.n;
                for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
                st.depth = c.a[8];
                st.views = {};
                if (c.blob < list.blobs.size() && list.blobs[c.blob].size() == sizeof(TargetViews))
                    std::memcpy(&st.views, list.blobs[c.blob].data(), sizeof st.views);
                break;
            }
            case Cmd::ClearColor:
            case Cmd::ClearDepthStencil: draw_clear(cb, st, c); break;
            case Cmd::CallList:
                if (depth < 8) {
                    if (auto l = submit_command_list(c.a[0])) {
                        replay_list(cb, st, *l, depth + 1);
                        cb = frame_cb(cb);  // NX_PIPELINED: the called list may have split the submit
                    } else {
                        static uint64_t missing = 0;
                        if (++missing <= 20 || missing % 1000 == 0)
                            log("nvn: called handle %llx is unknown (%llu calls of unknown handles): %s",
                                (unsigned long long)c.a[0], (unsigned long long)missing,
                                explain_handle(c.a[0]).c_str());
                    }
                }
                break;
            case Cmd::CopyTextureToTexture:
                draw_end_rendering(cb, 5);
                if (gpu_profile_on()) gpu_profile_mark(cb, "texture copies");
                replay_copy(cb, c, list);
                if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
                break;
            case Cmd::CopyBufferToTexture:
            case Cmd::ClearTexture:
                draw_end_rendering(cb, 5);
                if (c.op == Cmd::CopyBufferToTexture)
                    replay_buffer_copy(cb, c, list);
                else
                    replay_clear_texture(cb, c, list);
                break;
            default:
                draw_command(cb, c, list, st);
                // NX_PIPELINED: after the submit's last dispatch the rest goes into a second command buffer.
                if (c.op == Cmd::DispatchCompute) cb = frame_after_dispatch(cb);
                break;
        }
    }
}

}  // namespace nx::nvn
