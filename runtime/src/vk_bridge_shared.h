// What vk_bridge.cpp (loader, instance, device, surfaces) and vk_swapchain.cpp (swapchains, acquire, present,
// screenshots) share. Split from vk_bridge.cpp (700-line rule).
#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "vk_gen.h"

namespace nx::vk::bridge {

VkPhysicalDevice physical();               // the GPU the game was given (vkEnumeratePhysicalDevices)
VkSurfaceKHR host_surface(uint64_t game);  // the host surface behind a game handle, re-made on a new window
// The host surface behind a game handle after re-making it if the OS replaced the window; VK_NULL_HANDLE when the
// handle is unknown or there is no window.
VkSurfaceKHR refresh_host_surface(uint64_t game);
uint64_t new_handle();  // a game-visible surface or swapchain handle (ours, never a host value)

// Two-call enumeration helper: fills the guest array from `all`.
template <typename T>
VkResult enumerate(const std::vector<T>& all, uint64_t count_ptr, uint64_t out) {
    uint32_t n = ld<uint32_t>(count_ptr);
    if (!out) {
        st<uint32_t>(count_ptr, uint32_t(all.size()));
        return VK_SUCCESS;
    }
    uint32_t w = std::min<uint32_t>(n, uint32_t(all.size()));
    std::memcpy(reinterpret_cast<void*>(out), all.data(), sizeof(T) * w);
    st<uint32_t>(count_ptr, w);
    return w < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

}  // namespace nx::vk::bridge
