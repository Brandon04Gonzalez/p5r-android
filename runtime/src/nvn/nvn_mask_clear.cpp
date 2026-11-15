// Colour clears with some channels masked off (docs/nvn/GLITCH-PLAN.md G6). NVN's ClearColor(index, rgba, mask)
// writes only the channels in the mask (an alpha-only clear keeps RGB), and vkCmdClearAttachments writes them all.
// Such a clear is a fullscreen triangle inside the open pass instead: the colour comes from a push constant, the
// pipeline writes attachment `slot` only, through colorWriteMask = mask, and the scissor is the clear's rectangle.
// Depth and stencil are neither tested nor written. Pipelines are cached by the pass's formats, slot and mask.
// Float and normalized targets only (the fragment output is a vec4): integer ones keep the all-channel clear.
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// glslc -O --target-env=vulkan1.0 of:
//   #version 450
//   void main() {
//       vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
//       gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
//   }
const uint32_t kVertex[] = {
    0x07230203,0x00010000,0x000d000b,0x0000002c,
0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,
0x00000000,0x0003000e,0x00000000,0x00000001,
0x0007000f,0x00000000,0x00000004,0x6e69616d,
0x00000000,0x0000000c,0x0000001d,0x00040047,
0x0000000c,0x0000000b,0x0000002a,0x00030047,
0x0000001b,0x00000002,0x00050048,0x0000001b,
0x00000000,0x0000000b,0x00000000,0x00050048,
0x0000001b,0x00000001,0x0000000b,0x00000001,
0x00050048,0x0000001b,0x00000002,0x0000000b,
0x00000003,0x00050048,0x0000001b,0x00000003,
0x0000000b,0x00000004,0x00020013,0x00000002,
0x00030021,0x00000003,0x00000002,0x00030016,
0x00000006,0x00000020,0x00040017,0x00000007,
0x00000006,0x00000002,0x00040015,0x0000000a,
0x00000020,0x00000001,0x00040020,0x0000000b,
0x00000001,0x0000000a,0x0004003b,0x0000000b,
0x0000000c,0x00000001,0x0004002b,0x0000000a,
0x0000000e,0x00000001,0x0004002b,0x0000000a,
0x00000010,0x00000002,0x00040017,0x00000017,
0x00000006,0x00000004,0x00040015,0x00000018,
0x00000020,0x00000000,0x0004002b,0x00000018,
0x00000019,0x00000001,0x0004001c,0x0000001a,
0x00000006,0x00000019,0x0006001e,0x0000001b,
0x00000017,0x00000006,0x0000001a,0x0000001a,
0x00040020,0x0000001c,0x00000003,0x0000001b,
0x0004003b,0x0000001c,0x0000001d,0x00000003,
0x0004002b,0x0000000a,0x0000001e,0x00000000,
0x0004002b,0x00000006,0x00000020,0x40000000,
0x0004002b,0x00000006,0x00000022,0x3f800000,
0x0004002b,0x00000006,0x00000025,0x00000000,
0x00040020,0x00000029,0x00000003,0x00000017,
0x0005002c,0x00000007,0x0000002b,0x00000022,
0x00000022,0x00050036,0x00000002,0x00000004,
0x00000000,0x00000003,0x000200f8,0x00000005,
0x0004003d,0x0000000a,0x0000000d,0x0000000c,
0x000500c4,0x0000000a,0x0000000f,0x0000000d,
0x0000000e,0x000500c7,0x0000000a,0x00000011,
0x0000000f,0x00000010,0x0004006f,0x00000006,
0x00000012,0x00000011,0x000500c7,0x0000000a,
0x00000014,0x0000000d,0x00000010,0x0004006f,
0x00000006,0x00000015,0x00000014,0x00050050,
0x00000007,0x00000016,0x00000012,0x00000015,
0x0005008e,0x00000007,0x00000021,0x00000016,
0x00000020,0x00050083,0x00000007,0x00000024,
0x00000021,0x0000002b,0x00050051,0x00000006,
0x00000026,0x00000024,0x00000000,0x00050051,
0x00000006,0x00000027,0x00000024,0x00000001,
0x00070050,0x00000017,0x00000028,0x00000026,
0x00000027,0x00000025,0x00000022,0x00050041,
0x00000029,0x0000002a,0x0000001d,0x0000001e,
0x0003003e,0x0000002a,0x00000028,0x000100fd,
0x00010038
};

// glslc -O --target-env=vulkan1.0 of:
//   #version 450
//   layout(push_constant) uniform P { vec4 color; } pc;
//   layout(location = 0) out vec4 o;
//   void main() { o = pc.color; }
// The Location literal is patched to the cleared slot (one module per slot).
const uint32_t kFragment[] = {
    0x07230203,0x00010000,0x000d000b,0x00000012,
0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,
0x00000000,0x0003000e,0x00000000,0x00000001,
0x0006000f,0x00000004,0x00000004,0x6e69616d,
0x00000000,0x00000009,0x00030010,0x00000004,
0x00000007,0x00040047,0x00000009,0x0000001e,
0x00000000,0x00030047,0x0000000a,0x00000002,
0x00050048,0x0000000a,0x00000000,0x00000023,
0x00000000,0x00020013,0x00000002,0x00030021,
0x00000003,0x00000002,0x00030016,0x00000006,
0x00000020,0x00040017,0x00000007,0x00000006,
0x00000004,0x00040020,0x00000008,0x00000003,
0x00000007,0x0004003b,0x00000008,0x00000009,
0x00000003,0x0003001e,0x0000000a,0x00000007,
0x00040020,0x0000000b,0x00000009,0x0000000a,
0x0004003b,0x0000000b,0x0000000c,0x00000009,
0x00040015,0x0000000d,0x00000020,0x00000001,
0x0004002b,0x0000000d,0x0000000e,0x00000000,
0x00040020,0x0000000f,0x00000009,0x00000007,
0x00050036,0x00000002,0x00000004,0x00000000,
0x00000003,0x000200f8,0x00000005,0x00050041,
0x0000000f,0x00000010,0x0000000c,0x0000000e,
0x0004003d,0x00000007,0x00000011,0x00000010,
0x0003003e,0x00000009,0x00000011,0x000100fd,
0x00010038
};

// Fragment outputs of these formats are integers: a vec4 output would be undefined there.
bool integer_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_B8G8R8A8_SINT: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return true;
        default: return false;
    }
}

VkShaderModule make_module(const uint32_t* code, size_t words) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) log("nvn/vk: mask clear shader rejected");
    return m;
}

VkShaderModule vertex_module() {
    static VkShaderModule m = make_module(kVertex, sizeof kVertex / 4);
    return m;
}

VkShaderModule fragment_module(uint32_t slot) {
    static VkShaderModule modules[8] = {};
    if (slot >= 8) return VK_NULL_HANDLE;
    if (!modules[slot]) {
        std::vector<uint32_t> code(kFragment, kFragment + sizeof kFragment / 4);
        // OpDecorate (word count 4, opcode 71) <id> Location (30) <literal>: the only one in this module.
        for (size_t k = 5; k + 3 < code.size(); k += code[k] >> 16 ? code[k] >> 16 : 1)
            if (code[k] == 0x00040047u && code[k + 2] == 30u) code[k + 3] = slot;
        modules[slot] = make_module(code.data(), code.size());
    }
    return modules[slot];
}

VkPipelineLayout layout() {
    static VkPipelineLayout l = [] {
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        VkPipelineLayout out = VK_NULL_HANDLE;
        p_vkCreatePipelineLayout(vk_device(), &ci, nullptr, &out);
        return out;
    }();
    return l;
}

struct Key {
    VkFormat colors[8] = {};
    VkFormat depth = VK_FORMAT_UNDEFINED;
    uint32_t count = 0, slot = 0, mask = 0;
};

VkPipeline pipeline(const Key& key) {
    static std::map<std::string, VkPipeline> cache;
    std::string k(reinterpret_cast<const char*>(&key), sizeof key);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vertex_module(), stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fragment_module(key.slot);
    stages[1].pName = "main";
    if (!stages[0].module || !stages[1].module) return cache[k] = VK_NULL_HANDLE;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};  // all off
    VkPipelineColorBlendAttachmentState att[8]{};  // write masks 0: the other targets are untouched
    att[key.slot].colorWriteMask = key.mask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = key.count;
    cb.pAttachments = att;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    // The pass's formats, as graphics_pipeline() declares them for the draws of the same pass.
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.count;
    ri.pColorAttachmentFormats = key.colors;
    bool stencil = key.depth == VK_FORMAT_D32_SFLOAT_S8_UINT || key.depth == VK_FORMAT_D24_UNORM_S8_UINT ||
                   key.depth == VK_FORMAT_S8_UINT;
    ri.depthAttachmentFormat = key.depth == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth;
    ri.stencilAttachmentFormat = stencil ? key.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    if (render_pass_objects()) ci.pNext = nullptr, ci.renderPass = compatible_render_pass(key.count, key.colors, key.depth);
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout();
    VkPipeline p = VK_NULL_HANDLE;
    if (p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p) != VK_SUCCESS) {
        log("nvn/vk: mask clear pipeline (slot %u mask %#x) failed", key.slot, key.mask);
        p = VK_NULL_HANDLE;
    }
    return cache[k] = p;
}

}  // namespace

bool mask_clear(VkCommandBuffer cb, const Targets& t, uint32_t slot, uint32_t mask, const float rgba[4],
                const VkRect2D& rect) {
    if (slot >= t.color_count || slot >= 8 || !pass_open_on(t)) return false;
    Key key;
    std::memset(&key, 0, sizeof key);  // compared byte for byte
    key.count = t.color_count, key.slot = slot, key.mask = mask & 0xF;
    for (uint32_t k = 0; k < t.color_count; k++) {  // as draw_call builds its PipelineKey
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        key.colors[k] = img ? view_format(*img, t.views.format[k]) : unused_slot_format();
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    key.depth = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    if (integer_format(key.colors[slot])) return false;
    VkPipeline p = pipeline(key);
    if (!p) return false;
    static uint64_t count = 0;
    if (++count == 1 || count % 1000 == 0)
        log("nvn: %llu masked colour clears drawn (this one: target %u, mask %#x, %ux%u at %d,%d)",
            (unsigned long long)count, slot, mask, rect.extent.width, rect.extent.height, rect.offset.x, rect.offset.y);
    // The next draw sets its own pipeline, descriptor sets, viewport, scissor and dynamic state (nvn_draw.cpp).
    VkExtent2D pe = pass_extent();
    VkViewport vp{0, 0, float(pe.width), float(pe.height), 0, 1};
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    p_vkCmdSetScissor(cb, 0, 1, &rect);
    p_vkCmdPushConstants(cb, layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rgba);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    return true;
}

}  // namespace nx::nvn
