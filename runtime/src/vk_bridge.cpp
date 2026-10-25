// Hand-written Vulkan bridges: loader, proc-address lookups (they hand out guest-callable thunks), instance and
// device creation (extension filtering), the Switch surface (VK_NN_vi_surface -> VK_KHR_win32_surface), physical
// device order (discrete GPU first) and debug callbacks (guest function pointers the host must never call).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nx/game.h>

#include "vk_bridge_shared.h"
#include "vk_present.h"

namespace nx {
}

namespace nx::vk {

namespace {

PFN_vkGetInstanceProcAddr host_gipa = nullptr;
PFN_vkCreateInstance host_create_instance = nullptr;
PFN_vkEnumerateInstanceExtensionProperties host_enum_inst_ext = nullptr;
PFN_vkEnumerateInstanceLayerProperties host_enum_inst_layers = nullptr;
PFN_vkDestroyInstance host_destroy_instance = nullptr;
PFN_vkCreateDevice host_create_device = nullptr;
PFN_vkGetDeviceProcAddr host_gdpa = nullptr;
PFN_vkEnumeratePhysicalDevices host_enum_phys = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;

const std::unordered_set<std::string> kSpecial = {
    "vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkCreateInstance", "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateInstanceLayerProperties", "vkCreateDevice", "vkCreateViSurfaceNN", "vkCreateDebugUtilsMessengerEXT",
    "vkCreateDebugReportCallbackEXT", "vkDestroyInstance", "vkEnumeratePhysicalDevices",
    "vkDestroyDebugUtilsMessengerEXT", "vkDestroyDebugReportCallbackEXT", "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceQueueFamilyProperties2", "vkGetPhysicalDeviceQueueFamilyProperties2KHR", "vkQueuePresentKHR", "vkGetDeviceQueue", "vkCreateSwapchainKHR",
    "vkAcquireNextImageKHR", "vkGetSwapchainImagesKHR", "vkDestroySwapchainKHR", "vkWaitForFences",
    "vkGetPhysicalDeviceSurfaceSupportKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceFormatsKHR", "vkGetPhysicalDeviceSurfacePresentModesKHR", "vkDestroySurfaceKHR",
};

void load_loader() {
    if (host_gipa) return;
    host_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!host_gipa) fatal("no Vulkan loader");
    host_create_instance = reinterpret_cast<PFN_vkCreateInstance>(host_gipa(nullptr, "vkCreateInstance"));
    host_enum_inst_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        host_gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    host_enum_inst_layers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        host_gipa(nullptr, "vkEnumerateInstanceLayerProperties"));
    // Global commands of the generated table.
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = host_gipa(nullptr, g_entries[k].name);
}

void load_instance(VkInstance inst) {
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = host_gipa(inst, g_entries[k].name)) *g_entries[k].pfn = p;
    host_destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(host_gipa(inst, "vkDestroyInstance"));
    host_create_device = reinterpret_cast<PFN_vkCreateDevice>(host_gipa(inst, "vkCreateDevice"));
    host_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(host_gipa(inst, "vkGetDeviceProcAddr"));
    host_enum_phys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(host_gipa(inst, "vkEnumeratePhysicalDevices"));
}

std::string guest_str(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// Guest-callable address for a command we bridge, or 0.
uint64_t proc_address(const std::string& name, bool host_has) {
    if (kSpecial.count(name) || (host_has && hle_lookup(name.c_str()))) return hle_address(name.c_str());
    return 0;
}

// ---- instance -------------------------------------------------------------------------------------------------

void w_vkGetInstanceProcAddr(CPU& c) {
    load_loader();
    std::string name = guest_str(c.x[1]);
    VkInstance inst = arg<VkInstance>(c.x[0]);
    bool host_has = host_gipa(inst ? inst : g_instance, name.c_str()) != nullptr;
    c.x[0] = proc_address(name, host_has);
    if (g_trace || !c.x[0]) log("vkGetInstanceProcAddr(%s) -> %s", name.c_str(), c.x[0] ? "bridged" : "null");
}

void w_vkGetDeviceProcAddr(CPU& c) {
    std::string name = guest_str(c.x[1]);
    bool host_has = host_gdpa && host_gdpa(arg<VkDevice>(c.x[0]), name.c_str()) != nullptr;
    c.x[0] = proc_address(name, host_has);
    if (g_trace || !c.x[0]) log("vkGetDeviceProcAddr(%s) -> %s", name.c_str(), c.x[0] ? "bridged" : "null");
}

void w_vkEnumerateInstanceExtensionProperties(CPU& c) {  // (layer, uint32_t* count, props*)
    load_loader();
    uint32_t n = 0;
    host_enum_inst_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> all(n);
    host_enum_inst_ext(nullptr, &n, all.data());
    all.resize(n);
    VkExtensionProperties vi{};
    std::strcpy(vi.extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME);
    vi.specVersion = 1;
    all.push_back(vi);  // the game asks for the Switch surface; we provide it on top of win32
    c.x[0] = ret_u64(bridge::enumerate(all, c.x[1], c.x[2]));
}

void w_vkEnumerateInstanceLayerProperties(CPU& c) {
    load_loader();
    st<uint32_t>(c.x[0], 0);  // the game runs without layers; validation is added by the host side on request
    c.x[0] = VK_SUCCESS;
}

void w_vkCreateInstance(CPU& c) {  // (const VkInstanceCreateInfo*, alloc, VkInstance*)
    load_loader();
    game::renderer_start();
    VkInstanceCreateInfo ci = *reinterpret_cast<const VkInstanceCreateInfo*>(c.x[0]);
    uint32_t nhave = 0;
    host_enum_inst_ext(nullptr, &nhave, nullptr);
    std::vector<VkExtensionProperties> have(nhave);
    host_enum_inst_ext(nullptr, &nhave, have.data());
    auto host_has = [&](const char* e) {
        return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& h) { return std::strcmp(h.extensionName, e) == 0; });
    };
    std::vector<const char*> exts;
    for (uint32_t k = 0; k < ci.enabledExtensionCount; k++) {
        const char* e = ci.ppEnabledExtensionNames[k];
        bool ok = std::strcmp(e, VK_NN_VI_SURFACE_EXTENSION_NAME) != 0 && host_has(e);
        log("vkCreateInstance: game wants %s%s", e, ok ? "" : " (not on this host, dropped)");
        if (ok) exts.push_back(e);
    }
    auto add = [&](const char* e) {
        if (std::none_of(exts.begin(), exts.end(), [&](const char* x) { return std::strcmp(x, e) == 0; })) exts.push_back(e);
    };
    add(VK_KHR_SURFACE_EXTENSION_NAME);
    add(plat::vulkan_surface_extension());
    std::vector<const char*> layers;
    if (const char* v = std::getenv("NX_VK_VALIDATION"); v && *v == '1') layers.push_back("VK_LAYER_KHRONOS_validation");
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    ci.pNext = nullptr;  // debug messenger infos carry guest callbacks
    // With validation on, report through a host messenger.
    static VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    if (!layers.empty()) {
        add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        ci.enabledExtensionCount = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                 const VkDebugUtilsMessengerCallbackDataEXT* d, void*) -> VkBool32 {
            log("[vk] %s", d->pMessage);
            return VK_FALSE;
        };
        ci.pNext = &dbg;
    }
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = host_create_instance(&ci, nullptr, &inst);
    log("vkCreateInstance -> %d (%zu extensions, %zu layers)", r, exts.size(), layers.size());
    if (r == VK_SUCCESS) {
        g_instance = inst;
        load_instance(inst);
        st<uint64_t>(c.x[2], reinterpret_cast<uint64_t>(inst));
    }
    c.x[0] = ret_u64(r);
}

void w_vkDestroyInstance(CPU& c) {
    if (host_destroy_instance) host_destroy_instance(arg<VkInstance>(c.x[0]), nullptr);
}

void w_vkEnumeratePhysicalDevices(CPU& c) {  // (instance, uint32_t* count, VkPhysicalDevice*)
    VkInstance inst = arg<VkInstance>(c.x[0]);
    uint32_t n = 0;
    host_enum_phys(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    host_enum_phys(inst, &n, all.data());
    // Discrete GPUs first: the game takes the first device, like the single GPU of a Switch.
    std::stable_sort(all.begin(), all.end(), [](VkPhysicalDevice a, VkPhysicalDevice b) {
        VkPhysicalDeviceProperties pa, pb;
        p_vkGetPhysicalDeviceProperties(a, &pa);
        p_vkGetPhysicalDeviceProperties(b, &pb);
        return (pa.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) > (pb.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    });
    // Only one: a Switch has one GPU, and The Forge's Switch path leaves per-GPU arrays unset for the others.
    if (all.size() > 1) all.resize(1);
    if (!all.empty()) {
        g_physical = all[0];
        VkPhysicalDeviceProperties p;
        p_vkGetPhysicalDeviceProperties(all[0], &p);
        log("vk: using %s (api %u.%u.%u)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    c.x[0] = ret_u64(bridge::enumerate(all, c.x[1], c.x[2]));
}

// The Forge's Switch build keeps queue create infos in a 4-entry array; desktop GPUs expose more families
// (video decode/encode, optical flow). Report the first three: indices stay valid, family 0 does everything.
constexpr uint32_t kMaxQueueFamilies = 3;

void w_vkGetPhysicalDeviceQueueFamilyProperties(CPU& c) {  // (physical, uint32_t* count, props*)
    uint32_t n = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(arg<VkPhysicalDevice>(c.x[0]), &n, nullptr);
    std::vector<VkQueueFamilyProperties> all(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(arg<VkPhysicalDevice>(c.x[0]), &n, all.data());
    all.resize(std::min(n, kMaxQueueFamilies));
    bridge::enumerate(all, c.x[1], c.x[2]);
}

void w_vkGetPhysicalDeviceQueueFamilyProperties2(CPU& c) {  // (physical, uint32_t* count, VkQueueFamilyProperties2*)
    uint32_t n = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties2(arg<VkPhysicalDevice>(c.x[0]), &n, nullptr);
    uint32_t want = ld<uint32_t>(c.x[1]);
    n = std::min(n, kMaxQueueFamilies);
    if (!c.x[2]) {
        st<uint32_t>(c.x[1], n);
        return;
    }
    uint32_t w = std::min(want, n);
    p_vkGetPhysicalDeviceQueueFamilyProperties2(arg<VkPhysicalDevice>(c.x[0]), &w,
                                                reinterpret_cast<VkQueueFamilyProperties2*>(c.x[2]));
    st<uint32_t>(c.x[1], w);
}

// ---- device ---------------------------------------------------------------------------------------------------

void w_vkCreateDevice(CPU& c) {  // (physical, const VkDeviceCreateInfo*, alloc, VkDevice*)
    VkPhysicalDevice pd = arg<VkPhysicalDevice>(c.x[0]);
    VkDeviceCreateInfo ci = *reinterpret_cast<const VkDeviceCreateInfo*>(c.x[1]);
    uint32_t n = 0;
    p_vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    p_vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, have.data());
    std::vector<const char*> exts;
    for (uint32_t k = 0; k < ci.enabledExtensionCount; k++) {
        const char* e = ci.ppEnabledExtensionNames[k];
        bool ok = std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& h) { return std::strcmp(h.extensionName, e) == 0; });
        log("vkCreateDevice: game wants %s%s", e, ok ? "" : " (not on this GPU, dropped)");
        if (ok) exts.push_back(e);
    }
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = 0;
    ci.ppEnabledLayerNames = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = host_create_device(pd, &ci, nullptr, &dev);
    log("vkCreateDevice -> %d", r);
    if (r == VK_SUCCESS) st<uint64_t>(c.x[3], reinterpret_cast<uint64_t>(dev));
    c.x[0] = ret_u64(r);
}

// ---- surface and debug ----------------------------------------------------------------------------------------

// The OS can take the window away (Android: home, app switcher, screen lock) and hand out a new one; the Switch
// never loses its screen, so the game keeps its surface and swapchain for good. The handles it holds stay the
// same (the first host handles, used as names from then on) and the host surface and swapchain behind them are
// replaced when the window changes or the driver says they are lost.
struct SurfaceInfo {
    VkInstance instance;
    void* window;  // a reference we hold (plat::create_window)
    uint64_t generation;
    VkSurfaceKHR host;
};
std::unordered_map<uint64_t, SurfaceInfo> g_surfaces;  // game handle -> current

// The game's surface and swapchain handles are ours (a host handle value could come back for another object once
// the host one behind it was re-made).
uint64_t g_next_handle = 0x0000FA4E00000001ull;

// The host surface behind a game handle, re-made on the current window if the OS replaced it. False: no window.
bool refresh_surface(SurfaceInfo& s) {
    uint64_t gen = plat::window_generation();
    if (s.host && s.generation == gen) return true;
    if (s.host) {
        p_vkDestroySurfaceKHR(s.instance, s.host, nullptr);
        plat::release_window(s.window);
        s.host = VK_NULL_HANDLE;
    }
    s.window = plat::create_window();
    s.generation = gen;
    uint64_t h = 0;
    VkResult r = VkResult(plat::vulkan_create_surface(s.instance, s.window, &h, reinterpret_cast<void*>(host_gipa)));
    log("vk: surface re-made on window %p (generation %llu): %d", s.window, (unsigned long long)gen, r);
    s.host = reinterpret_cast<VkSurfaceKHR>(h);
    return r == VK_SUCCESS;
}

VkSurfaceKHR host_surface(uint64_t game) {
    auto it = g_surfaces.find(game);
    if (it == g_surfaces.end()) return reinterpret_cast<VkSurfaceKHR>(game);
    refresh_surface(it->second);
    return it->second.host;
}

void w_vkCreateViSurfaceNN(CPU& c) {  // (instance, const VkViSurfaceCreateInfoNN*, alloc, VkSurfaceKHR*)
    auto* vi = reinterpret_cast<const VkViSurfaceCreateInfoNN*>(c.x[1]);
    uint64_t s = 0;
    auto r = VkResult(plat::vulkan_create_surface(arg<VkInstance>(c.x[0]), vi->window, &s,
                                                  reinterpret_cast<void*>(host_gipa)));
    log("vkCreateViSurfaceNN -> %s surface on %p: %d", plat::vulkan_surface_extension(), vi->window, r);
    if (r == VK_SUCCESS) {
        uint64_t game = g_next_handle++;
        st<uint64_t>(c.x[3], game);
        // The window came from nn::vi::GetNativeWindow (a reference the game never gives back): ours now.
        g_surfaces[game] = {arg<VkInstance>(c.x[0]), vi->window, plat::window_generation(), reinterpret_cast<VkSurfaceKHR>(s)};
    }
    c.x[0] = ret_u64(r);
}

void w_vkDestroySurfaceKHR(CPU& c) {  // (instance, surface, alloc)
    auto it = g_surfaces.find(c.x[1]);
    if (it == g_surfaces.end()) {
        p_vkDestroySurfaceKHR(arg<VkInstance>(c.x[0]), arg<VkSurfaceKHR>(c.x[1]), nullptr);
        return;
    }
    if (it->second.host) p_vkDestroySurfaceKHR(it->second.instance, it->second.host, nullptr);
    plat::release_window(it->second.window);
    g_surfaces.erase(it);
}

void w_vkGetPhysicalDeviceSurfaceSupportKHR(CPU& c) {  // (physical, family, surface, VkBool32*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfaceSupportKHR(arg<VkPhysicalDevice>(c.x[0]), uint32_t(c.x[1]),
                                                            host_surface(c.x[2]), arg<VkBool32*>(c.x[3])));
}

void w_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(CPU& c) {  // (physical, surface, caps*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(arg<VkPhysicalDevice>(c.x[0]), host_surface(c.x[1]),
                                                                 arg<VkSurfaceCapabilitiesKHR*>(c.x[2])));
}

void w_vkGetPhysicalDeviceSurfaceFormatsKHR(CPU& c) {  // (physical, surface, count*, formats*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfaceFormatsKHR(arg<VkPhysicalDevice>(c.x[0]), host_surface(c.x[1]),
                                                            arg<uint32_t*>(c.x[2]), arg<VkSurfaceFormatKHR*>(c.x[3])));
}

void w_vkGetPhysicalDeviceSurfacePresentModesKHR(CPU& c) {  // (physical, surface, count*, modes*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfacePresentModesKHR(
        arg<VkPhysicalDevice>(c.x[0]), host_surface(c.x[1]), arg<uint32_t*>(c.x[2]), arg<VkPresentModeKHR*>(c.x[3])));
}


}  // namespace

namespace bridge {  // vk_bridge_shared.h

VkPhysicalDevice physical() { return g_physical; }
VkSurfaceKHR host_surface(uint64_t game) { return nx::vk::host_surface(game); }
VkSurfaceKHR refresh_host_surface(uint64_t game) {
    auto it = g_surfaces.find(game);
    return it != g_surfaces.end() && refresh_surface(it->second) ? it->second.host : VK_NULL_HANDLE;
}
uint64_t new_handle() { return g_next_handle++; }

}  // namespace bridge

namespace {

void w_create_debug_callback(CPU& c) {  // a handle that is never used: the host must not call guest callbacks
    st<uint64_t>(c.x[3], 0xDEB6);
    c.x[0] = VK_SUCCESS;
}
void w_destroy_debug_callback(CPU&) {}

}  // namespace

NX_HLE("vkGetInstanceProcAddr", w_vkGetInstanceProcAddr);
NX_HLE("vkGetDeviceProcAddr", w_vkGetDeviceProcAddr);
NX_HLE("vkEnumerateInstanceExtensionProperties", w_vkEnumerateInstanceExtensionProperties);
NX_HLE("vkEnumerateInstanceLayerProperties", w_vkEnumerateInstanceLayerProperties);
NX_HLE("vkCreateInstance", w_vkCreateInstance);
NX_HLE("vkDestroyInstance", w_vkDestroyInstance);
NX_HLE("vkEnumeratePhysicalDevices", w_vkEnumeratePhysicalDevices);
NX_HLE("vkCreateDevice", w_vkCreateDevice);
NX_HLE("vkGetPhysicalDeviceQueueFamilyProperties", w_vkGetPhysicalDeviceQueueFamilyProperties);
NX_HLE("vkGetPhysicalDeviceQueueFamilyProperties2", w_vkGetPhysicalDeviceQueueFamilyProperties2);
NX_HLE("vkGetPhysicalDeviceQueueFamilyProperties2KHR", w_vkGetPhysicalDeviceQueueFamilyProperties2);
NX_HLE("vkCreateViSurfaceNN", w_vkCreateViSurfaceNN);
NX_HLE("vkDestroySurfaceKHR", w_vkDestroySurfaceKHR);
NX_HLE("vkGetPhysicalDeviceSurfaceSupportKHR", w_vkGetPhysicalDeviceSurfaceSupportKHR);
NX_HLE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", w_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
NX_HLE("vkGetPhysicalDeviceSurfaceFormatsKHR", w_vkGetPhysicalDeviceSurfaceFormatsKHR);
NX_HLE("vkGetPhysicalDeviceSurfacePresentModesKHR", w_vkGetPhysicalDeviceSurfacePresentModesKHR);
NX_HLE("vkCreateDebugUtilsMessengerEXT", w_create_debug_callback);
NX_HLE("vkCreateDebugReportCallbackEXT", w_create_debug_callback);
NX_HLE("vkDestroyDebugUtilsMessengerEXT", w_destroy_debug_callback);
NX_HLE("vkDestroyDebugReportCallbackEXT", w_destroy_debug_callback);

}  // namespace nx::vk
