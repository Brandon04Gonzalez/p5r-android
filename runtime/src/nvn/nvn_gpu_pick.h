// Which Vulkan device Wonder runs on (docs/nvn/WINDOWS-PLAN.md, gap 8). A laptop has two: on the PC this was written
// on, the loader lists the integrated Intel GPU first and the NVIDIA one second, so "the first device" was the slow one.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nx::nvn {

struct GpuCandidate {
    std::string name;
    int type = 0;  // VkPhysicalDeviceType: 1 integrated, 2 discrete, 3 virtual, 4 CPU
};

// The device to use for NX_GPU = `want`:
//   null or ""   the first discrete GPU, else the first integrated one, else device 0;
//   "discrete"   the first discrete GPU;  "integrated": the first integrated one;
//   "<n>"        device n (one or two digits; longer numbers such as "3050" are name parts);
//   other text   the first device whose name contains it (case-insensitive), e.g. "nvidia", "intel", "3050".
// A `want` nothing matches falls back to the default, and `matched` is false then (the caller logs it).
size_t pick_gpu(const std::vector<GpuCandidate>& gpus, const char* want, bool& matched);

}  // namespace nx::nvn
