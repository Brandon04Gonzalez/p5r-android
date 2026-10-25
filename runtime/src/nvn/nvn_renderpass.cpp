// Render pass and framebuffer objects instead of dynamic rendering (docs/nvn/android-perf/PHASE3.md C1). On Adreno,
// vkCmdBeginRendering makes a render pass and a framebuffer of its own on every call (an allocation, a memset, and
// their release in the command buffer's reset): ~57 us per pass, ~3.6 ms of the replay per course frame at ~64
// passes. Here both are made once and kept: a render pass per (formats, load operations), a framebuffer per (views,
// extent), made against the LOAD/STORE variant, which every variant is compatible with (compatibility ignores load and
// store operations), as are the pipelines (graphics_pipeline, the masked clear). Every attachment is stored and stays
// in GENERAL, as with dynamic rendering; there are no subpass dependencies, the replay's own barriers order passes.
// A colour slot with no image is VK_ATTACHMENT_UNUSED, so its pipeline format is UNDEFINED (unused_slot_format).
// NX_RENDER_PASS_OBJECTS=1: on (off by default until measured).
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// Formats, then the load operations of the colours, the depth and the stencil (0 for the compatible variant).
using PassKey = std::array<uint32_t, 8 + 1 + 1 + 8 + 2>;
constexpr uint32_t kNoStore = 0x100;  // in the count word: depth and stencil not stored (NX_EXP_RP nostore)
std::map<PassKey, VkRenderPass> g_passes;

struct FramebufferKey {
    VkRenderPass pass = VK_NULL_HANDLE;  // the compatible one
    VkImageView views[9] = {};
    uint32_t count = 0, width = 0, height = 0;
    bool operator<(const FramebufferKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};
std::map<FramebufferKey, VkFramebuffer> g_framebuffers;
uint64_t g_framebuffers_made = 0;

bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT;
}

VkRenderPass make_pass(const PassKey& k, uint32_t count) {
    VkAttachmentDescription att[9]{};
    VkAttachmentReference refs[8]{}, dref{};
    uint32_t n = 0;
    for (uint32_t c = 0; c < count; c++) {
        if (!k[c]) {
            refs[c] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
            continue;
        }
        VkAttachmentDescription& a = att[n];
        a.format = VkFormat(k[c]);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = k[10 + c] ? VkAttachmentLoadOp(k[10 + c] - 1) : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        refs[c] = {n++, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = count;
    sp.pColorAttachments = refs;
    if (k[8]) {
        VkAttachmentDescription& a = att[n];
        a.format = VkFormat(k[8]);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = k[18] ? VkAttachmentLoadOp(k[18] - 1) : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = k[9] & kNoStore ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        bool st = has_stencil(VkFormat(k[8]));
        a.stencilLoadOp = !st ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                              : k[19] ? VkAttachmentLoadOp(k[19] - 1) : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.stencilStoreOp = st && !(k[9] & kNoStore) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        dref = {n++, VK_IMAGE_LAYOUT_GENERAL};
        sp.pDepthStencilAttachment = &dref;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (p_vkCreateRenderPass(vk_device(), &ci, nullptr, &rp) != VK_SUCCESS) log("nvn/vk: vkCreateRenderPass failed");
    return rp;
}

VkRenderPass pass_for(uint32_t count, const VkFormat* colors, VkFormat depth, const uint32_t* loads) {
    PassKey k{};
    count = count > 8 ? 8 : count;
    for (uint32_t c = 0; c < count; c++) k[c] = uint32_t(colors[c]);
    k[8] = uint32_t(depth), k[9] = count;
    if (loads) {
        for (uint32_t c = 0; c < 10; c++) k[10 + c] = loads[c];
        if (exp_render_pass() & 2) k[9] |= kNoStore;
    }
    auto it = g_passes.find(k);
    if (it != g_passes.end()) return it->second;
    return g_passes[k] = make_pass(k, count);
}

}  // namespace

bool render_pass_objects() {
    static const bool on = [] {  // also on a device without dynamic rendering (before Vulkan 1.3; nvn_vk.cpp)
        const char* e = std::getenv("NX_RENDER_PASS_OBJECTS");
        bool v = (e && e[0] == '1') || !vk_dynamic_rendering();
        if (v) log("nvn/vk: render pass and framebuffer objects instead of dynamic rendering");
        return v;
    }();
    return on;
}

VkFormat unused_slot_format() { return render_pass_objects() ? VK_FORMAT_UNDEFINED : VK_FORMAT_R8G8B8A8_UNORM; }

VkRenderPass compatible_render_pass(uint32_t count, const VkFormat* colors, VkFormat depth) {
    return pass_for(count, colors, depth, nullptr);
}

void begin_render_pass(VkCommandBuffer cb, uint32_t count, const VkFormat* formats,
                       const VkRenderingAttachmentInfo* colors, VkFormat depth_format,
                       const VkRenderingAttachmentInfo& depth, const VkRenderingAttachmentInfo& stencil,
                       VkExtent2D extent) {
    count = count > 8 ? 8 : count;
    VkFormat fmt[8];
    uint32_t loads[10] = {};  // load operation + 1 (0: the compatible variant's LOAD)
    FramebufferKey fk;
    VkClearValue clears[9]{};
    uint32_t n = 0;
    for (uint32_t c = 0; c < count; c++) {
        fmt[c] = colors[c].imageView ? formats[c] : VK_FORMAT_UNDEFINED;
        if (!colors[c].imageView) continue;
        loads[c] = uint32_t(colors[c].loadOp) + 1;
        clears[n] = colors[c].clearValue;
        fk.views[n++] = colors[c].imageView;
    }
    VkImageView dview = depth.imageView ? depth.imageView : stencil.imageView;
    if (!dview) depth_format = VK_FORMAT_UNDEFINED;
    if (depth_format != VK_FORMAT_UNDEFINED) {
        loads[8] = depth.imageView ? uint32_t(depth.loadOp) + 1 : 0;
        loads[9] = stencil.imageView ? uint32_t(stencil.loadOp) + 1 : 0;
        clears[n].depthStencil = {depth.clearValue.depthStencil.depth, stencil.clearValue.depthStencil.stencil};
        fk.views[n++] = dview;
    }
    VkRenderPass rp = pass_for(count, fmt, depth_format, loads);
    fk.pass = pass_for(count, fmt, depth_format, nullptr);
    fk.count = n, fk.width = extent.width, fk.height = extent.height;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (auto it = g_framebuffers.find(fk); it != g_framebuffers.end()) {
        fb = it->second;
    } else {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = fk.pass;
        fi.attachmentCount = n;
        fi.pAttachments = fk.views;
        fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
        if (VkResult r = p_vkCreateFramebuffer(vk_device(), &fi, nullptr, &fb); r == VK_SUCCESS) {
            g_framebuffers[fk] = fb;
        } else {  // not kept: the next pass on these views tries again (a null one kept failed every later pass)
            log("nvn/vk: vkCreateFramebuffer failed (%d): %u attachments, %ux%u, colours %d %d %d %d, depth %d, "
                "%zu framebuffers kept",
                int(r), n, extent.width, extent.height, int(fmt[0]), count > 1 ? int(fmt[1]) : 0,
                count > 2 ? int(fmt[2]) : 0, count > 3 ? int(fmt[3]) : 0, int(depth_format), g_framebuffers.size());
            fb = VK_NULL_HANDLE;
        }
        g_framebuffers_made++;
    }
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, extent};
    bi.clearValueCount = n;
    bi.pClearValues = clears;
    p_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

// A view is being destroyed (its image freed after the submits that used it were retired): its framebuffers too.
void forget_framebuffers(VkImageView view) {
    if (!view || g_framebuffers.empty()) return;
    for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();) {
        bool uses = false;
        for (uint32_t k = 0; k < it->first.count; k++) uses = uses || it->first.views[k] == view;
        if (!uses) {
            ++it;
            continue;
        }
        if (it->second) p_vkDestroyFramebuffer(vk_device(), it->second, nullptr);
        it = g_framebuffers.erase(it);
    }
}

void render_pass_log() {
    if (render_pass_objects())
        log("nvn/vk: %zu render passes, %zu framebuffers kept, %llu framebuffers made", g_passes.size(),
            g_framebuffers.size(), (unsigned long long)g_framebuffers_made);
}

}  // namespace nx::nvn
