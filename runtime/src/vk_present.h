// The present pass of a virtual swapchain: draws the game's image (a virtual swapchain image) into the host
// swapchain image, rotated to the display's own orientation. Phones scan out portrait; a landscape image would be
// rotated by the compositor, which falls back to the GPU for that whenever system UI (volume, notifications) shows
// on top. Pre-rotated, the display controller takes it as is.
#pragma once

#include <vector>

#include "vk_bridge.h"

namespace nx::vk {

class PresentPass {
public:
    // Pipeline and one sampling view per game image. False: not available (the caller copies instead).
    bool init(VkDevice device, VkFormat format, const std::vector<VkImage>& game_images, bool scaled);
    // Views and framebuffers of the host images, and the mapping for `transform` (the swapchain's preTransform).
    bool set_targets(const std::vector<VkImage>& host_images, VkExtent2D extent, VkSurfaceTransformFlagBitsKHR transform);
    // Draws game image `src` into host image `dst` (host image: undefined -> present src; game image: present src
    // -> sampled -> present src).
    void record(VkCommandBuffer cb, uint32_t src, uint32_t dst);
    void destroy_targets();
    void destroy();
    bool ready() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkImage> src_images_;
    std::vector<VkImageView> src_views_;
    std::vector<VkDescriptorSet> sets_;
    std::vector<VkImageView> dst_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkExtent2D extent_{};
    float map_[6] = {1, 0, 0, 1, 0, 0};  // host uv -> game uv: mat2 columns (m0 m1)(m2 m3), offset (m4 m5)
};

}  // namespace nx::vk
