// The Vulkan side of NVN, shared between nvn_vk.cpp (device, images, submit, present) and nvn_draw.cpp (pipelines,
// resources, draws). Everything here runs under the GPU lock taken by gpu_submit / gpu_present.
#pragma once

#include <string>
#include <vector>

#include "nvn.h"
#include "vk_gen.h"

namespace nx::nvn {

struct GpuImage {
    // NX_TEX_AUDIT (nvn_pipeline.cpp tex_audit): a hash of the guest memory its last upload read, and when it was
    // checked. On the image, so textures that share it (same memory) are compared with what it holds.
    uint64_t audit_hash = 0, audit_frame = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // every level and layer
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, levels = 1, layers = 1, depth = 1;  // depth: 3D images only
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool rendered = false;  // written by the GPU: never re-uploaded from guest memory
    bool uploaded = false;  // filled from the texture's pool memory
    // Given texels by nvnTextureWriteTexels: those never reach guest memory here, so it is not uploaded from there
    // again (a pool flush over it wiped the light probes' direction map, docs/nvn/DEBUG.md #49).
    bool texels = false;
    VkComponentMapping swizzle{};  // for sampling (attachment views stay identity)
    bool storage = false;          // created with VK_IMAGE_USAGE_STORAGE_BIT (compute shaders write it)
    bool mutable_format = true;
    uint64_t rendered_at = 0;      // the pass-end serial of the last pass that rendered into it (nvn_draw.cpp, G4)    // views may use another format (MUTABLE_FORMAT; nvn_image.cpp, ubwc_flags)
    // An R8 texture always read as (1, 1, 1, R) (a glyph cache): stored as RGBA8 (255, 255, 255, R) and sampled
    // without swizzle. MoltenVK returned alpha 0 for that view swizzle on a 2D-array view, so all text was invisible.
    bool alpha_from_r = false;
    uint32_t nvn_format = 0;  // the texture's NVN format
    bool uw = false;          // NX_ULTRAWIDE: a widened render target (nvn_ultrawide.cpp)
    bool hud = false;         // NX_HUD_1080: a 1280x720 render target made at 1920x1080 or 1600x900 (hud_1080_texture)
};

// NX_HUD_1080 (nvn_image.cpp): render targets of the handheld screen's size, 1280x720, are made at 3/2 of it,
// 1920x1080, so the 2D layers drawn into them (a game's HUD, menus and the window) reach the screen at the size their
// art was made for while the 3D keeps its own targets. NX_HUD_RES=900: at 5/4 instead, 1600x900 (a third fewer pixels
// for the GPU, art shrunk 1.2x instead of 1.5x). Guest pixel spans on such an image (viewports, scissors, copy and
// clear rectangles, the window crop) are scaled the same way: offsets rounded down, ends up.
struct Texture;
bool hud_1080_texture(const Texture& t);
int32_t hud_quarters();  // the scale in quarters: 6 (1080p) or 5 (900p)
inline int32_t hud_up(int32_t v) { return int32_t((int64_t(v) * hud_quarters()) >> 2); }
inline int32_t hud_up_end(int32_t v) { return int32_t((int64_t(v) * hud_quarters() + 3) >> 2); }
inline float hud_scale() { return float(hud_quarters()) / 4.0f; }

VkDevice vk_device();
bool vk_depth_clip_control();
bool vk_indirect_count();  // VK_KHR_draw_indirect_count enabled (nvn_occlusion.cpp)
bool vk_list_restart();  // VK_EXT_primitive_topology_list_restart enabled (nvn_draw.cpp)
bool vk_dynamic_rendering();  // core 1.3 dynamic rendering; without it, render pass objects (nvn_renderpass.cpp)
// The core features the device was created with (all the GPU has): depthClamp, depthBiasClamp, depthBounds, ...
const VkPhysicalDeviceFeatures& vk_features();
bool gpu_require_subgroup32();  // compute pipelines pin the subgroup size to 32 (nvn_pipeline.cpp)
bool gpu_is_integrated();  // the device in use is an integrated GPU (set when the device is made)
VkPhysicalDevice vk_physical();
// nvn_gpu_pick.cpp: logs every device and returns the one NX_GPU asks for (default: the first discrete GPU).
VkPhysicalDevice pick_physical_device(const std::vector<VkPhysicalDevice>& devices);
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);

// The image behind an NVN texture (guest NVNtexture*), made on first use.
GpuImage* image_of(uint64_t texture);
// NX_1D_IMAGES=1 (bisecting, nvn_pipeline.cpp): NVN 1D and 1D-array textures as Vulkan 1D images, the old way. By
// default they are 2D / 2D-array images of height 1, which the translated shaders (1D sampled as 2D) can view (G8).
bool vk_1d_images();
void transition(VkCommandBuffer cb, GpuImage& img, VkImageLayout to);
// nvn_present.cpp: the window texture drawn into the swapchain (rotated, fit, alpha 1) instead of blitted.
bool present_pass_wanted();
bool present_pass_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent,
                          VkSurfaceTransformFlagBitsKHR transform);
bool present_pass_record(VkCommandBuffer cb, GpuImage& src, const int32_t* crop, uint32_t index);
void present_pass_release();  // its views of the swapchain images, before the swapchain is destroyed
bool present_pass_failed();   // the pass could not be made: frames are blitted, upright
GpuImage* present_pass_test_target();  // NX_PRESENT_TEST: an offscreen "display" for the pass (headless dumps)
// nvn_bc.cpp: BC textures decoded on the CPU where the GPU has no textureCompressionBC (NX_FORCE_BC_DECODE=1 anyway).
bool bc_host_decode(uint32_t nvn_format);         // a BC format this device cannot sample (BC6H excepted)
VkFormat bc_decoded_format(uint32_t nvn_format);  // what it is stored as instead
uint32_t bc_decoded_bytes(uint32_t nvn_format);   // bytes per texel of that
void bc_decode(uint32_t nvn_format, const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices,
               uint8_t* out);  // tightly packed blocks -> tightly packed texels
bool bc_view_format(const GpuImage& img, uint32_t nvn, VkFormat& out);  // a view of a decoded image

// Per-submit upload memory (host visible, coherent), reset at every submit: data the replayed commands read from
// guest memory is copied here at replay time (vertex, index, uniform, storage data, texture staging).
struct Upload {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* ptr = nullptr;
};
// What it is bound as, and GPU copies within it and dispatch arguments read from it (nvn_written.cpp).
constexpr VkBufferUsageFlags kUploadUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
Upload upload_alloc(uint64_t size, uint64_t align);
// nvn_frames.cpp: a buffer of its own for one big texture upload, freed when the submit being recorded retires.
Upload staging_alloc(uint64_t size);
// nvn_pipeline_cache.cpp: the on-disk pipeline cache, and the cost of what the replay creates (logged per window).
VkPipelineCache pipeline_cache();
void pipeline_cache_save();
void note_pipeline_created(uint64_t ns, bool worker);  // worker: compiled off the replay thread
void note_pipeline_alias();               // a new raw key that maps to a pipeline already built (canonical_key)
void note_module_created(uint64_t ns);    // a shader module built (SPIR-V patches + vkCreateShaderModule)
void note_pipeline_waited(uint64_t ns);   // the replay waited this long for a compile in flight
void note_pipeline_skipped();             // a draw skipped because its pipeline compiles (NX_ASYNC_PIPELINES)
void note_pipeline_prewarmed();           // a pipeline of an earlier session sent to a worker
void note_astc_decoded(uint64_t ns);
void creation_log(uint64_t frames);
Upload upload(const void* data, uint64_t size, uint64_t align);

// Descriptor sets for this submit (freed when it completes).
VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout);
VkDescriptorPool make_descriptor_pool();
bool descriptor_layout_counts(VkDescriptorSetLayout layout, uint32_t (&by_type)[16]);  // descriptors of a layout, by VkDescriptorType (nvn_pipeline.cpp)

// A lost device or a submit running past 2 s: the process ends at once (docs/nvn/DEBUG.md #22).
[[noreturn]] void gpu_hung(const char* why);

// A compute shader's storage buffer, copied back to guest memory once the submit completed: (upload copy, guest
// address, size).
struct WriteBack {
    uint8_t* from;
    uint8_t* to;
    uint64_t size;
    // The guest bytes as they were when the copy was made: only what the GPU changed goes back (nvn_written.cpp).
    std::shared_ptr<std::vector<uint8_t>> before;
};
void apply_write_back(const WriteBack& w);  // nvn_written.cpp
// Deferred write-backs (nvn_frames.cpp): those of earlier submits over [p, p + size) are applied first. Every
// read of guest memory for the GPU calls it (uploads, compute bindings, texture uploads).
void frame_guest_read(const uint8_t* p, uint64_t size);
void write_back_log(uint64_t frames);       // nvn_written.cpp: their volume and time per frame
std::vector<WriteBack> draw_take_write_backs();  // nvn_draw.cpp: this submit's, for an asynchronous submit
std::vector<WriteBack> draw_take_late_write_backs();  // nvn_draw.cpp: its linear render targets', never waited for
void debug_dump_textures(uint64_t frame);  // nvn_debug.cpp: NX_DUMP_TEXTURES and the requested ones
void debug_dump_requested(uint64_t n);      // nvn_debug.cpp: NX_DUMP_ON_TARGET / NX_DUMP_ON_FS
void debug_dump_add(uint64_t texture);  // nvn_debug.cpp: read back at the end of this submit
// A file named dump_hud in the data folder (nvn_draw_trace.cpp, read about once a second and removed): the next frame's
// draws into NX_HUD_1080 targets are logged as trace_draws does, and at its present every texture they sampled is
// written as stored (block-compressed ones as their blocks) and the target as RGBA8, alpha kept, to
// <data>/frames/hud_<frame>_<texture>.bin (header: width, height, Vulkan format, bytes), to rebuild the composition
// offline (the battle portrait's specks).
void debug_hud_note(uint64_t target, const std::vector<uint64_t>& textures);
void debug_hud_dump(uint64_t frame);

// nvn_frames.cpp: submits that are not waited for (NX_PIPELINED, NX_ASYNC_GPU). Slots of command buffers, fences,
// descriptor pool and ring stretch; retired (fences waited, write-backs copied if not yet) on reuse, when the ring is
// full, or when something waits for them.
bool async_gpu();  // the slots are in use (either mode)
bool pipelined();  // NX_PIPELINED: fences not waited for, write-backs copied before gpu_submit returns
// The extensions and memory types that the Android performance plan depends on, logged once (nvn_caps.cpp).
void log_device_caps(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& extensions);
uint32_t upload_memory_type(uint32_t bits);
bool native_r11g11b10();
bool ubwc_flags();  // MUTABLE_FORMAT only with a format list (nvn_caps.cpp; Android)  // NVN 0x3f as B10G11R11_UFLOAT (nvn_caps.cpp; Android)  // the upload ring's memory type (nvn_caps.cpp)
void frame_init(VkCommandPool pool, uint32_t family, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size);
VkCommandBuffer frame_begin(bool replay);  // NX_ASYNC_GPU, replay: submits with write-backs retired first
VkSemaphore frame_acquire_semaphore();
VkSemaphore frame_rendered_semaphore(uint32_t image);
void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done,
                  std::vector<WriteBack> late_write_backs = {});  // late: applied at retire, never waited for
void frame_wait_all();
uint64_t frame_upload_used();
uint64_t frame_ring_size();  // the slots' upload ring (0: synchronous submits, no ring)
uint64_t frame_completed();  // the serial of the last retired submit (for free_retired_images)
Upload frame_upload_alloc(uint64_t size, uint64_t align);
VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout);
// Pipelined: a submit with dispatches is split after the last one; its first part is queued at once and carries the
// write-backs. frame_plan_split counts the submit's DispatchCompute commands and returns the last top-level list that
// has one (SIZE_MAX: no split). frame_split ends the first part and returns the command buffer of the rest (once per
// submit; outside the mode it returns `cb`). frame_after_dispatch is for replay_list, after each DispatchCompute: it
// splits right after the last one. frame_cb: the command buffer to go on with (after a call that may have split).
size_t frame_plan_split(const std::vector<std::shared_ptr<CmdList>>& lists);
VkCommandBuffer frame_split(VkCommandBuffer cb);
VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb);
VkCommandBuffer frame_cb(VkCommandBuffer cb);
// Chunks (nvn_frames.cpp, NX_SUBMIT_CHUNK_DRAWS): for replay_list before each command, with the draws done so far;
// returns the command buffer to go on with (a new part once enough draws were recorded).
VkCommandBuffer frame_chunk(VkCommandBuffer cb, uint64_t draws, bool pass_open);
// Host time inside vkQueueSubmit (with vkEndCommandBuffer: Metal encoding) and inside vkWaitForFences, running
// totals in every mode (the synchronous path adds its own).
struct FrameTimes {
    uint64_t encode_ns = 0, fence_ns = 0;
};
FrameTimes& frame_times();
void frame_log(uint64_t frames);  // the mode's counters, with the timing log
void free_retired_images(uint64_t completed);
// A GPU-written image of a destroyed texture with the same memory and shape (nvn_frames.cpp), or null.
std::shared_ptr<GpuImage> reuse_image(const Texture& t);  // images of textures destroyed before submit `completed` + 1

// nvn_debug.cpp: NX_DUMP_FRAMES, the presented texture read back (recorded into the present's command buffer, written
// after its fence).
uint32_t frame_dump_every();
bool frame_dump_due(uint64_t presents);  // NX_DUMP_FRAMES, or a dump_frames file in the data folder
bool frame_dump_record(VkCommandBuffer cb, GpuImage& img);
void frame_dump_write(const GpuImage& img, uint64_t frame);
bool debug_snapshots_pending();  // NX_SNAP_*: this submit recorded snapshots (it is then waited for)

// nvn_draw.cpp: replays one draw-related command; ends the render pass it may have open when asked.
struct Targets {
    uint64_t colors[8] = {};
    uint32_t color_count = 0;
    uint64_t depth = 0;
    TargetViews views;  // level and layer of each (index 8: depth)
};
// Field by field: a memcmp also compared the padding after color_count, which a copy need not keep.
inline bool targets_same(const Targets& a, const Targets& b) {
    if (a.color_count != b.color_count || a.depth != b.depth) return false;
    for (uint32_t k = 0; k < 9; k++) {
        if (k < 8 && a.colors[k] != b.colors[k]) return false;
        if (a.views.level[k] != b.views.level[k] || a.views.layer[k] != b.views.layer[k] ||
            a.views.format[k] != b.views.format[k])
            return false;
    }
    return true;
}
void draw_begin_submit();
void draw_end_submit();  // after the GPU finished the submit
void linear_targets_write_back(VkCommandBuffer cb);  // nvn_draw.cpp: pitch-linear targets drawn into, back to guest memory
void draw_log_stats();
void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets);
// reason (counted in the `nvn passes ended` log): 0 submit end, 1 draw, 2 dispatch, 3 clear of other targets,
// 4 clear larger than the pass, 5 copy, 6 snapshot, 7 SetRenderTargets (NX_TARGETS_BREAK).
void draw_end_rendering(VkCommandBuffer cb, int reason = 0);  // before transfers (clears, copies, present) and at the end
// ClearColor / ClearDepthStencil inside the render pass of the current targets (nvn_draw.cpp).
void draw_clear(VkCommandBuffer cb, const Targets& targets, const Cmd& c);  // nvn_clears.cpp
// nvn_clears.cpp: clears pending as the next pass's loadOps; nvn_draw.cpp: the open pass, for it.
bool clears_pending_image(const GpuImage* img);
bool clears_pending_other(const Targets& t);
uint32_t clears_apply(const Targets& t, VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo& depth,
                      VkRenderingAttachmentInfo& stencil);
void clears_flush(VkCommandBuffer cb);  // outside a pass: the pending clears in a pass of their own
void clears_flush_other(VkCommandBuffer cb, const Targets& t);  // before a pass on t (NX_CLEARS_V2)
bool clears_keep_pending();  // NX_CLEARS_V2
void clears_begin_submit();
void clears_log();
bool pass_open();
void clears_flush_reason(int why);  // nvn_clears.cpp: what the next clears_flush is counted as
void clears_log_mismatch(const char* what, const Targets& t);  // live_flags clears_why
bool live_clears_why();  // nvn_draw.cpp  // a render pass is being recorded
// Render pass and framebuffer objects instead of dynamic rendering (nvn_renderpass.cpp, NX_RENDER_PASS_OBJECTS).
bool render_pass_objects();
VkFormat unused_slot_format();  // a pipeline's format for a colour slot with no image
VkRenderPass compatible_render_pass(uint32_t count, const VkFormat* colors, VkFormat depth);
void begin_render_pass(VkCommandBuffer cb, uint32_t count, const VkFormat* formats,
                       const VkRenderingAttachmentInfo* colors, VkFormat depth_format,
                       const VkRenderingAttachmentInfo& depth, const VkRenderingAttachmentInfo& stencil,
                       VkExtent2D extent);
void forget_framebuffers(VkImageView view);
void render_pass_log();
void pass_end_only(VkCommandBuffer cb, int reason);  // draw_end_rendering without the pending clears
bool pass_open_on(const Targets& t);
VkExtent2D pass_extent();
void pass_begin(VkCommandBuffer cb, const Targets& t);
void pass_mark_written(uint32_t bits);
// nvn_mask_clear.cpp: colour target `slot` of the open pass on `t` cleared to `rgba` in `rect`, only the channels in
// `mask` (bit 0 r .. 3 a) written, by a fullscreen draw. False (nothing recorded) for integer targets or no pipeline.
bool mask_clear(VkCommandBuffer cb, const Targets& t, uint32_t slot, uint32_t mask, const float rgba[4],
                const VkRect2D& rect);
// nvn_replay.cpp: a command list into `cb` (`depth`: CallList nesting).
void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth);
std::shared_ptr<CmdList> submit_command_list(uint64_t handle);  // command_list(), once per handle per submit
void submit_lists_reset();  // at each submit's start (draw_begin_submit)

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list);  // nvn_copy.cpp: texture copies
void replay_buffer_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list);    // buffer -> texture
void replay_clear_texture(VkCommandBuffer cb, const Cmd& c, const CmdList& list);  // nvnCommandBufferClearTexture
void forget_image_views(const GpuImage& img);
// nvn_debug.cpp: NX_SNAP_DRAWS mid-frame snapshots of colour target 0.
bool debug_snap_wanted(uint64_t draw);
// nvn_occlusion.cpp: P5R's occlusion queries (ResetCounter / ReportCounter of samples passed) and the draws they
// make conditional (SetRenderEnableConditional).
void occlusion_begin_submit(VkCommandBuffer cb);  // the queries of the last submit reset
void occlusion_reset(VkCommandBuffer cb, const Cmd& c, const Targets& t);
void occlusion_report(VkCommandBuffer cb, const Cmd& c, const Targets& t);
void occlusion_conditional(VkCommandBuffer cb, const Cmd& c, const Targets& t);
void occlusion_render_enable();  // SetRenderEnable: the conditional draws end
// Around a draw call inside the pass: its query when it is a query pass draw (-1: none).
int32_t occlusion_query_begin(VkCommandBuffer cb);
void occlusion_query_end(VkCommandBuffer cb, int32_t q);
// A conditional draw as an indirect draw counted by its object's query; false: not conditional, draw it as it is.
bool occlusion_draw(VkCommandBuffer cb, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                    int32_t vertex_offset, uint32_t first_instance);
void occlusion_log();

// nvn_debug.cpp: NX_ZERO_DRAWS, shader pairs whose draws produce no pixel.
bool zero_draws_on();
void zero_draws_begin_submit(VkCommandBuffer cb);
bool zero_draws_begin(VkCommandBuffer cb, const std::string& label);
void zero_draws_end(VkCommandBuffer cb);
void zero_draws_collect();
void zero_draws_log();
void frame_sig_flush(uint64_t presents);  // NX_FRAME_SIG=1: the frame's draw signature, logged (nvn_draw_trace.cpp)
void debug_snapshot(VkCommandBuffer cb, GpuImage& img, uint64_t draw);
extern uint32_t g_snap_layer, g_snap_level;  // the layer and level debug_snapshot reads (default 0)
void debug_snapshots_write();  // nvn_pipeline.cpp: before an image is freed

// nvn_debug.cpp: GPU time per render pass (NX_GPU_PROFILE=1).
bool gpu_profile_on();  // NX_GPU_PROFILE=1: per-pass labels, every submit waited for (nvn_debug.cpp)
// NX_GPU_PROFILE=3: the GPU's busy time per frame without waits, from per-part timestamps (nvn_gpu_busy.cpp).
// The janitor thread (nvn_janitor.cpp, NX_JANITOR): pool resets of retired slots, off the replay thread.
bool janitor_on();
void janitor_run(std::function<void()> job);
void janitor_wait(const std::atomic<bool>& done);
bool gpu_busy_on();
void gpu_busy_part_begin(uint32_t slot, uint32_t part, VkCommandBuffer cb);
void gpu_busy_part_end(uint32_t slot, uint32_t part, VkCommandBuffer cb);
void gpu_busy_retired(uint32_t slot, uint32_t queued, bool replay);
void gpu_busy_log(uint64_t frames);
// NX_GPU_PROBE=1: fixed compute jobs timed in each present, the GPU clock and its memory bandwidth from inside
// (nvn_gpu_probe.cpp).
bool gpu_probe_on();
void gpu_probe_record(VkCommandBuffer cb, uint64_t presents);
void gpu_probe_log();
void gpu_profile_begin(VkCommandBuffer cb);
void gpu_profile_mark(VkCommandBuffer cb, const std::string& label);
// What the profile counts per label: 0 a draw (n vertices), 1 a pipeline bind, 2 a descriptor set bind.
void gpu_profile_count(int what, uint64_t n = 1);
void gpu_profile_annotate(const std::string& text);  // appended to the last label
// NX_EXP_RP or the live_flags words rp_noload / rp_nostore / rp_split (experiments): bit 1, no pass loads its targets;
// bit 2, no pass stores its depth and stencil (both break the picture); bit 4, every pass ends after its first draw
// and begins again (one more store and load of its targets, the picture unchanged: what they cost).
int exp_render_pass();
// NX_EXP_NO_WB or the live_flags word no_wb (an experiment): compute results are not copied back to guest memory and
// no submit is waited for them (nvn_frames.cpp): what that wait costs, and what breaks without the copies.
bool exp_no_write_backs();
void gpu_profile_collect();
void gpu_profile_log(uint64_t frames);

namespace replay {
void dedup_reset();  // nvn_dedup.cpp
bool written_separate();         // nvn_written.cpp (nvn_draw_state.h)
uint64_t written_bind_count();   // nvn_written.cpp: written_bind calls so far (a dispatch bound storage it writes)
void trace_nan_write_back(const uint8_t* from, const uint8_t* to, uint64_t size);  // nvn_draw_trace.cpp
}

}  // namespace nx::nvn
