// NVN sampler states as Vulkan samplers (split from nvn_pipeline.cpp).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "nvn_pipeline.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkSamplerAddressMode vk_wrap(uint32_t w) {  // NVN: 0 clamp, 1 repeat, 2 mirror clamp, 3 mirror clamp to edge,
    switch (w) {                            // 4 mirror clamp to border, 5 clamp to border, 6 mirrored repeat, 7 edge
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2:
        case 3:
        case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

}  // namespace

// ---- samplers -------------------------------------------------------------------------------------------------------

VkSampler sampler_for(const SamplerInfo& s, bool shadow, bool biased) {
    // Asked per texture binding of every draw: the key is the state's bytes and the flags (what the std::string key
    // of before held, compared the same way), hashed; no allocation on a hit.
    using Key = ByteKey<sizeof(SamplerInfo) + 2>;
    static std::unordered_map<Key, VkSampler, ByteKeyHash> cache;
    Key key;
    std::memcpy(key.b, &s, sizeof s);
    key.b[sizeof s] = shadow ? '1' : '0';
    key.b[sizeof s + 1] = biased ? '1' : '0';
    auto& v = cache[key];
    if (v) return v;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // NVN min filter: 0 nearest, 1 linear, 2 nearest mip nearest, 3 linear mip nearest, 4 nearest mip linear,
    // 5 linear mip linear; mag: 0 nearest, 1 linear.
    uint32_t m = s.min_filter;
    ci.minFilter = (m == 1 || m == 3 || m == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.magFilter = s.mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (m == 4 || m == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = vk_wrap(s.wrap[0]);
    ci.addressModeV = vk_wrap(s.wrap[1]);
    ci.addressModeW = vk_wrap(s.wrap[2]);
    // Graphics quality (the P5R launcher's preset): NX_LOD_BIAS=<f> is added to the bias of mipmapped samplers (a
    // smaller mip: less texture bandwidth, softer surfaces) of the 3D draws only (`biased`: the draw tests depth): the
    // 2D ones (menus, HUD) keep their sharpness.
    static const float lod_bias_add = [] {
        const char* e = std::getenv("NX_LOD_BIAS");
        return std::clamp(e ? float(std::atof(e)) : 0.0f, -2.0f, 4.0f);
    }();
    ci.mipLodBias = s.lod_bias + (m >= 2 && biased ? lod_bias_add : 0.0f);
    // Games pass lod_min -1000 ("no limit"); Metal wants lodMinClamp >= 0, and a negative one made the glyph
    // sampler return 0 (all text invisible).
    ci.minLod = m <= 1 ? 0.0f : std::max(0.0f, s.lod_min);
    ci.maxLod = m <= 1 ? 0.25f : std::max(ci.minLod, s.lod_max);  // no mipmapping: level 0 only
    // NX_MAX_ANISO=<n>: anisotropic filtering at most n taps (1: off). P5R asks 16 for its ground and walls, which a
    // small GPU pays in every pixel of the 3D passes.
    static const float aniso_cap = [] {
        const char* e = std::getenv("NX_MAX_ANISO");
        return std::clamp(e ? float(std::atof(e)) : 16.0f, 1.0f, 16.0f);
    }();
    ci.maxAnisotropy = std::min(s.max_anisotropy, aniso_cap);
    ci.anisotropyEnable = ci.maxAnisotropy > 1.0f;
    ci.compareEnable = shadow;
    ci.compareOp = shadow ? vk_compare(s.compare_func) : VK_COMPARE_OP_ALWAYS;
    bool white = s.border[3] > 0.5f && s.border[0] > 0.5f;
    ci.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                     : s.border[3] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                          : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    p_vkCreateSampler(vk_device(), &ci, nullptr, &v);
    return v;
}

// ---- pitch-linear texture contents (moved from nvn_pipeline.cpp) ------------------------------------------------

// Four independent lanes: one multiply chain ran at ~1 byte per cycle (Wonder's 16 MB glyph cache: ~5 ms).
uint64_t hash_memory(const uint8_t* p, uint64_t size) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 1, 2, 3}, n = size / 32;
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    for (uint64_t k = 0; k < n; k++)
        for (int l = 0; l < 4; l++) h[l] = (h[l] ^ w[4 * k + l]) * 0x100000001B3ull + (h[l] >> 29);
    uint64_t r = h[0] ^ h[1] * 3 ^ h[2] * 5 ^ h[3] * 7;
    for (uint64_t k = n * 4; k < size / 8; k++) r = (r ^ w[k]) * 0x100000001B3ull + (r >> 29);
    return r;
}

// A pitch-linear texture the GPU does not write: did the CPU change it since the last upload?
// NX_TRACE_VIDEO: what the film's planes (pitch-linear, 640x360 and more) went through.
bool film_plane(const Texture& t) { return t.stride && uint64_t(t.width) * t.height >= 640 * 360; }
void film_trace(const Texture& t, const char* what, uint64_t a, uint64_t b) {
    static long left = std::getenv("NX_TRACE_VIDEO") ? 6 * std::atol(std::getenv("NX_TRACE_VIDEO")) : 0;
    if (left <= 0 || !film_plane(t)) return;
    left--;
    log("movie trace: check %llx %ux%u %s %llu %llu (watched %d, hash %llx, image %p up %d) at %.1f ms",
        (unsigned long long)t.memory, t.width, t.height, what, (unsigned long long)a, (unsigned long long)b,
        int(t.watched), (unsigned long long)t.content_hash, (void*)t.image.get(), t.image ? int(t.image->uploaded) : -1,
        double(plat::monotonic_ns() % 1000000000000ull) / 1e6);
}

void check_linear_content(Texture& t) {
    // Every 4th presented frame at most (every 4th submit was about every frame, ~1 ms); a new glyph shows 3 frames late.
    uint64_t frame = g_frame.load(std::memory_order_relaxed) + 1;
    if (film_plane(t) && t.image && !t.image->rendered && t.hashed_submit && t.hashed_submit + 1 > frame)
        film_trace(t, "skipped (checked this frame)", t.hashed_submit, frame);
    static const uint64_t hash_small = [] {
        const char* e = std::getenv("NX_HASH_SMALL");
        return e ? std::strtoull(e, nullptr, 10) : 0;
    }();
    bool small = hash_small && t.storage_size <= hash_small;
    // Film-sized pitch-linear planes (P5R's NV12 chroma, under 1 MB) every frame: a new picture each frame.
    if ((!t.stride && !small) || !t.memory || !t.image || t.image->rendered ||
        (t.hashed_submit && t.hashed_submit + (t.watched || film_plane(t) ? 1 : 4) > frame))
        return;
    t.hashed_submit = frame;
    // 1 MB and more (the glyph cache): page protection says whether the CPU wrote it, every frame, at no cost.
    if (t.stride && t.storage_size >= (1u << 20)) {
        // A watch armed over an image uploaded before (an earlier frame, or by another texture sharing it) cannot
        // tell what the CPU wrote since: its first take counts. Over one not uploaded yet it only arms the watch (the
        // upload follows). P5R's film luma, drawn two frames in a row, kept the first frame's upload while the CPU
        // had written the next picture in between.
        bool armed_late = false;
        if (!t.watched && !t.content_hash) {
            t.watched = plat::write_watch_add(t.memory, t.storage_size);
            armed_late = t.watched && t.image->uploaded;
        }
        if (t.watched) {
            uint64_t lo, hi;
            bool w = plat::write_watch_take(t.memory, &lo, &hi);
            film_trace(t, w ? "watch: written" : "watch: clean", lo, hi);
            if (w && (t.content_hash || armed_late)) {
                t.mark_dirty(lo, hi);
                static uint64_t n = 0;
                if (++n == 1 || n % 100 == 0) log("nvn: watched texture written by the CPU (%llu times)", (unsigned long long)n);
            }
            t.content_hash = 1;  // armed: every later take counts
            return;
        }
    }
    uint64_t h = hash_memory(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size);
    film_trace(t, h != t.content_hash ? "hash: changed" : "hash: same", t.content_hash, h);
    // The first hash of a texture whose image was uploaded before (an earlier frame, or a texture sharing it) cannot
    // tell a change either (as armed_late above): it counts as one.
    if (t.content_hash ? h != t.content_hash : t.image->uploaded) {
        t.mark_dirty(0, UINT64_MAX);
        static int logged = 0;
        if (!t.stride && logged++ < 100)
            log("nvn: block-linear texture %llux%u f%x (%llu bytes) changed by the CPU", (unsigned long long)t.width,
                t.height, t.format, (unsigned long long)t.storage_size);
    }
    t.content_hash = h;
}

}  // namespace nx::nvn
