// Kernel diagnostics: the hang report (no frame for 5 s, or a dump_threads file in the data folder), the host
// profiler request (a profile file), the stall sampler (NX_THREAD_DUMP_AT, stalls over 100 ms) and guest backtraces.
// Split from kernel.cpp (700-line rule); the state it reads is kernel_state.h.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kernel_state.h"
#include "platform.h"

namespace nx {

using namespace kstate;

static uint32_t load32(uint64_t a) { return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST); }
// Guest call stack from the AArch64 frame records (x29 -> [prev x29, lr]).
void backtrace(CPU& c) {
    log("  at lr %s", describe(c.x[30]).c_str());
    uint64_t fp = c.x[29];
    for (int k = 0; k < 24 && fp && (fp & 7) == 0; k++) {
        const VmInfo v = vm_query(fp);
        if (v.state == kMemFree) break;
        uint64_t lr = ld<uint64_t>(fp + 8);
        if (!lr) break;
        log("  #%-2d %s  (fp %llx)", k, describe(lr).c_str(), (unsigned long long)fp);
        fp = ld<uint64_t>(fp);
    }
}


// ---- hang report: when frames stop for a while, log what every guest thread is doing --------------------------

static void report_threads() {
    std::vector<std::shared_ptr<KThread>> all;
    {
        std::lock_guard al(g_all_lock);
        for (auto& w : g_all)
            if (auto t = w.lock()) all.push_back(t);
    }
    // The wait lists are read under the kernel lock when it can be had; a thread stuck holding it is itself news.
    std::unique_lock lk(g_k, std::defer_lock);
    for (int k = 0; k < 100 && !lk.try_lock(); k++) plat::sleep_ns(10'000'000);
    if (!lk.owns_lock()) log("hang: kernel lock held by someone, reading without it");
    auto in = [](const std::vector<KThread*>& v, KThread* t) { return std::find(v.begin(), v.end(), t) != v.end(); };
    for (auto& t : all) {
        if (!t->started || t->exited) continue;
        const char* state = "running (guest code or a host call)";
        uint64_t on = 0;
        if (lk.owns_lock() && in(g_mutex_waiters, t.get())) {
            state = "waits for mutex";
            on = t->wait_mutex;
        } else if (lk.owns_lock() && in(g_cv_waiters, t.get())) {
            state = "waits on condvar";
            on = t->wait_key;
        } else if (lk.owns_lock() && in(g_addr_waiters, t.get())) {
            state = "waits on address";
            on = t->wait_addr;
        }
        log("hang: thread %llx (handle %x, entry %s): %s %llx", (unsigned long long)t->id, t->handle,
            describe(t->entry).c_str(), state, (unsigned long long)on);
        if (on && std::strcmp(state, "waits for mutex") == 0)
            log("  mutex word %08x (owner handle %x)", load32(on), load32(on) & ~kHandleWaitMask);
        backtrace(t->ctx);
    }
}

static void watchdog(void*) {
    uint64_t last = 0, since = plat::monotonic_ns();
    bool reported = false;
    for (;;) {
        plat::sleep_ns(1'000'000'000);
        // On request: a file named dump_threads in the data folder (e.g. adb shell touch .../dump_threads). Frames
        // can keep coming while the game's logic is stuck (the render thread is separate).
        std::string req = plat::data_root() + "/dump_threads";
        if (FILE* f = plat::kDiagnostics ? plat::fopen_utf8(req, "rb") : nullptr) {
            std::fclose(f);
            std::remove(req.c_str());
            log("hang: thread dump requested");
            report_threads();
        }
        // A file named profile (optionally holding a number of seconds): sample every guest thread's host pc into
        // profile.txt next to it.
        req = plat::data_root() + "/profile";
        if (FILE* f = plat::kDiagnostics ? plat::fopen_utf8(req, "rb") : nullptr) {
            int seconds = 10;
            if (std::fscanf(f, "%d", &seconds) != 1 || seconds <= 0) seconds = 10;
            std::fclose(f);
            std::remove(req.c_str());
            std::vector<int64_t> tids;
            std::vector<const uint64_t*> lrs;
            std::vector<std::shared_ptr<KThread>> keep;  // alive while sampled
            {
                std::lock_guard al(g_all_lock);
                for (auto& w : g_all)
                    if (auto t = w.lock(); t && t->host_tid && !t->exited) {
                        tids.push_back(t->host_tid);
                        lrs.push_back(&t->ctx.x[30]);
                        keep.push_back(t);
                        log("profile: thread %llx is host tid %lld (entry %s)", (unsigned long long)t->id,
                            (long long)t->host_tid, describe(t->entry).c_str());
                    }
            }
            log("profile: sampling %zu threads for %d s", tids.size(), seconds);
            bool ok = plat::profile(tids.data(), lrs.data(), tids.size(), seconds, plat::data_root() + "/profile.txt");
            log("profile: %s", ok ? "written to profile.txt" : "not available on this platform");
        }
        uint64_t f = g_frame, now = plat::monotonic_ns();
        if (f != last || plat::window_minimized()) {
            last = f;
            since = now;
            reported = false;
            continue;
        }
        if (f && !reported && now - since > 5'000'000'000ull) {
            reported = true;
            log("hang: no frame for 5 s after frame %llu; threads (two samples, 1 s apart):", (unsigned long long)f);
            report_threads();
            plat::sleep_ns(1'000'000'000);
            log("hang: second sample");
            report_threads();
        }
    }
}


// Stalls: while no frame comes for more than 60 ms, sample where every guest thread is (its lr, every 5 ms); a
// stall over 100 ms is logged with each thread's most frequent place, to tell what the frame waited for.
// NX_THREAD_DUMP_AT=<seconds>: once, every guest thread's call chain (frame-pointer walk), to see what a stuck game
// waits for.
static void dump_threads() {
    std::lock_guard al(g_all_lock);
    for (auto& w : g_all) {
        auto t = w.lock();
        if (!t || !t->started || t->exited) continue;
        std::string chain = describe(t->ctx.x[30]);
        uint64_t fp = t->ctx.x[29];
        for (int k = 0; k < 12 && fp && (fp & 7) == 0 && fp >= kModuleBase; k++) {
            uint64_t lr = ld<uint64_t>(fp + 8);
            if (!lr) break;
            chain += " < " + describe(lr);
            uint64_t next = ld<uint64_t>(fp);
            if (next <= fp) break;
            fp = next;
        }
        // x0..x2 as the thread left them: for one blocked in a call, what it waits on (a mutex, an event, a handle).
        const char* hle = t->hle.load(std::memory_order_relaxed);
        log("threads: %llx (handle %x)%s%s %s  [x0 %llx x1 %llx x2 %llx]", (unsigned long long)t->id, t->handle,
            hle ? " in hle:" : "", hle ? hle : "", chain.c_str(), (unsigned long long)t->ctx.x[0],
            (unsigned long long)t->ctx.x[1], (unsigned long long)t->ctx.x[2]);
    }
}

static void stall_sampler(void*) {
    static const char* dump_env = std::getenv("NX_THREAD_DUMP_AT");
    uint64_t dump_at = dump_env ? plat::monotonic_ns() + uint64_t(std::atof(dump_env) * 1e9) : 0;
    uint64_t last_frame = 0, since = plat::monotonic_ns();
    std::map<std::tuple<uint64_t, uint64_t, const char*>, uint32_t> hits;  // (thread id, guest lr, HLE) -> samples
    for (;;) {
        plat::sleep_ns(5'000'000);
        uint64_t f = g_frame, now = plat::monotonic_ns();
        if (dump_at && now >= dump_at) dump_threads(), dump_at = 0;
        if (f != last_frame || plat::window_minimized()) {
            uint64_t ms = (now - since) / 1'000'000;
            if (last_frame && ms > 100 && !hits.empty()) {
                log("stall: %llu ms without a frame after frame %llu; where the threads were:", (unsigned long long)ms,
                    (unsigned long long)last_frame);
                struct Top {
                    uint64_t lr = 0;
                    const char* hle = nullptr;
                    uint32_t n = 0;
                };
                std::map<uint64_t, Top> top;  // thread -> most frequent place
                std::map<uint64_t, uint32_t> total;
                for (auto& [k, n] : hits) {
                    total[std::get<0>(k)] += n;
                    if (n > top[std::get<0>(k)].n) top[std::get<0>(k)] = {std::get<1>(k), std::get<2>(k), n};
                }
                for (auto& [id, t] : top)
                    log("  thread %llx: %s%s%s (%u of %u samples)", (unsigned long long)id, describe(t.lr).c_str(),
                        t.hle ? " in hle:" : "", t.hle ? t.hle : "", t.n, total[id]);
            }
            hits.clear();
            last_frame = f;
            since = now;
            continue;
        }
        if (!f || now - since < 60'000'000) continue;
        std::lock_guard al(g_all_lock);
        for (auto& w : g_all)
            if (auto t = w.lock(); t && t->started && !t->exited)
                hits[{t->id, t->ctx.x[30], t->hle.load(std::memory_order_relaxed)}]++;
    }
}

void log_guest_threads() { dump_threads(); }

void kernel_init() {
    plat::start_thread(watchdog, nullptr, 1ull << 20);
    if (std::getenv("NX_LOG") || std::getenv("NX_STALL_LOG") || std::getenv("NX_THREAD_DUMP_AT"))
        plat::start_thread(stall_sampler, nullptr, 1ull << 20);
}

}  // namespace nx
