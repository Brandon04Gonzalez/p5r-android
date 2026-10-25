// The draw replay's bound state (nvn_draw.cpp), shared with its debug traces (nvn_draw_trace.cpp).
#pragma once

#include <array>
#include <utility>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn::replay {

constexpr int kStages = 6;  // NVN shader stages: 0 vertex, 1 fragment, 2 geometry, 3/4 tessellation, 5 compute

struct Range {
    uint64_t address = 0, size = 0;
};

struct State {
    uint64_t program = 0;  // the last program bound
    // BindProgram(cmd, program, stageBits) binds a program to the stages of its mask only: Wonder binds one program
    // for all stages, Persona 5 Royal one program per stage (a vertex-only and a fragment-only program).
    uint64_t stage_programs[kStages] = {};
    Range uniforms[kStages][18], storage[kStages][16], vertex_buffers[16];
    uint64_t textures[kStages][32] = {};  // NVN texture handles by sampler binding
    uint64_t images[kStages][16] = {};    // NVN image handles by image binding (BindImage)
    uint64_t texture_pool = 0, sampler_pool = 0;
    std::vector<VertexAttrib> attribs;
    std::vector<VertexStream> streams;
    BlendTarget blend[8];
    ColorStateInfo color;
    ChannelMaskInfo mask;
    DepthStencilInfo depth;
    PolygonInfo polygon;
    float blend_color[4] = {};
    int32_t viewport[4] = {}, scissor[4] = {};
    bool rasterizer_discard = false, render_enable = true;  // SetRasterizerDiscard / SetRenderEnable: draws skipped
    bool restart = false;  // SetPrimitiveRestart (P5R: on, index 0xFFFF)
    uint32_t restart_index = 0xFFFF;
    // The conditional render around the draws (SetRenderEnableConditional until SetRenderEnable), for the draw trace.
    bool cond_on = false;
    uint32_t cond_mode = 0;
    uint64_t cond_addr = 0;
    bool has_viewport = false, has_scissor = false;
    float depth_near = 0, depth_far = 1;
    // Command-buffer rasterizer state (nvn_raster.cpp), per face 0 front, 1 back. NVN's defaults: masks 0xFF, ref 0.
    uint8_t stencil_ref[2] = {0, 0}, stencil_compare_mask[2] = {0xFF, 0xFF}, stencil_write_mask[2] = {0xFF, 0xFF};
    float bias_factor = 0, bias_units = 0, bias_clamp = 0;  // SetPolygonOffsetClamp
    bool depth_bounds = false, depth_clamp = false;
    bool alpha_to_coverage = false;  // BindMultisampleState (G7)
    float depth_bounds_range[2] = {0, 1};
};

struct Stats {
    uint64_t total = 0, no_program = 0, no_vertex = 0, no_pipeline = 0, no_indices = 0, done = 0, to_window = 0;
    uint64_t dispatches = 0;
    uint64_t breaks_textures = 0, breaks_targets = 0;  // render passes ended by a draw: uploads/layouts, new targets
};

extern State g_state;
extern Stats g_stats;
extern bool g_hud_target;  // NX_HUD_1080: this draw's target is a 1920x1080 image of a 1280x720 texture (nvn_vk.h)

const uint8_t* guest_bytes(uint64_t gpu);
uint32_t handle_texture(uint64_t handle, uint32_t* sampler);
// The texture and sampler a handle names in the bound pools.
void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view = nullptr);
std::string explain_texture_slot(uint64_t handle);  // NX_FRAME_SIG: the slot at the mark and now (nvn_sets.cpp)

// NX_SNAP_FOLLOW (nvn_draw_trace.cpp): log and snapshot this draw.
bool snap_follow(const Targets& t, const ShaderInfo* vs, const ShaderInfo* fs, const std::vector<uint64_t>& handles);
// NX_TRACE_NAN=1 (nvn_draw_trace.cpp): non-finite or huge floats in what compute shaders wrote, with the shader.
void trace_nan_dispatch(const ShaderInfo* cs);
void trace_nan_write_back(const uint8_t* from, const uint8_t* to, uint64_t size);
// NX_TRACE_PASSES=<first draw> (nvn_draw_trace.cpp): 4000 pass events from that draw on (begin with its load ops,
// end with its reason, clears deferred or done in the pass), each with its colour target 0 and depth.
void trace_pass(const char* what, const Targets& t, uint32_t a, uint32_t b);
using TexBindings = std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>>;
// NX_TRACE_DRAWS & co (docs/nvn/DEBUG.md): logs a replayed draw with its state, when the switches select it.
void trace_draw(const Targets& t, uint32_t mode, bool indexed, const Cmd& c, const ShaderInfo* vs,
                const ShaderInfo* fs, const TexBindings& tex_bindings, const std::vector<uint64_t>& handles);

}  // namespace nx::nvn::replay

namespace nx::nvn {
uint64_t submit_start_ns();  // host ns at the start of the submit being replayed
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c);
void dispatch_call(VkCommandBuffer cb, const Cmd& c);
}  // namespace nx::nvn

namespace nx::nvn::replay {

// nvn_dedup.cpp: one copy per submit of the same guest bytes (padded to `padded` with zeros).
Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align);
// NX_UPLOAD_STATS=1: what the copies are (set by the callers around them) and how much of each is the same as the
// last copy of that address and size (hashed: costs time).
enum UploadKind : uint8_t { kUploadOther, kUploadVertex, kUploadIndex, kUploadBlock, kUploadKinds };
extern UploadKind g_upload_kind;  // also: only guest buffers (not kUploadOther) go through the buffer cache
// nvn_buffer_cache.cpp: copies of guest buffers kept across submits while their bytes stay the same.
bool cache_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out);
void cache_begin_submit();  // frees the copies retired submits read
void cache_log();
void dedup_reset();                                     // at each submit
void dedup_invalidate(const uint8_t* p, uint64_t size);  // guest bytes written during the replay
void dedup_log();
// nvn_written.cpp: compute results within a submit (docs/nvn/GLITCH-PLAN.md G5), one copy per written guest range.
bool written_separate();  // NX_COMPUTE_SEPARATE=1: a copy per binding, as before
// A dispatch's storage binding it writes: the range's copy (made, reused or merged), kAlign aligned.
Upload written_bind(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded);
// Bytes read by later work: the written copy when [p, p + size) meets one (may end the render pass), else false.
bool written_read(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out);
Upload read_upload(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align);  // or dedup
bool written_overlaps(const uint8_t* p, uint64_t size);
// A draw's storage binding over a written region (it may write the copy): counted as written_bind calls are, so a
// pipelined split (nvn_frames.cpp) does not copy the region back before that draw ran.
void written_count_bind();
bool written_indirect(VkCommandBuffer cb, const uint8_t* p);  // vkCmdDispatchIndirect when a dispatch wrote {x, y, z}
// A draw's vertex buffers (the streams `vs` reads) and index buffer; false: its indices are not in guest memory.
struct DrawInputs {
    Upload vertex[15], index;
    uint32_t used = 0;  // bit s: stream s
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
};
bool draw_inputs(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in);
// NX_CHECK_VERTS=1: logs draws that fetch vertices or instances past their bound vertex buffer range (debugging).
void check_vertex_ranges(const ShaderInfo* vs, const ShaderInfo* fs, bool indexed, const Cmd& c, const DrawInputs& in);

// nvn_sets.cpp: the descriptor sets of a draw or dispatch, and the replay's CPU shortcuts (docs/nvn/PERF-PLAN.md).
// NX_DRAW_SLOW=1 turns every shortcut off, for bisecting; letters instead of 1 pick them: m the scratch memory kept
// from draw to draw, e empty descriptor sets not allocated, h the submit's handle cache, s state the command buffer
// already has not set again (nvn_draw.cpp), u uploads shared (indices, zero stream, support buffer, c[0]).
enum : uint32_t { kSlowScratch = 1, kSlowEmptySets = 2, kSlowHandles = 4, kSlowState = 8, kSlowUploads = 16 };
bool draw_slow(uint32_t what);
// What a texture binding of the draw being replayed samples.
struct ResolvedTexture {
    uint64_t texture = 0;  // guest NVNtexture*; 0: nothing behind the binding (the dummy)
    SamplerInfo sampler;
    ViewInfo view;
    VkSampler vk_sampler = VK_NULL_HANDLE;  // sampler_for(sampler, shadow) when the handle cache has it
};
struct DrawTextures {
    TexBindings bindings;  // the set 2 bindings of the program's stages
    std::vector<uint64_t> handles;
    std::vector<ResolvedTexture> resolved;
};
extern DrawTextures g_textures;  // filled by gather_textures, read by build_sets and the traces of the same draw
enum : uint32_t { kNeedBreak = 1, kSampledPending = 2 };
uint32_t gather_textures(const std::vector<const ShaderInfo*>& shaders);
void build_sets(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout);
// vkUpdateDescriptorSets for what build_sets made, then the sets bound at `point`.
void bind_sets(VkCommandBuffer cb, VkPipelineBindPoint point, const ProgramLayout& layout);
Upload zero_stream();  // 64 zero bytes for vertex binding 15
// draw_inputs, with 16- and 32-bit indices through dedup_upload.
bool draw_inputs_shared(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in);
std::vector<WriteBack>& separate_write_backs();  // NX_COMPUTE_SEPARATE=1: a write-back per binding
void sets_begin_submit();
void sets_log();
// nvn_draw.cpp: `img` is a target of the open pass that a draw or clear of the pass wrote.
bool pass_wrote(const GpuImage* img);
void written_cpu_write(const uint8_t* p, uint64_t size);       // replay-time guest writes (logged when they meet one)
std::vector<WriteBack> written_take();                         // one write-back per range, at the end of the submit
void written_reset();
void written_log();
// c[0], the NVN driver constants of a stage (nvn_dedup.cpp).
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st);

bool debug_shader_overrides(const std::vector<const ShaderInfo*>*& shaders, const ShaderInfo*& fs,
                            std::vector<const ShaderInfo*>& swapped);
// nvn_raster.cpp: the stencil, polygon offset, depth bounds and depth clamp commands into g_state (false for other
// commands), and their dynamic state for the next draw (stencil reference and masks, depth bias).
bool raster_command(const Cmd& c);
void raster_dynamic(VkCommandBuffer cb, bool slack);  // slack: equal_slack_pass (nvn.h)

void trace_compute_once(const ShaderInfo* cs, uint32_t x, uint32_t y, uint32_t z);

// NX_TRACE_SKIN: skinned draws' inputs, vertex uniform blocks and the bone rows they select.
void trace_skin(bool indexed, const Cmd& c, const ShaderInfo* vs, const ShaderInfo* fs);

}  // namespace nx::nvn::replay
