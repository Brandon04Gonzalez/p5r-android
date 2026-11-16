// The descriptor sets of a draw or dispatch (split from nvn_draw.cpp): the textures its bindings name, resolved once
// per draw (gather_textures), the sets built from the bound state (build_sets) and bound (bind_sets), and the draw's
// shared inputs. The replay's CPU shortcuts of docs/nvn/PERF-PLAN.md live here and in nvn_draw.cpp; NX_DRAW_SLOW
// turns them off (draw_slow, nvn_draw_state.h). Everything here runs under the GPU lock, one draw at a time.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#if defined(__aarch64__)
#include <arm_acle.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#endif
#define HAS_ARM_CRC32 1
#endif

#include "nvn_draw_state.h"
#include "nvn_ultrawide.h"
#include "nvn_vk.h"

namespace nx::nvn::replay {

using namespace nx::vk;

DrawTextures g_textures;

bool draw_slow(uint32_t what) {
    static const uint32_t mask = [] {
        const char* e = std::getenv("NX_DRAW_SLOW");
        if (!e) return 0u;
        uint32_t m = 0;
        for (; *e; e++) {  // only letters of "mehsu": those; anything else (1): all
            const char* letters = "mehsu";
            const char* at = std::strchr(letters, *e);
            if (!at) return ~0u;
            m |= 1u << (at - letters);
        }
        return m ? m : ~0u;
    }();
    return (mask & what) != 0;
}

uint32_t handle_texture(uint64_t handle, uint32_t* sampler) {
    if (sampler) *sampler = uint32_t(handle >> 20) & 0xFFF;
    return uint32_t(handle & 0xFFFFF);
}

namespace {

int api_stage(ShaderStage s) { return s == ShaderStage::Fragment ? 1 : s == ShaderStage::Compute ? 5 : 0; }

// The texture and sampler a handle names in the bound pools, looked up (texture_for without its cache).
// `has_sampler`: the sampler pool had the slot (`sampler` is left alone otherwise).
void lookup_handle(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view,
                   bool* has_sampler = nullptr) {
    uint32_t sid = 0;
    uint32_t tid = handle_texture(handle, &sid);
    texture = 0;
    if (view) *view = {};
    // The pools' maps are read under the objects lock, and at the replay's mark (nvn_incarnation.cpp): the game's
    // recording thread registers the next frame's slots meanwhile (98% of the registrations, DEBUG.md #56).
    auto* tp = get<DescriptorPool>(g_state.texture_pool);
    auto* sp = get<DescriptorPool>(g_state.sampler_pool);
    PoolEntry e;
    {
        std::shared_lock lk(objects_lock());
        // A slot empty at the mark was registered after the lists were recorded but before the submit: the current
        // entry (NVN reads descriptors when the GPU runs; DEBUG.md #64). NX_MARK_EMPTY_OLD=1: the empty entry.
        if (tp && (!pool_entry_at(g_state.texture_pool, tid, t_replay_mark, e) || (!e.has && !mark_empty_old()))) {
            e = {};
            if (auto it = tp->textures.find(tid); it != tp->textures.end()) e.has = true, e.texture = it->second;
            if (auto it = tp->snapshots.find(tid); it != tp->snapshots.end()) e.snapshot = it->second;
            if (auto it = tp->views.find(tid); it != tp->views.end()) e.has_view = true, e.view = it->second;
        }
        SamplerSlot ss;
        // A slot empty at the mark was registered after the lists were recorded but before the submit (NVN reads
        // descriptors when the GPU runs): the current entry is the one.
        if (sp && sampler_entry_at(g_state.sampler_pool, sid, t_replay_mark, ss) && ss.has) {
            sampler = ss.info;
            if (has_sampler) *has_sampler = true;
        } else if (sp) {
            if (auto it = sp->samplers.find(sid); it != sp->samplers.end()) {
                sampler = it->second;
                if (has_sampler) *has_sampler = true;
            }
        }
    }
    if (tp) {
        texture = e.texture;
        // The slot's texture gone or re-initialized as another kind: the copy made at registration (nvn_objects.cpp).
        // NX_SNAPSHOT_SHAPE=1 also uses the copy when only the memory or size changed (the pool's descriptor copy):
        // it broke the course's foreground flowers (dynamic-resolution targets re-initialized every frame are meant
        // to be followed) and did not light the Goombas (docs/nvn/DEBUG.md #45).
        static const bool kind_only = std::getenv("NX_SNAPSHOT_SHAPE") == nullptr;
        // Re-initialized over other memory: the struct now holds another texture, while the pool's descriptor still
        // names the registered memory. Wonder registers its 64x36 aerial-haze texture in a new slot every frame and
        // reuses the struct for a dynamic-resolution target of the same format before the replay: 6.5% of course
        // frames sampled that target instead, and the background lost its haze for a frame (DEBUG-2 #92). A target
        // re-initialized in place (same memory, new size) is still followed (#45). NX_SNAPSHOT_MEMORY=0: as before.
        static const bool memory_rule = [] {
            const char* e = std::getenv("NX_SNAPSHOT_MEMORY");
            return !(e && e[0] == '0');
        }();
        if (texture && e.snapshot) {
            Texture* now = get<Texture>(texture);
            Texture* then = get<Texture>(e.snapshot);
            bool moved = now && then && !kind_only &&
                         (now->memory != then->memory || now->width != then->width || now->height != then->height ||
                          now->depth != then->depth || now->levels != then->levels);
            moved = moved || (now && then && memory_rule && now->memory != then->memory);
            if (then && (!now || moved || now->target != then->target || now->format != then->format)) texture = e.snapshot;
        }
        if (view && e.has_view) *view = e.view;
    }
}

}  // namespace

// NX_FRAME_SIG's watched draws: how a handle's texture slot resolves (the mark's entry against the current one).
std::string explain_texture_slot(uint64_t handle) {
    uint32_t sid = 0, tid = handle_texture(handle, &sid);
    auto* tp = get<DescriptorPool>(g_state.texture_pool);
    if (!tp) return "no pool";
    std::shared_lock lk(objects_lock());
    PoolEntry e;
    bool at_mark = pool_entry_at(g_state.texture_pool, tid, t_replay_mark, e);
    auto cur = tp->textures.find(tid);
    auto cs = tp->snapshots.find(tid);
    auto desc = [](uint64_t key) {
        auto o = objects().find(key);
        auto* t = o == objects().end() ? nullptr : dynamic_cast<Texture*>(o->second.get());
        char b[96];
        std::snprintf(b, sizeof b, "%llx(%ux%u f%x m%llx)", (unsigned long long)(key & 0xFFFFF), t ? t->width : 0,
                      t ? t->height : 0, t ? t->format : 0, (unsigned long long)(t ? t->memory : 0));
        return std::string(b);
    };
    char b[160];
    std::snprintf(b, sizeof b, "{id %x mark %llu now %llu: ", tid, (unsigned long long)t_replay_mark,
                  (unsigned long long)current_epoch());
    std::string s = b;
    if (at_mark) s += "at mark " + (e.has ? desc(e.texture) : std::string("empty")) + " snap " + desc(e.snapshot) + "; ";
    s += "current " + (cur != tp->textures.end() ? desc(cur->second) : std::string("none")) + " snap " +
         (cs != tp->snapshots.end() ? desc(cs->second) : std::string("none")) + "}";
    return s;
}

namespace {

// What a handle resolves to, once per submit (docs/nvn/PERF-PLAN.md step 4). The replay's mark (nvn_incarnation.cpp,
// DEBUG.md #55-#57) makes (texture pool, sampler pool, handle) -> (texture, its object, view) constant within a
// submit: every lookup answers as of the mark, and what the mark resolves to is freed only by drain_objects, between
// replays. Sampler slots have no such history; one registered again during a replay keeps its first value here. The
// image's state is not cached (sampled_ready and sampled_view run for every draw). Open addressing, no removal;
// emptied by bumping the generation.
struct HandleSlot {
    uint64_t generation = 0;  // g_handle_generation when filled
    uint64_t handle = 0, texture_pool = 0, sampler_pool = 0;  // the key (handle: its 32 bits)
    uint64_t texture = 0;       // as lookup_handle gives it
    Texture* object = nullptr;  // get<Texture>(texture)
    bool has_sampler = false;
    SamplerInfo sampler;
    ViewInfo view;
    VkSampler vk_sampler[4] = {};  // sampler_for(sampler, shadow, biased) by shadow + 2 * biased, once a binding asked
    uint64_t last_sampled_ns = 0;
    VkImage vk_image = VK_NULL_HANDLE;
    VkImageView vk_view = VK_NULL_HANDLE;
    uint64_t vk_view_serial = 0;
    ImageShape vk_img_binding{};
};
constexpr uint32_t kHandleSlots = 8192;  // keeps handle slots across submits when epoch/mark are unchanged
HandleSlot g_handle_slots[kHandleSlots];
uint64_t g_handle_generation = 1, g_handle_mark = 0, g_handle_epoch = 0;
uint32_t g_handle_used = 0;
uint64_t g_handle_hits = 0, g_handle_misses = 0, g_sets_skipped = 0;

void handles_reset() { g_handle_generation++, g_handle_used = 0; }

// The slot of `handle` in the bound pools, filled on first use; null without a replay mark (NX_TEXTURE_RACE_OLD: a
// lookup then answers as of now) or with NX_DRAW_SLOW. Valid until the next call.
HandleSlot* handle_slot(uint64_t handle) {
    uint64_t mark = t_replay_mark;
    if (!mark || draw_slow(kSlowHandles)) return nullptr;
    if (mark != g_handle_mark || g_handle_used > kHandleSlots / 2) {
        handles_reset();
        g_handle_mark = mark;
        g_handle_epoch = current_epoch();
    }
    handle &= 0xFFFFFFFFull;  // 20 bits of texture id, 12 of sampler id
    uint64_t tp = g_state.texture_pool, sp = g_state.sampler_pool;
    uint64_t h = (handle + 1) * 0x9E3779B97F4A7C15ull ^ tp * 0xC2B2AE3D27D4EB4Full ^ sp * 0x165667B19E3779F9ull;
    for (uint32_t k = uint32_t(h >> 40);; k++) {
        HandleSlot& s = g_handle_slots[k & (kHandleSlots - 1)];
        if (s.generation == g_handle_generation) {
            if (s.handle != handle || s.texture_pool != tp || s.sampler_pool != sp) continue;
            g_handle_hits++;
            return &s;
        }
        s = HandleSlot{};
        s.handle = handle, s.texture_pool = tp, s.sampler_pool = sp;
        lookup_handle(handle, s.texture, s.sampler, &s.view, &s.has_sampler);
        s.object = s.texture ? get<Texture>(s.texture) : nullptr;
        s.generation = s.object ? g_handle_generation : 0;
        if (s.object) g_handle_used++;
        g_handle_misses++;
        return &s;
    }
}

// Host data every draw binds (the support buffer, c[0]) is the same for most draws of a submit, and was built and
// copied for each stage of each draw (~11 KB per draw). DEBUG.md #30's rule stands: dedup shares bytes by address, so
// only bytes that do not change during the submit may go through it, and the stack copies these were built in did.
// Here each distinct content gets a block of its own for the submit, found by what it is made from, and dedup keeps
// one upload of it. The blocks are reused from sets_begin_submit on, where dedup starts over too.
struct SupportBlock {
    float width = 0, height = 0, scale = 1;  // the key: support_buffer_set_viewport's arguments
    SupportBuffer sb;
};
struct ConstantsBlock {
    int hw = 0;         // the key: the Maxwell stage (vertex 0, fragment 4) and the stage's storage bindings,
    Range storage[16];  // all driver_constants reads outside compute
    std::array<uint8_t, 0x1000> bytes;
    uint64_t upload_gen = 0;
    VkDescriptorBufferInfo upload_info{};
};
constexpr size_t kBlocks = 256;  // was 32: prevents >32 skinned vertex SSBO states in a submit from falling back to 4 KB/draw copies
std::vector<std::unique_ptr<SupportBlock>> g_support;
std::vector<std::unique_ptr<ConstantsBlock>> g_constants;
size_t g_support_used = 0, g_constants_used = 0;
static thread_local ConstantsBlock* s_last_const_block[2] = {};

const SupportBuffer* support_block(float width, float height, float scale) {
    for (size_t k = 0; k < g_support_used; k++)
        if (std::memcmp(&g_support[k]->width, &width, 4) == 0 && std::memcmp(&g_support[k]->height, &height, 4) == 0 &&
            g_support[k]->scale == scale)
            return &g_support[k]->sb;
    if (g_support_used == kBlocks) return nullptr;
    if (g_support_used == g_support.size()) g_support.push_back(std::make_unique<SupportBlock>());
    SupportBlock& b = *g_support[g_support_used++];
    b.width = width, b.height = height, b.scale = scale;
    b.sb = support_buffer_defaults();
    support_buffer_set_viewport(b.sb, width, height, false, scale);
    return &b.sb;
}

ConstantsBlock* constants_block_obj(ShaderStage stage, int st) {
    if (stage == ShaderStage::Compute) return nullptr;  // its table also lists the uniform blocks; a few per frame
    int hw = stage == ShaderStage::Fragment ? 4 : 0;
    const Range* key = g_state.storage[st];
    if (st >= 0 && st < 2 && s_last_const_block[st] &&
        s_last_const_block[st]->hw == hw &&
        std::memcmp(s_last_const_block[st]->storage, key, sizeof(s_last_const_block[st]->storage)) == 0) {
        return s_last_const_block[st];
    }
    for (size_t k = 0; k < g_constants_used; k++) {
        if (g_constants[k]->hw == hw && std::memcmp(g_constants[k]->storage, key, sizeof g_constants[k]->storage) == 0) {
            if (st >= 0 && st < 2) s_last_const_block[st] = g_constants[k].get();
            return g_constants[k].get();
        }
    }
    if (g_constants_used == kBlocks) return nullptr;
    if (g_constants_used == g_constants.size()) g_constants.push_back(std::make_unique<ConstantsBlock>());
    ConstantsBlock& b = *g_constants[g_constants_used++];
    b.hw = hw;
    std::memcpy(b.storage, key, sizeof b.storage);
    b.bytes = driver_constants(stage, st);
    b.upload_gen = 0;
    if (st >= 0 && st < 2) s_last_const_block[st] = &b;
    return &b;
}

// The buffer a descriptor points to: `bytes` copied into the upload buffer (at least 16 bytes, zero padded).
// Storage buffers written by compute shaders go back to guest memory after the submit (the GPU's writes are what the
// game and later work read; WriteBack in nvn_vk.h). One copy per written range, shared by the later work of the
// submit (nvn_written.cpp); g_write_backs: NX_COMPUTE_SEPARATE=1, a copy per binding.
std::vector<WriteBack> g_write_backs;
bool all_write_backs() {  // NX_WRITE_BACK_ALL=1: every compute storage binding, written or not (bisecting)
    static const bool on = std::getenv("NX_WRITE_BACK_ALL") != nullptr;
    return on;
}

// Zero padding after bound data (4 KB for uniform blocks, 64 KB for dynamically indexed ones and storage buffers) exists
// because Metal does not bound-check. Where the GPU has robustBufferAccess (Vulkan on Intel, NVIDIA, Adreno: every
// device but MoltenVK) a read past a binding's range returns 0 by itself, so the padding is pure CPU cost (a memset
// and a copy of up to 64 KB per unique block: ~6 MB per frame in gameplay on the Intel iGPU, 30% of the replay thread).
// NX_PAD_BUFFERS=1 brings the padding back (bisecting); Apple always pads.
uint64_t pad_min(uint64_t want) {
    static const bool pad = [] {
#if defined(__APPLE__)
        return true;
#else
        return std::getenv("NX_PAD_BUFFERS") != nullptr || !vk_features().robustBufferAccess;
#endif
    }();
    return pad ? want : 16;
}

// guest: the bytes are guest memory (a copy is shared by later draws of the submit); host scratch data (the support
// buffer, c[0]: on the stack, at the same address for every draw) is copied every time (docs/nvn/DEBUG.md #30).
VkDescriptorBufferInfo buffer_of(VkCommandBuffer cb, const void* bytes, uint64_t size, uint64_t min_size = 16,
                                 bool write_back = false, bool guest = true) {
    uint64_t n = std::max<uint64_t>((size + 15) & ~15ull, min_size);
    const auto* p = static_cast<const uint8_t*>(bytes);
    if (!write_back && guest) {
        g_upload_kind = kUploadBlock;
        Upload u = read_upload(cb, p, size, n, 256);
        g_upload_kind = kUploadOther;
        return {u.buffer, u.offset, n};
    }
    if (write_back && p && size && !written_separate()) {
        Upload u = written_bind(cb, p, size, n);
        return {u.buffer, u.offset, n};
    }
    Upload u = upload_alloc(n, 256);
    std::memset(u.ptr, 0, n);
    if (bytes && size) std::memcpy(u.ptr, bytes, size);
    // Only what a compute shader may have written goes back. Host scratch data came here too and was "written back"
    // after the submit to the stack address it had been built at (the support buffer and c[0] of every stage of every
    // draw, into a dead frame; with NX_ASYNC_GPU from whichever thread retired the submit). NX_DRAW_SLOW keeps that.
    if (bytes && size && (write_back || draw_slow(kSlowUploads)))
        g_write_backs.push_back({u.ptr, const_cast<uint8_t*>(static_cast<const uint8_t*>(bytes)), size});
    return {u.buffer, u.offset, n};
}

// The sets being built: scratch kept from draw to draw (three vectors, one of them ~130 KB, were allocated per draw).
struct SetWrite {  // one write of a set, as the set cache compares it
    uint32_t binding = 0, type = 0;
    uint64_t a = 0, b = 0, c = 0;  // buffer, offset, range; or view, sampler, layout
};
struct Descriptors {
    VkDescriptorSet sets[4] = {};
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<uint8_t> set_index;  // the set of each write (its dstSet is filled when the set is known)
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<SetWrite> key;  // scratch for the set cache
    SetWrite set_keys[4][64];
    uint8_t set_counts[4] = {};
    bool direct_keys = false;
    uint64_t dynamic = 0;              // set 0 bindings given a dynamic offset (ProgramLayout::dynamic)
    uint32_t dynamic_offset[64] = {};  // their ring offsets, by binding
    uint64_t dynamic_ssbo = 0;         // set 1 bindings given a dynamic offset (ProgramLayout::dynamic_ssbo)
    uint32_t dynamic_ssbo_offset[64] = {};
};
Descriptors g_sets;

// The set cache (docs/nvn/android-perf/PLAN.md R3): a set with the same layout and the same writes as one made earlier
// in this submit is that set. Every draw allocated and wrote up to four sets (~1,500 allocations and ~5,000 writes per
// course frame), most of it driver time on Adreno; texture sets repeat across the draws of a material, and buffer sets
// whenever dedup gave the same ring offsets. Sets come from the submit's pool and are reset with it, so entries live
// one submit (sets_begin_submit). The writes are compared in full, not by hash only. NX_SET_CACHE=0: off.
struct CachedSet {
    uint64_t generation = 0, hash = 0;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    uint32_t at = 0, n = 0;  // its writes in g_set_arena
    VkDescriptorSet set = VK_NULL_HANDLE;
};
constexpr uint32_t kSetSlots = 32768;  // was 8192: supports dense draw scenes without early cutoff
CachedSet g_set_cache[kSetSlots];
std::vector<SetWrite> g_set_arena;
uint64_t g_set_generation = 1, g_set_hits = 0, g_set_misses = 0;
uint32_t g_set_used = 0;

constexpr uint32_t kSet1Slots = 32768;  // separate Set 1 (SSBO) cache so compute skinning ring churn never wipes Set 0 / Set 2
CachedSet g_set1_cache[kSet1Slots];
std::vector<SetWrite> g_set1_arena;
uint64_t g_set1_generation = 1;
uint32_t g_set1_used = 0;

#if defined(HAS_ARM_CRC32)
__attribute__((target("crc")))
static inline uint64_t hash_step_crc32(uint64_t h, uint64_t v) {
    uint32_t lo = __crc32cd(static_cast<uint32_t>(h), v);
    uint32_t hi = __crc32cd(static_cast<uint32_t>(h >> 32), v ^ 0x9E3779B97F4A7C15ull);
    return (uint64_t(hi) << 32) | lo;
}
#endif

static inline uint64_t hash_step(uint64_t h, uint64_t v) {
#if defined(HAS_ARM_CRC32)
#if defined(__linux__) || defined(__ANDROID__)
    static const bool s_has_crc32 = (getauxval(AT_HWCAP) & (1UL << 7)) != 0;
    if (__builtin_expect(s_has_crc32, 1)) return hash_step_crc32(h, v);
#else
    return hash_step_crc32(h, v);
#endif
#endif
    return (h ^ v) * 0x100000001B3ull ^ (h >> 31);
}

bool set_cache_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_SET_CACHE");
        return !(e && e[0] == '0') && !draw_slow(kSlowEmptySets);
    }();
    return on;
}

template <typename H>
uint64_t handle_bits(H h) {
    uint64_t v = 0;
    std::memcpy(&v, &h, sizeof h < sizeof v ? sizeof h : sizeof v);
    return v;
}

template <typename H>
H bits_handle(uint64_t v) {
    H h{};
    std::memcpy(&h, &v, sizeof h < sizeof v ? sizeof h : sizeof v);
    return h;
}

void append_write_from_key(Descriptors& d, uint32_t k, const SetWrite& sw, VkDescriptorSet dst_set) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = dst_set;
    w.dstBinding = sw.binding;
    w.descriptorCount = 1;
    w.descriptorType = VkDescriptorType(sw.type);
    if (k < 2) {
        d.buffers.push_back({bits_handle<VkBuffer>(sw.a), sw.b, sw.c});
        w.pBufferInfo = &d.buffers.back();
    } else {
        d.images.push_back({bits_handle<VkSampler>(sw.b), bits_handle<VkImageView>(sw.a), VkImageLayout(sw.c)});
        w.pImageInfo = &d.images.back();
    }
    d.writes.push_back(w);
}

static bool set2_reuse_on() {
    static int s_state = -1;
    int st = s_state;
    if (__builtin_expect(st < 0, 0)) {
        const char* e = std::getenv("NX_SET2_REUSE");
        st = (e && e[0] == '0') ? 0 : 1;
        s_state = st;
    }
    return st != 0;
}

static thread_local VkDescriptorSetLayout s_last_set2_layout = VK_NULL_HANDLE;
static thread_local uint64_t s_last_set2_gen = 0;
static thread_local VkDescriptorSet s_last_set2 = VK_NULL_HANDLE;
static thread_local uint64_t s_last_set2_serial = ~0ull;  // g_textures.serial the stored set was built for

struct LastResolvedSet {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    uint64_t gen = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t arena_at = 0, arena_n = 0;
};
static thread_local LastResolvedSet s_last_resolved[4];

struct SlotBufferCache {
    uint64_t address = 0;
    const uint8_t* host_p = nullptr;
    uint64_t copy_size = 0;
    uint64_t size = 0;
    uint64_t min_size = 0;
    uint64_t gen = 0;
    bool overlaps = false;
    VkDescriptorBufferInfo info{};
};
static thread_local SlotBufferCache s_ubo_cache[2][18] = {};
static thread_local SlotBufferCache s_ssbo_cache[2][16] = {};

struct SupportUploadCache {
    float vw = -1.0f, vh = -1.0f, scale = -1.0f;
    uint64_t gen = 0;
    VkDescriptorBufferInfo info{};
};
static thread_local SupportUploadCache s_support_cache{};

struct ConstDataCache {
    const ShaderInfo* shader = nullptr;
    uint64_t gen = 0;
    VkDescriptorBufferInfo info{};
};
static thread_local ConstDataCache s_const_data_cache[2]{};

// d.sets from the cache or newly made, the writes of the cached ones dropped and the rest pointed at their new sets.
void resolve_sets(const ProgramLayout& layout) {
    Descriptors& d = g_sets;
    uint16_t set_indices[4][64];
    uint8_t legacy_counts[4] = {};
    bool overflow = false;
    if (!d.direct_keys) {
        if (d.writes.empty()) {
            for (uint32_t k = 0; k < 4; k++) {
                if (k == 2 && d.sets[2] != VK_NULL_HANDLE) continue;
                d.sets[k] = VK_NULL_HANDLE;
            }
            return;
        }
        for (size_t i = 0; i < d.writes.size(); i++) {
            uint8_t s = d.set_index[i];
            if (s < 4) {
                if (legacy_counts[s] < 64) {
                    set_indices[s][legacy_counts[s]++] = static_cast<uint16_t>(i);
                } else {
                    overflow = true;
                    break;
                }
            }
        }
    }
    for (uint32_t k = 0; k < 4; k++) {
        if (k == 2 && d.sets[2] != VK_NULL_HANDLE) {
            g_set_hits++;
            continue;
        }
        d.sets[k] = VK_NULL_HANDLE;
        const SetWrite* key_ptr = nullptr;
        uint32_t key_n = 0;
        if (d.direct_keys) {
            key_n = d.set_counts[k];
            if (!key_n) continue;
            key_ptr = d.set_keys[k];
        } else {
            if (!overflow && !legacy_counts[k]) continue;
            d.key.clear();
            auto push_from_write = [&](const VkWriteDescriptorSet& w) {
                SetWrite e;
                e.binding = w.dstBinding, e.type = uint32_t(w.descriptorType);
                if (w.pBufferInfo)
                    e.a = handle_bits(w.pBufferInfo->buffer), e.b = w.pBufferInfo->offset, e.c = w.pBufferInfo->range;
                else if (w.pImageInfo)
                    e.a = handle_bits(w.pImageInfo->imageView), e.b = handle_bits(w.pImageInfo->sampler),
                    e.c = uint64_t(w.pImageInfo->imageLayout);
                d.key.push_back(e);
            };
            if (!overflow) {
                d.key.reserve(legacy_counts[k]);
                for (uint8_t j = 0; j < legacy_counts[k]; j++) push_from_write(d.writes[set_indices[k][j]]);
            } else {
                for (size_t i = 0; i < d.writes.size(); i++)
                    if (d.set_index[i] == k) push_from_write(d.writes[i]);
            }
            if (d.key.empty()) continue;
            key_ptr = d.key.data();
            key_n = uint32_t(d.key.size());
        }

        const bool is_set1 = (k == 1);
        const uint64_t cur_set_gen = is_set1 ? g_set1_generation : g_set_generation;
        CachedSet* const cache_table = is_set1 ? g_set1_cache : g_set_cache;
        const uint32_t cache_mask = (is_set1 ? kSet1Slots : kSetSlots) - 1;
        const uint32_t cache_limit = (is_set1 ? kSet1Slots : kSetSlots) / 2;
        uint32_t& cache_used = is_set1 ? g_set1_used : g_set_used;
        std::vector<SetWrite>& cache_arena = is_set1 ? g_set1_arena : g_set_arena;

        LastResolvedSet& last_r = s_last_resolved[k];
        if (last_r.gen == cur_set_gen && last_r.layout == layout.sets[k] &&
            last_r.set != VK_NULL_HANDLE && last_r.arena_n == key_n &&
            last_r.arena_at + last_r.arena_n <= cache_arena.size() &&
            std::memcmp(&cache_arena[last_r.arena_at], key_ptr, key_n * sizeof(SetWrite)) == 0) {
            d.sets[k] = last_r.set;
            g_set_hits++;
            continue;
        }

        uint64_t h = handle_bits(layout.sets[k]) * 0x9E3779B97F4A7C15ull;
        for (uint32_t j = 0; j < key_n; j++) {
            const SetWrite& e = key_ptr[j];
            for (uint64_t v : {uint64_t(e.binding) << 32 | e.type, e.a, e.b, e.c})
                h = hash_step(h, v);
        }

        CachedSet* slot = nullptr;
        uint32_t hit_at = 0, hit_n = 0;
        for (uint32_t probe = uint32_t(h >> 32);; probe++) {
            CachedSet& c = cache_table[probe & cache_mask];
            if (c.generation != cur_set_gen) {
                slot = &c;
                break;
            }
            if (c.hash == h && c.layout == layout.sets[k] && c.n == key_n &&
                std::memcmp(&cache_arena[c.at], key_ptr, key_n * sizeof(SetWrite)) == 0) {
                d.sets[k] = c.set, hit_at = c.at, hit_n = c.n, g_set_hits++;
                break;
            }
        }
        if (d.sets[k]) {
            last_r = {layout.sets[k], cur_set_gen, d.sets[k], hit_at, hit_n};
            continue;
        }
        d.sets[k] = is_set1 ? ssbo_descriptor_set(layout.sets[k]) : descriptor_set(layout.sets[k]);
        g_set_misses++;

        if (d.direct_keys) {
            if (d.sets[k]) {
                if (d.buffers.capacity() < 256) d.buffers.reserve(256);
                if (d.images.capacity() < 256) d.images.reserve(256);
                if (d.writes.capacity() < 512) d.writes.reserve(512);
                for (uint32_t j = 0; j < key_n; j++) append_write_from_key(d, k, key_ptr[j], d.sets[k]);
            }
        } else if (!overflow) {
            for (uint8_t j = 0; j < legacy_counts[k]; j++)
                d.writes[set_indices[k][j]].dstSet = d.sets[k];
        } else {
            for (size_t i = 0; i < d.writes.size(); i++)
                if (d.set_index[i] == k) d.writes[i].dstSet = d.sets[k];
        }

        if (!d.sets[k] || cache_used >= cache_limit) continue;
        const uint32_t new_at = uint32_t(cache_arena.size());
        *slot = {cur_set_gen, h, layout.sets[k], new_at, key_n, d.sets[k]};
        cache_arena.insert(cache_arena.end(), key_ptr, key_ptr + key_n);
        cache_used++;
        last_r = {layout.sets[k], cur_set_gen, d.sets[k], new_at, key_n};
    }
    if (d.sets[2] != VK_NULL_HANDLE) {
        s_last_set2 = d.sets[2];
        s_last_set2_layout = layout.sets[2];
        s_last_set2_gen = g_set_generation;
        s_last_set2_serial = g_textures.serial;
    }
    if (!d.direct_keys) {
        size_t kept = 0;  // the writes of new sets only
        for (size_t i = 0; i < d.writes.size(); i++)
            if (d.writes[i].dstSet) d.writes[kept++] = d.writes[i];
        d.writes.resize(kept);
    }
}

}  // namespace

std::vector<WriteBack>& separate_write_backs() { return g_write_backs; }

void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view) {
    HandleSlot* s = handle_slot(handle);
    if (!s) return lookup_handle(handle, texture, sampler, view);
    texture = s->texture;
    if (s->has_sampler) sampler = s->sampler;
    if (view) *view = s->view;
}

// Descriptor sets for a program's stages from the bound state: buffers (sets 0/1), textures (set 2, g_textures as
// gather_textures left them), storage images (set 3). Records uploads and layout changes, so it runs outside render
// passes when textures or images need them. A set is allocated when its first binding is written: the sets no stage
// declares anything in (usually two of the four) are neither allocated nor bound.
void build_sets(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout) {
    Descriptors& d = g_sets;
    if (draw_slow(kSlowScratch))
        d = Descriptors{};
    else
        d.writes.clear(), d.set_index.clear(), d.buffers.clear(), d.images.clear();
    d.set_counts[0] = d.set_counts[1] = d.set_counts[2] = d.set_counts[3] = 0;
    d.dynamic = 0;
    d.dynamic_ssbo = 0;
    const bool every_set = draw_slow(kSlowEmptySets), shared = !draw_slow(kSlowUploads), cached = set_cache_on();
    d.direct_keys = cached && !every_set;
    auto alloc_set_k = [&](uint32_t k) {
        return k == 1 ? ssbo_descriptor_set(layout.sets[k]) : descriptor_set(layout.sets[k]);
    };
    for (int k = 0; k < 4; k++) d.sets[k] = every_set ? alloc_set_k(uint32_t(k)) : VK_NULL_HANDLE;
    auto set_of = [&](uint32_t k) {  // with the set cache, made (or found) in bind_sets
        d.set_index.push_back(uint8_t(k));
        if (cached) return VkDescriptorSet(VK_NULL_HANDLE);
        if (!d.sets[k]) d.sets[k] = alloc_set_k(k);
        return d.sets[k];
    };
    auto& writes = d.writes;
    auto& buffers = d.buffers;
    auto& images = d.images;
    if (!d.direct_keys) buffers.reserve(1024), images.reserve(1024), writes.reserve(2048);  // pointers into them are kept
    auto materialize_fallback = [&]() {
        if (!d.direct_keys) return;
        d.direct_keys = false;
        buffers.reserve(1024), images.reserve(1024), writes.reserve(2048);
        for (uint32_t sk = 0; sk < 4; sk++) {
            for (uint8_t j = 0; j < d.set_counts[sk]; j++)
                append_write_from_key(d, sk, d.set_keys[sk][j], set_of(sk));
            d.set_counts[sk] = 0;
        }
    };
    VkExtent2D pe = pass_extent();
    float fallback_w = g_low_3d_target ? (pe.width ? float(pe.width) : 720.0f) : 1280.0f;
    float fallback_h = g_low_3d_target ? (pe.height ? float(pe.height) : 405.0f) : 720.0f;
    float vw = g_state.has_viewport ? float(g_state.viewport[2]) : fallback_w;
    float vh = g_state.has_viewport ? float(g_state.viewport[3]) : fallback_h;
    if (g_uw_wide && g_state.has_viewport) {  // NX_ULTRAWIDE: the viewport the shaders see is the widened one
        int32_t x = g_state.viewport[0], w = g_state.viewport[2];
        uw_span(x, w, UINT32_MAX);
        vw = float(w);
    }
    if (g_low_3d_target && g_state.has_viewport) {
        vw *= 0.75f;
        vh *= 0.75f;
    }
    const float scale = g_hud_target ? hud_scale() : g_low_3d_target ? 0.75f : 1.0f;  // NX_HUD_1080 / NX_LOW_3D: the shaders' render scale (gl_FragCoord)
    const uint64_t cur_gen = dedup_generation();
    SupportBuffer sb;  // built when a binding gets a copy of its own
    bool sb_built = false;
    auto support = [&]() -> VkDescriptorBufferInfo {
        if (shared && s_support_cache.gen == cur_gen &&
            s_support_cache.vw == vw && s_support_cache.vh == vh && s_support_cache.scale == scale) {
            return s_support_cache.info;
        }
        const SupportBuffer* block = shared ? support_block(vw, vh, scale) : nullptr;
        if (block) {
            Upload u = dedup_upload(reinterpret_cast<const uint8_t*>(block), sizeof sb, sizeof sb, 256);
            VkDescriptorBufferInfo info{u.buffer, u.offset, sizeof sb};
            s_support_cache = {vw, vh, scale, cur_gen, info};
            return info;
        }
        if (!sb_built) {
            sb = support_buffer_defaults();
            support_buffer_set_viewport(sb, vw, vh, false, scale);
            sb_built = true;
        }
        return buffer_of(cb, &sb, sizeof sb, 16, false, false);
    };
    // A storage binding's bytes, and whether this compute stage writes them.
    auto storage = [](const ShaderInfo* s, const DescriptorBinding& b, int st, const uint8_t*& p, uint64_t& size) {
        const Range& r = b.nvn.index >= 0 && b.nvn.index < 16 ? g_state.storage[st][b.nvn.index] : Range{};
        p = r.address ? guest_bytes(r.address) : nullptr;
        size = p ? std::min<uint64_t>(r.size, 16ull << 20) : 0;
        return s->stage == ShaderStage::Compute &&
               (all_write_backs() || (b.binding < 64 && written_storage_bindings(s) >> b.binding & 1));
    };
    // The written ranges first (nvn_written.cpp): two bindings of one dispatch that meet get one copy, and bindings
    // that read what the dispatch writes see it, as on the GPU.
    if (shaders.size() == 1 && shaders[0]->stage == ShaderStage::Compute) {
        const ShaderInfo* s = shaders[0];
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 2) break;
            const uint8_t* p;
            uint64_t size;
            if (b.nvn.source == NvnSource::StorageBuffer && storage(s, b, 5, p, size) &&
                p && size && !written_separate())
                written_bind(cb, p, size, std::max<uint64_t>((size + 15) & ~15ull, pad_min(0x10000)));
        }
    }
    for (const ShaderInfo* s : shaders) {
        int st = api_stage(s->stage);
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 2) break;  // bindings are sorted by (set, binding)
            VkDescriptorBufferInfo info{};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer: info = support(); break;
                case NvnSource::ConstantData: {
                    if (shared && st < 2 && s_const_data_cache[st].shader == s && s_const_data_cache[st].gen == cur_gen) {
                        info = s_const_data_cache[st].info;
                    } else {
                        info = buffer_of(cb, s->constants.data(), s->constants.size());
                        if (shared && st < 2) s_const_data_cache[st] = {s, cur_gen, info};
                    }
                    break;
                }
                case NvnSource::UniformBlock:
                case NvnSource::UniformAsStorage: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 18 ? g_state.uniforms[st][b.nvn.index] : Range{};
                    bool indexed = b.set == 0 && b.binding < 64 && (dynamic_uniform_bindings(s) >> b.binding & 1);
                    const uint64_t want_min = pad_min(indexed ? 0x10000 : 0x1000);
                    if (shared && st < 2 && b.nvn.index >= 0 && b.nvn.index < 18 && r.address) {
                        SlotBufferCache& slot = s_ubo_cache[st][b.nvn.index];
                        if (slot.address == r.address && slot.size == r.size && slot.min_size == want_min &&
                            slot.gen == cur_gen) {
                            info = slot.info;
                            break;
                        }
                    }
                    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
                    // Reads past the bound size return 0 on Maxwell and garbage on Metal (a UI shader read c3[10]
                    // of a 0x90-byte block and wrote Inf): the bound data goes in a zero-padded 4 KB at least.
                    // Banks the shader indexes with a computed value (clamped to 64 KB, nvn_spirv_patch.cpp): all
                    // 64 KB, so every read past the data returns 0 as on Maxwell.
                    if (static const bool tr8 = std::getenv("NX_TRACE_C8") != nullptr; tr8 && s->stage == ShaderStage::Fragment && b.nvn.index == 5 && p) {
                        static int shown = 0;
                        if (shown++ % 5000 == 0) {  // tile light counts of the uber shaders' c8: word at 8196 + tile * 256
                            const uint32_t* w = reinterpret_cast<const uint32_t*>(p);
                            std::string line;
                            uint32_t nonzero = 0;
                            for (uint32_t t = 0; t < 144 && (8196 + t * 256 + 4) <= r.size; t++) {
                                uint32_t c = w[(8196 + t * 256) / 4];
                                nonzero += c != 0;
                                if (t < 24) line += " " + std::to_string(c);
                            }
                            uint64_t nz = 0, first = UINT64_MAX;
                            for (uint64_t k = 0; k < r.size / 4; k++)
                                if (w[k]) nz++, first = std::min<uint64_t>(first, k);
                            log("c8 @%llx: fs %s size %llu overlaps_written %d tiles with lights %u; nonzero words %llu first at %lld; counts:%s",
                                (unsigned long long)r.address, s->hash.c_str(), (unsigned long long)r.size, int(written_overlaps(p, r.size)), nonzero,
                                (unsigned long long)nz, (long long)first, line.c_str());
                        }
                    }
                    const uint64_t copy_sz = p ? std::min<uint64_t>(r.size, 0x10000) : 0;
                    info = buffer_of(cb, p, copy_sz, want_min);
                    if (shared && st < 2 && b.nvn.index >= 0 && b.nvn.index < 18 && r.address && p) {
                        s_ubo_cache[st][b.nvn.index] = {r.address, p, copy_sz, r.size, want_min, cur_gen, false, info};
                    }
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 16 ? g_state.storage[st][b.nvn.index] : Range{};
                    const uint64_t want_min = pad_min(0x10000);
                    if (shared && s->stage != ShaderStage::Compute && st < 2 && b.nvn.index >= 0 && b.nvn.index < 16 && r.address) {
                        SlotBufferCache& slot = s_ssbo_cache[st][b.nvn.index];
                        if (slot.address == r.address && slot.size == r.size && slot.min_size == want_min &&
                            slot.gen == cur_gen) {
                            if (slot.overlaps) written_count_bind();
                            info = slot.info;
                            break;
                        }
                    }
                    const uint8_t* p;
                    uint64_t size;
                    bool written = storage(s, b, st, p, size);
                    bool ov = !written && s->stage != ShaderStage::Compute && written_overlaps(p, size);
                    if (ov) written_count_bind();
                    // At least 64 KB: Metal does not bound-check, and a shader writing past a small or missing
                    // buffer overwrote the next data in the upload buffer (garbage everywhere after a compute).
                    info = buffer_of(cb, p, size, want_min, written);
                    if (shared && !written && s->stage != ShaderStage::Compute && st < 2 && b.nvn.index >= 0 && b.nvn.index < 16 && r.address && p) {
                        s_ssbo_cache[st][b.nvn.index] = {r.address, p, size, r.size, want_min, cur_gen, ov, info};
                    }
                    break;
                }
                case NvnSource::DriverConstants: {
                    if (ConstantsBlock* cb_blk = shared ? constants_block_obj(s->stage, st) : nullptr) {
                        if (cb_blk->upload_gen != cur_gen) {
                            Upload u = dedup_upload(cb_blk->bytes.data(), 0x1000, 0x1000, 256);
                            cb_blk->upload_info = {u.buffer, u.offset, 0x1000};
                            cb_blk->upload_gen = cur_gen;
                        }
                        info = cb_blk->upload_info;
                        break;
                    }
                    auto c0 = driver_constants(s->stage, st);
                    info = buffer_of(cb, c0.data(), c0.size(), 16, false, false);
                    break;
                }
                default: info = buffer_of(cb, nullptr, 0x1000, 16, false, false); break;
            }
            VkDescriptorType type = VkDescriptorType(b.type);
            if (b.set == 0 && b.binding < 64 && layout.dynamic >> b.binding & 1) {  // the offset goes with the bind
                d.dynamic |= 1ull << b.binding, d.dynamic_offset[b.binding] = uint32_t(info.offset);
                info.offset = 0, type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            } else if (b.set == 1 && b.binding < 64 && layout.dynamic_ssbo >> b.binding & 1) {
                d.dynamic_ssbo |= 1ull << b.binding, d.dynamic_ssbo_offset[b.binding] = uint32_t(info.offset);
                info.offset = 0, type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            }
            if (d.direct_keys && b.set < 4 && d.set_counts[b.set] < 64) {
                d.set_keys[b.set][d.set_counts[b.set]++] = {
                    b.binding, uint32_t(type), handle_bits(info.buffer), info.offset, info.range};
            } else {
                materialize_fallback();
                buffers.push_back(info);
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = set_of(b.set);
                w.dstBinding = b.binding;
                w.descriptorCount = 1;
                w.descriptorType = type;
                w.pBufferInfo = &buffers.back();
                writes.push_back(w);
            }
        }
    }
    auto push_image_write = [&](uint32_t set_k, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo& ii) {
        if (d.direct_keys && d.set_counts[set_k] < 64) {
            d.set_keys[set_k][d.set_counts[set_k]++] = {
                binding, uint32_t(type), handle_bits(ii.imageView), handle_bits(ii.sampler), uint64_t(ii.imageLayout)};
        } else {
            materialize_fallback();
            images.push_back(ii);
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set_of(set_k);
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = &images.back();
            writes.push_back(w);
        }
    };
    const DrawTextures& t = g_textures;
    if (cached && !every_set && t.set2_unchanged && !t.bindings.empty() &&
        s_last_set2 != VK_NULL_HANDLE && s_last_set2_layout == layout.sets[2] && s_last_set2_gen == g_set_generation &&
        s_last_set2_serial == t.serial && !draw_slow(kSlowScratch)) {
        d.sets[2] = s_last_set2;
    } else {
        const uint64_t vf_ser = view_forget_serial();
        for (size_t k = 0; k < t.bindings.size(); k++) {
            auto [s, b] = t.bindings[k];
            ResolvedTexture& r = g_textures.resolved[k];
            VkDescriptorImageInfo ii{};
            GpuImage* cur = set2_reuse_on() && r.texture ? (r.gpu_image ? r.gpu_image : image_of(r.texture)) : nullptr;
            const VkImageLayout want_now = cur ? (cur->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                               : VK_IMAGE_LAYOUT_UNDEFINED;
            if (cur && r.ready_cached && r.vk_view != VK_NULL_HANDLE && r.vk_image == cur->image &&
                r.vk_view_serial == vf_ser && cur->layout == want_now) {
                ii.imageView = r.vk_view;
                ii.imageLayout = want_now;
            } else {
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ii.imageView = r.texture ? sampled_view(cb, r.texture, b->image, &r.view, &ii.imageLayout)
                                         : dummy_view(cb, b->image);
                if (r.texture) {
                    GpuImage* made = r.gpu_image ? r.gpu_image : image_of(r.texture);
                    r.gpu_image = made;
                    if (made && ii.imageView != VK_NULL_HANDLE) {
                        const uint64_t cur_vf = view_forget_serial();
                        r.vk_view = ii.imageView;
                        r.vk_image = made->image;
                        r.vk_layout = ii.imageLayout;
                        r.vk_view_serial = cur_vf;
                        r.ready_cached = true;
                        if (k < t.handles.size()) {
                            if (HandleSlot* slot = handle_slot(t.handles[k])) {
                                slot->vk_image = made->image;
                                slot->vk_view = ii.imageView;
                                slot->vk_view_serial = cur_vf;
                                slot->vk_img_binding = b->image;
                            }
                        }
                    } else {
                        r.vk_view = VK_NULL_HANDLE;
                        r.ready_cached = false;
                    }
                }
            }
            ii.sampler = r.vk_sampler ? r.vk_sampler : sampler_for(r.sampler, b->image.depth, g_state.depth.depth_test);
            push_image_write(2, b->binding, VkDescriptorType(b->type), ii);
        }
    }
    if (layout.sets[3] != VK_NULL_HANDLE) {
        for (const ShaderInfo* s : shaders) {
            int st = api_stage(s->stage);
            for (const DescriptorBinding& b : s->refl.bindings) {
                if (b.set != 3) continue;
                uint64_t handle = b.nvn.source == NvnSource::ImageBinding && b.nvn.index >= 0 && b.nvn.index < 16
                                      ? g_state.images[st][b.nvn.index]
                                      : 0;
                uint64_t texture = 0;
                SamplerInfo si;
                ViewInfo view;
                if (handle) texture_for(handle & 0xFFFFF, texture, si, &view);
                VkDescriptorImageInfo ii{};
                ii.imageView = storage_view(cb, texture, b.image, texture ? &view : nullptr);
                ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                push_image_write(3, b.binding, VkDescriptorType(b.type), ii);
            }
        }
    }
}

// A set with no binding in the layout is statically used by no shader, so nothing need be bound there: the runs of
// sets build_sets allocated are bound, in ascending order (each bind leaves the lower sets, bound with this same
// layout, undisturbed). Redundant binds of sets already in place under the same pipeline layout are skipped.
struct BoundSets {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[4] = {};
    uint32_t dynamic_offsets[64] = {};
    uint32_t dynamic_count = 0;
    uint32_t dynamic_ssbo_offsets[64] = {};
    uint32_t dynamic_ssbo_count = 0;
};
BoundSets g_bound_sets[2] = {};  // 0: graphics, 1: compute
uint64_t g_sets_bind_calls = 0, g_sets_bound = 0, g_sets_bind_skipped = 0;

void sets_invalidate_bound() {
    g_bound_sets[0] = BoundSets{};
    g_bound_sets[1] = BoundSets{};
}

void bind_sets(VkCommandBuffer cb, VkPipelineBindPoint point, const ProgramLayout& layout) {
    Descriptors& d = g_sets;
    if (set_cache_on()) resolve_sets(layout);
    if (!d.writes.empty()) {
        p_vkUpdateDescriptorSets(vk_device(), uint32_t(d.writes.size()), d.writes.data(), 0, nullptr);
        d.writes.clear();
    }
    // Set 0 and Set 1 dynamic offsets, in binding order (every dynamic binding of the layout; one no stage wrote: 0).
    uint32_t offsets[128];
    uint32_t noff0 = 0;
    for (uint64_t m = layout.dynamic; m; m &= m - 1) {
        uint32_t b = uint32_t(__builtin_ctzll(m));
        offsets[noff0++] = d.dynamic >> b & 1 ? d.dynamic_offset[b] : 0;
    }
    uint32_t noff1 = 0;
    for (uint64_t m = layout.dynamic_ssbo; m; m &= m - 1) {
        uint32_t b = uint32_t(__builtin_ctzll(m));
        offsets[noff0 + noff1++] = d.dynamic_ssbo >> b & 1 ? d.dynamic_ssbo_offset[b] : 0;
    }
    if (draw_slow(kSlowEmptySets) || draw_slow(kSlowState)) {
        p_vkCmdBindDescriptorSets(cb, point, layout.layout, 0, 4, d.sets, noff0 + noff1, offsets);
        return;
    }

    const int pt = (point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0);
    BoundSets& b = g_bound_sets[pt];
    if (b.cb != cb) {
        b = BoundSets{};
        b.cb = cb;
    }
    if (b.layout != layout.layout) {
        b.layout = layout.layout;
        for (int k = 0; k < 4; k++) b.sets[k] = VK_NULL_HANDLE;
        b.dynamic_count = 0;
        b.dynamic_ssbo_count = 0;
    }

    bool need[4] = {};
    for (uint32_t k = 0; k < 4; k++) {
        if (!d.sets[k]) {
            b.sets[k] = VK_NULL_HANDLE;
            g_sets_skipped++;
            continue;
        }
        if (k == 0) {
            need[0] = (b.sets[0] != d.sets[0] || b.dynamic_count != noff0 ||
                       std::memcmp(b.dynamic_offsets, offsets, noff0 * sizeof(uint32_t)) != 0);
        } else if (k == 1) {
            need[1] = (b.sets[1] != d.sets[1] || b.dynamic_ssbo_count != noff1 ||
                       std::memcmp(b.dynamic_ssbo_offsets, offsets + noff0, noff1 * sizeof(uint32_t)) != 0);
        } else {
            need[k] = (b.sets[k] != d.sets[k]);
        }
        if (!need[k]) g_sets_bind_skipped++;
    }

    g_sets_bind_calls++;
    for (uint32_t k = 0; k < 4;) {
        if (!need[k]) {
            k++;
            continue;
        }
        uint32_t n = 1;
        while (k + n < 4 && need[k + n]) n++;
        uint32_t dyn_count = 0;
        const uint32_t* dyn_ptr = nullptr;
        if (k == 0) {
            dyn_count = (n >= 2) ? (noff0 + noff1) : noff0;
            dyn_ptr = offsets;
        } else if (k == 1) {
            dyn_count = noff1;
            dyn_ptr = offsets + noff0;
        }
        p_vkCmdBindDescriptorSets(cb, point, layout.layout, k, n, d.sets + k, dyn_count, dyn_ptr);
        if (gpu_profile_on()) gpu_profile_count(2);
        for (uint32_t i = 0; i < n; i++) {
            b.sets[k + i] = d.sets[k + i];
            g_sets_bound++;
        }
        if (k == 0) {
            b.dynamic_count = noff0;
            std::memcpy(b.dynamic_offsets, offsets, noff0 * sizeof(uint32_t));
        }
        if (k <= 1 && k + n > 1) {
            b.dynamic_ssbo_count = noff1;
            std::memcpy(b.dynamic_ssbo_offsets, offsets + noff0, noff1 * sizeof(uint32_t));
        }
        k += n;
    }
}

// Texture bindings of a program's stages and their handles, into g_textures. kNeedBreak: one needs a transfer or a
// layout change (so no render pass may be open while the sets are built); kSampledPending: one has a pending clear.
static thread_local const ShaderInfo* s_cached_sh0 = nullptr;
static thread_local const ShaderInfo* s_cached_sh1 = nullptr;
static thread_local size_t s_cached_sh_count = 0;
static thread_local uint32_t s_cached_need_break = 0;
static thread_local bool s_cached_biased = false;
static thread_local uint64_t s_cached_tex_pool = 0;
static thread_local uint64_t s_cached_smp_pool = 0;
static thread_local bool s_cached_has_uniform_handle = false;
static thread_local bool s_cached_all_fast_ready = false;
static thread_local uint64_t s_cached_fast_ready_frame = 0;
static thread_local uint32_t s_cached_tex_version = 0;
static thread_local uint64_t s_cached_pass_serial = UINT64_MAX;
static thread_local uint32_t s_cached_pass_written = UINT32_MAX;
static thread_local uint64_t s_cached_vf_serial = 0;

// Set 2 reuse, cached image views and the fast readiness check (NX_SET2_REUSE=0 disables).
uint32_t gather_textures(const std::vector<const ShaderInfo*>& shaders) {
    DrawTextures& t = g_textures;
    const bool reuse = set2_reuse_on();
    if (draw_slow(kSlowScratch))
        t = DrawTextures{};

    const size_t n_sh = shaders.size();
    const ShaderInfo* sh0 = n_sh > 0 ? shaders[0] : nullptr;
    const ShaderInfo* sh1 = n_sh > 1 ? shaders[1] : nullptr;
    const bool biased = g_state.depth.depth_test;

    const bool shaders_same = (n_sh == s_cached_sh_count && sh0 == s_cached_sh0 && sh1 == s_cached_sh1 && s_cached_sh_count > 0);
    const bool pools_same = (g_state.texture_pool == s_cached_tex_pool && g_state.sampler_pool == s_cached_smp_pool);
    const bool biased_changed = (biased != s_cached_biased);
    const uint64_t pass_ser = current_pass_serial();
    const uint32_t pass_wrt = current_pass_written();
    const uint64_t vf_ser = view_forget_serial();
    const uint64_t cur_frame_id = g_frame.load(std::memory_order_relaxed) + 1;

    if (shaders_same && pools_same && !biased_changed && reuse && !draw_slow(kSlowScratch) &&
        !s_cached_has_uniform_handle && s_cached_all_fast_ready && s_cached_fast_ready_frame == cur_frame_id &&
        pass_open() && g_tex_state_version == s_cached_tex_version &&
        pass_ser == s_cached_pass_serial && pass_wrt == s_cached_pass_written &&
        vf_ser == s_cached_vf_serial) {
        t.set2_unchanged = true;
        return 0;
    }

    s_cached_tex_pool = g_state.texture_pool;
    s_cached_smp_pool = g_state.sampler_pool;
    s_cached_biased = biased;

    if (!shaders_same || draw_slow(kSlowScratch)) {
        struct ShaderTexBindingsSlot {
            const ShaderInfo* sh0 = nullptr;
            const ShaderInfo* sh1 = nullptr;
            size_t n_sh = 0;
            uint32_t need_break = 0;
            bool has_uniform_handle = false;
            std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>> bindings;
        };
        static thread_local ShaderTexBindingsSlot s_sh_cache[64];
        const size_t c_idx = ((reinterpret_cast<uintptr_t>(sh0) >> 4) ^ (reinterpret_cast<uintptr_t>(sh1) >> 3) ^ n_sh) & 63;
        ShaderTexBindingsSlot& cs = s_sh_cache[c_idx];
        if (!draw_slow(kSlowScratch) && cs.n_sh == n_sh && cs.sh0 == sh0 && cs.sh1 == sh1 && n_sh <= 2) {
            t.bindings = cs.bindings;
            s_cached_need_break = cs.need_break;
            s_cached_has_uniform_handle = cs.has_uniform_handle;
        } else {
            t.bindings.clear();
            s_cached_need_break = 0;
            s_cached_has_uniform_handle = false;
            for (const ShaderInfo* s : shaders)
                for (const DescriptorBinding& b : s->refl.bindings) {
                    if (b.set == 2) {
                        t.bindings.push_back({s, &b});
                        if (b.nvn.source == NvnSource::HandleInUniform) s_cached_has_uniform_handle = true;
                    }
                    if (b.set == 3) s_cached_need_break |= kNeedBreak;  // storage images change layout
                }
            if (!draw_slow(kSlowScratch) && n_sh <= 2) {
                cs.sh0 = sh0;
                cs.sh1 = sh1;
                cs.n_sh = n_sh;
                cs.need_break = s_cached_need_break;
                cs.has_uniform_handle = s_cached_has_uniform_handle;
                cs.bindings = t.bindings;
            }
        }
        s_cached_sh0 = sh0;
        s_cached_sh1 = sh1;
        s_cached_sh_count = n_sh;
        t.handles.assign(t.bindings.size(), 0);
        t.resolved.assign(t.bindings.size(), ResolvedTexture{});
    }

    uint32_t out = s_cached_need_break;
    const uint64_t submit_ns = submit_start_ns();
    bool any_changed = !shaders_same || !pools_same || biased_changed;
    bool all_fast_ready = (out == 0);

    for (size_t k = 0; k < t.bindings.size(); k++) {
        auto [s, b] = t.bindings[k];
        int st = api_stage(s->stage);
        uint64_t handle = 0;
        if (b->nvn.source == NvnSource::SamplerBinding && b->nvn.index >= 0 && b->nvn.index < 32)
            handle = g_state.textures[st][b->nvn.index];
        else if (b->nvn.source == NvnSource::HandleInUniform && b->nvn.bank >= 3) {
            const Range& r = g_state.uniforms[st][b->nvn.bank - 3];
            const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
            if (p && uint64_t(b->nvn.word) * 4 + 4 <= r.size) handle = *reinterpret_cast<const uint32_t*>(p + b->nvn.word * 4);
        }

        ResolvedTexture& r = t.resolved[k];
        if (handle != t.handles[k] || !shaders_same || !pools_same || biased_changed) {
            any_changed = true;
            t.handles[k] = handle;
            uint64_t texture = 0;
            Texture* obj = nullptr;
            // A pool entry naming a texture that no longer exists samples the dummy, like no texture: it ended the
            // render pass at every draw (~1.4M times in a run, docs/nvn/DEBUG.md #34).
            if (HandleSlot* slot = handle_slot(handle)) {
                if (slot->object) {
                    texture = slot->texture;
                    obj = slot->object;
                    if (slot->last_sampled_ns != submit_ns) {
                        slot->last_sampled_ns = submit_ns;
                        obj->last_sampled.store(submit_ns, std::memory_order_relaxed);
                    }
                }
                r.sampler = slot->sampler, r.view = slot->view;
                // The same for every draw (a cache key built per binding otherwise, nvn_sampler.cpp). NX_LOD_BIAS applies
                // to the 3D draws only: those that test depth.
                VkSampler& vk = slot->vk_sampler[(b->image.depth ? 1 : 0) + (biased ? 2 : 0)];
                if (!vk) vk = sampler_for(slot->sampler, b->image.depth, biased);
                r.vk_sampler = vk;
                r.texture = texture;
                r.object = obj;
                r.gpu_image = obj ? obj->image.get() : nullptr;
                if (reuse && r.gpu_image && slot->vk_view != VK_NULL_HANDLE &&
                    slot->vk_image == r.gpu_image->image && slot->vk_view_serial == vf_ser &&
                    std::memcmp(&slot->vk_img_binding, &b->image, sizeof(ImageShape)) == 0) {
                    r.vk_view = slot->vk_view;
                    r.vk_image = slot->vk_image;
                    r.vk_layout = r.gpu_image->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    r.vk_view_serial = vf_ser;
                    r.ready_cached = true;
                } else {
                    r.vk_view = VK_NULL_HANDLE;
                    r.ready_cached = false;
                }
            } else {
                lookup_handle(handle, texture, r.sampler, &r.view);
                if (Texture* sampled = texture ? get<Texture>(texture) : nullptr) {
                    obj = sampled;
                    sampled->last_sampled.store(submit_ns, std::memory_order_relaxed);
                } else {
                    texture = 0;
                }
                r.vk_sampler = VK_NULL_HANDLE;
                r.texture = texture;
                r.object = obj;
                r.gpu_image = obj ? obj->image.get() : nullptr;
                r.vk_view = VK_NULL_HANDLE;
                r.ready_cached = false;
            }
        }

        if (r.texture) {
            Texture* tx = reuse ? (r.object ? r.object : get<Texture>(r.texture)) : nullptr;
            GpuImage* img = tx ? tx->image.get() : nullptr;
            auto check_tx_fast_ready = [&](Texture* t_obj, GpuImage* g_img) -> bool {
                if (!t_obj || !g_img) return false;
                if (t_obj->stride && !g_img->rendered && t_obj->hashed_submit != cur_frame_id) return false;
                return !(t_obj->dirty.load(std::memory_order_relaxed) && !g_img->rendered) &&
                       !t_obj->has_writes.load(std::memory_order_relaxed) &&
                       (g_img->rendered ? g_img->layout == VK_IMAGE_LAYOUT_GENERAL
                                        : (g_img->uploaded && g_img->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            };
            bool fast_ready = check_tx_fast_ready(tx, img);
            if (!fast_ready && !sampled_ready(r.texture)) {
                out |= kNeedBreak;
                r.ready_cached = false;
                any_changed = true;
                all_fast_ready = false;
            } else {
                r.ready_cached = true;
                if (!fast_ready) {
                    if (!img && tx) img = tx->image.get();
                    if (!check_tx_fast_ready(tx, img)) all_fast_ready = false;
                }
            }
            if (!img) img = image_of(r.texture);
            r.gpu_image = img;
            // Sampling one of the open pass's targets (a depth buffer read back by the character lighting) needs what
            // was rendered stored first: a fresh pass. SetRenderTargets used to end every pass, which did this by chance
            // (docs/nvn/DEBUG.md #35). Only when a draw of this pass wrote it (pass_wrote, nvn_draw.cpp).
            // These need sampled_view's transition (and pending clear) in build_sets: never the cached view.
            if (pass_wrote(img)) out |= kNeedBreak, any_changed = true, r.ready_cached = false, all_fast_ready = false;
            if (clears_pending_image(img)) out |= kNeedBreak | kSampledPending, any_changed = true, r.ready_cached = false, all_fast_ready = false;
            if (img) {
                VkImageLayout want_layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (r.vk_view == VK_NULL_HANDLE || r.vk_layout != want_layout || r.vk_image != img->image ||
                    r.vk_view_serial != vf_ser) {
                    any_changed = true;
                    all_fast_ready = false;
                    if (r.vk_image != img->image || r.vk_view_serial != vf_ser) r.vk_view = VK_NULL_HANDLE;
                }
            }
        } else {
            if (!dummy_ready(b->image)) out |= kNeedBreak, any_changed = true, all_fast_ready = false;
            r.ready_cached = false;
            all_fast_ready = false;
        }
    }
    if (any_changed) t.serial++;
    t.set2_unchanged = reuse && !any_changed && !(out & (kNeedBreak | kSampledPending));
    s_cached_all_fast_ready = all_fast_ready && t.set2_unchanged;
    s_cached_fast_ready_frame = cur_frame_id;
    s_cached_tex_version = g_tex_state_version;
    s_cached_pass_serial = pass_ser;
    s_cached_pass_written = pass_wrt;
    s_cached_vf_serial = vf_ser;
    return out;
}

// The zero stream (vertex binding 15): one per submit instead of 64 bytes per draw. Host bytes that never change, so
// dedup may share them.
static thread_local Upload s_cached_zero_stream{};
static thread_local bool s_cached_zero_valid = false;

Upload zero_stream() {
    if (__builtin_expect(s_cached_zero_valid, 1)) return s_cached_zero_stream;
    static const uint8_t zeros[64] = {};
    if (!draw_slow(kSlowUploads)) {
        s_cached_zero_stream = dedup_upload(zeros, sizeof zeros, sizeof zeros, 16);
        s_cached_zero_valid = true;
        return s_cached_zero_stream;
    }
    Upload z = upload_alloc(64, 16);
    std::memset(z.ptr, 0, 64);
    return z;
}

struct LastIndexEntry {
    const uint8_t* host_p = nullptr;
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint32_t mask = 0;
    uint64_t gen = 0;
    Upload upload{};
};
constexpr size_t kIndexCacheSlots = 4;
static LastIndexEntry s_index_cache[kIndexCacheSlots];
static size_t s_index_cursor = 0;

void index_cache_invalidate(const uint8_t* p, uint64_t size) {
    if (!p || !size) return;
    const uint8_t* p_end = p + size;
    for (size_t k = 0; k < kIndexCacheSlots; k++) {
        if (s_index_cache[k].host_p &&
            p < s_index_cache[k].host_p + s_index_cache[k].bytes &&
            s_index_cache[k].host_p < p_end) {
            s_index_cache[k].addr = 0;
            s_index_cache[k].host_p = nullptr;
        }
    }
}

void sets_invalidate_range(const uint8_t* p, uint64_t size) {
    if (!p || !size) return;
    const uint8_t* p_end = p + size;
    for (int st = 0; st < 2; st++) {
        for (int i = 0; i < 18; i++) {
            SlotBufferCache& s = s_ubo_cache[st][i];
            if (s.host_p && p < s.host_p + s.copy_size && s.host_p < p_end) {
                s.address = 0;
                s.host_p = nullptr;
            }
        }
        for (int i = 0; i < 16; i++) {
            SlotBufferCache& s = s_ssbo_cache[st][i];
            if (s.host_p && p < s.host_p + s.copy_size && s.host_p < p_end) {
                s.address = 0;
                s.host_p = nullptr;
            }
        }
    }
}

// draw_inputs (nvn_written.cpp) with the indices shared: a mesh drawn in several passes of a submit (depth, shadows,
// colour) had its index buffer copied each time. Guest bytes, like the vertex streams, so dedup's rule holds; 8-bit
// indices are widened per draw as before.
bool draw_inputs_shared(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in) {
    if (!indexed || draw_slow(kSlowUploads) || vk_index_type(c.mask) == VK_INDEX_TYPE_UINT8_EXT)
        return draw_inputs(cb, vs, indexed, c, in);
    draw_inputs(cb, vs, false, c, in);  // the vertex streams
    in.index_type = vk_index_type(c.mask);
    uint64_t bytes = uint64_t(uint32_t(c.i[0])) * index_size(c.mask);
    const uint64_t cur_gen = dedup_generation();

    if (c.a[0]) {
        for (size_t k = 0; k < kIndexCacheSlots; k++) {
            const LastIndexEntry& e = s_index_cache[k];
            if (e.addr == c.a[0] && e.bytes == bytes && e.mask == c.mask && e.gen == cur_gen) {
                in.index = e.upload;
                return true;
            }
        }
    }
    const uint8_t* p = guest_bytes(c.a[0]);
    if (!p) return false;
    g_upload_kind = kUploadIndex;
    if (!written_read(cb, p, bytes, bytes, index_size(c.mask), in.index))
        in.index = dedup_upload(p, bytes, bytes, 16);
    g_upload_kind = kUploadOther;
    s_index_cache[s_index_cursor] = {p, c.a[0], bytes, c.mask, cur_gen, in.index};
    s_index_cursor = (s_index_cursor + 1) & (kIndexCacheSlots - 1);
    return true;
}

void sets_begin_submit() {
    s_cached_zero_valid = false;
    s_cached_sh_count = 0;
    s_cached_sh0 = nullptr;
    s_cached_sh1 = nullptr;
    s_cached_need_break = 0;
    s_cached_tex_pool = 0;
    s_cached_smp_pool = 0;
    s_cached_all_fast_ready = false;
    s_cached_fast_ready_frame = 0;
    s_cached_pass_serial = UINT64_MAX;
    s_cached_pass_written = UINT32_MAX;
    s_last_set2 = VK_NULL_HANDLE;
    s_last_set2_layout = VK_NULL_HANDLE;
    s_last_set2_gen = 0;
    for (int k = 0; k < 4; k++) s_last_resolved[k].gen = 0, s_last_resolved[k].set = VK_NULL_HANDLE;
    s_last_const_block[0] = s_last_const_block[1] = nullptr;
    std::memset(s_ubo_cache, 0, sizeof s_ubo_cache);
    std::memset(s_ssbo_cache, 0, sizeof s_ssbo_cache);
    s_support_cache.gen = 0;
    s_const_data_cache[0].gen = s_const_data_cache[1].gen = 0;
    uint64_t mark = t_replay_mark, epoch = current_epoch();
    if (!mark || mark != g_handle_mark || epoch != g_handle_epoch || g_handle_used > kHandleSlots / 2) {
        handles_reset();
        g_handle_mark = mark;
        g_handle_epoch = epoch;
    }
    sets_invalidate_bound();
    static uint64_t s_last_vf_serial = 0;
    static uint64_t s_last_desc_epoch = 0;
    static uint64_t s_last_ssbo_epoch = 0;
    const uint64_t vf_ser = view_forget_serial();
    const bool force_rotate = (vf_ser != s_last_vf_serial) || (g_set_used >= kSetSlots / 2);
    const uint64_t desc_epoch = frame_descriptor_epoch(force_rotate);
    if (desc_epoch != s_last_desc_epoch) {
        if (g_set_arena.capacity() < 65536) g_set_arena.reserve(65536);
        g_set_generation++, g_set_used = 0, g_set_arena.clear();
        s_last_desc_epoch = desc_epoch;
        s_last_vf_serial = vf_ser;
    }
    const bool force_ssbo_rotate = (g_set1_used >= kSet1Slots / 2);
    const uint64_t ssbo_epoch = frame_ssbo_descriptor_epoch(force_ssbo_rotate);
    if (ssbo_epoch != s_last_ssbo_epoch) {
        if (g_set1_arena.capacity() < 32768) g_set1_arena.reserve(32768);
        g_set1_generation++, g_set1_used = 0, g_set1_arena.clear();
        s_last_ssbo_epoch = ssbo_epoch;
    }
    g_support_used = g_constants_used = 0;
}

void sets_log() {
    if (g_handle_hits + g_handle_misses + g_sets_skipped)
        log("nvn: texture handles: %llu from the submit's cache, %llu looked up; %llu empty descriptor sets skipped",
            (unsigned long long)g_handle_hits, (unsigned long long)g_handle_misses, (unsigned long long)g_sets_skipped);
    if (g_set_hits + g_set_misses)
        log("nvn: descriptor sets: %llu reused from the submit's cache, %llu made", (unsigned long long)g_set_hits,
            (unsigned long long)g_set_misses);
    if (g_sets_bound + g_sets_bind_skipped)
        log("nvn: descriptor set binds: %llu calls, %llu sets bound, %llu redundant binds skipped",
            (unsigned long long)g_sets_bind_calls, (unsigned long long)g_sets_bound,
            (unsigned long long)g_sets_bind_skipped);
    g_handle_hits = g_handle_misses = g_sets_skipped = g_set_hits = g_set_misses = 0;
    g_sets_bind_calls = g_sets_bound = g_sets_bind_skipped = 0;
}

}  // namespace nx::nvn::replay
