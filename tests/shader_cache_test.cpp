// Checks runtime/src/nvn/nvn_shaders.cpp against the offline translation (docs/nvn/TRANSLATOR.md, "Runtime side"):
//   1. SHA-1 against the FIPS 180 test vectors.
//   2. Every stage of private/wonder/shaders_extracted/*/manifest.tsv: the code block size from control +0x704, the
//      constant data at +0x700/+0x6fc and the hash of the block agree with the manifest, and each distinct block goes
//      through make_shader_info (hash -> .spv -> reflection), the path shader_for takes at run time.
//   3. Every shader of private/wonder/spirv/results.tsv: the bindings reflected from the SPIR-V, described the way the
//      harness does, equal its "resources" column; every binding gets an NVN meaning.
//
// Build and run (standalone, no runtime):
//   clang++ -std=c++20 -O2 -DNX_SHADERS_STANDALONE -Iruntime/src/nvn tests/shader_cache_test.cpp \
//       runtime/src/nvn/nvn_shaders.cpp runtime/src/nvn/nvn_spirv_reflect.cpp -o private/build/shader_cache_test
//   private/build/shader_cache_test [private/wonder]
// or through CMake (-DNX_TESTS=ON, ctest), which runs it on private/wonder when that folder exists.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "nvn_shaders.h"

using namespace nx::nvn;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void fail(const char* fmt, const std::string& a, const std::string& b = "") {
    if (++g_failures <= 30) {
        std::printf("FAIL ");
        std::printf(fmt, a.c_str(), b.c_str());
        std::printf("\n");
    }
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    if (!s.empty() && s.back() == sep) out.push_back("");
    return out;
}

std::vector<std::string> sorted_tokens(const std::string& s) {
    std::vector<std::string> t;
    for (auto& x : split(s, ' '))
        if (!x.empty()) t.push_back(x);
    std::sort(t.begin(), t.end());
    return t;
}

bool load_spirv(const fs::path& p, std::vector<uint32_t>& words) {
    std::string bytes;
    if (!read_file(p, bytes) || bytes.empty() || bytes.size() % 4) return false;
    words.resize(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return true;
}

const char* type_name(DescriptorType t) {
    switch (t) {
    case DescriptorType::Sampler: return "sampler";
    case DescriptorType::CombinedImageSampler: return "combined image sampler";
    case DescriptorType::SampledImage: return "sampled image";
    case DescriptorType::StorageImage: return "storage image";
    case DescriptorType::UniformTexelBuffer: return "uniform texel buffer";
    case DescriptorType::StorageTexelBuffer: return "storage texel buffer";
    case DescriptorType::UniformBuffer: return "uniform buffer";
    case DescriptorType::StorageBuffer: return "storage buffer";
    }
    return "?";
}

const char* scalar_name(ScalarType t) {
    switch (t) {
    case ScalarType::Float: return "float";
    case ScalarType::Sint: return "int";
    case ScalarType::Uint: return "uint";
    case ScalarType::Bool: return "bool";
    default: return "other";
    }
}

void test_sha1() {
    struct V {
        std::string in, out;
    } v[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    };
    for (auto& t : v)
        if (sha1_hex(t.in.data(), t.in.size()) != t.out) fail("sha1(\"%s\") != %s", t.in, t.out);
    // One million 'a', fed in uneven pieces to exercise the buffering.
    Sha1 s;
    std::string a(1000000, 'a');
    size_t off = 0, step = 1;
    while (off < a.size()) {
        size_t n = std::min(step, a.size() - off);
        s.update(a.data() + off, n);
        off += n;
        step = step * 7 % 131 + 1;
    }
    uint8_t d[20];
    s.finish(d);
    char hex[41];
    for (int i = 0; i < 20; i++) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
    if (std::string(hex) != "34aa973cd4c4daa4f61eeb2bdbad27316534016f") fail("sha1(1M a) = %s%s", hex);
    std::printf("sha1: FIPS 180 vectors checked\n");
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = argc > 1 ? argv[1] : "private/wonder";
    fs::path spv_dir = root / "spirv";
    test_sha1();

    std::string tsv;
    if (!read_file(spv_dir / "results.tsv", tsv)) {
        std::printf("no %s\n", (spv_dir / "results.tsv").string().c_str());
        return 1;
    }
    std::unordered_map<std::string, std::string> resources;
    size_t records = parse_results_tsv(tsv, resources);
    std::map<std::string, std::string> tsv_stage;  // hash -> stage column
    for (auto& line : split(tsv, '\n')) {
        auto c = split(line, '\t');
        if (c.size() > 2 && c[0] != "hash") tsv_stage[c[0]] = c[2];
    }

    // ---- 2. extracted code blocks: hash and the full make_shader_info path ----
    size_t rows = 0, blocks = 0, local_size_checked = 0, const_checked = 0;
    std::set<std::string> seen;
    for (auto& dir : fs::directory_iterator(root / "shaders_extracted")) {
        std::string manifest;
        if (!dir.is_directory() || !read_file(dir.path() / "manifest.tsv", manifest)) continue;
        std::vector<std::string> head;
        for (auto& line : split(manifest, '\n')) {
            auto c = split(line, '\t');
            if (c.size() < 13) continue;
            if (head.empty()) {
                head = c;
                continue;
            }
            auto col = [&](const char* name) {
                for (size_t i = 0; i < head.size(); i++)
                    if (head[i] == name) return c[i];
                return std::string();
            };
            rows++;
            std::string code, ctrl;
            if (!read_file(dir.path() / col("code_file"), code) || !read_file(dir.path() / col("ctrl_file"), ctrl) ||
                ctrl.size() < 0x880) {
                fail("%s: missing files (%s)", col("id"), dir.path().string());
                continue;
            }
            auto* cb = reinterpret_cast<const uint8_t*>(code.data());
            auto* cc = reinterpret_cast<const uint8_t*>(ctrl.data());
            ShaderControl sc = read_control(cc);
            if (!sc.valid || sc.code_size != std::stoul(col("code_size"), nullptr, 16) || sc.code_size != code.size())
                fail("%s: code size from control +0x704 disagrees (%s)", col("id"), col("code_size"));
            if (sc.const_offset != std::stoul(col("const_off"), nullptr, 16) ||
                sc.const_size != std::stoul(col("const_size"), nullptr, 16) ||
                sc.program_start != std::stoul(col("program_start"), nullptr, 16))
                fail("%s: constant data / program start disagree with the manifest%s", col("id"));
            else
                const_checked++;
            std::string hash = shader_hash(cb, sc.code_size);
            if (hash != col("code_sha1_16") || "code_" + hash + ".bin" != col("code_file"))
                fail("%s: hash %s", col("id"), hash);
            if (!seen.insert(hash).second) continue;
            blocks++;
            std::vector<uint32_t> words;
            if (!load_spirv(spv_dir / (hash + ".spv"), words)) {
                fail("%s: no .spv for %s", col("id"), hash);
                continue;
            }
            auto res = resources.find(hash);
            ShaderInfo info;
            std::string err;
            const std::string* record = res == resources.end() ? nullptr : &res->second;
            if (!make_shader_info(cb, cc, std::move(words), record, info, &err)) {
                fail("%s: make_shader_info: %s", hash, err);
                continue;
            }
            std::string want = col("stage") == "pixel" ? "fragment" : col("stage");
            if (stage_name(info.stage) != want || tsv_stage[hash] != want)
                fail("%s: stage %s", hash, std::string(stage_name(info.stage)) + " vs " + want);
            if (info.constants.size() != sc.const_size) fail("%s: constant data size%s", hash);
            if (info.stage == ShaderStage::Compute) {
                local_size_checked++;
                for (int i = 0; i < 3; i++)
                    if (info.local_size[i] != info.refl.local_size[i])
                        fail("%s: work-group size from control != SPIR-V LocalSize%s", hash);
            }
        }
    }
    std::printf("hash: %zu stages in the manifests, %zu distinct blocks; size (+0x704), constant data (+0x700/+0x6fc, "
                "%zu agree) and SHA-1 checked; %zu compute work-group sizes (+0x748) equal to the SPIR-V's\n",
                rows, blocks, const_checked, local_size_checked);

    // ---- 3. reflection against the harness's resources column, for every shader ----
    size_t shaders = 0, bindings = 0, matched = 0, with_support = 0, sb_named = 0;
    std::map<std::string, size_t> by_type, shapes, by_source, by_stage, io_types, sb_meaning, tex_meaning;
    for (auto& [hash, text] : resources) {
        shaders++;
        std::vector<uint32_t> words;
        if (!load_spirv(spv_dir / (hash + ".spv"), words)) {
            fail("%s: no .spv%s", hash);
            continue;
        }
        ShaderReflection r;
        std::string err;
        if (!reflect_spirv(words.data(), words.size(), r, &err)) {
            fail("%s: %s", hash, err);
            continue;
        }
        std::vector<HarnessResource> list;
        if (!parse_harness_resources(text, list)) fail("%s: cannot parse \"%s\"", hash, text);
        // The storage-buffer slot in the SPIR-V name must give its binding before any harness data is applied.
        for (auto& b : r.bindings)
            if (b.type == DescriptorType::StorageBuffer) {
                sb_named++;
                int base = r.stage == ShaderStage::Fragment ? 16 : 0;
                if (b.nvn.sb_slot < 0 || b.binding != uint32_t(base + b.nvn.sb_slot))
                    fail("%s: storage buffer %s slot/binding", hash, b.name);
            }
        // Texture/image handles must come from the SPIR-V names alone (apply_harness only fills what they lack).
        for (auto& b : r.bindings)
            if (b.type != DescriptorType::UniformBuffer && b.type != DescriptorType::StorageBuffer && b.nvn.word < 0)
                fail("%s: texture %s: no handle in its name", hash, b.name);
        if (!apply_harness(r, list)) fail("%s: harness entries without SPIR-V binding: %s", hash, text);
        std::string mine = describe_like_harness(r);
        if (sorted_tokens(mine) != sorted_tokens(text)) {
            fail("%s: reflected \"%s\"", hash, mine);
            fail("%s:   harness \"%s\"", hash, text);
        } else {
            matched++;
        }
        by_stage[stage_name(r.stage)]++;
        if (r.uses_support_buffer) with_support++;
        for (auto& b : r.bindings) {
            bindings++;
            by_type[type_name(b.type)]++;
            by_source[source_name(b.nvn.source)]++;
            if (b.nvn.source == NvnSource::Unknown) fail("%s: binding %s has no NVN meaning", hash, b.name);
            // Binding numbers follow the scheme of TRANSLATOR.md.
            int slot = r.stage == ShaderStage::Fragment ? 1 : 0;
            if (b.type == DescriptorType::UniformBuffer && b.nvn.source != NvnSource::SupportBuffer &&
                (b.set != 0 || b.binding != uint32_t(slot * 18 + 1 + b.nvn.bank)))
                fail("%s: uniform %s outside the scheme", hash, b.name);
            if (b.type == DescriptorType::CombinedImageSampler && (b.set != 2 || b.binding / 128 != uint32_t(slot)))
                fail("%s: texture %s outside the scheme", hash, b.name);
            char key[96];
            if (b.type == DescriptorType::StorageBuffer) {
                std::snprintf(key, sizeof key, "%-8s c[0]+0x%03x -> %s %d", stage_name(r.stage), b.nvn.c0_offset,
                              source_name(b.nvn.source), b.nvn.index);
                sb_meaning[key]++;
            } else if (b.type != DescriptorType::UniformBuffer) {
                const char* kind = b.type == DescriptorType::StorageImage ? "image" : "tex";
                std::snprintf(key, sizeof key, "%-5s %s c[%d]", kind, source_name(b.nvn.source), b.nvn.bank);
                tex_meaning[key]++;
                std::snprintf(key, sizeof key, "%s %s", type_name(b.type), sampler_type_text(b.image).c_str());
                shapes[key]++;
            }
        }
        if (r.stage == ShaderStage::Vertex)
            for (auto& io : r.inputs)
                io_types[std::string("vertex input ") + scalar_name(io.type) + std::to_string(io.components)]++;
        if (r.stage == ShaderStage::Fragment)
            for (auto& io : r.outputs)
                io_types[std::string("fragment output ") + scalar_name(io.type) + std::to_string(io.components)]++;
        if (r.stage == ShaderStage::Fragment && !r.outputs.empty() && r.outputs.back().location > 7)
            fail("%s: fragment output location > 7%s", hash);
    }

    std::printf("reflection: %zu records in results.tsv, %zu shaders reflected, %zu match the resources column exactly "
                "(%zu bindings; %zu storage buffers, slot from the SPIR-V name = binding)\n",
                records, shaders, matched, bindings, sb_named);
    std::printf("  support buffer (set 0 binding 0) declared by %zu of %zu\n", with_support, shaders);
    auto dump = [](const char* title, const std::map<std::string, size_t>& m) {
        std::printf("%s\n", title);
        for (auto& [k, v] : m) std::printf("  %7zu  %s\n", v, k.c_str());
    };
    dump("stages:", by_stage);
    dump("descriptor types (bindings):", by_type);
    dump("image shapes (OpTypeImage):", shapes);
    dump("NVN meaning (bindings):", by_source);
    dump("storage buffers: c[0] descriptor -> NVN binding:", sb_meaning);
    dump("textures/images: handle source:", tex_meaning);
    dump("vertex inputs / fragment outputs (type, components):", io_types);

    SupportBuffer sb = support_buffer_defaults();
    support_buffer_set_viewport(sb, 1920, 1080);
    if (sb.render_scale[72][0] != 1.0f || sb.viewport_inverse[1] != 2.0f / 1080 || sb.is_bgra[0][0] != 0)
        fail("support buffer defaults%s%s", "");

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
