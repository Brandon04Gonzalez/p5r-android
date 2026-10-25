// A VkPipelineCache kept on disk (<data root>/pipelines_<vendor>_<device>.bin), and the cost of what the replay
// creates on the fly. Pipelines are compiled the first time a draw needs them, inside the replay: the course's
// stutters when new objects come in (16-42 ms replays on the RTX 3050). The driver checks the cache header itself
// (vendor, device, driver build): a stale file is ignored. Saved every 600 presents when it grew.
// NX_PIPELINE_CACHE=0: no cache (bisecting, or to measure first-run stutter).
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "nvn_pipeline.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_tried = false;
size_t g_saved_size = 0;
std::string g_path;
uint64_t g_alias = 0, g_modules = 0, g_module_ns = 0, g_astc = 0, g_astc_ns = 0;
std::atomic<uint64_t> g_pipelines{0}, g_pipeline_ns{0}, g_worker_pipelines{0}, g_worker_ns{0};  // also counted from the compile worker threads
uint64_t g_waited = 0, g_waited_ns = 0, g_skipped = 0, g_prewarmed = 0;
std::unordered_set<uint64_t> g_known;  // hashes of the records in the .keys file or already queued to it
std::vector<WarmRec> g_new_recs;
constexpr uint32_t kKeysMagic = 0x314b5750;  // "PWK1"

}  // namespace

VkPipelineCache pipeline_cache() {
    if (g_tried) return g_cache;
    g_tried = true;
    if (const char* e = std::getenv("NX_PIPELINE_CACHE"); e && e[0] == '0') return VK_NULL_HANDLE;
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.bin", p.vendorID, p.deviceID);
    g_path = plat::data_root() + name;
    std::vector<uint8_t> data;
    if (FILE* f = plat::fopen_utf8(g_path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) data.resize(size_t(n)), data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;  // a cache the driver refuses: start empty
        p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache);
    }
    g_saved_size = data.size();
    log("nvn/vk: pipeline cache %s (%zu bytes loaded)", g_path.c_str(), data.size());
    return g_cache;
}

void pipeline_cache_save() {
    if (!g_cache) return;
    size_t n = 0;
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, nullptr) != VK_SUCCESS || n <= g_saved_size) return;
    std::vector<uint8_t> data(n);
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, data.data()) != VK_SUCCESS) return;
    std::string tmp = g_path + ".tmp";
    if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
        bool ok = std::fwrite(data.data(), 1, n, f) == n;
        ok = std::fclose(f) == 0 && ok;
        std::remove(g_path.c_str());
        if (ok && std::rename(tmp.c_str(), g_path.c_str()) == 0) g_saved_size = n;
    }
}

void note_pipeline_created(uint64_t ns, bool worker) {
    (worker ? g_worker_pipelines : g_pipelines)++, (worker ? g_worker_ns : g_pipeline_ns) += ns;
}
void note_pipeline_waited(uint64_t ns) { g_waited++, g_waited_ns += ns; }
void note_pipeline_skipped() { g_skipped++; }
void note_pipeline_prewarmed() { g_prewarmed++; }

// ---- the keys of earlier sessions (<cache path minus .bin>.keys) ---------------------------------------------------
// Header {magic, sizeof(WarmRec)} then WarmRec records. A file of another layout is ignored (and rewritten).

static std::string keys_path() {
    pipeline_cache();  // sets g_path
    return (g_path.size() > 4 ? g_path.substr(0, g_path.size() - 4) : g_path) + ".keys";
}

static uint64_t rec_hash(const WarmRec& r) { return hash_bytes(reinterpret_cast<const unsigned char*>(&r), sizeof r); }

bool warm_load(std::vector<WarmRec>& out) {
    if (!pipeline_cache()) return false;
    FILE* f = plat::fopen_utf8(keys_path(), "rb");
    if (!f) return false;
    uint32_t head[2] = {};
    bool ok = std::fread(head, 4, 2, f) == 2 && head[0] == kKeysMagic && head[1] == sizeof(WarmRec);
    WarmRec r;
    while (ok && std::fread(&r, sizeof r, 1, f) == 1) {
        out.push_back(r);
        g_known.insert(rec_hash(r));
    }
    std::fclose(f);
    return ok;
}

void warm_record(const PipelineKey& k) {
    WarmRec r;
    std::memset(&r, 0, sizeof r);
    if (!k.vs || k.vs->hash.size() >= sizeof r.vs || (k.fs && k.fs->hash.size() >= sizeof r.fs)) return;
    std::memcpy(r.vs, k.vs->hash.c_str(), k.vs->hash.size());
    if (k.fs) std::memcpy(r.fs, k.fs->hash.c_str(), k.fs->hash.size());
    r.key = k;
    r.key.vs = nullptr, r.key.fs = nullptr;
    if (g_known.size() < 16384 && g_known.insert(rec_hash(r)).second) g_new_recs.push_back(r);  // ~700 bytes each
}

static void warm_save() {
    if (g_new_recs.empty() || !g_cache) return;
    std::string path = keys_path();
    bool fresh = true;
    if (FILE* t = plat::fopen_utf8(path, "rb")) {
        uint32_t head[2] = {};
        fresh = !(std::fread(head, 4, 2, t) == 2 && head[0] == kKeysMagic && head[1] == sizeof(WarmRec));
        std::fclose(t);
    }
    if (FILE* f = plat::fopen_utf8(path, fresh ? "wb" : "ab")) {
        if (fresh) {
            uint32_t head[2] = {kKeysMagic, uint32_t(sizeof(WarmRec))};
            std::fwrite(head, 4, 2, f);
        }
        std::fwrite(g_new_recs.data(), sizeof(WarmRec), g_new_recs.size(), f);
        std::fclose(f);
        g_new_recs.clear();
    }
}
void note_pipeline_alias() { g_alias++; }
void note_module_created(uint64_t ns) { g_modules++, g_module_ns += ns; }
void note_astc_decoded(uint64_t ns) { g_astc++, g_astc_ns += ns; }

// One line per timing window (nvn_vk.cpp log_timing): what the replay created, and the time it took.
void creation_log(uint64_t frames) {
    if (g_pipelines || g_astc)
        log("nvn/vk: created over %llu frames: %llu pipelines (%.1f ms), %llu ASTC textures decoded (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_pipelines.load(), double(g_pipeline_ns.load()) / 1e6,
            (unsigned long long)g_astc, double(g_astc_ns) / 1e6);
    if (g_alias || g_modules)
        log("nvn/vk: over %llu frames: %llu raw keys shared an existing pipeline, %llu shader modules (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_alias, (unsigned long long)g_modules, double(g_module_ns) / 1e6);
    g_alias = g_modules = g_module_ns = 0;
    if (g_prewarmed || g_skipped || g_waited || g_worker_pipelines)
        log("nvn/vk: over %llu frames: %llu pipelines sent to workers, %llu compiled there (%.1f ms), the replay waited for %llu (%.1f ms), %llu draws skipped",
            (unsigned long long)frames, (unsigned long long)g_prewarmed, (unsigned long long)g_worker_pipelines.load(),
            double(g_worker_ns.load()) / 1e6, (unsigned long long)g_waited, double(g_waited_ns) / 1e6,
            (unsigned long long)g_skipped);
    g_worker_pipelines = 0, g_worker_ns = 0;
    g_prewarmed = g_waited = g_waited_ns = g_skipped = 0;
    g_pipelines = 0, g_pipeline_ns = 0, g_astc = g_astc_ns = 0;
    static uint64_t windows = 0;
    if (++windows % 10 == 0) pipeline_cache_save(), warm_save();
}

}  // namespace nx::nvn
