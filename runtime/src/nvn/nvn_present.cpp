// The NVN present pass (docs/nvn/ANDROID-PORT.md A2; DEBUG-2.md #79): the window texture drawn into the swapchain
// image by a render pass instead of vkCmdBlitImage.
// - Rotated by the swapchain's preTransform: phones scan out portrait, and a pre-rotated image goes to the display
//   controller as is (the compositor rotates otherwise, on the GPU whenever system UI is on top).
// - Fit with bars: a 16:9 picture on a 19.5:9 screen keeps its shape.
// - Alpha 1: an Android surface may take the image's alpha as transparency (compositeAlpha INHERIT), and the game's
//   final image has no meaningful alpha.
// On by default on Android. NX_PRESENT_PASS=1 turns it on elsewhere (checking it on the Mac), =0 off (the blit).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"
#include "touch_layout.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// glslc -O --target-env=vulkan1.0 of present.vert:
//   #version 450
//   // A triangle covering the target; uv 0..1 across it (y down, as Vulkan framebuffer coordinates).
//   layout(location = 0) out vec2 uv;
//   void main() {
//       vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
//       uv = p;
//       gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
//   }
const uint32_t kVertex[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000002f, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00000018, 0x00000020, 0x00040047, 0x0000000c, 0x0000000b,
    0x0000002a, 0x00040047, 0x00000018, 0x0000001e, 0x00000000, 0x00030047, 0x0000001e, 0x00000002, 0x00050048,
    0x0000001e, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001e, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000001e, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001e, 0x00000003, 0x0000000b,
    0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020,
    0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a,
    0x0000000e, 0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040020, 0x00000017, 0x00000003,
    0x00000007, 0x0004003b, 0x00000017, 0x00000018, 0x00000003, 0x00040017, 0x0000001a, 0x00000006, 0x00000004,
    0x00040015, 0x0000001b, 0x00000020, 0x00000000, 0x0004002b, 0x0000001b, 0x0000001c, 0x00000001, 0x0004001c,
    0x0000001d, 0x00000006, 0x0000001c, 0x0006001e, 0x0000001e, 0x0000001a, 0x00000006, 0x0000001d, 0x0000001d,
    0x00040020, 0x0000001f, 0x00000003, 0x0000001e, 0x0004003b, 0x0000001f, 0x00000020, 0x00000003, 0x0004002b,
    0x0000000a, 0x00000021, 0x00000000, 0x0004002b, 0x00000006, 0x00000023, 0x40000000, 0x0004002b, 0x00000006,
    0x00000025, 0x3f800000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000, 0x00040020, 0x0000002c, 0x00000003,
    0x0000001a, 0x0005002c, 0x00000007, 0x0000002e, 0x00000025, 0x00000025, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4,
    0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010,
    0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x000500c7, 0x0000000a, 0x00000014, 0x0000000d, 0x00000010,
    0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015,
    0x0003003e, 0x00000018, 0x00000016, 0x0005008e, 0x00000007, 0x00000024, 0x00000016, 0x00000023, 0x00050083,
    0x00000007, 0x00000027, 0x00000024, 0x0000002e, 0x00050051, 0x00000006, 0x00000029, 0x00000027, 0x00000000,
    0x00050051, 0x00000006, 0x0000002a, 0x00000027, 0x00000001, 0x00070050, 0x0000001a, 0x0000002b, 0x00000029,
    0x0000002a, 0x00000028, 0x00000025, 0x00050041, 0x0000002c, 0x0000002d, 0x00000020, 0x00000021, 0x0003003e,
    0x0000002d, 0x0000002b, 0x000100fd, 0x00010038,
};

// The picture (tools/shaders/present/present.frag, kFragment) and the touch controls drawn over it, a quad per circle
// (overlay.vert and overlay.frag, kOverlayVertex and kOverlayFragment).
#include "nvn_present_spirv.inc"

constexpr size_t kOverlayMax = 32;  // Overlay.shape[32] in tools/shaders/present/overlay.glsl

// The labels' 5x7 glyphs, Overlay.font in overlay.glsl (a uniform block: a constant array indexed per pixel was slow on
// the Adreno 610). One row per entry, bit 4 = the left column: 0 none, 1 A, 2 B, 3 X, 4 Y, 5 L, 6 R, 7 Z, 8 +, 9 -,
// 10 up, 11 down, 12 left, 13 right, 14 3, 15..24 the digits 0..9 (the FPS counter).
const uint32_t kFont[25 * 7] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x0Eu, 0x11u, 0x11u, 0x1Fu, 0x11u, 0x11u, 0x11u,
    0x1Eu, 0x11u, 0x11u, 0x1Eu, 0x11u, 0x11u, 0x1Eu,
    0x11u, 0x11u, 0x0Au, 0x04u, 0x0Au, 0x11u, 0x11u,
    0x11u, 0x11u, 0x0Au, 0x04u, 0x04u, 0x04u, 0x04u,
    0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x1Fu,
    0x1Eu, 0x11u, 0x11u, 0x1Eu, 0x14u, 0x12u, 0x11u,
    0x1Fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x1Fu,
    0x00u, 0x04u, 0x04u, 0x1Fu, 0x04u, 0x04u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x1Fu, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x04u, 0x0Eu, 0x1Fu, 0x00u, 0x00u,
    0x00u, 0x00u, 0x1Fu, 0x0Eu, 0x04u, 0x00u, 0x00u,
    0x00u, 0x02u, 0x06u, 0x0Eu, 0x06u, 0x02u, 0x00u,
    0x00u, 0x08u, 0x0Cu, 0x0Eu, 0x0Cu, 0x08u, 0x00u,
    0x1Eu, 0x01u, 0x01u, 0x0Eu, 0x01u, 0x01u, 0x1Eu,
    0x0Eu, 0x11u, 0x13u, 0x15u, 0x19u, 0x11u, 0x0Eu,
    0x04u, 0x0Cu, 0x04u, 0x04u, 0x04u, 0x04u, 0x0Eu,
    0x0Eu, 0x11u, 0x01u, 0x02u, 0x04u, 0x08u, 0x1Fu,
    0x1Fu, 0x02u, 0x04u, 0x02u, 0x01u, 0x11u, 0x0Eu,
    0x02u, 0x06u, 0x0Au, 0x12u, 0x1Fu, 0x02u, 0x02u,
    0x1Fu, 0x10u, 0x1Eu, 0x01u, 0x01u, 0x11u, 0x0Eu,
    0x06u, 0x08u, 0x10u, 0x1Eu, 0x11u, 0x11u, 0x0Eu,
    0x1Fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x08u, 0x08u,
    0x0Eu, 0x11u, 0x11u, 0x0Eu, 0x11u, 0x11u, 0x0Eu,
    0x0Eu, 0x11u, 0x11u, 0x0Fu, 0x01u, 0x02u, 0x0Cu
};
constexpr size_t kFontWords = 44 * 4;  // uvec4 font[44]
constexpr size_t kOverlayBytes = 16 * kOverlayMax + 4 * kFontWords;
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct Push {
    float rot[4], off[4], box[4], crop[4];
};

#include "nvn_fsr_spirv.inc"

struct Pass {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE, overlay = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool failed = false;
    // FSR 1 (NX_FSR=1): EASU draws the shown part of the texture, upscaled, into `inter` (the size of the picture on
    // the target); the main pass then sharpens it (RCAS) into the swapchain image.
    VkRenderPass easu_pass = VK_NULL_HANDLE;
    VkPipeline easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE;
    VkImage inter = VK_NULL_HANDLE;
    VkDeviceMemory inter_memory = VK_NULL_HANDLE;
    VkImageView inter_view = VK_NULL_HANDLE;
    VkFramebuffer inter_fb = VK_NULL_HANDLE;
    uint32_t inter_w = 0, inter_h = 0;
    bool fsr_logged = false;
} g;

bool fsr_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_FSR");
        return e && e[0] == '1';
    }();
    return on;
}

float fsr_sharpness() {  // stops of sharpening: 0 the most, 2 none (FSR's own default is 0.2)
    static const float v = [] {
        const char* e = std::getenv("NX_FSR_SHARP");
        return e ? std::max(0.0f, std::min(2.0f, float(std::atof(e)))) : 0.2f;
    }();
    return v;
}

// The intermediate image for a picture of w x h pixels on the target, made again when the size changes.
bool fsr_inter(uint32_t w, uint32_t h) {
    if (g.inter && g.inter_w == w && g.inter_h == h) return true;
    VkDevice d = vk_device();
    if (g.inter) {  // a new size means a new window: the swapchain was remade, which waited for the queue
        p_vkDestroyFramebuffer(d, g.inter_fb, nullptr);
        p_vkDestroyImageView(d, g.inter_view, nullptr);
        p_vkDestroyImage(d, g.inter, nullptr);
        p_vkFreeMemory(d, g.inter_memory, nullptr);
        g.inter = VK_NULL_HANDLE, g.inter_view = VK_NULL_HANDLE, g.inter_fb = VK_NULL_HANDLE;
        g.inter_memory = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R16G16B16A16_SFLOAT, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &g.inter) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, g.inter, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &g.inter_memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, g.inter, g.inter_memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g.inter, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &g.inter_view) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.easu_pass, fi.attachmentCount = 1, fi.pAttachments = &g.inter_view;
    fi.width = w, fi.height = h, fi.layers = 1;
    if (p_vkCreateFramebuffer(d, &fi, nullptr, &g.inter_fb) != VK_SUCCESS) return false;
    g.inter_w = w, g.inter_h = h;
    return true;
}

VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m);
    return m;
}

void destroy_targets() {
    VkDevice d = vk_device();
    for (VkFramebuffer f : g.framebuffers) p_vkDestroyFramebuffer(d, f, nullptr);
    for (VkImageView v : g.views) p_vkDestroyImageView(d, v, nullptr);
    g.framebuffers.clear(), g.views.clear();
    if (g.inter_fb) p_vkDestroyFramebuffer(d, g.inter_fb, nullptr);
    if (g.inter_view) p_vkDestroyImageView(d, g.inter_view, nullptr);
    if (g.inter) p_vkDestroyImage(d, g.inter, nullptr);
    if (g.inter_memory) p_vkFreeMemory(d, g.inter_memory, nullptr);
    g.inter_fb = VK_NULL_HANDLE, g.inter_view = VK_NULL_HANDLE, g.inter = VK_NULL_HANDLE;
    g.inter_memory = VK_NULL_HANDLE, g.inter_w = g.inter_h = 0;
}

// A full-target triangle drawn with the fragment shader `code` in render pass `pass` (g.layout). With `vertex`: that
// vertex shader instead, a triangle strip, blended as overlay.frag wants (colour ONE + SRC_ALPHA, alpha kept).
bool build_pipeline(VkRenderPass pass, const uint32_t* code, size_t bytes, VkPipeline* out,
                    const uint32_t* vertex = nullptr, size_t vertex_bytes = 0) {
    VkDevice d = vk_device();
    VkShaderModule vs = vertex ? module(vertex, vertex_bytes) : module(kVertex, sizeof kVertex);
    VkShaderModule fs = module(code, bytes);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = vertex ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    if (vertex) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE, blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2, ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2, gp.pStages = stages;
    gp.pVertexInputState = &vin, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp;
    gp.pRasterizationState = &rs, gp.pMultisampleState = &ms, gp.pColorBlendState = &cb, gp.pDynamicState = &ds;
    gp.layout = g.layout, gp.renderPass = pass;
    VkResult r = p_vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, out);
    p_vkDestroyShaderModule(d, vs, nullptr);
    p_vkDestroyShaderModule(d, fs, nullptr);
    return r == VK_SUCCESS;
}

// FSR's render pass (RGBA16F, left readable by the main pass) and the two pipelines.
bool make_fsr() {
    VkDevice d = vk_device();
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL, dep[0].dstSubpass = 0;  // the last frame's RCAS has read it
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0, dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;  // RCAS reads what EASU wrote
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1, rpi.pAttachments = &color;
    rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2, rpi.pDependencies = dep;
    if (p_vkCreateRenderPass(d, &rpi, nullptr, &g.easu_pass) != VK_SUCCESS) return false;
    return build_pipeline(g.easu_pass, kEasu, sizeof kEasu, &g.easu) &&
           build_pipeline(g.render_pass, kRcas, sizeof kRcas, &g.rcas);
}

bool make_pipeline(VkFormat format) {
    VkDevice d = vk_device();
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1, rpi.pAttachments = &color;
    rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1, rpi.pDependencies = &dep;
    if (p_vkCreateRenderPass(d, &rpi, nullptr, &g.render_pass) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(d, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &g.sampler},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2, dli.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{kPushStages, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;

    VkResult ok = build_pipeline(g.render_pass, kFragment, sizeof kFragment, &g.pipeline) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
    if (ok == VK_SUCCESS && !build_pipeline(g.render_pass, kOverlayFragment, sizeof kOverlayFragment, &g.overlay,
                                            kOverlayVertex, sizeof kOverlayVertex)) {
        log("nvn/vk: touch controls pipeline unavailable: they are not drawn");
        g.overlay = VK_NULL_HANDLE;
    }
    if (ok == VK_SUCCESS && fsr_on() && !make_fsr()) log("nvn/vk: FSR unavailable: plain scaling");
    g.format = format;
    return ok == VK_SUCCESS;
}

}  // namespace

bool present_pass_wanted() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PRESENT_PASS");
#if defined(__ANDROID__)
        return !e || e[0] != '0';
#else
        return (e && e[0] == '1') || fsr_on();  // FSR is a pass of this one
#endif
    }();
    return on;
}

void present_pass_release() { destroy_targets(); }  // before the swapchain whose images they view is destroyed
bool present_pass_failed() { return g.failed; }

bool present_pass_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent,
                          VkSurfaceTransformFlagBitsKHR transform) {
    if (!present_pass_wanted() || g.failed) return false;
    destroy_targets();
    if (g.format != format && g.pipeline) {  // a new swapchain format: a new render pass and pipeline
        VkDevice d = vk_device();
        p_vkDestroyPipeline(d, g.pipeline, nullptr);
        if (g.overlay) p_vkDestroyPipeline(d, g.overlay, nullptr);
        p_vkDestroyRenderPass(d, g.render_pass, nullptr);
        g.pipeline = g.overlay = VK_NULL_HANDLE, g.render_pass = VK_NULL_HANDLE;
        if (g.easu) p_vkDestroyPipeline(d, g.easu, nullptr);
        if (g.rcas) p_vkDestroyPipeline(d, g.rcas, nullptr);
        if (g.easu_pass) p_vkDestroyRenderPass(d, g.easu_pass, nullptr);
        g.easu = g.rcas = VK_NULL_HANDLE, g.easu_pass = VK_NULL_HANDLE;
    }
    if (!g.pipeline && !make_pipeline(format)) {
        log("nvn/vk: present pass unavailable: frames are blitted");
        g.failed = true;
        return false;
    }
    VkDevice d = vk_device();
    for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        p_vkCreateImageView(d, &vi, nullptr, &v);
        g.views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g.render_pass, fi.attachmentCount = 1, fi.pAttachments = &v;
        fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (p_vkCreateFramebuffer(d, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        g.framebuffers.push_back(fb);
    }
    g.extent = extent, g.transform = transform;
    return true;
}

bool present_pass_record(VkCommandBuffer cb, GpuImage& src, const int32_t* crop, uint32_t index) {
    if (!g.pipeline || index >= g.framebuffers.size() || !src.view || src.levels != 1 || src.layers != 1 ||
        src.view_type != VK_IMAGE_VIEW_TYPE_2D || !(src.aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The set and the circles' uniform data come from this submit's slot (descriptor pool and upload ring, reset when
    // the slot is retired): nothing a frame still in flight reads is written. Per swapchain image they were not safe
    // (MAILBOX can hand an image back before the frame that last drew into it finished; review of DEBUG-2.md #88).
    VkDescriptorSet set = descriptor_set(g.set_layout);
    Upload ubo = upload_alloc(kOverlayBytes, 256);
    std::memset(ubo.ptr, 0, kOverlayBytes);
    std::memcpy(static_cast<uint8_t*>(ubo.ptr) + 16 * kOverlayMax, kFont, sizeof kFont);
    VkDescriptorImageInfo ii{g.sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi{ubo.buffer, ubo.offset, kOverlayBytes};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = set, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &ii;
    w[1].dstSet = set, w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &bi;
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);

    Push p{};
    // Host uv -> upright uv, undoing the clockwise turn the display applies (as vk_present.cpp).
    static const float kRot[4][6] = {{1, 0, 0, 1, 0, 0}, {0, -1, 1, 0, 0, 1}, {-1, 0, 0, -1, 1, 1}, {0, 1, -1, 0, 1, 0}};
    int r = g.transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR    ? 1
            : g.transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
            : g.transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3
                                                                     : 0;
    std::memcpy(p.rot, kRot[r], 4 * sizeof(float));
    p.off[0] = kRot[r][4], p.off[1] = kRot[r][5];
    // The part of the texture shown (nvnWindowSetCrop), fit into the upright target keeping its aspect.
    float cx = 0, cy = 0, cw = float(src.width), ch = float(src.height);
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(src.width) &&
        crop[1] + crop[3] <= int32_t(src.height))
        cx = float(crop[0]), cy = float(crop[1]), cw = float(crop[2]), ch = float(crop[3]);
    p.crop[0] = cx / float(src.width), p.crop[1] = cy / float(src.height);
    p.crop[2] = cw / float(src.width), p.crop[3] = ch / float(src.height);
    bool sideways = r == 1 || r == 3;
    float tw = float(sideways ? g.extent.height : g.extent.width), th = float(sideways ? g.extent.width : g.extent.height);
    float bw = 1.0f, bh = 1.0f;
    const char* stretch_env = std::getenv("NX_STRETCH");
    bool stretch = stretch_env && (stretch_env[0] == '1' || std::strcmp(stretch_env, "true") == 0);
    if (stretch) {
        p.box[0] = 0.0f, p.box[1] = 0.0f, p.box[2] = 1.0f, p.box[3] = 1.0f;
    } else {
        float scale = std::min(tw / cw, th / ch);
        bw = cw * scale / tw, bh = ch * scale / th;
        p.box[0] = (1 - bw) / 2, p.box[1] = (1 - bh) / 2, p.box[2] = bw, p.box[3] = bh;
    }
    // The touch controls (platform_android_input.cpp), in upright uv; none elsewhere.
    plat::OverlayShape shapes[kOverlayMax];
    size_t count = plat::touch_overlay(shapes, kOverlayMax);
    // NX_SHOW_FPS=1 (the launcher's FPS counter, off by default): presents over the last second, top left.
    static const bool show_fps = [] {
        const char* e = std::getenv("NX_SHOW_FPS");
        return e && e[0] == '1';
    }();
    if (show_fps && count < kOverlayMax) {
        static uint64_t t0 = plat::monotonic_ns(), frames = 0;
        static uint32_t fps = 0;
        uint64_t now = plat::monotonic_ns();
        frames++;
        if (now - t0 >= 1'000'000'000ull) fps = uint32_t((frames * 1'000'000'000ull + (now - t0) / 2) / (now - t0)), frames = 0, t0 = now;
        uint32_t v = std::min<uint32_t>(fps, 99);
        uint32_t label = (plat::kGlyphDigit0 + v / 10) | (plat::kGlyphDigit0 + v % 10) << 5;
        shapes[count++] = {0.20f, 0.05f, 0.035f, 0.0f, label};
    }
    auto* dst = reinterpret_cast<float*>(ubo.ptr);
    for (size_t k = 0; k < count; k++)
        dst[4 * k] = shapes[k].x, dst[4 * k + 1] = shapes[k].y, dst[4 * k + 2] = shapes[k].r,
        dst[4 * k + 3] = std::clamp(shapes[k].alpha, 0.0f, 1.0f) + 4.0f * float(shapes[k].label);  // overlay.frag
    p.off[3] = tw / th;

    // FSR 1: upscale the shown part into `inter` (the picture's size on the target), then sharpen it in the main pass.
    uint32_t out_w = uint32_t(bw * tw + 0.5f), out_h = uint32_t(bh * th + 0.5f);
    VkDescriptorSet rcas_set = VK_NULL_HANDLE;
    if (g.easu && g.rcas && r == 0 && out_w > cw * 1.01f && out_h > ch * 1.01f && fsr_inter(out_w, out_h)) {
        if (!g.fsr_logged)
            log("nvn/vk: FSR 1: %.0fx%.0f -> %ux%u (sharpness %.2f stops)", cw, ch, out_w, out_h, fsr_sharpness()),
                g.fsr_logged = true;
        struct { float a[4], b[4], c[4], d[4]; } e{};
        e.a[0] = cw / float(out_w), e.a[1] = ch / float(out_h);
        e.a[2] = 0.5f * e.a[0] - 0.5f, e.a[3] = 0.5f * e.a[1] - 0.5f;
        e.b[0] = cx, e.b[1] = cy, e.b[2] = cw, e.b[3] = ch;
        VkRenderPassBeginInfo eb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        eb.renderPass = g.easu_pass, eb.framebuffer = g.inter_fb, eb.renderArea = {{0, 0}, {out_w, out_h}};
        p_vkCmdBeginRenderPass(cb, &eb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport ev{0, 0, float(out_w), float(out_h), 0, 1};
        VkRect2D es{{0, 0}, {out_w, out_h}};
        p_vkCmdSetViewport(cb, 0, 1, &ev);
        p_vkCmdSetScissor(cb, 0, 1, &es);
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.easu);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
        p_vkCmdPushConstants(cb, g.layout, kPushStages, 0, sizeof e, &e);
        p_vkCmdDraw(cb, 3, 1, 0, 0);
        p_vkCmdEndRenderPass(cb);
        rcas_set = descriptor_set(g.set_layout);
        VkDescriptorImageInfo ri{g.sampler, g.inter_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet rw[2] = {w[0], w[1]};
        rw[0].dstSet = rcas_set, rw[0].pImageInfo = &ri;
        rw[1].dstSet = rcas_set;
        p_vkUpdateDescriptorSets(vk_device(), 2, rw, 0, nullptr);
        p.crop[0] = float(out_w), p.crop[1] = float(out_h), p.crop[2] = std::exp2(-fsr_sharpness()), p.crop[3] = 0;
    }

    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.render_pass, rb.framebuffer = g.framebuffers[index], rb.renderArea = {{0, 0}, g.extent};
    p_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    VkRect2D sc{{0, 0}, g.extent};
    p_vkCmdSetViewport(cb, 0, 1, &v);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rcas_set ? g.rcas : g.pipeline);
    VkDescriptorSet main_set = rcas_set ? rcas_set : set;
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &main_set, 0, nullptr);
    p_vkCmdPushConstants(cb, g.layout, kPushStages, 0, sizeof p, &p);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    if (count && g.overlay) {  // the touch controls: a quad per circle, blended over the picture (overlay.frag)
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.overlay);
        p_vkCmdDraw(cb, 4, uint32_t(count), 0, 0);
    }
    p_vkCmdEndRenderPass(cb);
    return true;
}

// NX_PRESENT_TEST=<w>x<h>[:<90|180|270>] (headless, with NX_DUMP_FRAMES): the pass draws into an offscreen image
// of that size and transform, and that image is dumped in place of the window texture. It checks the rotation, the
// fit and the alpha without a phone (a Galaxy S23 held sideways: 1080x2340:90).
GpuImage* present_pass_test_target() {
    static GpuImage img;
    static bool tried = false;
    if (tried) return img.image ? &img : nullptr;
    tried = true;
    const char* e = std::getenv("NX_PRESENT_TEST");
    uint32_t w = 0, h = 0, deg = 0;
    if (!e || std::sscanf(e, "%ux%u:%u", &w, &h, &deg) < 2 || !w || !h) return nullptr;
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R8G8B8A8_UNORM, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &img.image) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    p_vkAllocateMemory(d, &ai, nullptr, &img.memory);
    p_vkBindImageMemory(d, img.image, img.memory, 0);
    img.format = VK_FORMAT_R8G8B8A8_UNORM, img.width = w, img.height = h;
    VkSurfaceTransformFlagBitsKHR t = deg == 90    ? VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR
                                      : deg == 180 ? VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR
                                      : deg == 270 ? VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR
                                                   : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (!present_pass_targets(img.format, {img.image}, {w, h}, t)) {
        log("nvn/vk: present test: the pass is off (NX_PRESENT_PASS=1)");
        return nullptr;
    }
    log("nvn/vk: present test: %ux%u, turned %u degrees", w, h, deg);
    return &img;
}

}  // namespace nx::nvn
