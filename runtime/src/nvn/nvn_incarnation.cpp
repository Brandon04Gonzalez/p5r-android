// Texture incarnations (nvn.h, docs/nvn/DEBUG.md #55). A submit is replayed on the thread that submits it (or the
// replay thread) while the game's other threads keep calling nvnTextureInitialize / nvnTextureFinalize: Wonder
// re-initializes the structs of its dynamic-resolution targets every frame with new sizes and memory. Looked up by
// guest address, one texture could then change in the middle of a replay: a pass rendered into the old image and a
// later draw sampled the new one (fresh, stale), or sizes no longer matched (the intermittent horizon band).
//
// Every change of a texture's registry entry records the object it retires, with an epoch, under objects_lock. A
// replay takes a mark (the epoch) when it begins, and its lookups return the object that was current at the mark: the
// one retired by the first change after it, else the current one. Retired objects are deferred objects, freed by
// drain_objects under the GPU lock (no replay then), which forgets the records at the same time.
//
// Thread roles: game threads (HLE procs: Initialize/Finalize, getters, pool registration, recording) never set a mark
// and see the current object; only gpu_submit sets one, for its own thread. Lock order is unchanged: the GPU lock,
// then objects_lock; nothing here waits for the GPU lock.
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include "nvn.h"

namespace nx::nvn {

namespace {

std::atomic<uint64_t> g_epoch{1};  // bumped under objects_lock (alone) by each recorded change
// guest -> (epoch of the change, object it retired; null: there was none), in epoch order. Under objects_lock.
std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, Object*>>> g_retired;
std::atomic<uint64_t> g_resolved_old{0};  // replay lookups answered with a retired incarnation

}  // namespace

bool texture_race_old() {
    static const bool old = [] {
        const char* e = std::getenv("NX_TEXTURE_RACE_OLD");
        return e && e[0] == '1';
    }();
    return old;
}

bool mark_empty_old() {
    static const bool on = std::getenv("NX_MARK_EMPTY_OLD") != nullptr;
    return on;
}

void note_replaced(uint64_t guest) {
    if (texture_race_old()) return;
    auto it = objects().find(guest);
    Object* current = it == objects().end() ? nullptr : it->second.get();
    g_retired[guest].push_back({g_epoch.fetch_add(1, std::memory_order_acq_rel) + 1, current});
}

Object* find_object(uint64_t guest, uint64_t mark) {
    auto it = objects().find(guest);
    Object* current = it == objects().end() ? nullptr : it->second.get();
    if (!mark || g_retired.empty()) return current;
    auto h = g_retired.find(guest);
    if (h == g_retired.end()) return current;
    for (auto& [epoch, object] : h->second)
        if (epoch > mark) {  // the first change after the mark retired what the replay must see
            // Created after the mark (null then) and before the replay: the object the lists mean (DEBUG.md #64).
            if (!object && !mark_empty_old()) return current;
            if (object != current) {
                uint64_t n = g_resolved_old.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n == 1 || n == 100 || n % 10000 == 0)
                    log("nvn: %llu replay lookups resolved a texture re-initialized or finalized mid-replay (%llx)",
                        (unsigned long long)n, (unsigned long long)guest);
            }
            return object;
        }
    return current;
}

namespace {
// (pool, id) -> (epoch of the change, the entry before it), in epoch order. Under objects_lock.
std::map<std::pair<uint64_t, uint32_t>, std::vector<std::pair<uint64_t, PoolEntry>>> g_pool_changes;
std::atomic<uint64_t> g_pool_old{0};
}  // namespace

void note_pool_change(uint64_t pool, uint32_t id, const PoolEntry& before) {
    if (texture_race_old()) return;
    g_pool_changes[{pool, id}].push_back({g_epoch.fetch_add(1, std::memory_order_acq_rel) + 1, before});
}

bool pool_entry_at(uint64_t pool, uint32_t id, uint64_t mark, PoolEntry& out) {
    if (!mark || g_pool_changes.empty()) return false;
    auto h = g_pool_changes.find({pool, id});
    if (h == g_pool_changes.end()) return false;
    for (auto& [epoch, entry] : h->second)
        if (epoch > mark) {
            out = entry;
            uint64_t n = g_pool_old.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || n % 100000 == 0)
                log("nvn: %llu replay lookups resolved a pool slot registered again mid-replay", (unsigned long long)n);
            return true;
        }
    return false;
}

namespace {
std::map<std::pair<uint64_t, uint32_t>, std::vector<std::pair<uint64_t, SamplerSlot>>> g_sampler_changes;
}  // namespace

void note_sampler_change(uint64_t pool, uint32_t id, const SamplerSlot& before) {
    if (texture_race_old()) return;
    g_sampler_changes[{pool, id}].push_back({g_epoch.fetch_add(1, std::memory_order_acq_rel) + 1, before});
}

bool sampler_entry_at(uint64_t pool, uint32_t id, uint64_t mark, SamplerSlot& out) {
    if (!mark || g_sampler_changes.empty()) return false;
    auto h = g_sampler_changes.find({pool, id});
    if (h == g_sampler_changes.end()) return false;
    for (auto& [epoch, entry] : h->second)
        if (epoch > mark) return out = entry, true;
    return false;
}

void purge_incarnations(uint64_t upto) {
    auto trim = [upto](auto& map) {
        for (auto it = map.begin(); it != map.end();) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [upto](auto& r) { return r.first <= upto; }), v.end());
            it = v.empty() ? map.erase(it) : std::next(it);
        }
    };
    trim(g_retired), trim(g_pool_changes), trim(g_sampler_changes);
}

namespace {
std::atomic<uint64_t> g_floor{UINT64_MAX};  // the last replay's mark (everything, until a replay sets one)
}
uint64_t current_epoch() { return g_epoch.load(std::memory_order_acquire); }
uint64_t replay_floor() { return g_floor.load(std::memory_order_acquire); }

// The mark is the epoch at which the submit's command lists ended recording, not the submit's start: the game's
// recording thread runs ahead of the submit thread (by more than a frame when our replay is slower than 60 fps), so by
// the time a submit starts, the next frames' textures and pool slots are already there (DEBUG.md #57).
// NX_MARK_AT_SUBMIT=1: the mark at the submit's start (#55's).
ReplayScope::ReplayScope(uint64_t lists_epoch) {
    if (texture_race_old()) {
        g_floor = UINT64_MAX;
        drain_objects();
        return;
    }
    static const bool at_submit = std::getenv("NX_MARK_AT_SUBMIT") != nullptr;
    uint64_t now;
    {
        std::shared_lock lk(objects_lock());  // orders the mark against the recorded changes (made under the lock alone)
        now = g_epoch.load(std::memory_order_acquire);
    }
    uint64_t mark = lists_epoch && !at_submit ? std::min(lists_epoch, now) : now;
    uint64_t floor = g_floor.load();
    if (floor != UINT64_MAX && mark < floor) mark = floor;  // never back: older records are gone
    g_floor = mark;
    drain_objects();
    t_replay_mark = mark;
    static std::atomic<uint64_t> serial{0};
    t_replay_serial = ++serial;
}

ReplayScope::~ReplayScope() { t_replay_mark = 0, t_replay_serial = 0; }

bool resolve_cache_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_RESOLVE_CACHE");
        return !(e && e[0] == '0');
    }();
    return on;
}

}  // namespace nx::nvn
