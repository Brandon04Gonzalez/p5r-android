#include "vk_present.h"

#include <cstring>

#include "shaders_spv.h"
#include "vk_gen.h"

namespace nx::vk {

namespace {

VkShaderModule shader(VkDevice device, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

VkImageView view(VkDevice device, VkImage image, VkFormat format) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    p_vkCreateImageView(device, &ci, nullptr, &v);
    return v;
}

}  // namespace

bool PresentPass::init(VkDevice device, VkFormat format, const std::vector<VkImage>& game_images, bool scaled) {
    device_ = device;
    format_ = format;
    src_images_ = game_images;

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
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &color;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (p_vkCreateRenderPass(device, &rpi, nullptr, &render_pass_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // Same size: one texel per pixel. Rendered smaller (render scale): bilinear up to the screen.
    si.magFilter = si.minFilter = scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(device, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &b;
    if (p_vkCreateDescriptorSetLayout(device, &dli, nullptr, &set_layout_) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof map_};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &layout_) != VK_SUCCESS) return false;

    VkShaderModule vs = shader(device, k_present_vert, sizeof k_present_vert);
    VkShaderModule fs = shader(device, k_present_frag, sizeof k_present_frag);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout_;
    gp.renderPass = render_pass_;
    VkResult r = p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
    p_vkDestroyShaderModule(device, vs, nullptr);
    p_vkDestroyShaderModule(device, fs, nullptr);
    if (r != VK_SUCCESS) {
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }

    uint32_t n = uint32_t(game_images.size());
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = n;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    if (p_vkCreateDescriptorPool(device, &dpi, nullptr, &pool_) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayout> layouts(n, set_layout_);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool_;
    dai.descriptorSetCount = n;
    dai.pSetLayouts = layouts.data();
    sets_.resize(n);
    if (p_vkAllocateDescriptorSets(device, &dai, sets_.data()) != VK_SUCCESS) return false;
    for (uint32_t k = 0; k < n; k++) {
        src_views_.push_back(view(device, game_images[k], format));
        VkDescriptorImageInfo ii{sampler_, src_views_[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets_[k];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        p_vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
    return true;
}

bool PresentPass::set_targets(const std::vector<VkImage>& host_images, VkExtent2D extent,
                              VkSurfaceTransformFlagBitsKHR transform) {
    destroy_targets();
    extent_ = extent;
    // The host image holds the game's image turned by `transform` (clockwise); map each host pixel back.
    //   game uv = (m0, m1) * u + (m2, m3) * v + (m4, m5)
    static const float kIdentity[6] = {1, 0, 0, 1, 0, 0};
    static const float kRot90[6] = {0, -1, 1, 0, 0, 1};   // game (x, y) -> host (H - y, x)
    static const float kRot180[6] = {-1, 0, 0, -1, 1, 1};
    static const float kRot270[6] = {0, 1, -1, 0, 1, 0};  // game (x, y) -> host (y, W - x)
    const float* m = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR    ? kRot90
                     : transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? kRot180
                     : transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? kRot270
                                                                            : kIdentity;
    std::memcpy(map_, m, sizeof map_);
    for (VkImage img : host_images) {
        VkImageView v = view(device_, img, format_);
        dst_views_.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &v;
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (p_vkCreateFramebuffer(device_, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        framebuffers_.push_back(fb);
    }
    return true;
}

void PresentPass::record(VkCommandBuffer cb, uint32_t src, uint32_t dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = src_images_[src];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);

    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = render_pass_;
    rb.framebuffer = framebuffers_[dst];
    rb.renderArea = {{0, 0}, extent_};
    p_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D sc{{0, 0}, extent_};
    p_vkCmdSetViewport(cb, 0, 1, &v);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[src], 0, nullptr);
    p_vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof map_, map_);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);

    // Back to the layout the game left it in; later work on it waits for this read.
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
}

void PresentPass::destroy_targets() {
    for (VkFramebuffer f : framebuffers_) p_vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : dst_views_) p_vkDestroyImageView(device_, v, nullptr);
    framebuffers_.clear();
    dst_views_.clear();
}

void PresentPass::destroy() {
    if (!device_) return;
    destroy_targets();
    for (VkImageView v : src_views_) p_vkDestroyImageView(device_, v, nullptr);
    src_views_.clear();
    if (pool_) p_vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pipeline_) p_vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) p_vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (set_layout_) p_vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_) p_vkDestroySampler(device_, sampler_, nullptr);
    if (render_pass_) p_vkDestroyRenderPass(device_, render_pass_, nullptr);
    *this = PresentPass{};
}

}  // namespace nx::vk
