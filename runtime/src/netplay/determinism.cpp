// Deterministic mode for lockstep netplay: see determinism.h.
#include "determinism.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx::det {

namespace {

bool env_on(const char* name) {
    const char* e = std::getenv(name);
    return e && e[0] == '1';
}

std::atomic<bool> g_session{false};
uint64_t g_session_seed = 0;

}  // namespace

bool enabled() {
    static const bool on = env_on("NX_DETERMINISTIC");
    return on || g_session.load(std::memory_order_relaxed);
}

bool requested() {
    return env_on("NX_DETERMINISTIC") || !plat::setting("net_host").empty() || !plat::setting("net_join").empty();
}

void start_session(uint64_t seed) {
    g_session_seed = seed;
    g_session = true;
    log("det: deterministic mode on for the netplay session (seed %llu)", (unsigned long long)seed);
}

bool tracing() {
    static const bool on = env_on("NX_DET_TRACE");
    return on;
}

namespace {

// The census: calls per (source, thread, caller chain), reported every 600 frames.
struct Site {
    const char* source;
    uint64_t thread;
    std::array<uint64_t, 4> chain;
    bool operator<(const Site& o) const {
        return std::tie(source, thread, chain) < std::tie(o.source, o.thread, o.chain);
    }
};
std::mutex g_census_m;
std::map<Site, uint64_t> g_census;

void census_report() {
    std::lock_guard lk(g_census_m);
    for (auto& [s, n] : g_census) {
        if (!n) continue;
        std::string chain;
        for (uint64_t a : s.chain)
            if (a) chain += " < " + describe(a);
        log("det: %llu x %s, thread %#llx%s", (unsigned long long)n, s.source, (unsigned long long)s.thread,
            chain.c_str());
        n = 0;
    }
}

}  // namespace

void note(const char* source, CPU& c) {
    Site s{source, 0, {c.x[30], 0, 0, 0}};
    if (KThread* t = self_thread()) s.thread = t->id;
    uint64_t fp = c.x[29];
    for (int k = 1; k < 4 && fp && (fp & 7) == 0 && vm_query(fp).state != kMemFree; k++) {
        s.chain[size_t(k)] = ld<uint64_t>(fp + 8);
        fp = ld<uint64_t>(fp);
    }
    std::lock_guard lk(g_census_m);
    auto [it, fresh] = g_census.try_emplace(s, 0);
    it->second++;
    if (fresh && g_census.size() <= 512) {
        std::string chain;
        for (uint64_t a : s.chain)
            if (a) chain += " < " + describe(a);
        log("det: first %s, thread %#llx, frame %llu%s", source, (unsigned long long)s.thread,
            (unsigned long long)g_frame.load(), chain.c_str());
    }
}

namespace {
// The session's seed (NX_DET_SEED, 0 by default): both peers must use the same one. It moves the boot clock (so the
// game's own generators get another seed) and seeds GenerateRandomBytes.
uint64_t session_seed() {
    if (g_session.load()) return g_session_seed;  // the netplay session's (both peers the same)
    static const uint64_t seed = [] {
        const char* e = std::getenv("NX_DET_SEED");
        return e ? std::strtoull(e, nullptr, 0) : 0ull;
    }();
    return seed;
}
}  // namespace

// ---- the clock -----------------------------------------------------------------------------------------------
// P4AU reads the clock on its main thread through one platform function (main+3831c, microseconds since start-up):
// the frame controller (main+3849c) every frame, and at boot the seeds of two of its Mersenne Twisters (main+e20b8,
// main+e20e0: that clock in milliseconds, so a different seed in every run). In deterministic mode the main thread's
// clock is 1/60 s per presented frame, one tick more per read within a frame (strictly increasing, so a guest loop
// waiting on it still finishes), counted from 10 s of uptime plus the session seed modulo 1000 in milliseconds.
// Other threads (audio statistics, the sdk's timed waits) keep the host clock: what they measure does not reach the
// game's logic, and a virtual clock there could stretch a timeout.

namespace {
std::atomic<KThread*> g_main{nullptr};
uint64_t g_virtual = 0;  // the main thread's last reading (only the main thread touches it)
uint64_t g_reads = 0;    // its reads in the current frame
}  // namespace

void set_main_thread(KThread* t) { g_main = t; }

uint64_t system_tick(uint64_t host_tick) {
    if (!enabled() || !g_main || self_thread() != g_main.load(std::memory_order_relaxed)) return host_tick;
    constexpr uint64_t kPerFrame = 19'200'000 / 60;
    static const uint64_t base = 19'200'000ull * 10 + (session_seed() % 1000) * 19'200;
    g_virtual = std::max(g_virtual + 1, base + g_frame.load() * kPerFrame);
    if (++g_reads == 100'000)
        log("det: the main thread read the clock 100000 times in frame %llu (polling it?)",
            (unsigned long long)g_frame.load());
    return g_virtual;
}

// ---- loads ---------------------------------------------------------------------------------------------------
// The game loads asynchronously: the main thread queues files and a loader thread (0x55 in P4AU) opens and reads
// them while frames go on, so the frame on which a load is seen finished depends on the host's speed (on the PC a
// battle load spreads over 6 frames, 2287-2293). In deterministic mode every guest thread other than the main one
// that opens files counts as a loader, and each present waits until all loaders are idle (blocked with nothing
// pending, or gone: det::thread_idle in kernel.cpp): work queued during a frame is finished before the next frame
// begins, on every device. A loader that stays busy for NX_DET_LOAD_WAIT_MS (5000) is logged and passed.

namespace {
std::mutex g_loaders_m;
std::vector<uint64_t> g_loaders;  // guest thread ids
thread_local bool t_loader = false;  // this thread is in g_loaders (or is the main thread): the fast path

uint64_t env_ms(const char* name, uint64_t ms) {
    const char* e = std::getenv(name);
    return (e ? std::strtoull(e, nullptr, 10) : ms) * 1'000'000ull;
}

// Waits until every thread in `ids` is idle, for at most limit_ns; whether they all were, and the time waited.
std::pair<bool, uint64_t> wait_idle(const std::vector<uint64_t>& ids, uint64_t limit_ns) {
    const uint64_t t0 = plat::monotonic_ns();
    bool all = false;
    for (int spin = 0;; spin++) {
        all = true;
        for (uint64_t id : ids) all = all && thread_idle(id);
        if (all || plat::monotonic_ns() - t0 > limit_ns) break;
        if (spin < 64) plat::yield();
        else plat::sleep_ns(100'000);
    }
    return {all, plat::monotonic_ns() - t0};
}
}  // namespace

void file_io() {
    if (!enabled() || t_loader) return;
    KThread* t = self_thread();
    if (!t) return;
    t_loader = true;
    if (t == g_main.load()) return;
    std::lock_guard lk(g_loaders_m);
    if (std::find(g_loaders.begin(), g_loaders.end(), t->id) == g_loaders.end()) {
        g_loaders.push_back(t->id);
        log("det: thread %#llx does file I/O: a loader (frame %llu)", (unsigned long long)t->id,
            (unsigned long long)g_frame.load());
    }
}

// Without this the save workers' first open fell on frame 11 in one run and 13 in another (and 21 / 25 for the
// second save): the barrier at the present only knows a thread once it has opened a file.
void thread_started(uint64_t id) {
    KThread* self = self_thread();
    if (tracing())
        log("det: thread %#llx started by thread %#llx, frame %llu", (unsigned long long)id,
            (unsigned long long)(self ? self->id : 0), (unsigned long long)g_frame.load());
    if (!enabled() || !g_main || self != g_main.load()) return;
    static const uint64_t limit_ns = env_ms("NX_DET_START_WAIT_MS", 200);
    static int timeouts = 0;
    auto [all, dt] = wait_idle({id}, limit_ns);
    if (!all && timeouts++ < 20)
        log("det: frame %llu: thread %#llx still running %llu ms after it started, going on",
            (unsigned long long)g_frame.load(), (unsigned long long)id, (unsigned long long)(dt / 1'000'000));
}

namespace {

void wait_loaders() {
    std::vector<uint64_t> ids;
    {
        std::lock_guard lk(g_loaders_m);
        ids = g_loaders;
    }
    if (ids.empty()) return;
    static const uint64_t limit_ns = env_ms("NX_DET_LOAD_WAIT_MS", 5000);
    static uint64_t waits = 0, waited_ns = 0, timeouts = 0;
    auto [all, dt] = wait_idle(ids, limit_ns);
    if (!all && timeouts++ < 20)
        log("det: frame %llu: a loader still busy after %llu ms, going on", (unsigned long long)g_frame.load(),
            (unsigned long long)(dt / 1'000'000));
    if (dt > 50'000) waits++, waited_ns += dt;
    if (g_frame.load() % 600 == 0 && waits) {
        log("det: last 600 frames: %llu waited for loaders, %.1f ms in all", (unsigned long long)waits,
            double(waited_ns) / 1e6);
        waits = waited_ns = 0;
    }
}

}  // namespace

void on_present() {
    if (enabled()) {
        wait_loaders();
        g_reads = 0;
    }
    if (tracing() && g_frame.load() % 600 == 0) census_report();
    // NX_DET_SNAP="<frame>,<frame>,...": the main module's image (data and bss included) to <data root>/snap_<frame>.bin
    // at those frames, to find the battle state by comparing them (a debugging aid, off by default).
    static const std::vector<uint64_t> snaps = [] {
        std::vector<uint64_t> v;
        if (const char* e = std::getenv("NX_DET_SNAP"))
            for (const char* p = e; *p;) {
                char* end = nullptr;
                v.push_back(std::strtoull(p, &end, 10));
                if (end == p) break;
                p = *end ? end + 1 : end;
            }
        return v;
    }();
    if (!snaps.empty()) {
        uint64_t f = g_frame.load();
        for (uint64_t s : snaps)
            if (s == f) {
                const LoadedModule* m = module_at(find_symbol("nnMain"));
                std::string path = plat::data_root() + "/snap_" + std::to_string(f) + ".bin";
                if (FILE* out = m ? plat::fopen_utf8(path, "wb") : nullptr) {
                    std::fwrite(reinterpret_cast<const void*>(m->base), 1, size_t(m->size), out);
                    std::fclose(out);
                    log("det: frame %llu: main (%llu bytes) written to %s", (unsigned long long)f,
                        (unsigned long long)m->size, path.c_str());
                }
            }
    }
    static const uint64_t hash_every = [] {
        const char* e = std::getenv("NX_DET_HASH_EVERY");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    if (hash_every && g_frame.load() % hash_every == 0) {
        // The hash, then the first 16 words (for P4AU the round clock and both characters' health and position).
        uint32_t w[netplay::kMaxStateWords] = {};
        int n = netplay::state_words(w, netplay::kMaxStateWords);
        log("det: frame %llu state %016llx (%u %u %u %u) %d %d %d %d %d %d | %d %d %d %d %d %d",
            (unsigned long long)g_frame.load(), (unsigned long long)netplay::state_hash(), w[0], w[1], w[2], w[3],
            int32_t(w[4]), int32_t(w[5]), int32_t(w[6]), int32_t(w[7]), int32_t(w[8]), int32_t(w[9]),
            int32_t(w[10]), int32_t(w[11]), int32_t(w[12]), int32_t(w[13]), int32_t(w[14]), int32_t(w[15]));
        static const bool words = env_on("NX_DET_HASH_WORDS");
        if (words && n > 0) {
            std::string s;
            char hex[12];
            for (int k = 0; k < n; k++) {
                std::snprintf(hex, sizeof hex, " %x", w[k]);
                s += hex;
            }
            log("det: frame %llu words %d:%s", (unsigned long long)g_frame.load(), n, s.c_str());
        }
    }
}

namespace {

// nn::os::GenerateRandomBytes(void*, size_t): the sdk's own generator, seeded once from svcGetInfo(RandomEntropy)
// (a constant here, svc_process.cpp). Hooked to see who asks; in deterministic mode a stream seeded per session.
void GenerateRandomBytes(CPU& c) {
    if (tracing()) note("os::GenerateRandomBytes", c);
    static const uint64_t real = find_symbol("_ZN2nn2os19GenerateRandomBytesEPvm");
    if (!enabled() && real) {
        call(c, real);
        return;
    }
    static std::mutex m;
    static uint64_t state = session_seed();
    std::lock_guard lk(m);
    auto* out = reinterpret_cast<uint8_t*>(c.x[0]);
    for (uint64_t k = 0; k < c.x[1]; k++) {
        if (k % 8 == 0) state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;  // splitmix64
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        out[k] = uint8_t((z ^ (z >> 31)) >> (8 * (k % 8)));
    }
}

// nn::time::StandardUserSystemClock::GetCurrentTime(PosixTime*): the calendar clock. In deterministic mode a
// fixed date (NX_DET_TIME, seconds since 1970; 2026-01-01 by default) plus the presented frames / 60.
void GetCurrentTime(CPU& c) {
    if (tracing()) note("time::StandardUserSystemClock::GetCurrentTime", c);
    static const uint64_t real = find_symbol("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    if (!enabled() && real) {
        call(c, real);
        return;
    }
    static const int64_t base = [] {
        const char* e = std::getenv("NX_DET_TIME");
        return e ? std::strtoll(e, nullptr, 0) : 1767225600ll;
    }();
    st<int64_t>(c.x[0], base + int64_t(g_frame.load() / 60));
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn2os19GenerateRandomBytesEPvm", GenerateRandomBytes);
NX_HLE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", GetCurrentTime);

}  // namespace nx::det

namespace nx::netplay {

namespace {
std::atomic<StateReader> g_reader{nullptr};
}

void set_state_reader(StateReader r) { g_reader = r; }

int state_words(uint32_t* words, int max) {
    StateReader r = g_reader.load();
    return r ? r(words, max) : 0;
}

uint64_t state_hash() {
    uint32_t w[kMaxStateWords];
    int n = state_words(w, kMaxStateWords);
    if (n <= 0) return 0;
    uint64_t h = 0xCBF29CE484222325ull;  // FNV-1a over the words' bytes
    for (int k = 0; k < n; k++)
        for (int b = 0; b < 4; b++) h = (h ^ ((w[k] >> (8 * b)) & 0xFF)) * 0x100000001B3ull;
    return h;
}

}  // namespace nx::netplay
