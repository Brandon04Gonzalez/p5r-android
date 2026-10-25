// Copies of guest buffers kept across submits (games/p5r/OPTIMIZACION.md, session 7). Every submit copied each vertex
// buffer, index buffer and uniform/storage block it binds into the upload ring again, though on P5R's 3D title 72% of
// the vertex bytes, 97% of the index bytes and 88% of the block bytes were the same as their last copy
// (NX_UPLOAD_STATS): ~25 MB a frame through memcpy into uncached memory, a fifth of the render thread on the tablet.
//
// Here a copy outlives its submit, in blocks of host-visible memory of their own, keyed by the guest address with the
// size and a hash of the bytes: a later submit that binds the same bytes (the hash, which reads them once, is several
// times cheaper than the copy) binds that copy. Changed bytes get a new copy; the old one is freed once every submit
// that read it has retired, and copies no submit asked for in a while go the same way. Within a submit, nvn_dedup.cpp
// still answers repeats first.
// Only bytes seen unchanged by kStableSeen submits in a row get a copy here: the first version cached everything, and
// the blocks rewritten every frame (~900 a frame on the title) churned a first-fit allocator over thousands of free
// ranges: 10.6 fps against 12.6 without the cache. Copies come from power-of-two size classes now (O(1)). NX_BUFFER_CACHE=1: on (off by default until measured).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn {
uint64_t hash_memory(const uint8_t* p, uint64_t size);  // nvn_sampler.cpp
uint64_t frame_recording_serial();                     // nvn_frames.cpp
}  // namespace nx::nvn

namespace nx::nvn::replay {

using namespace nx::vk;

namespace {

constexpr uint64_t kBlock = 16ull << 20;  // bytes per memory block
constexpr uint64_t kAlign = 256;          // every copy: the largest buffer offset alignment Vulkan allows
constexpr uint64_t kMaxCopy = 4ull << 20; // larger ranges stay in the ring (one copy per submit)
constexpr uint64_t kStaleSubmits = 600;   // a copy no submit bound for this long is freed (~200 frames)
constexpr uint32_t kStableSeen = 2;       // the same bytes this many times in a row before they get a copy here
constexpr uint64_t kSeenGap = 6;          // "in a row": at most this many submits apart (P5R: 3 submits a frame)
constexpr uint32_t kClasses = 15;         // copy sizes 256 B << class, up to 4 MB

struct Block {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    uint64_t used = 0;  // carved so far (bump), in size-class slots
};
std::vector<Block> g_blocks;
struct Slot {
    uint32_t block;
    uint64_t offset;
};
std::vector<Slot> g_free[kClasses];  // free slots per size class

// Bytes not cached yet: how many submits in a row they were the same (address -> size, hash, count).
struct Seen {
    uint64_t size = 0, hash = 0;
    uint32_t count = 0;
    uint64_t submit = 0;  // g_submits when last seen
};
std::unordered_map<const uint8_t*, Seen> g_seen;

struct Entry {
    uint64_t size = 0, padded = 0, hash = 0;
    uint32_t block = 0;
    uint64_t offset = 0;
    uint32_t size_class = 0;  // the allocation: one slot of that class
    uint64_t serial = 0;             // the last submit that bound it (frees wait for it to retire)
    uint64_t seen = 0;               // g_submits then (copies unbound for kStaleSubmits are freed)
};
std::unordered_map<const uint8_t*, Entry> g_entries;

struct Retired {
    uint64_t serial;
    Slot slot;
    uint32_t size_class;
};
std::vector<Retired> g_retired;
uint64_t g_used = 0;     // bytes allocated
uint64_t g_submits = 0;  // cache_begin_submit calls
uint64_t g_budget = 0;

struct {
    uint64_t hits = 0, hit_bytes = 0, misses = 0, miss_bytes = 0, hash_ns = 0, full = 0, evicted = 0, wrong = 0;
    uint64_t unstable = 0;  // asked for, not stable yet: copied into the ring
} g_stats_cache;

bool on() {
    static const bool v = [] {
        const char* e = std::getenv("NX_BUFFER_CACHE");  // off until measured on the tablet
        return e && e[0] == '1';
    }();
    return v;
}

uint64_t budget() {
    if (!g_budget) {
        long mb = std::getenv("NX_BUFFER_CACHE_MB") ? std::atol(std::getenv("NX_BUFFER_CACHE_MB")) : 128;
        g_budget = uint64_t(std::clamp(mb, 16l, 1024l)) << 20;
    }
    return g_budget;
}

bool add_block() {
    if ((g_blocks.size() + 1) * kBlock > budget()) return false;
    VkDevice d = vk_device();
    Block b;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kBlock;
    bi.usage = kUploadUsage;
    if (p_vkCreateBuffer(d, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(d, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = upload_memory_type(req.memoryTypeBits);  // as the upload ring
    void* p = nullptr;
    if (p_vkAllocateMemory(d, &ai, nullptr, &b.memory) != VK_SUCCESS || p_vkBindBufferMemory(d, b.buffer, b.memory, 0) ||
        p_vkMapMemory(d, b.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        p_vkDestroyBuffer(d, b.buffer, nullptr);
        if (b.memory) p_vkFreeMemory(d, b.memory, nullptr);
        log("nvn: buffer cache: no memory for another %llu MB block", (unsigned long long)(kBlock >> 20));
        g_budget = g_blocks.size() * kBlock;  // no more tries
        return false;
    }
    b.ptr = static_cast<uint8_t*>(p);
    g_blocks.push_back(std::move(b));
    return true;
}

uint32_t size_class(uint64_t bytes) {
    uint32_t c = 0;
    while ((kAlign << c) < bytes) c++;
    return c;
}

uint64_t class_bytes(uint32_t c) { return kAlign << c; }

void retire(const Entry& e) {
    g_retired.push_back({e.serial, {e.block, e.offset}, e.size_class});
}

// Copies no submit bound lately, retired (their memory comes back once those submits are done).
void evict_stale() {
    for (auto it = g_entries.begin(); it != g_entries.end();)
        if (it->second.seen + kStaleSubmits < g_submits) {
            retire(it->second);
            g_stats_cache.evicted++;
            it = g_entries.erase(it);
        } else {
            ++it;
        }
}

// A slot of class c: a freed one, else carved from the last block (a new block when it is full).
bool alloc(uint32_t c, Slot& out) {
    if (!g_free[c].empty()) {
        out = g_free[c].back();
        g_free[c].pop_back();
        return true;
    }
    const uint64_t bytes = class_bytes(c);
    if (g_blocks.empty() || g_blocks.back().used + bytes > kBlock)
        if (!add_block()) return false;
    Block& b = g_blocks.back();
    out = {uint32_t(g_blocks.size() - 1), b.used};
    b.used += bytes;
    return true;
}

}  // namespace

void cache_begin_submit() {
    if (!on()) return;
    const uint64_t done = async_gpu() ? frame_completed() : UINT64_MAX;  // synchronous: every submit was waited for
    size_t kept = 0;
    for (const Retired& r : g_retired)
        if (r.serial <= done) {
            g_free[r.size_class].push_back(r.slot);
            g_used -= class_bytes(r.size_class);
        } else {
            g_retired[kept++] = r;
        }
    g_retired.resize(kept);
    if (++g_submits % 64 == 0) {
        evict_stale();
        for (auto it = g_seen.begin(); it != g_seen.end();)  // candidates not seen lately
            it = it->second.submit + kStaleSubmits < g_submits ? g_seen.erase(it) : std::next(it);
    }
}

bool cache_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out) {
    if (!on() || !p || !size || padded > kMaxCopy || align > kAlign) return false;
    const uint64_t t0 = plat::monotonic_ns();
    uint64_t h = hash_memory(p, size);  // whole 8-byte words: the last size % 8 bytes go in here
    for (uint64_t k = size & ~7ull; k < size; k++) h = (h ^ p[k]) * 0x100000001B3ull;
    g_stats_cache.hash_ns += plat::monotonic_ns() - t0;
    const uint64_t serial = frame_recording_serial();
    auto it = g_entries.find(p);
    if (it != g_entries.end()) {
        Entry& e = it->second;
        if (e.size == size && e.padded == padded && e.hash == h) {
            // NX_BUFFER_CACHE_VERIFY=1: every hit compared with the guest bytes (slow: the copy is uncached memory).
            static const bool verify = std::getenv("NX_BUFFER_CACHE_VERIFY") != nullptr;
            if (verify && std::memcmp(g_blocks[e.block].ptr + e.offset, p, size) != 0) {
                static int said = 0;
                if (said++ < 20) log("nvn: buffer cache: a hit differs from the guest bytes (%llu bytes at %p)",
                                     (unsigned long long)size, (const void*)p);
                g_stats_cache.wrong++;
            }
            e.serial = std::max(e.serial, serial), e.seen = g_submits;
            g_stats_cache.hits++, g_stats_cache.hit_bytes += padded;
            out = {g_blocks[e.block].buffer, e.offset, g_blocks[e.block].ptr + e.offset};
            return true;
        }
        retire(e);  // read by the submits up to e.serial: freed after them
        g_entries.erase(it);
    }
    // Not cached: a copy here only once the bytes stayed the same for kStableSeen submits in a row.
    Seen& seen = g_seen[p];
    if (seen.size == size && seen.hash == h && seen.submit + kSeenGap >= g_submits)
        seen.count++;
    else
        seen.count = 1;
    seen.size = size, seen.hash = h, seen.submit = g_submits;
    if (seen.count < kStableSeen) {
        g_stats_cache.unstable++;
        return false;  // the ring
    }
    const uint32_t c = size_class(padded);
    Slot slot;
    if (c >= kClasses || !alloc(c, slot)) {
        g_stats_cache.full++;
        return false;  // the ring then, as before
    }
    g_seen.erase(p);
    g_used += class_bytes(c);
    uint8_t* dst = g_blocks[slot.block].ptr + slot.offset;
    frame_guest_read(p, size);
    std::memcpy(dst, p, size);
    if (padded > size) std::memset(dst + size, 0, padded - size);
    g_entries[p] = {size, padded, h, slot.block, slot.offset, c, serial, g_submits};
    g_stats_cache.misses++, g_stats_cache.miss_bytes += padded;
    out = {g_blocks[slot.block].buffer, slot.offset, dst};
    return true;
}

void cache_log() {
    auto& s = g_stats_cache;
    if (s.hits + s.misses + s.full + s.unstable)
        log("nvn: buffer cache: %llu hits (%.1f MB not copied), %llu copies (%.1f MB), %llu not stable, %llu full, "
            "%llu evicted; hashing %.1f ms; %zu copies kept in %.1f of %zu MB%s",
            (unsigned long long)s.hits, double(s.hit_bytes) / 1e6, (unsigned long long)s.misses,
            double(s.miss_bytes) / 1e6, (unsigned long long)s.unstable, (unsigned long long)s.full,
            (unsigned long long)s.evicted,
            double(s.hash_ns) / 1e6, g_entries.size(), double(g_used) / 1e6, size_t(g_blocks.size() * kBlock >> 20),
            s.wrong ? "; HITS THAT DIFFER (NX_BUFFER_CACHE_VERIFY)" : "");
    s = {};
}

}  // namespace nx::nvn::replay
