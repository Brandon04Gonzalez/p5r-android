// NVN command buffers (docs/NVN.md, N6): binds, state objects and draws, recorded for nvn_draw.cpp to replay.
//
// State objects are plain structs of the SDK driver; they are decoded here, while recording, through its getters
// (the game reuses its structs right after binding them). Signatures from the game's call sites (nvn_trace.txt):
//   BindProgram(cmd, program, stageBits)            BindUniformBuffer(cmd, stage, index, address, size)
//   BindStorageBuffer(cmd, stage, index, address, size)   BindTexture(cmd, stage, index, handle)
//   BindVertexBuffer(cmd, index, address, size)     BindVertexAttribState(cmd, count, const state*)  (4 bytes each)
//   BindVertexStreamState(cmd, count, const state*) (8 bytes each)   Bind{Blend,Color,...}State(cmd, const state*)
//   UpdateUniformBuffer(cmd, address, size, offset, dataSize, const void* data)
//   DrawArrays(cmd, mode, first, count)             DrawElements(cmd, mode, indexType, count, indexAddress)
//   DrawElementsBaseVertex(..., baseVertex)         DrawElementsInstanced(..., baseVertex, baseInstance, instances)
//   DrawArraysInstanced(cmd, mode, first, count, baseInstance, instances)
// Shader stages are numbered 0 vertex, 1 fragment (the pairs the game binds), 5 compute.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nvn.h"
#include "nvn_shaders.h"
#include "platform.h"

namespace nx::nvn {

namespace {

// NX_NVN_STATES=1: log each distinct decoded state once (to check the NVN enum values against what the game uses).
template <typename T>
void log_state(const char* what, const T& s) {
    static const bool on = std::getenv("NX_NVN_STATES") != nullptr;
    if (!on) return;
    static std::mutex lock;
    static std::set<std::string> seen;
    std::string key(reinterpret_cast<const char*>(&s), sizeof s);
    std::lock_guard lk(lock);
    if (!seen.insert(key).second || seen.size() > 200) return;
    std::string words;
    for (size_t k = 0; k + 4 <= sizeof s; k += 4) {
        uint32_t w;
        std::memcpy(&w, reinterpret_cast<const uint8_t*>(&s) + k, 4);
        char b[16];
        std::snprintf(b, sizeof b, " %x", w);
        words += b;
    }
    log("nvn state %s:%s", what, words.c_str());
}

// ---- shaders and resources --------------------------------------------------------------------------------------

void BindProgram(CPU& c) {
    Cmd cmd{Cmd::BindProgram};
    cmd.a[0] = c.x[1];
    cmd.mask = uint32_t(c.x[2]);
    record(c.x[0], cmd);
    static const bool trace = std::getenv("NX_TRACE_PROGRAMS") != nullptr;  // binds of skinned programs (input 6)
    static int logged = 0;
    if (const auto* sh = trace && logged < 40 ? program_shaders(c.x[1]) : nullptr)
        for (const ShaderInfo* s : *sh)
            for (const ShaderIo& in : s->refl.inputs)
                if (s->stage == ShaderStage::Vertex && in.location == 6 && logged++ < 40)
                    log("nvn: skinned program %llx (vs %s) bound into cmd %llx", (unsigned long long)c.x[1],
                        s->hash.c_str(), (unsigned long long)c.x[0]);
}

void BindUniformBuffer(CPU& c) {
    Cmd cmd{Cmd::BindUniformBuffer};
    cmd.i[0] = int32_t(c.x[1]), cmd.i[1] = int32_t(c.x[2]);
    cmd.a[0] = c.x[3], cmd.a[1] = c.x[4];
    record(c.x[0], cmd);
}

void BindStorageBuffer(CPU& c) {
    Cmd cmd{Cmd::BindStorageBuffer};
    cmd.i[0] = int32_t(c.x[1]), cmd.i[1] = int32_t(c.x[2]);
    cmd.a[0] = c.x[3], cmd.a[1] = c.x[4];
    record(c.x[0], cmd);
}

void BindTexture(CPU& c) {
    Cmd cmd{Cmd::BindTexture};
    cmd.i[0] = int32_t(c.x[1]), cmd.i[1] = int32_t(c.x[2]);
    cmd.a[0] = c.x[3];
    record(c.x[0], cmd);
}

void BindVertexBuffer(CPU& c) {
    Cmd cmd{Cmd::BindVertexBuffer};
    cmd.i[0] = int32_t(c.x[1]);
    cmd.a[0] = c.x[2], cmd.a[1] = c.x[3];
    record(c.x[0], cmd);
}

void SetTexturePool(CPU& c) {
    Cmd cmd{Cmd::SetTexturePool};
    cmd.a[0] = c.x[1];
    record(c.x[0], cmd);
}

void SetSamplerPool(CPU& c) {
    Cmd cmd{Cmd::SetSamplerPool};
    cmd.a[0] = c.x[1];
    record(c.x[0], cmd);
}

void UpdateUniformBuffer(CPU& c) {  // the data is copied now: the game's buffer is on its stack
    Cmd cmd{Cmd::UpdateUniformBuffer};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[2], cmd.a[2] = c.x[3];
    size_t n = size_t(c.x[4]);
    record(c.x[0], cmd, reinterpret_cast<const void*>(c.x[5]), n);
}

// ---- state objects ------------------------------------------------------------------------------------------------

void BindVertexAttribState(CPU& c) {
    uint32_t n = std::min<uint32_t>(uint32_t(c.x[1]), 32);
    std::vector<VertexAttrib> v(n);
    uint64_t tmp = scratch(c, 16);
    for (uint32_t k = 0; k < n; k++) {
        uint64_t state = c.x[2] + 4 * k;
        driver(c, "nvnVertexAttribStateGetFormat", {state, tmp, tmp + 8});
        v[k].format = ld<uint32_t>(tmp);
        v[k].offset = uint32_t(ld<uint64_t>(tmp + 8));
        v[k].stream = uint32_t(driver(c, "nvnVertexAttribStateGetStreamIndex", {state}));
    }
    for (auto& a : v) log_state("attrib", a);
    Cmd cmd{Cmd::BindVertexAttribs};
    cmd.n = n;
    record(c.x[0], cmd, v.data(), v.size() * sizeof(VertexAttrib));
}

void BindVertexStreamState(CPU& c) {
    uint32_t n = std::min<uint32_t>(uint32_t(c.x[1]), 32);
    std::vector<VertexStream> v(n);
    for (uint32_t k = 0; k < n; k++) {
        uint64_t state = c.x[2] + 8 * k;
        v[k].stride = uint32_t(driver(c, "nvnVertexStreamStateGetStride", {state}));
        v[k].divisor = uint32_t(driver(c, "nvnVertexStreamStateGetDivisor", {state}));
    }
    Cmd cmd{Cmd::BindVertexStreams};
    cmd.n = n;
    record(c.x[0], cmd, v.data(), v.size() * sizeof(VertexStream));
}

void BindBlendState(CPU& c) {
    uint64_t state = c.x[1], tmp = scratch(c, 16);
    BlendTarget b;
    b.target = uint32_t(driver(c, "nvnBlendStateGetBlendTarget", {state}));
    driver(c, "nvnBlendStateGetBlendFunc", {state, tmp, tmp + 4, tmp + 8, tmp + 12});
    b.src_rgb = ld<uint32_t>(tmp), b.dst_rgb = ld<uint32_t>(tmp + 4);
    b.src_a = ld<uint32_t>(tmp + 8), b.dst_a = ld<uint32_t>(tmp + 12);
    driver(c, "nvnBlendStateGetBlendEquation", {state, tmp, tmp + 4});
    b.eq_rgb = ld<uint32_t>(tmp), b.eq_a = ld<uint32_t>(tmp + 4);
    Cmd cmd{Cmd::BindBlendState};
    log_state("blend", b);
    record(c.x[0], cmd, &b, sizeof b);
}

void BindColorState(CPU& c) {
    uint64_t state = c.x[1];
    ColorStateInfo s;
    for (uint32_t k = 0; k < 8; k++) s.blend_enable[k] = uint8_t(driver(c, "nvnColorStateGetBlendEnable", {state, k}) & 1);
    s.alpha_test = uint32_t(driver(c, "nvnColorStateGetAlphaTest", {state}));
    s.logic_op = uint32_t(driver(c, "nvnColorStateGetLogicOp", {state}));
    Cmd cmd{Cmd::BindColorState};
    log_state("color", s);
    record(c.x[0], cmd, &s, sizeof s);
}

void BindChannelMaskState(CPU& c) {
    uint64_t state = c.x[1], tmp = scratch(c, 16);
    ChannelMaskInfo s;
    for (uint32_t k = 0; k < 8; k++) {
        // Four NVNboolean* outputs, one slot of 4 bytes each: slots 1 byte apart overlapped when the driver stores
        // words, and draws lost channels (the title art came out green only).
        for (int ch = 0; ch < 4; ch++) st<uint32_t>(tmp + 4 * ch, 0);
        driver(c, "nvnChannelMaskStateGetChannelMask", {state, k, tmp, tmp + 4, tmp + 8, tmp + 12});
        s.mask[k] = uint8_t((ld<uint8_t>(tmp) & 1) | (ld<uint8_t>(tmp + 4) & 1) << 1 | (ld<uint8_t>(tmp + 8) & 1) << 2 |
                            (ld<uint8_t>(tmp + 12) & 1) << 3);
    }
    Cmd cmd{Cmd::BindChannelMaskState};
    record(c.x[0], cmd, &s, sizeof s);
}

void BindDepthStencilState(CPU& c) {
    uint64_t state = c.x[1], tmp = scratch(c, 16);
    DepthStencilInfo s;
    s.depth_test = uint8_t(driver(c, "nvnDepthStencilStateGetDepthTestEnable", {state}) & 1);
    s.depth_write = uint8_t(driver(c, "nvnDepthStencilStateGetDepthWriteEnable", {state}) & 1);
    s.depth_func = uint32_t(driver(c, "nvnDepthStencilStateGetDepthFunc", {state}));
    s.stencil_test = uint8_t(driver(c, "nvnDepthStencilStateGetStencilTestEnable", {state}) & 1);
    for (uint32_t face = 0; face < 2; face++) {  // NVNface: 1 front, 2 back
        s.stencil_func[face] = uint32_t(driver(c, "nvnDepthStencilStateGetStencilFunc", {state, face + 1}));
        driver(c, "nvnDepthStencilStateGetStencilOp", {state, face + 1, tmp, tmp + 4, tmp + 8});
        s.stencil_fail[face] = ld<uint32_t>(tmp), s.depth_fail[face] = ld<uint32_t>(tmp + 4);
        s.depth_pass[face] = ld<uint32_t>(tmp + 8);
    }
    Cmd cmd{Cmd::BindDepthStencilState};
    log_state("depth", s);
    record(c.x[0], cmd, &s, sizeof s);
}

void BindPolygonState(CPU& c) {
    uint64_t state = c.x[1];
    PolygonInfo s;
    s.cull = uint32_t(driver(c, "nvnPolygonStateGetCullFace", {state}));
    s.front = uint32_t(driver(c, "nvnPolygonStateGetFrontFace", {state}));
    s.mode = uint32_t(driver(c, "nvnPolygonStateGetPolygonMode", {state}));
    s.offset_enables = uint32_t(driver(c, "nvnPolygonStateGetPolygonOffsetEnables", {state}));
    Cmd cmd{Cmd::BindPolygonState};
    log_state("polygon", s);
    record(c.x[0], cmd, &s, sizeof s);
}

// BindMultisampleState(cmd, const NVNmultisampleState*) (G7; it was a trace stub). Render targets here are single
// sampled, so only alpha to coverage is decoded (through the driver's getter, like the other state objects); sample
// counts, coverage modulation and the alpha-to-coverage dither are not.
void BindMultisampleState(CPU& c) {
    uint64_t state = c.x[1];
    Cmd cmd{Cmd::BindMultisampleState};
    if (uint64_t fn = state ? real_proc(c, "nvnMultisampleStateGetAlphaToCoverageEnable") : 0)
        cmd.n = uint32_t(guest_call(c, fn, {state}) & 1);
    log_state("multisample", cmd.n);
    record(c.x[0], cmd);
}

void SetBlendColor(CPU& c) {
    Cmd cmd{Cmd::SetBlendColor};
    if (c.x[1]) std::memcpy(cmd.f, reinterpret_cast<const void*>(c.x[1]), 16);
    record(c.x[0], cmd);
}

void SetDepthRange(CPU& c) {  // (cmd, float near [s0], float far [s1])
    Cmd cmd{Cmd::SetDepthRange};
    cmd.f[0] = c.v[0].f32[0], cmd.f[1] = c.v[1].f32[0];
    record(c.x[0], cmd);
}

std::atomic<uint64_t> g_recorded_draws{0};

// ---- draws --------------------------------------------------------------------------------------------------------

void DrawArrays(CPU& c) {
    g_recorded_draws++;
    Cmd cmd{Cmd::DrawArrays};
    cmd.n = uint32_t(c.x[1]);                                    // mode
    cmd.i[0] = int32_t(c.x[2]), cmd.i[1] = int32_t(c.x[3]);      // first, count
    cmd.i[2] = 0, cmd.i[3] = 1;                                  // base instance, instances
    record(c.x[0], cmd);
}

void DrawArraysInstanced(CPU& c) {
    Cmd cmd{Cmd::DrawArrays};
    cmd.n = uint32_t(c.x[1]);
    cmd.i[0] = int32_t(c.x[2]), cmd.i[1] = int32_t(c.x[3]);
    cmd.i[2] = int32_t(c.x[4]), cmd.i[3] = int32_t(c.x[5]);
    record(c.x[0], cmd);
}

void draw_elements(CPU& c, int32_t base_vertex, int32_t base_instance, int32_t instances) {
    g_recorded_draws++;
    Cmd cmd{Cmd::DrawElements};
    cmd.n = uint32_t(c.x[1]);        // mode
    cmd.mask = uint32_t(c.x[2]);     // index type: 0 u8, 1 u16, 2 u32
    cmd.i[0] = int32_t(c.x[3]);      // count
    cmd.a[0] = c.x[4];               // index buffer address
    cmd.i[1] = base_vertex, cmd.i[2] = base_instance, cmd.i[3] = instances;
    record(c.x[0], cmd);
}

void DrawElements(CPU& c) { draw_elements(c, 0, 0, 1); }
void DrawElementsBaseVertex(CPU& c) { draw_elements(c, int32_t(c.x[5]), 0, 1); }
void DrawElementsInstanced(CPU& c) { draw_elements(c, int32_t(c.x[5]), int32_t(c.x[6]), int32_t(c.x[7])); }

void ignore(CPU&) {}

// ReportCounter(cmd, NVNcounterType, NVNbufferAddress): the GPU writes {u64 value, u64 timestamp} there when it gets
// to this point. Engines read these to know how far the GPU is (Wonder skips drawing the scene when they never
// arrive), so the replay writes them.
void ReportCounter(CPU& c) {
    Cmd cmd{Cmd::ReportCounter};
    cmd.n = uint32_t(c.x[1]);
    cmd.a[0] = c.x[2];
    record(c.x[0], cmd);
}

// ResetCounter(cmd, NVNcounterType): the counter restarts from 0 (P5R: samples passed, before each object of its
// occlusion pass; nvn_occlusion.cpp).
void ResetCounter(CPU& c) {
    Cmd cmd{Cmd::ResetCounter};
    cmd.n = uint32_t(c.x[1]);
    record(c.x[0], cmd);
}

// Our GPU timestamps are host nanoseconds: the conversions are identities.
void GetTimestampInNanoseconds(CPU& c) { c.x[0] = c.x[1] ? ld<uint64_t>(c.x[1] + 8) : 0; }  // (device, counterData*)
void GetCurrentTimestampInNanoseconds(CPU& c) { c.x[0] = plat::monotonic_ns(); }


}  // namespace

uint64_t recorded_draws() { return g_recorded_draws; }

NX_HLE("nvnCommandBufferBindProgram", BindProgram);
NX_HLE("nvnCommandBufferBindUniformBuffer", BindUniformBuffer);
NX_HLE("nvnCommandBufferBindStorageBuffer", BindStorageBuffer);
NX_HLE("nvnCommandBufferBindTexture", BindTexture);
NX_HLE("nvnCommandBufferBindVertexBuffer", BindVertexBuffer);
NX_HLE("nvnCommandBufferSetTexturePool", SetTexturePool);
NX_HLE("nvnCommandBufferSetSamplerPool", SetSamplerPool);
NX_HLE("nvnCommandBufferUpdateUniformBuffer", UpdateUniformBuffer);
NX_HLE("nvnCommandBufferBindVertexAttribState", BindVertexAttribState);
NX_HLE("nvnCommandBufferBindVertexStreamState", BindVertexStreamState);
NX_HLE("nvnCommandBufferBindBlendState", BindBlendState);
NX_HLE("nvnCommandBufferBindColorState", BindColorState);
NX_HLE("nvnCommandBufferBindChannelMaskState", BindChannelMaskState);
NX_HLE("nvnCommandBufferBindDepthStencilState", BindDepthStencilState);
NX_HLE("nvnCommandBufferBindPolygonState", BindPolygonState);
NX_HLE("nvnCommandBufferBindMultisampleState", BindMultisampleState);
NX_HLE("nvnCommandBufferSetBlendColor", SetBlendColor);
NX_HLE("nvnCommandBufferSetDepthRange", SetDepthRange);
NX_HLE("nvnCommandBufferDrawArrays", DrawArrays);
NX_HLE("nvnCommandBufferDrawArraysInstanced", DrawArraysInstanced);
NX_HLE("nvnCommandBufferDrawElements", DrawElements);
NX_HLE("nvnCommandBufferDrawElementsBaseVertex", DrawElementsBaseVertex);
NX_HLE("nvnCommandBufferDrawElementsInstanced", DrawElementsInstanced);
NX_HLE("nvnCommandBufferBarrier", ignore);
NX_HLE("nvnCommandBufferReportCounter", ReportCounter);
NX_HLE("nvnCommandBufferResetCounter", ResetCounter);
NX_HLE("nvnDeviceGetTimestampInNanoseconds", GetTimestampInNanoseconds);
NX_HLE("nvnDeviceGetCurrentTimestampInNanoseconds", GetCurrentTimestampInNanoseconds);  // every replayed command is serialized for now

}  // namespace nx::nvn
