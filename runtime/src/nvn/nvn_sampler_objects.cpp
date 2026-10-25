// NVN samplers (docs/NVN.md, N4; restored in docs/nvn/DEBUG.md #50: commit c41b534 replaced this file with the
// sampler cache, and every texture sampled with SamplerInfo{} since: nearest, clamp, mip 0, shadow compare ALWAYS).
// Sampler objects and sampler-pool entries, decoded into SamplerInfo from the
// builder through the SDK driver's getters. The sampler objects' own getters (nvnSamplerGetMinMagFilter, ... the
// game reads them back) answer from the decoded state. Getter shapes from the game's call sites:
//   GetMinMagFilter(s, &min, &mag)   GetWrapMode(s, &s, &t, &r)   GetLodClamp(s, &min, &max)   GetLodBias(s) -> float
//   GetCompare(s, &mode, &func)      GetBorderColor(s, float[4])  GetMaxAnisotropy(s) -> float
//   GetReductionFilter(s) -> enum
#include <cstdlib>
#include <cstring>

#include "nvn.h"

namespace nx::nvn {

namespace {

struct Sampler : Object {
    SamplerInfo info;
};

SamplerInfo decode_builder(CPU& c, uint64_t b) {
    SamplerInfo s;
    uint64_t tmp = scratch(c, 32);
    driver(c, "nvnSamplerBuilderGetMinMagFilter", {b, tmp, tmp + 4});
    s.min_filter = ld<uint32_t>(tmp), s.mag_filter = ld<uint32_t>(tmp + 4);
    driver(c, "nvnSamplerBuilderGetWrapMode", {b, tmp, tmp + 4, tmp + 8});
    for (int k = 0; k < 3; k++) s.wrap[k] = ld<uint32_t>(tmp + 4 * k);
    driver(c, "nvnSamplerBuilderGetLodClamp", {b, tmp, tmp + 4});
    s.lod_min = ld<float>(tmp), s.lod_max = ld<float>(tmp + 4);
    s.lod_bias = guest_call_f(c, real_proc(c, "nvnSamplerBuilderGetLodBias"), {b});
    driver(c, "nvnSamplerBuilderGetCompare", {b, tmp, tmp + 4});
    s.compare_mode = ld<uint32_t>(tmp), s.compare_func = ld<uint32_t>(tmp + 4);
    driver(c, "nvnSamplerBuilderGetBorderColor", {b, tmp});
    std::memcpy(s.border, reinterpret_cast<const void*>(tmp), 16);
    s.max_anisotropy = guest_call_f(c, real_proc(c, "nvnSamplerBuilderGetMaxAnisotropy"), {b});
    s.reduction = uint32_t(driver(c, "nvnSamplerBuilderGetReductionFilter", {b}));
    return s;
}

void log_sampler(const SamplerInfo& s) {
    static const bool on = std::getenv("NX_NVN_STATES") != nullptr;
    if (on)
        log("nvn state sampler: filter %u/%u wrap %u %u %u lod %g..%g bias %g compare %u/%u aniso %g reduction %u",
            s.min_filter, s.mag_filter, s.wrap[0], s.wrap[1], s.wrap[2], double(s.lod_min), double(s.lod_max),
            double(s.lod_bias), s.compare_mode, s.compare_func, double(s.max_anisotropy), s.reduction);
}

void SamplerInitialize(CPU& c) {  // NVNboolean (NVNsampler*, const NVNsamplerBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    SamplerInfo info = decode_builder(c, builder);
    make<Sampler>(guest).info = info;
    log_sampler(info);
    c.x[0] = 1;
}

void SamplerFinalize(CPU& c) { destroy(c.x[0]); }

void register_info(uint64_t pool, uint32_t id, const SamplerInfo& info) {
    if (auto* d = get<DescriptorPool>(pool)) {
        std::lock_guard lk(objects_lock());
        // A replay in flight resolves the slot as of its mark (nvn_incarnation.cpp): a changed sampler keeps the
        // one it replaces (re-registering the same sampler every frame records nothing).
        SamplerSlot before;
        if (auto it = d->samplers.find(id); it != d->samplers.end()) before.has = true, before.info = it->second;
        if (!before.has || std::memcmp(&before.info, &info, sizeof info) != 0) note_sampler_change(pool, id, before);
        d->samplers[id] = info;
    }
}

void RegisterSampler(CPU& c) {  // (NVNsamplerPool*, int id, const NVNsampler*)
    if (auto* s = get<Sampler>(c.x[2])) register_info(c.x[0], uint32_t(c.x[1]), s->info);
}

void RegisterSamplerBuilder(CPU& c) {  // (NVNsamplerPool*, int id, const NVNsamplerBuilder*)
    SamplerInfo info = decode_builder(c, c.x[2]);
    register_info(c.x[0], uint32_t(c.x[1]), info);
}

const SamplerInfo& info_of(uint64_t guest) {
    static const SamplerInfo none;
    auto* s = get<Sampler>(guest);
    return s ? s->info : none;
}

void GetMinMagFilter(CPU& c) {
    const SamplerInfo& s = info_of(c.x[0]);
    if (c.x[1]) st<uint32_t>(c.x[1], s.min_filter);
    if (c.x[2]) st<uint32_t>(c.x[2], s.mag_filter);
}

void GetWrapMode(CPU& c) {
    const SamplerInfo& s = info_of(c.x[0]);
    for (int k = 0; k < 3; k++)
        if (c.x[1 + k]) st<uint32_t>(c.x[1 + k], s.wrap[k]);
}

void GetLodClamp(CPU& c) {
    const SamplerInfo& s = info_of(c.x[0]);
    if (c.x[1]) st<float>(c.x[1], s.lod_min);
    if (c.x[2]) st<float>(c.x[2], s.lod_max);
}

void GetLodBias(CPU& c) { c.v[0].f32[0] = info_of(c.x[0]).lod_bias; }
void GetMaxAnisotropy(CPU& c) { c.v[0].f32[0] = info_of(c.x[0]).max_anisotropy; }
void GetReductionFilter(CPU& c) { c.x[0] = info_of(c.x[0]).reduction; }

void GetCompare(CPU& c) {
    const SamplerInfo& s = info_of(c.x[0]);
    if (c.x[1]) st<uint32_t>(c.x[1], s.compare_mode);
    if (c.x[2]) st<uint32_t>(c.x[2], s.compare_func);
}

void GetBorderColor(CPU& c) {
    if (c.x[1]) std::memcpy(reinterpret_cast<void*>(c.x[1]), info_of(c.x[0]).border, 16);
}

}  // namespace

NX_HLE("nvnSamplerInitialize", SamplerInitialize);
NX_HLE("nvnSamplerFinalize", SamplerFinalize);
NX_HLE("nvnSamplerPoolRegisterSampler", RegisterSampler);
NX_HLE("nvnSamplerPoolRegisterSamplerBuilder", RegisterSamplerBuilder);
NX_HLE("nvnSamplerGetMinMagFilter", GetMinMagFilter);
NX_HLE("nvnSamplerGetWrapMode", GetWrapMode);
NX_HLE("nvnSamplerGetLodClamp", GetLodClamp);
NX_HLE("nvnSamplerGetLodBias", GetLodBias);
NX_HLE("nvnSamplerGetMaxAnisotropy", GetMaxAnisotropy);
NX_HLE("nvnSamplerGetReductionFilter", GetReductionFilter);
NX_HLE("nvnSamplerGetCompare", GetCompare);
NX_HLE("nvnSamplerGetBorderColor", GetBorderColor);

}  // namespace nx::nvn
