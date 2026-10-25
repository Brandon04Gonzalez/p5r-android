// SPIR-V reflection of the offline-translated shaders, and the translator harness's resource records
// (nvn_shaders.h, docs/nvn/TRANSLATOR.md "Runtime side"). Split from nvn_shaders.cpp (700-line rule); the naming
// rules it reads are described at the top of that file.
#include "nvn_shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace nx::nvn {
// ---- SPIR-V reflection ----------------------------------------------------------------------------------------------

namespace {

// The handful of SPIR-V opcodes, decorations and storage classes reflection needs (SPIR-V 1.x unified spec).
enum : uint32_t {
    OpName = 5, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43,
    OpVariable = 59, OpDecorate = 71, OpMemberDecorate = 72,
};
enum : uint32_t {
    DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecComponent = 31, DecBinding = 33,
    DecDescriptorSet = 34,
};
enum : uint32_t {
    ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScStorageBuffer = 12,
};
constexpr uint32_t kExecutionModeLocalSize = 17;
constexpr uint32_t kDimBuffer = 5;

struct Decorations {
    int set = -1, binding = -1, location = -1, component = 0, builtin = -1;
    bool block = false, buffer_block = false;
    std::vector<uint32_t> member_builtins;
};

struct Type {
    uint32_t op = 0;
    std::vector<uint32_t> w;  // operands after the result id
};

std::string spirv_string(const uint32_t* w, size_t n, size_t* used) {
    std::string s;
    size_t i = 0;
    for (; i < n; i++) {
        bool end = false;
        for (int b = 0; b < 4; b++) {
            char ch = char((w[i] >> (8 * b)) & 0xff);
            if (!ch) {
                end = true;
                break;
            }
            s.push_back(ch);
        }
        if (end) {
            i++;
            break;
        }
    }
    if (used) *used = i;
    return s;
}

bool parse_uint(std::string_view s, int base, int& out) {
    if (s.empty()) return false;
    long v = 0;
    for (char ch : s) {
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 99;
        if (d >= base) return false;
        v = v * base + d;
        if (v > 0x7fffffff) return false;
    }
    out = int(v);
    return true;
}

// Ryujinx's stage slot inside each set's binding range: vertex and compute 0, fragment 1 (GetStageIndex remap;
// the harness's StaticGpuAccessor gives the same numbers).
int stage_slot(ShaderStage s) { return s == ShaderStage::Fragment ? 1 : 0; }

// The storage buffer descriptors NVN keeps in c[0] (TRANSLATOR.md, bank table): graphics stage i at
// 0x110 + 0x100 * i + 0x10 * n (vertex i = 0, fragment i = 4); compute uniform blocks at 0x210 + 0x10 * n and
// storage buffers at 0x310 + 0x10 * n.
void set_c0_offset(NvnMeaning& m, ShaderStage stage, int byte_offset) {
    m.c0_offset = byte_offset;
    m.source = NvnSource::Unknown;
    m.index = -1;
    auto in = [&](int base, int count) {
        return byte_offset >= base && byte_offset < base + 0x10 * count && (byte_offset - base) % 0x10 == 0;
    };
    if (stage == ShaderStage::Compute) {
        if (in(0x210, 16)) m.source = NvnSource::UniformAsStorage, m.index = (byte_offset - 0x210) / 0x10;
        else if (in(0x310, 16)) m.source = NvnSource::StorageBuffer, m.index = (byte_offset - 0x310) / 0x10;
    } else if (stage == ShaderStage::Vertex || stage == ShaderStage::Fragment) {
        int base = 0x110 + 0x100 * (stage == ShaderStage::Vertex ? 0 : 4);
        if (in(base, 16)) m.source = NvnSource::StorageBuffer, m.index = (byte_offset - base) / 0x10;
    }
}

void set_texture_handle(NvnMeaning& m, bool image, int bank, int word) {
    m.default_bank = bank < 0;
    m.bank = bank < 0 ? 2 : bank;
    m.word = word;
    m.index = -1;
    m.source = NvnSource::Unknown;
    if (m.bank == 2) {
        // c[2]: texture handle of sampler binding n at byte 0x20 + 8n (verified on every stage), images after 32
        // texture slots at 0x120 + 8n (inferred from the agl compute shaders: words 72, 74, 76).
        int first = image ? 72 : 8;
        if (word >= first && (word - first) % 2 == 0 && (word - first) / 2 < 32) {
            m.source = image ? NvnSource::ImageBinding : NvnSource::SamplerBinding;
            m.index = (word - first) / 2;
        }
    } else if (m.bank >= 3 && !image) {
        m.source = NvnSource::HandleInUniform;
        m.index = m.bank - 3;
    }
}

// The NVN meaning from the variable's name and binding. Names first; the binding number alone fixes uniform banks.
void derive_meaning(DescriptorBinding& b, ShaderStage stage) {
    NvnMeaning& m = b.nvn;
    std::string_view n = b.name;
    if (n == "support_buffer") {
        m.source = NvnSource::SupportBuffer;
        return;
    }
    if (size_t us = n.find('_'); us != std::string_view::npos) {
        std::string_view prefix = n.substr(0, us);  // vp fp cp (tcp tep gp): Ryujinx GetShaderStagePrefix
        if (prefix == "vp" || prefix == "fp" || prefix == "cp" || prefix == "tcp" || prefix == "tep" || prefix == "gp")
            n.remove_prefix(us + 1);
    }
    int v = 0;
    if (b.type == DescriptorType::UniformBuffer) {
        int bank = -1;
        if (n.size() > 1 && n[0] == 'c' && parse_uint(n.substr(1), 10, v)) bank = v;
        else if (b.set == 0 && b.binding >= 1) bank = int(b.binding) - 1 - stage_slot(stage) * 18;  // fallback
        m.bank = bank;
        m.source = bank == 0 ? NvnSource::DriverConstants : bank == 1 ? NvnSource::ConstantData
                   : bank >= 3 ? NvnSource::UniformBlock : NvnSource::Unknown;
        m.index = bank >= 3 ? bank - 3 : -1;
        return;
    }
    if (b.type == DescriptorType::StorageBuffer) {
        if (n.size() > 1 && n[0] == 's' && parse_uint(n.substr(1), 10, v)) m.sb_slot = v;
        else if (b.set == 1) m.sb_slot = int(b.binding) - stage_slot(stage) * 16;
        m.source = NvnSource::Unknown;  // until apply_harness gives the c[0] offset
        return;
    }
    // Textures and images: "<t|i|s>[type]_tcb_<hex>[_fmt]" or "<t|i|s>[type]_cb<k>_<hex>[_fmt]".
    bool image = b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer;
    size_t us = n.find('_');
    if (us == std::string_view::npos) return;
    std::string_view rest = n.substr(us + 1);
    int bank = -2;
    if (rest.substr(0, 4) == "tcb_") {
        bank = -1;
        rest.remove_prefix(4);
    } else if (rest.substr(0, 2) == "cb") {
        size_t us2 = rest.find('_');
        if (us2 == std::string_view::npos || !parse_uint(rest.substr(2, us2 - 2), 10, bank)) return;
        rest.remove_prefix(us2 + 1);
    } else {
        return;
    }
    std::string_view hex = rest.substr(0, rest.find('_'));
    int word = 0;
    if (!parse_uint(hex, 16, word)) return;
    set_texture_handle(m, image, bank, word);
}

}  // namespace

const DescriptorBinding* ShaderReflection::find(uint32_t set, uint32_t binding) const {
    for (const auto& b : bindings)
        if (b.set == set && b.binding == binding) return &b;
    return nullptr;
}

bool reflect_spirv(const uint32_t* words, size_t count, ShaderReflection& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    out = ShaderReflection{};
    if (count < 5 || words[0] != 0x07230203) return fail("not a SPIR-V module");

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, Decorations> dec;
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, uint32_t> constants;
    struct Var {
        uint32_t id, ptr_type, storage;
    };
    std::vector<Var> vars;
    uint32_t entry_id = 0;
    bool have_entry = false;

    for (size_t i = 5; i < count;) {
        uint32_t wc = words[i] >> 16, op = words[i] & 0xffff;
        if (wc == 0 || i + wc > count) return fail("truncated instruction");
        const uint32_t* w = words + i + 1;  // operands
        size_t n = wc - 1;
        switch (op) {
        case OpEntryPoint:
            if (!have_entry && n >= 3) {
                have_entry = true;
                uint32_t model = w[0];
                entry_id = w[1];
                out.entry = spirv_string(w + 2, n - 2, nullptr);
                out.stage = model == 0 ? ShaderStage::Vertex : model == 4 ? ShaderStage::Fragment
                            : model == 5 ? ShaderStage::Compute : ShaderStage::Unknown;
            }
            break;
        case OpExecutionMode:
            if (n >= 5 && w[0] == entry_id && w[1] == kExecutionModeLocalSize)
                out.local_size[0] = w[2], out.local_size[1] = w[3], out.local_size[2] = w[4];
            break;
        case OpName:
            if (n >= 2) names[w[0]] = spirv_string(w + 1, n - 1, nullptr);
            break;
        case OpDecorate:
            if (n >= 2) {
                Decorations& d = dec[w[0]];
                uint32_t lit = n >= 3 ? w[2] : 0;
                switch (w[1]) {
                case DecDescriptorSet: d.set = int(lit); break;
                case DecBinding: d.binding = int(lit); break;
                case DecLocation: d.location = int(lit); break;
                case DecComponent: d.component = int(lit); break;
                case DecBuiltIn: d.builtin = int(lit); break;
                case DecBlock: d.block = true; break;
                case DecBufferBlock: d.buffer_block = true; break;
                }
            }
            break;
        case OpMemberDecorate:
            if (n >= 4 && w[2] == DecBuiltIn) dec[w[0]].member_builtins.push_back(w[3]);
            break;
        case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeImage:
        case OpTypeSampler: case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct:
        case OpTypePointer:
            if (n >= 1) types[w[0]] = Type{op, std::vector<uint32_t>(w + 1, w + n)};
            break;
        case OpConstant:
            if (n >= 3) constants[w[1]] = w[2];  // low word is enough for array lengths
            break;
        case OpVariable:
            if (n >= 3) vars.push_back({w[1], w[0], w[2]});
            break;
        }
        i += wc;
    }
    if (!have_entry) return fail("no OpEntryPoint");

    auto type = [&](uint32_t id) -> const Type* {
        auto it = types.find(id);
        return it == types.end() ? nullptr : &it->second;
    };
    auto scalar = [&](uint32_t id, uint32_t& comps) -> ScalarType {
        const Type* t = type(id);
        comps = 1;
        if (t && t->op == OpTypeVector && t->w.size() >= 2) comps = t->w[1], t = type(t->w[0]);
        if (!t) return ScalarType::Other;
        if (t->op == OpTypeFloat) return ScalarType::Float;
        if (t->op == OpTypeBool) return ScalarType::Bool;
        if (t->op == OpTypeInt && t->w.size() >= 2) return t->w[1] ? ScalarType::Sint : ScalarType::Uint;
        return ScalarType::Other;
    };
    auto builtins_of = [&](uint32_t var_id, uint32_t pointee, std::vector<uint32_t>& list) {
        auto add = [&](uint32_t b) {
            if (std::find(list.begin(), list.end(), b) == list.end()) list.push_back(b);
        };
        if (auto it = dec.find(var_id); it != dec.end() && it->second.builtin >= 0) add(uint32_t(it->second.builtin));
        // gl_PerVertex-style blocks, possibly arrays of them
        for (const Type* t = type(pointee); t;) {
            if (t->op == OpTypeArray && !t->w.empty()) {
                pointee = t->w[0];
                t = type(pointee);
                continue;
            }
            if (t->op == OpTypeStruct)
                if (auto it = dec.find(pointee); it != dec.end())
                    for (uint32_t b : it->second.member_builtins) add(b);
            break;
        }
    };

    for (const Var& v : vars) {
        const Type* ptr = type(v.ptr_type);
        if (!ptr || ptr->op != OpTypePointer || ptr->w.size() < 2) continue;
        uint32_t pointee = ptr->w[1];
        const Decorations* d = nullptr;
        if (auto it = dec.find(v.id); it != dec.end()) d = &it->second;

        if (v.storage == ScInput || v.storage == ScOutput) {
            auto& list = v.storage == ScInput ? out.builtins_in : out.builtins_out;
            builtins_of(v.id, pointee, list);
            if (!d || d->location < 0 || d->builtin >= 0) continue;
            ShaderIo io;
            io.location = uint32_t(d->location);
            io.component = uint32_t(d->component);
            uint32_t elem = pointee;
            if (const Type* t = type(elem); t && t->op == OpTypeArray && t->w.size() >= 2) {
                io.array = constants.count(t->w[1]) ? constants[t->w[1]] : 1;
                elem = t->w[0];
            }
            io.type = scalar(elem, io.components);
            (v.storage == ScInput ? out.inputs : out.outputs).push_back(io);
            continue;
        }
        if (v.storage != ScUniformConstant && v.storage != ScUniform && v.storage != ScStorageBuffer) continue;
        if (!d || d->set < 0 || d->binding < 0) continue;

        DescriptorBinding b;
        b.set = uint32_t(d->set);
        b.binding = uint32_t(d->binding);
        if (auto it = names.find(v.id); it != names.end()) b.name = it->second;
        uint32_t inner = pointee;
        for (const Type* t = type(inner); t && (t->op == OpTypeArray || t->op == OpTypeRuntimeArray); t = type(inner)) {
            if (t->op == OpTypeRuntimeArray) b.count = 0;
            else if (t->w.size() >= 2) b.count *= constants.count(t->w[1]) ? constants[t->w[1]] : 1;
            inner = t->w.empty() ? 0 : t->w[0];
        }
        const Type* t = type(inner);
        if (!t) return fail("descriptor of unknown type");
        auto image_shape = [&](const Type* img) {
            ImageShape s;
            if (img && img->op == OpTypeImage && img->w.size() >= 7) {
                uint32_t c;
                s.component = scalar(img->w[0], c);
                s.dim = uint8_t(img->w[1]);
                s.depth = img->w[2] == 1, s.arrayed = img->w[3] != 0, s.ms = img->w[4] != 0;
                s.sampled = uint8_t(img->w[5]);
                s.format = img->w[6];
            }
            return s;
        };
        if (t->op == OpTypeStruct) {
            const Decorations* sd = nullptr;
            if (auto it = dec.find(inner); it != dec.end()) sd = &it->second;
            bool storage = v.storage == ScStorageBuffer || (sd && sd->buffer_block);
            b.type = storage ? DescriptorType::StorageBuffer : DescriptorType::UniformBuffer;
        } else if (t->op == OpTypeSampledImage) {
            b.image = image_shape(t->w.empty() ? nullptr : type(t->w[0]));
            b.type = b.image.dim == kDimBuffer ? DescriptorType::UniformTexelBuffer
                                               : DescriptorType::CombinedImageSampler;
        } else if (t->op == OpTypeImage) {
            b.image = image_shape(t);
            bool storage = b.image.sampled == 2;
            if (b.image.dim == kDimBuffer)
                b.type = storage ? DescriptorType::StorageTexelBuffer : DescriptorType::UniformTexelBuffer;
            else
                b.type = storage ? DescriptorType::StorageImage : DescriptorType::SampledImage;
        } else if (t->op == OpTypeSampler) {
            b.type = DescriptorType::Sampler;
        } else {
            return fail("descriptor of unexpected type");
        }
        derive_meaning(b, out.stage);
        if (b.nvn.source == NvnSource::SupportBuffer) out.uses_support_buffer = true;
        out.bindings.push_back(std::move(b));
    }
    std::sort(out.bindings.begin(), out.bindings.end(), [](const DescriptorBinding& a, const DescriptorBinding& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
    auto by_location = [](const ShaderIo& a, const ShaderIo& b) {
        return a.location != b.location ? a.location < b.location : a.component < b.component;
    };
    std::sort(out.inputs.begin(), out.inputs.end(), by_location);
    std::sort(out.outputs.begin(), out.outputs.end(), by_location);
    return true;
}

// ---- harness records ------------------------------------------------------------------------------------------------

bool parse_harness_resources(std::string_view text, std::vector<HarnessResource>& out) {
    out.clear();
    while (!text.empty()) {
        size_t sp = text.find(' ');
        std::string_view tok = text.substr(0, sp);
        text = sp == std::string_view::npos ? std::string_view{} : text.substr(sp + 1);
        if (tok.empty()) continue;
        // "...=s<set>b<binding>"
        size_t eq = tok.rfind('=');
        if (eq == std::string_view::npos || eq + 1 >= tok.size() || tok[eq + 1] != 's') return false;
        std::string_view sb = tok.substr(eq + 2);
        size_t bpos = sb.find('b');
        HarnessResource r;
        if (bpos == std::string_view::npos || !parse_uint(sb.substr(0, bpos), 10, r.set) ||
            !parse_uint(sb.substr(bpos + 1), 10, r.binding))
            return false;
        std::string_view lhs = tok.substr(0, eq);
        if (lhs.substr(0, 2) == "cb") {
            r.kind = HarnessResource::Uniform;
            if (!parse_uint(lhs.substr(2), 10, r.bank)) return false;
        } else if (lhs.substr(0, 4) == "sb[c") {
            // sb[c<slot>+0x<off>]
            r.kind = HarnessResource::Storage;
            size_t plus = lhs.find("+0x"), close = lhs.find(']');
            if (plus == std::string_view::npos || close == std::string_view::npos) return false;
            if (!parse_uint(lhs.substr(4, plus - 4), 10, r.bank) ||
                !parse_uint(lhs.substr(plus + 3, close - plus - 3), 16, r.word))
                return false;
        } else if (lhs.substr(0, 3) == "tex" || lhs.substr(0, 3) == "img") {
            // tex<type>[c<slot>:<handle>(x<n>)]
            r.kind = lhs[0] == 't' ? HarnessResource::Texture : HarnessResource::Image;
            size_t open = lhs.find("[c"), colon = lhs.find(':'), close = lhs.find(']');
            if (open == std::string_view::npos || colon == std::string_view::npos || close == std::string_view::npos)
                return false;
            r.type = std::string(lhs.substr(3, open - 3));
            std::string_view bank = lhs.substr(open + 2, colon - open - 2);
            bool neg = !bank.empty() && bank[0] == '-';
            if (!parse_uint(neg ? bank.substr(1) : bank, 10, r.bank)) return false;
            if (neg) r.bank = -r.bank;
            std::string_view handle = lhs.substr(colon + 1, close - colon - 1);
            size_t x = handle.find('x');
            if (x != std::string_view::npos) {
                if (!parse_uint(handle.substr(x + 1), 10, r.array_length)) return false;
                handle = handle.substr(0, x);
            }
            if (!parse_uint(handle, 10, r.word)) return false;
        } else {
            return false;
        }
        out.push_back(std::move(r));
    }
    return true;
}

size_t parse_results_tsv(std::string_view text, std::unordered_map<std::string, std::string>& out) {
    size_t lines = 0;
    int resources_col = -1;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::vector<std::string_view> cols;
        for (size_t start = 0;;) {
            size_t tab = line.find('\t', start);
            cols.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
            if (tab == std::string_view::npos) break;
            start = tab + 1;
        }
        if (resources_col < 0) {  // header line names the columns
            for (size_t i = 0; i < cols.size(); i++)
                if (cols[i] == "resources") resources_col = int(i);
            if (resources_col < 0) return 0;
            continue;
        }
        if (cols.size() <= size_t(resources_col) || cols[0].empty()) continue;
        out[std::string(cols[0])] = std::string(cols[resources_col]);
        lines++;
    }
    return lines;
}

bool apply_harness(ShaderReflection& refl, const std::vector<HarnessResource>& resources) {
    bool ok = true;
    for (const HarnessResource& r : resources) {
        DescriptorBinding* b = nullptr;
        for (auto& x : refl.bindings)
            if (int(x.set) == r.set && int(x.binding) == r.binding) b = &x;
        if (!b) {
            ok = false;
            continue;
        }
        switch (r.kind) {
        case HarnessResource::Storage:
            if (b->type != DescriptorType::StorageBuffer || r.bank != 0) {
                ok = false;
                break;
            }
            set_c0_offset(b->nvn, refl.stage, r.word * 4);  // the harness prints 32-bit words
            break;
        case HarnessResource::Texture:
        case HarnessResource::Image:
            if (b->nvn.word < 0) set_texture_handle(b->nvn, r.kind == HarnessResource::Image, r.bank, r.word);
            break;
        case HarnessResource::Uniform:
            break;  // name and binding already say it
        }
    }
    return ok;
}

std::string sampler_type_text(const ImageShape& s) {
    static const char* kDims[] = {"Texture1D", "Texture2D", "Texture3D", "TextureCube", "?", "TextureBuffer"};
    std::string t = s.dim < 6 ? kDims[s.dim] : "?";
    if (s.arrayed) t += "|Array";
    if (s.ms) t += "|Multisample";
    if (s.depth) t += "|Shadow";
    return t;
}

std::string describe_like_harness(const ShaderReflection& refl) {
    std::string out;
    char tmp[160];
    for (const auto& b : refl.bindings) {
        const NvnMeaning& m = b.nvn;
        tmp[0] = 0;
        switch (b.type) {
        case DescriptorType::UniformBuffer:
            if (m.source != NvnSource::SupportBuffer)
                std::snprintf(tmp, sizeof tmp, "cb%d=s%ub%u", m.bank, b.set, b.binding);
            break;
        case DescriptorType::StorageBuffer:
            if (m.c0_offset >= 0)
                std::snprintf(tmp, sizeof tmp, "sb[c0+0x%x]=s%ub%u", m.c0_offset / 4, b.set, b.binding);
            else
                std::snprintf(tmp, sizeof tmp, "sb[?]=s%ub%u", b.set, b.binding);
            break;
        default: {
            bool image = b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer;
            std::string arr = b.count != 1 ? "x" + std::to_string(b.count) : "";
            std::snprintf(tmp, sizeof tmp, "%s%s[c%d:%d%s]=s%ub%u", image ? "img" : "tex",
                          sampler_type_text(b.image).c_str(), m.default_bank ? -1 : m.bank, m.word, arr.c_str(), b.set,
                          b.binding);
        }
        }
        if (!tmp[0]) continue;
        if (!out.empty()) out += ' ';
        out += tmp;
    }
    return out;
}


}  // namespace nx::nvn
