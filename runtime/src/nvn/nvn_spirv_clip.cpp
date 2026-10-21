// Clip-space depth -1..1 in the vertex shader (docs/nvn/DEBUG-2.md #67). NVN depth mode 0 (what Wonder uses) puts
// clip z in -w..w, Vulkan in 0..w. VK_EXT_depth_clip_control is meant to take the difference, but on MoltenVK the world
// map's depth linearization read half the distance (the stored depth was clip z as is): the player composite then took
// the player for hidden behind the scenery (a flat silhouette, issue #50). What Ryujinx does without the extension is
// done here instead: before each return of the vertex entry point, gl_Position.z = (z + w) / 2.
#include <cstdlib>
#include <map>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeStruct = 30, OpTypePointer = 32,
    OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62,
    OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpCompositeInsert = 82,
    OpFAdd = 129, OpFMul = 133, OpReturn = 253,
    DecorationBuiltIn = 11, BuiltInPosition = 0, StorageOutput = 3, ModelVertex = 0,
};

struct Inst {
    size_t at;
    uint32_t op, len;
};

}  // namespace

bool patch_clip_z(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3], entry = 0, f32 = 0, u32 = 0, v4f = 0, ptr_v4f = 0, zero = 0, half = 0;
    uint32_t block = 0, member = 0, position_var = 0;  // gl_PerVertex member, or a variable decorated Position
    std::map<uint32_t, uint32_t> struct_ptr;             // Output pointer type -> pointee
    size_t first_function = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPoint && n.len >= 3 && w[1] == ModelVertex && !entry) entry = w[2];
        else if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition)
            block = w[1], member = w[2];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition)
            position_var = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 0) u32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && w[2] == f32 && w[3] == 4) v4f = w[1];
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageOutput) {
            if (w[3] == v4f && v4f) ptr_v4f = w[1];
            struct_ptr[w[1]] = w[3];
        } else if (n.op == OpConstant && n.len == 4 && u32 && w[1] == u32 && w[3] == member) {
            zero = w[2];
        } else if (n.op == OpConstant && n.len == 4 && f32 && w[1] == f32 && w[3] == 0x3f000000u) {
            half = w[2];
        } else if (n.op == OpFunction && first_function == in.size()) {
            first_function = n.at;
        }
    }
    if (!entry || !f32 || !v4f) return false;
    // The Output variable that holds gl_Position: the gl_PerVertex block's, or one decorated Position itself.
    uint32_t var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageOutput) continue;
        auto p = struct_ptr.find(w[1]);
        if (p == struct_ptr.end()) continue;
        if (block && p->second == block) var = w[2];
        if (!block && w[2] == position_var && p->second == v4f) var = w[2];
    }
    if (!var) return false;
    std::vector<uint32_t> globals;  // types and constants missing, before the first function
    if (!ptr_v4f) ptr_v4f = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypePointer, ptr_v4f, StorageOutput, v4f});
    if (block) {
        if (!u32) u32 = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypeInt, u32, 32, 0});
        if (!zero) zero = bound++, globals.insert(globals.end(), {(4u << 16) | OpConstant, u32, zero, member});
    }
    if (!half) half = bound++, globals.insert(globals.end(), {(4u << 16) | OpConstant, f32, half, 0x3f000000u});

    out.assign(in.begin(), in.begin() + 5);
    uint32_t function = 0;
    int returns = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.at == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if (n.op == OpFunction) function = w[2];
        if (n.op == OpReturn && function == entry) {
            uint32_t p = var;
            if (block) {
                p = bound++;
                out.insert(out.end(), {(5u << 16) | OpAccessChain, ptr_v4f, p, var, zero});
            }
            uint32_t v = bound++, z = bound++, ww = bound++, s = bound++, h = bound++, r = bound++;
            out.insert(out.end(), {(4u << 16) | OpLoad, v4f, v, p});
            out.insert(out.end(), {(5u << 16) | OpCompositeExtract, f32, z, v, 2});
            out.insert(out.end(), {(5u << 16) | OpCompositeExtract, f32, ww, v, 3});
            out.insert(out.end(), {(5u << 16) | OpFAdd, f32, s, z, ww});
            out.insert(out.end(), {(5u << 16) | OpFMul, f32, h, s, half});
            out.insert(out.end(), {(6u << 16) | OpCompositeInsert, v4f, r, h, v, 2});
            out.insert(out.end(), {(3u << 16) | OpStore, p, r});
            returns++;
        }
        if (n.op == OpFunctionEnd) function = 0;
        out.insert(out.end(), w, w + n.len);
    }
    if (!returns) return false;
    out[3] = bound;
    return true;
}

// NVN depth mode 0 puts clip-space z in -w..w. MoltenVK offers VK_EXT_depth_clip_control, but the world map's depth
// linearization read half the distance (issue #50, DEBUG-2.md #67). NX_CLIP_Z_PATCH=1: the extension is not used and
// the vertex shaders map z themselves (nvn_spirv_clip.cpp). (Doing it with the viewport depth range instead clipped
// every 2D pass with z below 0: black frames.)
bool depth_remap() {
    static const bool on = [] {
        const char* e = std::getenv("NX_CLIP_Z_PATCH");
        return e && e[0] == '1';
    }();
    return on;
}

void clip_z_module(const ShaderInfo* s, std::vector<uint32_t>& code) {
    if (!depth_remap() || !clip_z_minus_one()) return;  // depth mode 1 (P5R): already 0..w
    std::vector<uint32_t> clipped;
    if (patch_clip_z(code, clipped))
        code.swap(clipped);
    else
        log("nvn/vk: shader %s: clip z not patched (no gl_Position found)", s->hash.c_str());
}

}  // namespace nx::nvn
