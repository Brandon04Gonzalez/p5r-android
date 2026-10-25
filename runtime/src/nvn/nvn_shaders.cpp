// NVN shader stages -> offline-translated SPIR-V (nvn_shaders.h, docs/nvn/TRANSLATOR.md "Runtime side").
//
// The meaning of each binding comes from the translator's own naming and numbering (Ryujinx.Graphics.Shader,
// Translation/ResourceManager.cs): uniform buffers are "<stage>_c<k>" at binding stageSlot * 18 + 1 + k, storage
// buffers "<stage>_s<slot>", textures "<stage>_t_tcb_<word hex>" (handle in the texture bank c[2]) or
// "<stage>_t_cb<k>_<word hex>" (handle in c[k]), images "<stage>_i_..." likewise with a format suffix. The one thing
// neither the names nor the numbers carry is the c[0] offset a storage buffer's address was loaded from; that comes
// from the harness's results.tsv (apply_harness).
#include "nvn_shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef NX_SHADERS_STANDALONE
#include <memory>
#include <mutex>

#include <nx/runtime.h>

#include "platform.h"
#include "vk_bridge.h"
#endif

namespace nx::nvn {

// ---- SHA-1 --------------------------------------------------------------------------------------------------------

namespace {

inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);  // little-endian host (arm64, x86-64), like the Switch
    return v;
}

}  // namespace

Sha1::Sha1() : h_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0} {}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
}

void Sha1::update(const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    bytes_ += size;
    if (used_) {
        size_t n = std::min(size, 64 - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n, p += n, size -= n;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    for (; size >= 64; p += 64, size -= 64) block(p);
    std::memcpy(buf_, p, size);
    used_ = size;
}

void Sha1::finish(uint8_t digest[20]) {
    uint64_t bits = bytes_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (used_ != 56) update(&pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 20; i++) digest[i] = uint8_t(h_[i / 4] >> (24 - 8 * (i % 4)));
}

std::string sha1_hex(const void* data, size_t size) {
    Sha1 s;
    s.update(data, size);
    uint8_t d[20];
    s.finish(d);
    static const char* kHex = "0123456789abcdef";
    std::string out(40, '0');
    for (int i = 0; i < 20; i++) out[2 * i] = kHex[d[i] >> 4], out[2 * i + 1] = kHex[d[i] & 15];
    return out;
}

std::string shader_hash(const uint8_t* code, uint32_t size) { return sha1_hex(code, size).substr(0, 16); }

// ---- control section ------------------------------------------------------------------------------------------------

ShaderControl read_control(const uint8_t* control) {
    ShaderControl c;
    c.valid = rd32(control) == kShaderControlMagic;
    c.program_size = rd32(control + 0x6f8);
    c.const_size = rd32(control + 0x6fc);
    c.const_offset = rd32(control + 0x700);
    c.code_size = rd32(control + 0x704);
    c.program_start = rd32(control + 0x708);
    for (int i = 0; i < 3; i++) c.local_size[i] = rd32(control + 0x748 + 4 * i);
    c.shared_size = rd32(control + 0x754);
    return c;
}

// ---- support buffer -------------------------------------------------------------------------------------------------

SupportBuffer support_buffer_defaults() {
    SupportBuffer sb;
    std::memset(&sb, 0, sizeof sb);
    for (auto& s : sb.render_scale) s[0] = 1.0f;  // Ryujinx's SupportBufferUpdater: (1, 0, 0, 0) for all 73
    return sb;
}

void support_buffer_set_viewport(SupportBuffer& sb, float width, float height, bool transform_disabled, float scale) {
    sb.render_scale[0][0] = scale;  // the render target's (NX_HUD_1080: 1.5); gl_FragCoord.xy is divided by it
    sb.viewport_inverse[0] = width != 0 ? scale * 2.0f / width : 0.0f;
    sb.viewport_inverse[1] = height != 0 ? scale * 2.0f / height : 0.0f;
    sb.viewport_inverse[2] = 1.0f;
    sb.viewport_inverse[3] = transform_disabled ? 1.0f : 0.0f;
    sb.viewport_size[0] = width;
    sb.viewport_size[1] = height < 0 ? -height : height;
    sb.viewport_size[2] = 1.0f;
    sb.viewport_size[3] = 0.0f;
}

// ---- one stage ------------------------------------------------------------------------------------------------------

bool make_shader_info(const uint8_t* code, const uint8_t* control, std::vector<uint32_t> spirv,
                      const std::string* resources, ShaderInfo& out, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    out = ShaderInfo{};
    out.control = read_control(control);
    const ShaderControl& c = out.control;
    if (!c.valid) return fail("control section without magic 0x98761234");
    if (c.code_size == 0 || c.code_size > 16u << 20) return fail("implausible code block size");
    out.hash = shader_hash(code, c.code_size);
    if (!reflect_spirv(spirv.data(), spirv.size(), out.refl, error)) return false;
    out.stage = out.refl.stage;
    out.spirv = std::move(spirv);
    if (c.const_size) {
        if (uint64_t(c.const_offset) + c.const_size > c.code_size) return fail("constant data outside the code block");
        out.constants.assign(code + c.const_offset, code + c.const_offset + c.const_size);
    }
    if (out.stage == ShaderStage::Compute)
        for (int i = 0; i < 3; i++) out.local_size[i] = std::max<uint32_t>(1, c.local_size[i]);
    if (resources) {
        std::vector<HarnessResource> list;
        if (!parse_harness_resources(*resources, list)) return fail("unreadable resources record");
        if (!apply_harness(out.refl, list)) return fail("resources record does not match the SPIR-V");
    }
    return true;
}

const char* stage_name(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Compute: return "compute";
    default: return "unknown";
    }
}

const char* source_name(NvnSource s) {
    switch (s) {
    case NvnSource::SupportBuffer: return "support buffer";
    case NvnSource::DriverConstants: return "c[0] driver constants";
    case NvnSource::ConstantData: return "c[1] constant data";
    case NvnSource::UniformBlock: return "uniform block";
    case NvnSource::StorageBuffer: return "storage buffer";
    case NvnSource::UniformAsStorage: return "uniform block (as storage)";
    case NvnSource::SamplerBinding: return "sampler binding";
    case NvnSource::ImageBinding: return "image binding";
    case NvnSource::HandleInUniform: return "handle in uniform block";
    default: return "unknown";
    }
}

// ---- the runtime cache ----------------------------------------------------------------------------------------------
#ifndef NX_SHADERS_STANDALONE

static_assert(uint32_t(DescriptorType::Sampler) == VK_DESCRIPTOR_TYPE_SAMPLER &&
                  uint32_t(DescriptorType::CombinedImageSampler) == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                  uint32_t(DescriptorType::SampledImage) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                  uint32_t(DescriptorType::StorageImage) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                  uint32_t(DescriptorType::UniformTexelBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                  uint32_t(DescriptorType::StorageTexelBuffer) == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
                  uint32_t(DescriptorType::UniformBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                  uint32_t(DescriptorType::StorageBuffer) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              "DescriptorType mirrors VkDescriptorType");

uint64_t dynamic_uniform_bindings(const ShaderInfo* s);  // nvn_spirv_patch.cpp
uint64_t written_storage_bindings(const ShaderInfo* s);

namespace {

bool read_whole(const std::string& path, std::string& out) {
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    plat::fseek64(f, 0, SEEK_END);
    int64_t size = plat::ftell64(f);
    out.resize(size > 0 ? size_t(size) : 0);
    bool ok = size >= 0 && plat::read_at(f, 0, out.data(), out.size()) == out.size();
    std::fclose(f);
    return ok;
}

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, std::unique_ptr<ShaderInfo>> by_hash;  // null: no usable .spv (logged once)
    std::unordered_map<std::string, std::string> resources;                // results.tsv, hash -> resources
    bool resources_loaded = false;
};

Cache& cache() {
    static Cache c;
    return c;
}

}  // namespace

const ShaderInfo* shader_by_hash(const std::string& hash) {
    Cache& k = cache();
    std::lock_guard lk(k.lock);
    auto it = k.by_hash.find(hash);
    return it == k.by_hash.end() ? nullptr : it->second.get();
}

const ShaderInfo* shader_for(uint64_t code_host, uint64_t control) {
    if (!code_host || !control) return nullptr;
    auto* ctrl = reinterpret_cast<const uint8_t*>(control);
    ShaderControl c = read_control(ctrl);
    if (!c.valid || c.code_size == 0 || c.code_size > 16u << 20) {
        log("nvn: shader control %llx: bad section (magic %08x, code size %#x)", (unsigned long long)control,
            rd32(ctrl), c.code_size);
        return nullptr;
    }
    auto* code = reinterpret_cast<const uint8_t*>(code_host);
    std::string hash = shader_hash(code, c.code_size);

    Cache& k = cache();
    std::lock_guard lk(k.lock);
    if (auto it = k.by_hash.find(hash); it != k.by_hash.end()) return it->second.get();
    std::string dir = plat::data_root() + "/spirv/";
    if (!k.resources_loaded) {
        k.resources_loaded = true;
        std::string text;
        if (read_whole(dir + "results.tsv", text))
            log("nvn: %zu shader records from %sresults.tsv", parse_results_tsv(text, k.resources), dir.c_str());
        else
            log("nvn: no %sresults.tsv: storage buffers of translated shaders cannot be bound", dir.c_str());
    }
    std::unique_ptr<ShaderInfo>& slot = k.by_hash[hash];
    std::string bytes;
    if (!read_whole(dir + hash + ".spv", bytes) || bytes.size() % 4 || bytes.empty()) {
        log("nvn: shader %s (code %#x bytes, control %llx): no %s%s.spv", hash.c_str(), c.code_size,
            (unsigned long long)control, dir.c_str(), hash.c_str());
        if (std::getenv("NX_DUMP_SHADER_MISSES")) {  // for translating shaders the offline inventory missed
            std::string out = plat::data_root() + "/shader_misses/";
            plat::make_dirs(out);
            if (FILE* f = plat::fopen_utf8(out + "code_" + hash + ".bin", "wb")) {
                std::fwrite(code, 1, c.code_size, f);
                std::fclose(f);
            }
            if (FILE* f = plat::fopen_utf8(out + "ctrl_" + hash + ".bin", "wb")) {
                std::fwrite(ctrl, 1, 0x880, f);
                std::fclose(f);
            }
        }
        return nullptr;
    }
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    auto res = k.resources.find(hash);
    auto info = std::make_unique<ShaderInfo>();
    std::string error;
    const std::string* record = res == k.resources.end() ? nullptr : &res->second;
    if (!make_shader_info(code, ctrl, std::move(words), record, *info, &error)) {
        log("nvn: shader %s: %s", hash.c_str(), error.c_str());
        return nullptr;
    }
    if (g_trace)
        log("nvn: shader %s %s: %zu bindings, %zu inputs, %zu outputs, c[1] %u bytes", hash.c_str(),
            stage_name(info->stage), info->refl.bindings.size(), info->refl.inputs.size(), info->refl.outputs.size(),
            uint32_t(info->constants.size()));
    info->dynamic_uniforms = dynamic_uniform_bindings(info.get());
    info->written_storage = written_storage_bindings(info.get());
    info->masks_known = true;
    slot = std::move(info);
    return slot.get();
}

#endif  // NX_SHADERS_STANDALONE

}  // namespace nx::nvn
