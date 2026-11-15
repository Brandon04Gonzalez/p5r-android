// Per-submit upload reuse (docs/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (ranges a
// dispatch writes are read from their written copy, nvn_written.cpp, and invalidated here): a copy made once can
// serve every later draw of the submit that binds the same bytes.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "nvn_draw_state.h"

namespace nx::nvn {
uint64_t hash_memory(const uint8_t* p, uint64_t size);  // nvn_sampler.cpp
}

namespace nx::nvn::replay {

namespace {

struct Entry {
    uint64_t size = 0, padded = 0;
    Upload upload;
};

struct DedupSpan {
    const uint8_t* p = nullptr;
    uint64_t size = 0;
    uint32_t front_idx = 0;
};
// Recorded spans in the current submit for linear invalidation, keeping reserved capacity across submits.
std::vector<DedupSpan> g_copies;
uint64_t g_saved = 0, g_copied = 0;

// Open-addressed front hash table (linear probing) per address hash. Slots of the current generation hold active
// entries or tombstones (p == nullptr) from dedup_invalidate; dedup_reset advances the generation in O(1).
struct Front {
    const uint8_t* p = nullptr;
    uint64_t generation = 0;
    Entry entry;
};
constexpr size_t kFrontSlots = 65536;
constexpr uint32_t kMaxProbe = 8;
Front g_front[kFrontSlots];
uint64_t g_generation = 1;
static uint64_t s_dedup_inval = 1;
inline uint32_t front_index_of(const uint8_t* p) {
    const uintptr_t v = uintptr_t(p);
    return uint32_t(((v >> 4) ^ (v >> 12) ^ (v >> 21) ^ (v >> 30)) & (kFrontSlots - 1));
}

// NX_UPLOAD_STATS: bytes copied per kind, and of those the bytes equal to the last copy of the same address and size.
struct KindStats {
    uint64_t copies = 0, bytes = 0, same = 0;
};
KindStats g_kind[kUploadKinds];
std::unordered_map<const uint8_t*, std::pair<uint64_t, uint64_t>> g_last_hash;  // address -> (size, hash)

bool upload_stats_on() {
    static const bool on = std::getenv("NX_UPLOAD_STATS") != nullptr;
    return on;
}

void count_copy(const uint8_t* p, uint64_t size) {
    KindStats& k = g_kind[g_upload_kind < kUploadKinds ? g_upload_kind : kUploadOther];
    k.copies++, k.bytes += size;
    const uint64_t h = hash_memory(p, size);
    auto& last = g_last_hash[p];
    if (last.first == size && last.second == h) k.same += size;
    last = {size, h};
}

}  // namespace

UploadKind g_upload_kind = kUploadOther;

namespace {
static const uint8_t* s_last_dedup_p = nullptr;
static uint64_t s_last_dedup_gen = 0;
static Entry s_last_dedup_entry{};
}  // namespace

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    if (!p || !size) {
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    if (!off && p == s_last_dedup_p && g_generation == s_last_dedup_gen) {
        if (s_last_dedup_entry.size == size && s_last_dedup_entry.padded == padded && s_last_dedup_entry.upload.offset % align == 0) {
            g_saved += padded;
            return s_last_dedup_entry.upload;
        }
    }
    const uint32_t base_idx = front_index_of(p);
    uint32_t target_idx = kFrontSlots;
    bool existing = false;
    uint64_t prev_size = 0;
    if (!off) {
        for (uint32_t step = 0; step < kMaxProbe; step++) {
            const uint32_t idx = (base_idx + step) & (kFrontSlots - 1);
            const Front& f = g_front[idx];
            if (f.generation != g_generation) {
                if (target_idx == kFrontSlots) target_idx = idx;
                break;
            }
            if (f.p == nullptr) {
                if (target_idx == kFrontSlots) target_idx = idx;
                continue;
            }
            if (f.p == p) {
                target_idx = idx;
                existing = true;
                prev_size = f.entry.size;
                if (f.entry.size == size && f.entry.padded == padded && f.entry.upload.offset % align == 0) {
                    g_saved += padded;
                    s_last_dedup_p = p;
                    s_last_dedup_gen = g_generation;
                    s_last_dedup_entry = f.entry;
                    return f.entry.upload;
                }
                break;
            }
        }
    }
    if (target_idx == kFrontSlots) target_idx = base_idx;
    frame_guest_read(p, size);  // deferred write-backs over these bytes land first (nvn_frames.cpp)
    auto record_entry = [&](const Upload& u) {
        Entry ent{size, padded, u};
        g_front[target_idx] = {p, g_generation, ent};
        s_last_dedup_p = p;
        s_last_dedup_gen = g_generation;
        s_last_dedup_entry = ent;
        if (!existing || size > prev_size) {
            if (g_copies.capacity() < 1024) g_copies.reserve(1024);
            g_copies.push_back({p, size, target_idx});
        }
    };
    Upload u;
    if (g_upload_kind != kUploadOther && cache_upload(p, size, padded, align, u)) {  // nvn_buffer_cache.cpp
        record_entry(u);
        return u;
    }
    u = upload_alloc(padded, align);
    plat::fast_memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    if (upload_stats_on()) count_copy(p, size);
    record_entry(u);
    g_copied += padded;
    return u;
}

uint64_t dedup_generation() { return g_generation; }
uint64_t dedup_inval_epoch() { return s_dedup_inval; }

void dedup_reset() {
    g_copies.clear(), g_generation++;
    s_last_dedup_p = nullptr;
}

void dedup_invalidate(const uint8_t* p, uint64_t size) {
    s_dedup_inval++;
    stream_cache_invalidate(p, size);
    index_cache_invalidate(p, size);
    sets_invalidate_range(p, size);
    s_last_dedup_p = nullptr;
    if (!p || !size || g_copies.empty()) return;
    const uint8_t* const end = p + size;
    size_t kept = 0;
    for (size_t i = 0; i < g_copies.size(); i++) {
        const DedupSpan& span = g_copies[i];
        if (span.p < end && span.p + span.size > p) {
            Front& f = g_front[span.front_idx];
            if (f.generation == g_generation && f.p == span.p) f.p = nullptr;
        } else {
            g_copies[kept++] = span;
        }
    }
    g_copies.resize(kept);
}

// c[0], the NVN driver's constants, as far as shaders read them (docs/nvn/TRANSLATOR.md): buffer descriptors
// {u64 GPU address; u32 size} of what is bound, which compute shaders search to find the buffer an address falls in
// (they wrote garbage with a zero table), and the maximum point size.
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st) {
    std::array<uint8_t, 0x1000> c0{};
    auto put = [&](uint32_t at, const Range& r) {
        if (at + 12 > c0.size()) return;
        std::memcpy(c0.data() + at, &r.address, 8);
        uint32_t size = uint32_t(std::min<uint64_t>(r.size, 0xFFFFFFFFu));
        std::memcpy(c0.data() + at + 8, &size, 4);
    };
    if (stage == ShaderStage::Compute) {
        for (int n = 0; n < 16; n++) put(0x210 + 0x10 * n, g_state.uniforms[st][n]);
        for (int n = 0; n < 16; n++) put(0x310 + 0x10 * n, g_state.storage[st][n]);
    } else {
        int hw = stage == ShaderStage::Fragment ? 4 : 0;  // Maxwell stage numbers: vertex 0, fragment 4
        for (int n = 0; n < 16; n++) put(0x110 + 0x100 * hw + 0x10 * n, g_state.storage[st][n]);
        float max_point = 64.0f;
        std::memcpy(c0.data() + 0x610, &max_point, 4);
    }
    return c0;
}

void dedup_log() {
    if (g_copied + g_saved)
        log("nvn: uploads copied %.1f MB, reused %.1f MB", double(g_copied) / 1e6, double(g_saved) / 1e6);
    g_copied = g_saved = 0;
    static const char* names[kUploadKinds] = {"other", "vertex", "index", "uniform/storage"};
    for (int k = 0; k < kUploadKinds; k++) {
        KindStats& s = g_kind[k];
        if (s.copies)
            log("nvn: uploads of %s: %llu copies, %.1f MB, %.0f%% the same as their last copy", names[k],
                (unsigned long long)s.copies, double(s.bytes) / 1e6, 100.0 * double(s.same) / double(s.bytes));
        s = {};
    }
}

}  // namespace nx::nvn::replay
