// Swapchain bridges (Hades): the game's swapchains on host ones, a virtual swapchain where the host hands out more
// images than the game's fixed arrays of 3, acquire/present with the window taken away and given back (Android),
// frame timing and NX_SCREENSHOT captures. Split from vk_bridge.cpp (700-line rule).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/game.h>

#include "vk_bridge_shared.h"
#include "vk_present.h"

namespace nx::vk {

namespace {

// ---- screenshots: NX_SCREENSHOT=<frame>[,<frame>...] writes private/captures/frame_<n>.bmp ---------------------

// Hades keeps its per-frame command pools and buffers in fixed arrays of 3 (main+26e428), indexed by the swapchain
// image index. Hosts that hand out more images (Android: 4+) get a virtual swapchain: the game sees 3 plain images
// and each present copies the rendered one into the host image acquired for it.
constexpr uint32_t kMaxGuestImages = 3;

// Where the frame time goes, reported with the fps line: host time spent blocked in fence waits (GPU behind),
// acquire and present, summed over the report interval.
std::atomic<uint64_t> g_wait_fence_ns{0}, g_wait_acquire_ns{0}, g_wait_present_ns{0};

struct Timed {
    std::atomic<uint64_t>& sum;
    uint64_t t0 = plat::monotonic_ns();
    ~Timed() { sum += plat::monotonic_ns() - t0; }
};

struct SwapchainInfo {
    VkDevice device;
    VkFormat format;
    VkExtent2D extent;
    uint32_t layers = 1;
    VkSwapchainKHR host_sc = VK_NULL_HANDLE;  // the current host swapchain (the game's handle is its first one)
    uint64_t surface = 0;                     // the game's surface handle
    VkSwapchainCreateInfoKHR ci{};            // as created (host-adjusted), to re-make it
    std::vector<uint32_t> families;
    uint64_t generation = 0;  // window generation it was made for
    bool lost = false;        // the host said out of date / surface lost: re-make before the next acquire
    VkQueue queue = VK_NULL_HANDLE;  // where the game presents
    PresentPass pass;        // draws the game's image into the host one (rotated); else a plain copy
    bool prerotate = false;  // host swapchain in the display's own orientation
    std::vector<VkImage> images;  // what the game sees (the virtual images when `virt`)
    // Virtual swapchain.
    bool virt = false;
    std::vector<VkImage> host;           // the host swapchain's images
    std::vector<VkDeviceMemory> memory;  // of the virtual images
    std::vector<uint32_t> host_of;       // virtual index -> host index acquired for it
    uint32_t next = 0;                   // next virtual index to hand out
    std::vector<VkSemaphore> copied;     // per host image: its copy is done, present may start
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;  // ring of copy command buffers
    std::vector<VkFence> fences;
    uint32_t slot = 0;
};
std::unordered_map<uint64_t, SwapchainInfo> g_swapchains;
std::unordered_map<uint64_t, std::pair<VkDevice, uint32_t>> g_queues;  // queue -> (device, family)

uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

void destroy_virtual(SwapchainInfo& s) {
    if (!s.virt) return;
    p_vkDeviceWaitIdle(s.device);
    s.pass.destroy();
    for (VkImage i : s.images) p_vkDestroyImage(s.device, i, nullptr);
    for (VkDeviceMemory m : s.memory) p_vkFreeMemory(s.device, m, nullptr);
    for (VkSemaphore x : s.copied) p_vkDestroySemaphore(s.device, x, nullptr);
    for (VkFence f : s.fences) p_vkDestroyFence(s.device, f, nullptr);
    if (s.pool) p_vkDestroyCommandPool(s.device, s.pool, nullptr);
}

void make_copied_semaphores(SwapchainInfo& s);

// Plain images with the swapchain's format, size and usage, standing in for the host swapchain's images.
bool make_virtual(SwapchainInfo& s, const VkSwapchainCreateInfoKHR& ci) {
    s.virt = true;
    s.images.assign(kMaxGuestImages, VK_NULL_HANDLE);
    s.memory.assign(kMaxGuestImages, VK_NULL_HANDLE);
    s.host_of.assign(kMaxGuestImages, 0);
    for (uint32_t k = 0; k < kMaxGuestImages; k++) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ci.imageFormat;
        ii.extent = {ci.imageExtent.width, ci.imageExtent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = ci.imageArrayLayers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (p_vkCreateImage(s.device, &ii, nullptr, &s.images[k]) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        p_vkGetImageMemoryRequirements(s.device, s.images[k], &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_memory_type(bridge::physical(), mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (p_vkAllocateMemory(s.device, &mai, nullptr, &s.memory[k]) != VK_SUCCESS) return false;
        p_vkBindImageMemory(s.device, s.images[k], s.memory[k], 0);
    }
    make_copied_semaphores(s);
    return true;
}

void w_vkGetDeviceQueue(CPU& c) {  // (device, family, index, VkQueue*)
    VkQueue q = VK_NULL_HANDLE;
    p_vkGetDeviceQueue(arg<VkDevice>(c.x[0]), uint32_t(c.x[1]), uint32_t(c.x[2]), &q);
    st<uint64_t>(c.x[3], reinterpret_cast<uint64_t>(q));
    g_queues[reinterpret_cast<uint64_t>(q)] = {arg<VkDevice>(c.x[0]), uint32_t(c.x[1])};
}

// The host's surface capabilities applied to the game's create info (what the Switch driver accepts is not what
// every host surface supports, Android/Adreno especially): keep the game's choices where the surface allows them.
void adapt_to_surface(VkSwapchainCreateInfoKHR& ci) {
    if (!bridge::physical()) return;
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(bridge::physical(), ci.surface, &caps);
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // for screenshots
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // for a virtual swapchain's copies
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                               VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    if (!(caps.supportedTransforms & ci.preTransform)) ci.preTransform = caps.currentTransform;
    ci.minImageCount = std::max(ci.minImageCount, caps.minImageCount);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    uint32_t nm = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(bridge::physical(), ci.surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(bridge::physical(), ci.surface, &nm, modes.data());
    if (std::find(modes.begin(), modes.end(), ci.presentMode) == modes.end()) ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
}

void get_host_images(SwapchainInfo& s) {
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(s.device, s.host_sc, &n, nullptr);
    s.host.resize(n);
    p_vkGetSwapchainImagesKHR(s.device, s.host_sc, &n, s.host.data());
}

void make_copied_semaphores(SwapchainInfo& s) {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    s.copied.assign(s.host.size(), VK_NULL_HANDLE);
    for (VkSemaphore& x : s.copied) p_vkCreateSemaphore(s.device, &sci, nullptr, &x);
}

// A new host surface/swapchain behind the game's handles (virtual swapchains only: the game's images stay).
bool remake(SwapchainInfo& s) {
    p_vkDeviceWaitIdle(s.device);
    for (VkSemaphore x : s.copied) p_vkDestroySemaphore(s.device, x, nullptr);
    s.copied.clear();
    if (s.host_sc) p_vkDestroySwapchainKHR(s.device, s.host_sc, nullptr);
    s.host_sc = VK_NULL_HANDLE;
    VkSurfaceKHR host = bridge::refresh_host_surface(s.surface);
    if (!host) return false;
    s.ci.surface = host;
    s.ci.pQueueFamilyIndices = s.families.empty() ? nullptr : s.families.data();
    adapt_to_surface(s.ci);
    if (s.prerotate) {
        VkSurfaceCapabilitiesKHR caps{};
        p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(bridge::physical(), s.ci.surface, &caps);
        bool sideways = caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
        s.ci.preTransform = caps.currentTransform;
        // The screen's own size (the game may render smaller: the present pass scales).
        uint32_t sw = 0, sh = 0;
        plat::screen_size(sw, sh);
        VkExtent2D screen = sw && sh ? VkExtent2D{sw, sh} : s.extent;
        s.ci.imageExtent = sideways ? VkExtent2D{screen.height, screen.width} : screen;
        s.ci.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    VkResult r = p_vkCreateSwapchainKHR(s.device, &s.ci, nullptr, &s.host_sc);
    s.generation = plat::window_generation();  // what refresh_host_surface just made the surface for
    s.lost = r != VK_SUCCESS;
    if (r != VK_SUCCESS) {
        log("vk: swapchain re-make failed: %d", r);
        s.host_sc = VK_NULL_HANDLE;
        return false;
    }
    get_host_images(s);
    make_copied_semaphores(s);
    s.next = 0;
    if (s.pass.ready() && !s.pass.set_targets(s.host, s.ci.imageExtent, s.ci.preTransform)) s.pass.destroy();
    log("vk: host swapchain %ux%u, transform %u, %zu images%s", s.ci.imageExtent.width, s.ci.imageExtent.height,
        unsigned(s.ci.preTransform), s.host.size(), s.pass.ready() ? ", drawn by the present pass" : ", copied");
    return true;
}

// Suboptimal from a pre-rotated swapchain: re-make it if the display's orientation changed (phone turned over).
void check_orientation(SwapchainInfo& s) {
    if (!s.prerotate || !s.ci.surface) return;
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(bridge::physical(), s.ci.surface, &caps);
    if (caps.currentTransform != s.ci.preTransform) s.lost = true;
}

void w_vkCreateSwapchainKHR(CPU& c) {  // (device, const VkSwapchainCreateInfoKHR*, alloc, VkSwapchainKHR*)
    VkSwapchainCreateInfoKHR ci = *reinterpret_cast<const VkSwapchainCreateInfoKHR*>(c.x[1]);
    uint64_t game_surface = reinterpret_cast<uint64_t>(ci.surface);
    ci.surface = bridge::host_surface(game_surface);
    ci.pNext = nullptr;
    if (ci.oldSwapchain)
        if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(ci.oldSwapchain)); it != g_swapchains.end())
            ci.oldSwapchain = it->second.host_sc;
    // What the Switch driver accepts is not what every host surface supports (Android/Adreno especially): keep
    // the game's choices where the surface allows them, otherwise fall back to what it does.
    adapt_to_surface(ci);
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = p_vkCreateSwapchainKHR(arg<VkDevice>(c.x[0]), &ci, nullptr, &sc);
    log("vkCreateSwapchainKHR %ux%u format %d, %u images, present mode %d -> %d", ci.imageExtent.width,
        ci.imageExtent.height, ci.imageFormat, ci.minImageCount, ci.presentMode, r);
    if (r == VK_SUCCESS) {
        uint64_t game = bridge::new_handle();
        st<uint64_t>(c.x[3], game);
        SwapchainInfo& s = g_swapchains[game];
        s.device = arg<VkDevice>(c.x[0]);
        s.format = ci.imageFormat;
        s.extent = ci.imageExtent;
        s.layers = ci.imageArrayLayers;
        s.host_sc = sc;
        s.surface = game_surface;
        s.ci = ci;
        if (ci.imageSharingMode == VK_SHARING_MODE_CONCURRENT && ci.pQueueFamilyIndices)
            s.families.assign(ci.pQueueFamilyIndices, ci.pQueueFamilyIndices + ci.queueFamilyIndexCount);
        s.generation = plat::window_generation();
        get_host_images(s);
        uint32_t n = uint32_t(s.host.size());
        s.images = s.host;
        // Virtual (the game's own images, copied on present) when the host has more images than the game handles,
        // and wherever the window can be replaced: the host swapchain can then be re-made under the game.
        if (n > kMaxGuestImages || std::strcmp(plat::vulkan_surface_extension(), "VK_KHR_android_surface") == 0) {
            if (!(ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !make_virtual(s, ci))
                fatal("swapchain has %u images, the game handles %u, and no virtual swapchain could be made", n,
                      kMaxGuestImages);
            log("vk: %u host swapchain images, the game sees %u (copied on present)", n, kMaxGuestImages);
            if (std::strcmp(plat::vulkan_surface_extension(), "VK_KHR_android_surface") == 0) {
                uint32_t sw = 0, sh = 0;
                plat::screen_size(sw, sh);
                if (s.pass.init(s.device, s.format, s.images, sw != s.extent.width || sh != s.extent.height)) {
                    s.prerotate = true;
                    remake(s);  // again, in the display's orientation
                } else {
                    s.pass.destroy();
                    log("vk: present pass not available, copying");
                }
            }
        }
    }
    c.x[0] = ret_u64(r);
}

void w_vkGetSwapchainImagesKHR(CPU& c) {  // (device, swapchain, uint32_t* count, VkImage*)
    auto it = g_swapchains.find(c.x[1]);
    if (it == g_swapchains.end()) {
        c.x[0] = ret_u64(p_vkGetSwapchainImagesKHR(arg<VkDevice>(c.x[0]), arg<VkSwapchainKHR>(c.x[1]),
                                                   arg<uint32_t*>(c.x[2]), arg<VkImage*>(c.x[3])));
        return;
    }
    if (!it->second.virt) get_host_images(it->second), it->second.images = it->second.host;
    c.x[0] = ret_u64(bridge::enumerate(it->second.images, c.x[2], c.x[3]));
}

void w_vkDestroySwapchainKHR(CPU& c) {  // (device, swapchain, alloc)
    auto it = g_swapchains.find(c.x[1]);
    if (it == g_swapchains.end()) {
        p_vkDestroySwapchainKHR(arg<VkDevice>(c.x[0]), arg<VkSwapchainKHR>(c.x[1]), nullptr);
        return;
    }
    destroy_virtual(it->second);
    if (it->second.host_sc) p_vkDestroySwapchainKHR(it->second.device, it->second.host_sc, nullptr);
    g_swapchains.erase(it);
}

std::vector<uint64_t> screenshot_frames() {
    std::vector<uint64_t> v;
    if (const char* e = std::getenv("NX_SCREENSHOT"))
        for (const char* p = e; *p;) {
            v.push_back(std::strtoull(p, const_cast<char**>(&p), 10));
            if (*p == ',') p++;
        }
    return v;
}

// Copies the image about to be presented into a host buffer (waiting on the game's semaphores and signalling a
// new one the present waits on instead), then writes a BMP.
void capture(VkQueue queue, VkPresentInfoKHR& pi, uint64_t frame, VkSemaphore& out_sem) {
    auto sit = g_swapchains.find(reinterpret_cast<uint64_t>(pi.pSwapchains[0]));
    auto qit = g_queues.find(reinterpret_cast<uint64_t>(queue));
    if (sit == g_swapchains.end() || qit == g_queues.end() || !bridge::physical()) return;
    SwapchainInfo& s = sit->second;
    VkDevice dev = s.device;
    VkImage img = s.images[pi.pImageIndices[0]];
    uint32_t w = s.extent.width, h = s.extent.height;
    VkDeviceSize size = VkDeviceSize(w) * h * 4;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    p_vkCreateBuffer(dev, &bci, nullptr, &buf);
    VkMemoryRequirements mr;
    p_vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory_type(bridge::physical(), mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    p_vkAllocateMemory(dev, &mai, nullptr, &mem);
    p_vkBindBufferMemory(dev, buf, mem, 0);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = qit->second.second;
    VkCommandPool pool;
    p_vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    p_vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    p_vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    std::swap(b.oldLayout, b.newLayout);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    p_vkEndCommandBuffer(cb);

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(dev, &sci, nullptr, &out_sem);
    std::vector<VkPipelineStageFlags> stages(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi.waitSemaphoreCount;
    si.pWaitSemaphores = pi.pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &out_sem;
    p_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(queue);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &out_sem;

    void* data = nullptr;
    p_vkMapMemory(dev, mem, 0, size, 0, &data);
    std::string dir = plat::data_root() + "/captures";
    plat::make_dirs(dir);
    std::string path = dir + "/frame_" + std::to_string(frame) + ".bmp";
    if (FILE* f = plat::fopen_utf8(path, "wb")) {
        // BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), little endian, top-down 32-bit.
        uint8_t hdr[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
        put32(2, uint32_t(54 + size));
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, uint32_t(-int32_t(h)));
        hdr[26] = 1;
        hdr[28] = 32;
        std::fwrite(hdr, 1, sizeof hdr, f);
        const uint8_t* px = static_cast<const uint8_t*>(data);
        bool rgba = s.format == VK_FORMAT_R8G8B8A8_UNORM || s.format == VK_FORMAT_R8G8B8A8_SRGB;
        std::vector<uint8_t> row(size_t(w) * 4);
        for (uint32_t y = 0; y < h; y++) {
            std::memcpy(row.data(), px + size_t(y) * w * 4, row.size());
            if (rgba)
                for (uint32_t x = 0; x < w; x++) std::swap(row[x * 4], row[x * 4 + 2]);  // BMP wants BGRA
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        log("screenshot: %s (%ux%u, format %d)", path.c_str(), w, h, s.format);
    }
    p_vkUnmapMemory(dev, mem);
    p_vkDestroyCommandPool(dev, pool, nullptr);
    p_vkDestroyBuffer(dev, buf, nullptr);
    p_vkFreeMemory(dev, mem, nullptr);
}

// A minimized window has no surface to draw to; the Switch never loses its screen, so the game does not handle
// VK_ERROR_OUT_OF_DATE_KHR. Hold the render thread while minimized (the game pauses, like a console asleep).
void wait_while_minimized() {
    while (plat::window_minimized()) plat::sleep_ns(50'000'000);
}

void w_vkWaitForFences(CPU& c) {  // (device, count, const VkFence*, waitAll, timeout)
    Timed t{g_wait_fence_ns};
    c.x[0] = ret_u64(p_vkWaitForFences(arg<VkDevice>(c.x[0]), uint32_t(c.x[1]), arg<const VkFence*>(c.x[2]),
                                       VkBool32(c.x[3]), c.x[4]));
}

void w_vkAcquireNextImageKHR(CPU& c) {  // (device, swapchain, timeout, semaphore, fence, uint32_t* index)
    wait_while_minimized();
    Timed timed{g_wait_acquire_ns};
    // The game's semaphore and fence go to the host acquire: they signal when the host image is free, which is
    // what the copy on present writes. The game gets the next virtual image.
    auto it = g_swapchains.find(c.x[1]);
    SwapchainInfo* s = it != g_swapchains.end() && it->second.virt ? &it->second : nullptr;
    uint32_t host = 0;
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    if (!s) {
        VkSwapchainKHR sc = it != g_swapchains.end() ? it->second.host_sc : arg<VkSwapchainKHR>(c.x[1]);
        r = p_vkAcquireNextImageKHR(arg<VkDevice>(c.x[0]), sc, c.x[2], arg<VkSemaphore>(c.x[3]), arg<VkFence>(c.x[4]),
                                    arg<uint32_t*>(c.x[5]));
    } else {
        // Window replaced or swapchain lost: re-make it under the game (waiting for a window as long as it takes).
        for (int attempt = 0; attempt < 50; attempt++) {
            wait_while_minimized();
            if (s->lost || !s->host_sc || s->generation != plat::window_generation()) {
                if (!remake(*s)) {
                    plat::sleep_ns(100'000'000);
                    continue;
                }
            }
            r = p_vkAcquireNextImageKHR(s->device, s->host_sc, c.x[2], arg<VkSemaphore>(c.x[3]), arg<VkFence>(c.x[4]), &host);
            if (r == VK_SUBOPTIMAL_KHR) check_orientation(*s);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_TIMEOUT || r == VK_NOT_READY) break;
            log("vkAcquireNextImageKHR -> %d: re-making the swapchain", r);
            s->lost = true;
        }
    }
    if (s && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
        uint32_t v = s->next;
        s->next = (v + 1) % kMaxGuestImages;
        s->host_of[v] = host;
        st<uint32_t>(c.x[5], v);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) log("vkAcquireNextImageKHR -> %d", r);
    // The game does not handle out-of-date (a console never loses its screen): suboptimal is success for it.
    c.x[0] = ret_u64(r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r);
}

// Records and submits the copies of a present's virtual images into their host images: waits on the game's
// semaphores, signals the per-host-image `copied` semaphores the host present waits on instead, and rewrites the
// image indices to the host ones.
void copy_virtual(VkQueue queue, VkPresentInfoKHR& pi, std::vector<uint32_t>& indices, std::vector<VkSemaphore>& waits) {
    SwapchainInfo* first = nullptr;
    for (uint32_t k = 0; k < pi.swapchainCount; k++) {
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(pi.pSwapchains[k]));
        if (it != g_swapchains.end() && it->second.virt) {
            first = &it->second;
            break;
        }
    }
    if (!first) return;
    VkDevice dev = first->device;
    if (!first->pool) {
        auto q = g_queues.find(reinterpret_cast<uint64_t>(queue));
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = q != g_queues.end() ? q->second.second : 0;
        p_vkCreateCommandPool(dev, &pci, nullptr, &first->pool);
        first->cmds.resize(kMaxGuestImages);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = first->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kMaxGuestImages;
        p_vkAllocateCommandBuffers(dev, &cai, first->cmds.data());
        first->fences.resize(kMaxGuestImages);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (VkFence& f : first->fences) p_vkCreateFence(dev, &fci, nullptr, &f);
    }
    uint32_t slot = first->slot;
    first->slot = (slot + 1) % kMaxGuestImages;
    VkFence fence = first->fences[slot];
    VkCommandBuffer cb = first->cmds[slot];
    p_vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    p_vkResetFences(dev, 1, &fence);
    p_vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &bi);

    indices.assign(pi.pImageIndices, pi.pImageIndices + pi.swapchainCount);
    std::vector<VkSemaphore> signals;
    for (uint32_t k = 0; k < pi.swapchainCount; k++) {
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(pi.pSwapchains[k]));
        if (it == g_swapchains.end() || !it->second.virt) continue;
        SwapchainInfo& s = it->second;
        uint32_t v = indices[k], h = s.host_of[v];
        if (s.pass.ready()) {
            s.pass.record(cb, v, h);
            indices[k] = h;
            signals.push_back(s.copied[h]);
            continue;
        }
        VkImage src = s.images[v], dst = s.host[h];
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s.layers};
        auto barrier = [&](VkImage img, VkAccessFlags sa, VkAccessFlags da, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = sa;
            b.dstAccessMask = da;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = range;
            return b;
        };
        VkImageMemoryBarrier pre[2] = {
            barrier(src, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            barrier(dst, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 2, pre);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.layers};
        region.extent = {s.extent.width, s.extent.height, 1};
        p_vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);
        // The virtual image goes back to the layout the game left it in; later work on it (next frames) waits
        // for the copy through this barrier.
        VkImageMemoryBarrier post[2] = {
            barrier(src, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
            barrier(dst, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        };
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                               nullptr, 2, post);
        indices[k] = h;
        signals.push_back(s.copied[h]);
    }
    p_vkEndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stages(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi.waitSemaphoreCount;
    si.pWaitSemaphores = pi.pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = uint32_t(signals.size());
    si.pSignalSemaphores = signals.data();
    p_vkQueueSubmit(queue, 1, &si, fence);
    waits = std::move(signals);
    pi.waitSemaphoreCount = uint32_t(waits.size());
    pi.pWaitSemaphores = waits.data();
    pi.pImageIndices = indices.data();
}

// The present info with the host swapchains behind the game's handles; records which queue presents.
void to_host_swapchains(VkQueue queue, VkPresentInfoKHR& pi, std::vector<VkSwapchainKHR>& out) {
    out.assign(pi.pSwapchains, pi.pSwapchains + pi.swapchainCount);
    for (VkSwapchainKHR& sc : out)
        if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(sc)); it != g_swapchains.end()) {
            sc = it->second.host_sc;
            it->second.queue = queue;
        }
    pi.pSwapchains = out.data();
}

void w_vkQueuePresentKHR(CPU& c) {
    wait_while_minimized();
    static uint64_t frames = 0;
    static auto t0 = std::chrono::steady_clock::now();
    static const std::vector<uint64_t> shots = screenshot_frames();
    VkQueue queue = arg<VkQueue>(c.x[0]);
    VkPresentInfoKHR pi = *arg<const VkPresentInfoKHR*>(c.x[1]);
    VkSemaphore extra = VK_NULL_HANDLE;
    if (std::find(shots.begin(), shots.end(), frames + 1) != shots.end()) capture(queue, pi, frames + 1, extra);
    std::vector<uint32_t> host_indices;
    std::vector<VkSemaphore> copied;
    copy_virtual(queue, pi, host_indices, copied);
    const VkSwapchainKHR* game_swapchains = pi.pSwapchains;
    std::vector<VkSwapchainKHR> host_swapchains;
    to_host_swapchains(queue, pi, host_swapchains);
    uint64_t tp = plat::monotonic_ns();
    VkResult r = p_vkQueuePresentKHR(queue, &pi);
    g_wait_present_ns += plat::monotonic_ns() - tp;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        // The window went away under us (or changed): re-make at the next acquire; the game never sees it.
        for (uint32_t k = 0; k < pi.swapchainCount; k++)
            if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(game_swapchains[k])); it != g_swapchains.end())
                it->second.lost = true;
        log("vkQueuePresentKHR -> %d: swapchain will be re-made", r);
        r = VK_SUCCESS;
    } else if (r == VK_SUBOPTIMAL_KHR) {
        for (uint32_t k = 0; k < pi.swapchainCount; k++)
            if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(game_swapchains[k])); it != g_swapchains.end())
                check_orientation(it->second);
        r = VK_SUCCESS;
    }
    nx::g_frame = frames + 1;
    if (extra) {
        p_vkQueueWaitIdle(queue);
        p_vkDestroySemaphore(g_swapchains.begin()->second.device, extra, nullptr);
    }
    // Worst frame (time between presents) of the interval: what is felt as stutter.
    static uint64_t prev_present = 0, worst = 0;
    uint64_t now_ns = plat::monotonic_ns();
    if (prev_present) worst = std::max(worst, now_ns - prev_present);
    prev_present = now_ns;
    if (frames == 0) game::first_frame();
    if (++frames == 1 || frames % 300 == 0) {
        static auto last = t0;
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - t0).count(), ds = std::chrono::duration<double>(now - last).count();
        auto per_frame = [](std::atomic<uint64_t>& a) { return a.exchange(0) / 300.0 / 1e6; };
        log("vk: frame %llu presented (%d), %.1f fps (last %.1f s), %.1f average; worst %.1f ms; per frame: "
            "fences %.2f ms, acquire %.2f ms, present %.2f ms",
            (unsigned long long)frames, r, frames == 1 ? 0.0 : 300 / ds, ds, frames / s, worst / 1e6,
            per_frame(g_wait_fence_ns), per_frame(g_wait_acquire_ns), per_frame(g_wait_present_ns));
        last = now;
        worst = 0;
    }
    c.x[0] = ret_u64(r);
}

}  // namespace

NX_HLE("vkQueuePresentKHR", w_vkQueuePresentKHR);
NX_HLE("vkAcquireNextImageKHR", w_vkAcquireNextImageKHR);
NX_HLE("vkWaitForFences", w_vkWaitForFences);
NX_HLE("vkGetDeviceQueue", w_vkGetDeviceQueue);
NX_HLE("vkCreateSwapchainKHR", w_vkCreateSwapchainKHR);
NX_HLE("vkGetSwapchainImagesKHR", w_vkGetSwapchainImagesKHR);
NX_HLE("vkDestroySwapchainKHR", w_vkDestroySwapchainKHR);

}  // namespace nx::vk
