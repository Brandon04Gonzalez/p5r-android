// The 1x1 stand-in images sampled where a texture has no usable image (nvn_pipeline.h). Split from nvn_pipeline.cpp
// (700-line rule).
#include <cstdlib>
#include <map>

#include "nvn_pipeline.h"

namespace nx::nvn {

using namespace nx::vk;

std::map<VkImageViewType, GpuImage>& dummies() {
    static std::map<VkImageViewType, GpuImage> d;
    return d;
}

bool dummy_ready(const ImageShape& shape) {
    auto it = dummies().find(view_type(shape));
    return it != dummies().end() && it->second.image;
}

VkImageView dummy_view(VkCommandBuffer cb, const ImageShape& shape) {
    VkImageViewType type = view_type(shape);
    auto& d = dummies()[type];
    if (d.image) return d.view;
    bool cube = type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                   : (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D
                                                                                            : VK_IMAGE_TYPE_2D;
    ci.format = shape.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = cube ? 6 : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
    p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
    d.format = ci.format;
    d.aspect = shape.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    d.layers = ci.arrayLayers;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = d.image;
    vi.viewType = type;
    vi.format = ci.format;
    vi.subresourceRange = {d.aspect, 0, 1, 0, ci.arrayLayers};
    p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
    transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{d.aspect, 0, 1, 0, ci.arrayLayers};
    if (shape.depth) {
        VkClearDepthStencilValue v{1.0f, 0};
        p_vkCmdClearDepthStencilImage(cb, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    } else {
        VkClearColorValue v{};
        if (std::getenv("NX_DUMMY_MAGENTA")) v = {{1.0f, 0.0f, 1.0f, 1.0f}};  // debug: where the dummy is sampled
        p_vkCmdClearColorImage(cb, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
    transition(cb, d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return d.view;
}

}  // namespace nx::nvn
