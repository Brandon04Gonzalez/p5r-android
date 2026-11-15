// The GPU probe (NX_GPU_PROBE=1): two small fixed compute jobs at the start of every present's command buffer, timed
// with timestamps. Without root the Adreno's clock and bus level cannot be read (kgsl sysfs and its trace events are
// closed), so the probe measures them from inside: the first job is a chain of multiply-adds that touches no memory
// (its time follows the GPU clock only), the second sums a device-local buffer (its time follows the memory bandwidth
// the GPU gets). The same work timed while the CPU waits (NX_PIPELINED=0) and while it plays (pipelined) tells a
// lower GPU clock from memory contention (P5R PROGRESO 75: the casino's GPU work takes ~46 ms alone and ~65 ms with
// the CPU working). NX_GPU_PROBE_ITERS (default 1024) and NX_GPU_PROBE_MB (default 8) size the two jobs. Logged every
// 60 presents with the kgsl busy ratio when readable.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

#include "nvn_gpu_probe_spirv.inc"

constexpr uint32_t kSlots = 8, kAluGroups = 128, kMemGroups = 256;
VkQueryPool g_pool = VK_NULL_HANDLE;
VkPipeline g_pipeline = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkDescriptorSet g_set = VK_NULL_HANDLE;
VkBuffer g_out = VK_NULL_HANDLE, g_in = VK_NULL_HANDLE;
bool g_filled = false, g_failed = false;
uint64_t g_recorded[kSlots] = {};  // the present that used each slot (0: none waiting)
uint32_t g_iters = 1024, g_vec4s = 0;
double g_period_ns = 1.0;
double g_alu_ns = 0, g_mem_ns = 0, g_alu_min = 1e18, g_mem_min = 1e18, g_alu_max = 0, g_mem_max = 0;
uint64_t g_samples = 0;

VkBuffer make_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer b = VK_NULL_HANDLE;
    if (p_vkCreateBuffer(vk_device(), &bi, nullptr, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(vk_device(), b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    p_vkBindBufferMemory(vk_device(), b, m, 0);
    return b;
}

bool init() {
    if (g_pipeline) return true;
    if (g_failed) return false;
    g_failed = true;  // until everything below exists
    VkDevice d = vk_device();
    if (const char* e = std::getenv("NX_GPU_PROBE_ITERS")) g_iters = uint32_t(std::max(1, std::atoi(e)));
    uint64_t mb = 8;
    if (const char* e = std::getenv("NX_GPU_PROBE_MB")) mb = uint64_t(std::clamp(std::atoi(e), 1, 256));
    g_vec4s = uint32_t(mb * 1024 * 1024 / 16);
    g_out = make_buffer(uint64_t(kMemGroups) * 64 * 16);
    g_in = make_buffer(uint64_t(g_vec4s) * 16);
    if (!g_out || !g_in) return false;
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t k = 0; k < 2; k++)
        b[k].binding = k, b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b[k].descriptorCount = 1,
        b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2, li.pBindings = b;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (p_vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &set_layout, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pc;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1, dpi.poolSizeCount = 1, dpi.pPoolSizes = &ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (p_vkCreateDescriptorPool(d, &dpi, nullptr, &pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = pool, sai.descriptorSetCount = 1, sai.pSetLayouts = &set_layout;
    if (p_vkAllocateDescriptorSets(d, &sai, &g_set) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bufs[2] = {{g_out, 0, VK_WHOLE_SIZE}, {g_in, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t k = 0; k < 2; k++)
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[k].dstSet = g_set, w[k].dstBinding = k,
        w[k].descriptorCount = 1, w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[k].pBufferInfo = &bufs[k];
    p_vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kProbe, mi.pCode = kProbe;
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(d, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, ci.stage.module = module, ci.stage.pName = "main";
    ci.layout = g_layout;
    if (p_vkCreateComputePipelines(d, pipeline_cache(), 1, &ci, nullptr, &g_pipeline) != VK_SUCCESS) return false;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 3 * kSlots;
    if (p_vkCreateQueryPool(d, &qi, nullptr, &g_pool) != VK_SUCCESS) return false;
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
    g_period_ns = props.limits.timestampPeriod;
    g_failed = false;
    log("gpu probe: on (NX_GPU_PROBE), %u multiply-add steps x %u invocations, %llu MB read", g_iters, kAluGroups * 64,
        (unsigned long long)mb);
    return true;
}

// The results of the present that last used `slot`, if they are there.
void collect(uint32_t slot) {
    if (!g_recorded[slot]) return;
    uint64_t t[3] = {};
    if (p_vkGetQueryPoolResults(vk_device(), g_pool, 3 * slot, 3, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;  // not done yet (or lost): skipped
    g_recorded[slot] = 0;
    if (t[1] < t[0] || t[2] < t[1]) return;
    double alu = double(t[1] - t[0]) * g_period_ns, mem = double(t[2] - t[1]) * g_period_ns;
    g_alu_ns += alu, g_mem_ns += mem, g_samples++;
    g_alu_min = std::min(g_alu_min, alu), g_mem_min = std::min(g_mem_min, mem);
    g_alu_max = std::max(g_alu_max, alu), g_mem_max = std::max(g_mem_max, mem);
}

}  // namespace

bool gpu_probe_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_GPU_PROBE");
        return e && e[0] == '1';
    }();
    return on;
}

void gpu_probe_record(VkCommandBuffer cb, uint64_t presents) {
    if (!gpu_probe_on() || !init()) return;
    const uint32_t slot = uint32_t(presents % kSlots);
    collect(slot);
    if (g_recorded[slot]) return;  // its last results are not back yet: this present is not probed
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (!g_filled) {
        p_vkCmdFillBuffer(cb, g_in, 0, VK_WHOLE_SIZE, 0x3c003c00u);
        g_filled = true;
    }
    p_vkCmdResetQueryPool(cb, g_pool, 3 * slot, 3);
    // Everything queued before (the frame's own work) is over before the first timestamp.
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pool, 3 * slot);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &g_set, 0, nullptr);
    uint32_t pc[4] = {0, g_iters, g_vec4s, 0};
    p_vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    p_vkCmdDispatch(cb, kAluGroups, 1, 1);
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pool, 3 * slot + 1);
    pc[0] = 1;
    p_vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    p_vkCmdDispatch(cb, kMemGroups, 1, 1);
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pool, 3 * slot + 2);
    g_recorded[slot] = presents;
}

void gpu_probe_log() {
    if (!gpu_probe_on() || !g_pool) return;
    for (uint32_t k = 0; k < kSlots; k++) collect(k);  // what is back of the last few
    unsigned long long busy = 0, total = 0;
    if (FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/gpubusy", "r")) {
        if (std::fscanf(f, "%llu %llu", &busy, &total) != 2) busy = total = 0;
        std::fclose(f);
    }
    if (g_samples) {
        const double n = double(g_samples), mem_ms = g_mem_ns / n / 1e6;
        log("gpu probe: alu %.3f ms (min %.3f, max %.3f), mem %.3f ms (min %.3f, max %.3f) = %.2f GB/s, %llu samples; "
            "kgsl busy %.0f%%",
            g_alu_ns / n / 1e6, g_alu_min / 1e6, g_alu_max / 1e6, mem_ms, g_mem_min / 1e6, g_mem_max / 1e6,
            double(g_vec4s) * 16.0 / (mem_ms * 1e6), (unsigned long long)g_samples,
            total ? 100.0 * double(busy) / double(total) : -1.0);
    }
    g_alu_ns = g_mem_ns = 0, g_alu_max = g_mem_max = 0, g_alu_min = g_mem_min = 1e18, g_samples = 0;
}

}  // namespace nx::nvn
