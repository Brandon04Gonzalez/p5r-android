// The GPU choice (nvn_gpu_pick.h): the rule, and the Vulkan side that lists the devices and logs the pick.
// NX_GPU_PICK_STANDALONE builds the rule alone, for tests/gpu_pick_test.cpp.
#include "nvn_gpu_pick.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace nx::nvn {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

size_t first_of_type(const std::vector<GpuCandidate>& gpus, int type) {
    for (size_t k = 0; k < gpus.size(); k++)
        if (gpus[k].type == type) return k;
    return SIZE_MAX;
}

}  // namespace

size_t pick_gpu(const std::vector<GpuCandidate>& gpus, const char* want, bool& matched) {
    matched = true;
    if (gpus.empty()) return 0;
    size_t fallback = first_of_type(gpus, 2);
    if (fallback == SIZE_MAX) fallback = first_of_type(gpus, 1);
    if (fallback == SIZE_MAX) fallback = 0;
    const std::string w = want ? lower(want) : "";
    if (w.empty()) return fallback;
    size_t k = SIZE_MAX;
    if (w == "discrete")
        k = first_of_type(gpus, 2);
    else if (w == "integrated")
        k = first_of_type(gpus, 1);
    else if (w.size() <= 2 && std::isdigit(static_cast<unsigned char>(w[0]))) {  // an index; "3050" is a name part
        char* end = nullptr;
        unsigned long n = std::strtoul(w.c_str(), &end, 10);
        if (end && !*end && n < gpus.size()) k = size_t(n);
    } else {
        for (size_t j = 0; j < gpus.size() && k == SIZE_MAX; j++)
            if (lower(gpus[j].name).find(w) != std::string::npos) k = j;
    }
    if (k != SIZE_MAX) return k;
    matched = false;
    return fallback;
}

}  // namespace nx::nvn

#ifndef NX_GPU_PICK_STANDALONE

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

VkPhysicalDevice pick_physical_device(const std::vector<VkPhysicalDevice>& devices) {
    static const char* const types[] = {"other", "integrated", "discrete", "virtual", "CPU"};
    std::vector<GpuCandidate> gpus;
    for (size_t k = 0; k < devices.size(); k++) {
        VkPhysicalDeviceProperties p{};
        p_vkGetPhysicalDeviceProperties(devices[k], &p);
        gpus.push_back({p.deviceName, int(p.deviceType)});
        log("nvn/vk: GPU %zu: %s (%s, Vulkan %u.%u, driver %08x)", k, p.deviceName,
            unsigned(p.deviceType) <= 4 ? types[p.deviceType] : "?", VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion);
    }
    if (devices.empty()) return VK_NULL_HANDLE;
    const char* want = std::getenv("NX_GPU");
    bool matched = true;
    size_t k = pick_gpu(gpus, want, matched);
    if (!matched) log("nvn/vk: NX_GPU=%s matches no GPU: using the default", want);
    log("nvn/vk: using GPU %zu, %s (NX_GPU=discrete|integrated|<index>|<name part>)", k, gpus[k].name.c_str());
    return devices[k];
}

}  // namespace nx::nvn

#endif
