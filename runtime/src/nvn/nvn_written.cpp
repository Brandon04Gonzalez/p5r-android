// Compute results within a submit (docs/nvn/GLITCH-PLAN.md G5). A storage buffer a compute shader writes is a copy in
// the upload buffer, written back to guest memory once the submit completed. Each binding used to get a copy of its
// own: later draws and dispatches of the submit read the guest bytes from before the dispatch, two dispatches writing
// one buffer overwrote each other at write-back, and indirect dispatch arguments were read before the GPU wrote them.
// Here the submit keeps one copy per written guest range (the ranges are disjoint, keyed by host address). Writers and
// readers of a range bind that copy; ranges that meet are merged into a new copy on the GPU (vkCmdCopyBuffer, so in
// command order); each range is written back once. NX_COMPUTE_SEPARATE=1: the previous behaviour.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn {
// Write-backs per log window (written_log): count, bytes compared, 8-byte words changed, host ns.
std::atomic<uint64_t> g_wb_count{0}, g_wb_bytes{0}, g_wb_changed{0}, g_wb_ns{0};
}  // namespace nx::nvn

namespace nx::nvn::replay {

using namespace nx::vk;

namespace {

constexpr uint64_t kAlign = 256;          // the largest buffer offset alignment Vulkan allows (all descriptor kinds)
constexpr uint64_t kTail = 0x10000 + 16;  // zeros after a range: storage bindings are padded to 64 KB (nvn_draw.cpp)

struct Region {
    uint64_t size = 0;  // guest bytes from the key
    uint64_t cap = 0;   // bytes of the copy from up.offset (size, then zeros)
    Upload up;          // the copy of the key's first byte
    std::shared_ptr<std::vector<uint8_t>> before;  // the guest bytes copied in (what the GPU did not change)
};
using Regions = std::map<const uint8_t*, Region>;
using It = Regions::iterator;
Regions g_regions;
bool g_read = false;  // a copy was bound for reading since the last write_after_read
uint64_t g_binds = 0;  // written_bind calls, ever (nvn_frames.cpp: did a dispatch write storage after the split)
uint64_t g_shared = 0, g_merged = 0, g_composed = 0, g_indirect = 0;  // for written_log

// The regions overlapping [p, p + size), in address order.
std::vector<It> overlapping(const uint8_t* p, uint64_t size) {
    std::vector<It> v;
    It it = g_regions.upper_bound(p);
    if (it != g_regions.begin() && std::prev(it)->first + std::prev(it)->second.size > p) v.push_back(std::prev(it));
    for (; it != g_regions.end() && it->first < p + size; ++it) v.push_back(it);
    return v;
}

// `bytes` of upload memory whose byte `anchor` is kAlign aligned (so it can start a binding).
Upload alloc_anchored(uint64_t bytes, uint64_t anchor) {
    Upload u = upload_alloc(bytes + kAlign, kAlign);
    uint64_t shift = (kAlign - anchor % kAlign) % kAlign;
    u.offset += shift, u.ptr += shift;
    return u;
}

// The parts of `regions` inside [p, p + size) copied on the GPU into `to` (whose first byte is p's), then made
// visible to everything after. What the dispatches recorded so far wrote is there when the copy runs: dispatch()
// ends each one with a barrier to all later commands. Outside render passes.
void copy_regions(VkCommandBuffer cb, const std::vector<It>& regions, const uint8_t* p, uint64_t size, const Upload& to) {
    for (It it : regions) {
        const uint8_t* lo = std::max(it->first, p);
        const uint8_t* hi = std::min(it->first + it->second.size, p + size);
        if (lo >= hi) continue;
        VkBufferCopy c{it->second.up.offset + uint64_t(lo - it->first), to.offset + uint64_t(lo - p), uint64_t(hi - lo)};
        p_vkCmdCopyBuffer(cb, it->second.up.buffer, to.buffer, 1, &c);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
}

// A dispatch about to write a copy that earlier commands of the submit read (draws binding it as vertices, uniforms,
// storage): they finish first. Earlier writes are already ordered by dispatch()'s barrier.
void write_after_read(VkCommandBuffer cb) {
    if (!g_read) return;
    g_read = false;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 0, nullptr);
}

// The device's buffer descriptor offset alignment (kAlign at most, by the Vulkan spec).
uint64_t bind_align() {
    static const uint64_t a = [] {
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        return std::max<uint64_t>({props.limits.minStorageBufferOffsetAlignment,
                                   props.limits.minUniformBufferOffsetAlignment, 16});
    }();
    return a;
}

Upload at(const It& it, uint64_t off) { return {it->second.up.buffer, it->second.up.offset + off, it->second.up.ptr + off}; }

// [p, p + size) as a sub-range of one region, bindable at `align` (at most the device's descriptor alignment; the
// callers ask 256 for descriptors) with `padded` bytes behind it: else end(). A copy is anchored at its first writer,
// so two pointers aligned in guest memory are aligned in the copy.
It containing(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, const std::vector<It>& regions) {
    if (regions.size() != 1) return g_regions.end();
    It it = regions[0];
    uint64_t off = uint64_t(p - it->first);
    if (p < it->first || off + size > it->second.size || off + padded > it->second.cap) return g_regions.end();
    if ((it->second.up.offset + off) % std::clamp<uint64_t>(align, 1, bind_align())) return g_regions.end();
    return it;
}

}  // namespace

bool written_separate() {
    static const bool on = std::getenv("NX_COMPUTE_SEPARATE") != nullptr;
    return on;
}

uint64_t written_bind_count() { return g_binds; }
void written_count_bind() { g_binds++; }

Upload written_bind(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded) {
    g_binds++;
    std::vector<It> old = overlapping(p, size);
    if (It it = containing(p, size, padded, bind_align(), old); it != g_regions.end()) {
        write_after_read(cb);
        g_shared++;
        return at(it, uint64_t(p - it->first));
    }
    // A new range, or one meeting others (or misaligned in its copy): one copy covering all of them, anchored at p.
    const uint8_t* lo = p;
    const uint8_t* hi = p + size;
    for (It it : old) lo = std::min(lo, it->first), hi = std::max(hi, it->first + it->second.size);
    uint64_t n = uint64_t(hi - lo), off = uint64_t(p - lo);
    Region r;
    r.size = n;
    r.cap = std::max(n + kTail, off + padded);
    r.up = alloc_anchored(r.cap, off);
    frame_guest_read(lo, n);  // deferred write-backs over these bytes land first (nvn_frames.cpp)
    std::memcpy(r.up.ptr, lo, n);
    std::memset(r.up.ptr + n, 0, r.cap - n);
    r.before = std::make_shared<std::vector<uint8_t>>(r.up.ptr, r.up.ptr + n);
    for (It it : old)  // their parts: as they were before their own GPU writes
        if (it->second.before)
            std::memcpy(r.before->data() + (it->first - lo), it->second.before->data(), it->second.before->size());
    if (!old.empty()) {
        copy_regions(cb, old, lo, n, r.up);  // dispatch(): no render pass is open
        for (It it : old) g_regions.erase(it);
        g_merged++;
    }
    dedup_invalidate(lo, n);  // copies of these bytes made earlier in the submit are not what later work reads
    It it = g_regions.emplace(lo, r).first;
    return at(it, off);
}

bool written_read(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out) {
    if (!p || !size || g_regions.empty() || written_separate()) return false;
    std::vector<It> regions = overlapping(p, size);
    if (regions.empty()) return false;
    if (It it = containing(p, size, padded, align, regions); it != g_regions.end()) {
        g_shared++, g_read = true;
        out = at(it, uint64_t(p - it->first));
        return true;
    }
    // Partly written, or not bindable in place: a copy of its own, the written parts copied in on the GPU. This is
    // what the range holds at this point of the submit (later dispatches do not change it).
    draw_end_rendering(cb, 2);
    out = upload_alloc(std::max(padded, size), std::max<uint64_t>(align, 16));
    frame_guest_read(p, size);
    std::memcpy(out.ptr, p, size);
    if (padded > size) std::memset(out.ptr + size, 0, padded - size);
    copy_regions(cb, regions, p, size, out);
    g_composed++;
    return true;
}

Upload read_upload(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    Upload u;
    if (written_read(cb, p, size, padded, align, u)) return u;
    return dedup_upload(p, size, padded, align);
}

bool written_overlaps(const uint8_t* p, uint64_t size) {
    return p && size && !g_regions.empty() && !overlapping(p, size).empty();
}

bool written_indirect(VkCommandBuffer cb, const uint8_t* p) {
    Upload u;
    if (!written_read(cb, p, 12, 16, 4, u)) return false;
    p_vkCmdDispatchIndirect(cb, u.buffer, u.offset);  // {x, y, z} as the earlier dispatches left them
    g_indirect++;
    return true;
}

bool draw_inputs(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in) {
    // Vertex buffers: the bound ranges of the streams the vertex shader reads (the pipeline declares no others).
    for (const ShaderIo& io : vs->refl.inputs)
        if (io.location < g_state.attribs.size() && g_state.attribs[io.location].stream < 15)
            in.used |= 1u << g_state.attribs[io.location].stream;
    for (uint32_t s = 0; s < 15; s++) {
        if (!(in.used >> s & 1)) continue;
        const Range& r = g_state.vertex_buffers[s];
        const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
        uint64_t size = p ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        g_upload_kind = kUploadVertex;
        in.vertex[s] = read_upload(cb, p, size, std::max<uint64_t>((size + 15) & ~15ull, 16), 16);
        g_upload_kind = kUploadOther;
    }
    if (!indexed) return true;
    uint32_t count = uint32_t(c.i[0]);
    const uint8_t* p = guest_bytes(c.a[0]);
    if (!p) return false;
    in.index_type = vk_index_type(c.mask);
    if (in.index_type == VK_INDEX_TYPE_UINT8_EXT) {
        // 8-bit indices: Metal has none, and MoltenVK converts them on the GPU at every draw (that was most of the
        // frame's GPU time), so they are widened here, from guest memory (a dispatch writing them is only logged).
        static int logs = 0;
        if (written_overlaps(p, count) && logs++ < 16) log("nvn: 8-bit indices written by a dispatch: stale");
        in.index = upload_alloc(uint64_t(count) * 2, 16);
        auto* d = reinterpret_cast<uint16_t*>(in.index.ptr);
        for (uint32_t k = 0; k < count; k++) d[k] = p[k];
        in.index_type = VK_INDEX_TYPE_UINT16;
        return true;
    }
    uint64_t bytes = uint64_t(count) * index_size(c.mask);
    if (!written_read(cb, p, bytes, bytes, index_size(c.mask), in.index)) in.index = upload(p, bytes, 16);
    return true;
}

void check_vertex_ranges(const ShaderInfo* vs, const ShaderInfo* fs, bool indexed, const Cmd& c, const DrawInputs& in) {
    static const bool on = std::getenv("NX_CHECK_VERTS") != nullptr;
    static uint64_t found = 0;
    if (!on || !vs) return;
    // The vertices and instances the draw fetches.
    uint64_t lo = 0, hi = 0;  // vertex indices [lo, hi]
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]);
    if (!count) return;
    if (indexed) {
        const uint8_t* p = guest_bytes(c.a[0]);
        if (!p) return;
        uint32_t isz = index_size(c.mask), restart = isz == 1 ? 0xFF : isz == 2 ? 0xFFFF : 0xFFFFFFFFu;
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v = isz == 1 ? p[k] : isz == 2 ? reinterpret_cast<const uint16_t*>(p)[k] : reinterpret_cast<const uint32_t*>(p)[k];
            if (v == restart) continue;
            mn = std::min(mn, v), mx = std::max(mx, v);
        }
        if (mn == UINT32_MAX) return;
        lo = uint64_t(int64_t(mn) + c.i[1]), hi = uint64_t(int64_t(mx) + c.i[1]);
    } else {
        lo = uint32_t(c.i[0]), hi = uint64_t(uint32_t(c.i[0])) + count - 1;
    }
    uint64_t inst_hi = uint64_t(uint32_t(c.i[2])) + uint32_t(std::max(1, c.i[3])) - 1;
    for (const ShaderIo& io : vs->refl.inputs) {
        if (io.location >= g_state.attribs.size()) continue;
        const VertexAttrib& va = g_state.attribs[io.location];
        if (va.stream >= 15 || va.stream >= g_state.streams.size() || !(in.used >> va.stream & 1)) continue;
        const VertexStream& st = g_state.streams[va.stream];
        const Range& r = g_state.vertex_buffers[va.stream];
        uint64_t last = st.divisor ? inst_hi / st.divisor : hi;
        uint64_t end = last * st.stride + va.offset + vk_block_bytes(vk_vertex_format(va.format));
        uint64_t copied = r.address && guest_bytes(r.address) ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        if (end <= copied) continue;
        found++;
        static std::map<std::string, uint64_t> seen;
        std::string key = vs->hash + (fs ? fs->hash : std::string("-")) + char('a' + va.stream);
        uint64_t n = ++seen[key];
        if (n <= 3 || (n & (n - 1)) == 0)
            log("nvn: vertex fetch past the bound range (%llu so far; #%llu of vs %s fs %s): stream %u loc %u fmt %#x off %u "
                "stride %u div %u, vertices %llu..%llu instances ..%llu, needs %llu bytes, bound %llu at %llx (copied %llu)",
                (unsigned long long)found, (unsigned long long)n, vs->hash.c_str(), fs ? fs->hash.c_str() : "-", va.stream,
                io.location, va.format, va.offset, st.stride, st.divisor, (unsigned long long)lo, (unsigned long long)hi,
                (unsigned long long)inst_hi, (unsigned long long)end, (unsigned long long)r.size,
                (unsigned long long)r.address, (unsigned long long)copied);
        break;
    }
}

void written_cpu_write(const uint8_t* p, uint64_t size) {
    // UpdateUniformBuffer / ReportCounter into a range a dispatch of this submit writes: the copy misses the bytes
    // (they reach guest memory at replay time) and the write-back replaces them. Not handled: logged.
    static int logs = 0;
    if (written_overlaps(p, size) && logs++ < 16)
        log("nvn: guest write into a compute-written range (%llu bytes): lost at write-back", (unsigned long long)size);
}

std::vector<WriteBack> written_take() {
    std::vector<WriteBack> v;
    v.reserve(g_regions.size());
    for (auto& [p, r] : g_regions) v.push_back({r.up.ptr, const_cast<uint8_t*>(p), r.size, r.before});
    g_regions.clear();
    return v;
}

void written_reset() { g_regions.clear(), g_read = false; }

void written_log() {
    if (g_shared + g_merged + g_composed + g_indirect)
        log("nvn: compute ranges: %llu bindings of written copies, %llu merged, %llu reads composed on the GPU, %llu indirect",
            (unsigned long long)g_shared, (unsigned long long)g_merged, (unsigned long long)g_composed,
            (unsigned long long)g_indirect);
    g_shared = g_merged = g_composed = g_indirect = 0;
}

}  // namespace nx::nvn::replay

namespace nx::nvn {

// A bound storage range is often larger than what the shader writes, and the game's recording thread writes the
// rest of those buffers while the submit is replayed. Copying the whole range back put stale bytes over them: meshes
// exploded into beams and a band of stretched triangles at the horizon, for the rest of the run (DEBUG.md #56). Only
// the words the GPU changed go back. NX_WRITE_BACK_WHOLE=1: the old whole-range copy.
// Per frame over the timing window (log_timing, nvn_vk.cpp).
void write_back_log(uint64_t frames) {
    const uint64_t n = g_wb_count.exchange(0), bytes = g_wb_bytes.exchange(0), changed = g_wb_changed.exchange(0),
                   ns = g_wb_ns.exchange(0);
    if (n && frames)
        log("nvn: write-backs per frame: %.0f, %.2f MB compared, %.0f%% of the words changed, %.2f ms", double(n) / double(frames),
            double(bytes) / 1e6 / double(frames), bytes ? 800.0 * double(changed) / double(bytes) : 0.0,
            double(ns) / 1e6 / double(frames));
}

void apply_write_back(const WriteBack& w) {
    static const bool whole = std::getenv("NX_WRITE_BACK_WHOLE") != nullptr;
    const uint64_t t0 = plat::monotonic_ns();
    g_wb_count++, g_wb_bytes += w.size;
    if (whole || !w.before || w.before->size() < w.size) {
        std::memcpy(w.to, w.from, w.size);
        g_wb_ns += plat::monotonic_ns() - t0;
        return;
    }
    const uint8_t* was = w.before->data();
    // The GPU's bytes copied out of the upload ring in one memcpy first: compared word by word in place, they were
    // read at ~34 MB/s on the Adreno 610 (55 ms a frame on the 3D title, 7.7 fps; with the copy 6 ms, 13.9 fps;
    // a host-cached ring alone changed nothing). NX_WB_BULK=0: in place, as before.
    static const bool bulk = !(std::getenv("NX_WB_BULK") && std::getenv("NX_WB_BULK")[0] == '0');
    static thread_local std::vector<uint8_t> copy;
    const uint8_t* from = w.from;
    if (bulk) {
        if (copy.size() < w.size) copy.resize(w.size);
        std::memcpy(copy.data(), w.from, w.size);
        from = copy.data();
    }
    uint64_t k = 0, changed = 0;
    for (; k + 8 <= w.size; k += 8) {
        if (std::memcmp(from + k, was + k, 8) == 0) continue;
        changed++;
        for (uint64_t b = k; b < k + 8; b += 4)
            if (std::memcmp(from + b, was + b, 4) != 0) std::memcpy(w.to + b, from + b, 4);
    }
    for (; k < w.size; k++)
        if (from[k] != was[k]) w.to[k] = from[k];
    g_wb_changed += changed;
    g_wb_ns += plat::monotonic_ns() - t0;
}

}  // namespace nx::nvn
