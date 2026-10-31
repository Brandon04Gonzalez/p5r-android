// What the device offers that the Android performance plan (docs/nvn/android-perf/PLAN.md, phase 0) decides on:
// descriptor and pipeline extensions (R3, P1), dynamic state, present timing (P2), and the memory types (R5: is there
// a host-cached one for reading compute results back). Logged once, when the device is made.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

void log_device_caps(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& extensions) {
    static const char* const wanted[] = {
        "VK_KHR_push_descriptor",          "VK_EXT_descriptor_buffer",        "VK_KHR_descriptor_update_template",
        "VK_EXT_graphics_pipeline_library", "VK_EXT_robustness2",              "VK_EXT_extended_dynamic_state",
        "VK_EXT_extended_dynamic_state2",  "VK_EXT_extended_dynamic_state3",  "VK_GOOGLE_display_timing",
        "VK_KHR_present_wait",             "VK_KHR_present_id",               "VK_EXT_attachment_feedback_loop_layout",
        "VK_KHR_dynamic_rendering_local_read", "VK_QCOM_render_pass_store_ops", "VK_EXT_load_store_op_none",
    };
    std::string have, missing;
    for (const char* w : wanted) {
        bool found = false;
        for (const VkExtensionProperties& e : extensions) found = found || std::strcmp(e.extensionName, w) == 0;
        std::string& list = found ? have : missing;
        if (!list.empty()) list += " ";
        list += w;
    }
    log("nvn/vk: extensions present: %s", have.c_str());
    log("nvn/vk: extensions missing: %s", missing.c_str());
    VkPhysicalDeviceMemoryProperties mp{};
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    std::string types;
    for (uint32_t k = 0; k < mp.memoryTypeCount; k++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[k].propertyFlags;
        types += " " + std::to_string(k) + ":";
        if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) types += "D";
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) types += "V";
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) types += "C";
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) types += "$";
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) types += "L";
        types += "/h" + std::to_string(mp.memoryTypes[k].heapIndex);
    }
    log("nvn/vk: memory types (D device-local, V host-visible, C coherent, $ cached, L lazy):%s", types.c_str());
}

// NVN's R11G11B10F (0x3f) as B10G11R11_UFLOAT_PACK32 (docs/nvn/android-perf/PHASE4.md G2): the same bits (R in 0-10,
// G 11-21, B 22-31), half the bytes of the RGBA16F it is stored as elsewhere, for every load, store and sample of the
// HDR chain. Vulkan converts negative results to 0 as Maxwell does; Metal wrote Inf/NaN into it (the green screens of
// nvn_format.cpp), so MoltenVK keeps RGBA16F. Android, when the format can be rendered to, blended, sampled with a
// linear filter and copied; NX_R11G11B10_EMULATE=1: RGBA16F there too.
bool native_r11g11b10() {
    static std::atomic<int> state{-1};
    int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s == 1;
#if defined(__ANDROID__)
    if (!vk_physical()) return false;  // asked before the device exists: not decided yet
    // Mali GPUs produce NaNs and green/purple flickering with B10G11R11_UFLOAT; keep RGBA16F
    if (is_mali()) {
        log("nvn/vk: Mali GPU detected: R11G11B10F kept as RGBA16F to prevent green/purple flickering and bloom overexposure");
        state.store(0, std::memory_order_release);
        return false;
    }
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormatProperties fp{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp);
    bool on = (fp.optimalTilingFeatures & need) == need && !std::getenv("NX_R11G11B10_EMULATE");
    log("nvn/vk: R11G11B10F %s (format features %#x)", on ? "native (B10G11R11_UFLOAT)" : "as RGBA16F",
        unsigned(fp.optimalTilingFeatures));
#else
    bool on = false;
#endif
    state.store(on ? 1 : 0, std::memory_order_release);
    return on;
}

// NX_D24S8=1: NVN's D24S8 (0x35) as D24_UNORM_S8_UINT where the GPU renders, samples and copies it, instead of
// D32_SFLOAT_S8_UINT (made for the Apple M1, which has no D24 formats). On a tiler that is a byte per pixel less in
// every load and store and in the tile memory (Adreno keeps the stencil apart: 5 bytes), and polygon offsets act on a
// 24-bit integer depth as the game set them. Off by default: P5R's depth is D32F_S8 (0x36), so it was not tried
// (PROGRESO 77). Buffer copies of it stay unsupported, as they were (vk_block_bytes 0).
bool native_d24s8() {
    static std::atomic<int> state{-1};
    int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s == 1;
    if (!vk_physical()) return false;  // asked before the device exists: not decided yet
    const char* e = std::getenv("NX_D24S8");
    const bool wanted = e && e[0] == '1';
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormatProperties fp{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    const bool on = wanted && (fp.optimalTilingFeatures & need) == need;
    if (wanted) log("nvn/vk: D24S8 %s (format features %#x)", on ? "native (D24_UNORM_S8_UINT)" : "as D32S8",
                    unsigned(fp.optimalTilingFeatures));
    state.store(on ? 1 : 0, std::memory_order_release);
    return on;
}

// Image flags that keep Adreno's framebuffer compression (PHASE4.md G1a): MUTABLE_FORMAT only with a format list.
// Android by default (NX_UBWC_FLAGS=0: every colour image mutable, as before); elsewhere NX_UBWC_FLAGS=1.
bool ubwc_flags() {
    static const bool on = [] {
        const char* e = std::getenv("NX_UBWC_FLAGS");
#if defined(__ANDROID__)
        return !(e && e[0] == '0');
#else
        return e && e[0] == '1';
#endif
    }();
    return on;
}

// The upload ring takes ~11 MB of CPU writes per course frame (copies and zero padding). Host-visible coherent memory
// is write-combined (uncached) on Adreno; NX_RING_CACHED=1 asks for a host-cached coherent type first, where the GPU
// snoops the CPU's caches (PLAN.md R5).
uint32_t upload_memory_type(uint32_t bits) {
    const VkMemoryPropertyFlags plain = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const char* e = std::getenv("NX_RING_CACHED");
    if (e && e[0] == '1') {
        VkPhysicalDeviceMemoryProperties mp{};
        p_vkGetPhysicalDeviceMemoryProperties(vk_physical(), &mp);
        for (uint32_t k = 0; k < mp.memoryTypeCount; k++)
            if (bits >> k & 1 && (mp.memoryTypes[k].propertyFlags & (plain | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                                     (plain | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                log("nvn/vk: upload ring in host-cached memory (type %u)", k);
                return k;
            }
    }
    return memory_type(bits, plain);
}

}  // namespace nx::nvn
