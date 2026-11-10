// Low-overhead frame diagnostics for the Android performance work (NX_PERF_DIAGNOSTICS=1 or the file
// <data>/perf_diag.txt). One [P5R_PERF] line every 2 s or 120 presents puts the frame's whole story in nx.log:
// present-to-present time, active work, pacing sleep, gpu_present, ADPF reports, and the time each frame thread spent
// blocked in WaitSynchronization (vsync separated from everything else). NX_RELEASE still allows the explicit
// environment variable, but not the flag file (kDiagnostics).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include "platform.h"

#include <nx/runtime.h>

namespace nx::plat {

namespace {

struct WaitCounts {
    uint64_t ns = 0, calls = 0, vsync_ns = 0, vsync_calls = 0;
};
thread_local WaitCounts t_wait;

struct Window {
    uint64_t from_ns = 0, frames = 0;
    uint64_t interval_sum = 0, interval_max = 0;
    uint64_t work_sum = 0, work_max = 0;
    uint64_t pacing_sum = 0, pacing_max = 0;
    uint64_t call_sum = 0, call_max = 0;
    uint64_t hist[8] = {};  // <17, <20, <25, <31, <36, <50, <75, >=75 ms
    uint64_t wait_ns = 0, wait_calls = 0, vsync_ns = 0, vsync_calls = 0;
    uint64_t main_frames = 0, main_sum = 0, main_max = 0, main_cpu_sum = 0;
    uint64_t main_wait_ns = 0, main_wait_calls = 0;
    uint64_t gpu_calls = 0, gpu_sum = 0, gpu_max = 0;
    uint64_t adpf_reports = 0, adpf_over = 0, adpf_threads = 0;
    int64_t adpf_target = 0, adpf_sum = 0, adpf_max = 0;
};
std::mutex g_mu;
Window g_win;

int interval_bin(uint64_t ns) {
    if (ns < 17'000'000) return 0;
    if (ns < 20'000'000) return 1;
    if (ns < 25'000'000) return 2;
    if (ns < 31'000'000) return 3;
    if (ns < 36'000'000) return 4;
    if (ns < 50'000'000) return 5;
    if (ns < 75'000'000) return 6;
    return 7;
}

void log_window(uint64_t now) {
    Window& w = g_win;
    const double secs = double(now - w.from_ns) / 1e9;
    const double fps = secs > 0 ? double(w.frames) / secs : 0.0;
    const double n = w.frames ? double(w.frames) : 1.0;
    const double mn = w.main_frames ? double(w.main_frames) : 1.0;
    char adpf[96] = "none";
    if (w.adpf_reports)
        std::snprintf(adpf, sizeof adpf, "target %.2f avg %.2f max %.2f over %llu", double(w.adpf_target) / 1e6,
                      double(w.adpf_sum) / 1e6 / double(w.adpf_reports), double(w.adpf_max) / 1e6,
                      (unsigned long long)w.adpf_over);
    log("[P5R_PERF] %.2f s, %llu presents, %.1f fps; interval avg %.2f max %.2f ms "
        "[<%u:%llu <%u:%llu <%u:%llu <%u:%llu <%u:%llu <%u:%llu <%u:%llu >=%u:%llu]; "
        "work avg %.2f max %.2f, pacing avg %.2f max %.2f, present call avg %.2f max %.2f ms; "
        "waits vsync %.2f ms/%llu other %.2f ms/%llu; gpu present avg %.2f max %.2f ms; "
        "main frame avg %.2f max %.2f cpu avg %.2f waits %.2f/%llu; adpf %s, %llu threads",
        secs, (unsigned long long)w.frames, fps, double(w.interval_sum) / 1e6 / n, double(w.interval_max) / 1e6,
        17, (unsigned long long)w.hist[0], 20, (unsigned long long)w.hist[1], 25, (unsigned long long)w.hist[2],
        31, (unsigned long long)w.hist[3], 36, (unsigned long long)w.hist[4], 50, (unsigned long long)w.hist[5],
        75, (unsigned long long)w.hist[6], 75, (unsigned long long)w.hist[7],
        double(w.work_sum) / 1e6 / n, double(w.work_max) / 1e6, double(w.pacing_sum) / 1e6 / n,
        double(w.pacing_max) / 1e6, double(w.call_sum) / 1e6 / n, double(w.call_max) / 1e6,
        double(w.vsync_ns) / 1e6 / n, (unsigned long long)w.vsync_calls, double(w.wait_ns) / 1e6 / n,
        (unsigned long long)w.wait_calls, double(w.gpu_sum) / 1e6 / (w.gpu_calls ? double(w.gpu_calls) : 1.0),
        double(w.gpu_max) / 1e6, double(w.main_sum) / 1e6 / mn, double(w.main_max) / 1e6,
        double(w.main_cpu_sum) / 1e6 / mn, double(w.main_wait_ns) / 1e6 / mn,
        (unsigned long long)w.main_wait_calls, adpf, (unsigned long long)w.adpf_threads);
    w = Window{};
    w.from_ns = now;
}

}  // namespace

bool perf_diag_enabled() {
    static const bool on = [] {
        if (const char* e = std::getenv("NX_PERF_DIAGNOSTICS"); e && *e) {
            const bool on = e[0] != '0';
            if (on) log("perf: diagnostics on (NX_PERF_DIAGNOSTICS=1)");
            return on;
        }
        if (!kDiagnostics) return false;
        std::ifstream f(data_root() + "/perf_diag.txt");
        if (!f) return false;
        std::string line;
        std::getline(f, line);
        const bool on = line.empty() || line[0] != '0';
        if (on) log("perf: diagnostics on (perf_diag.txt)");
        return on;
    }();
    return on;
}

void perf_diag_thread(int role) { (void)role; }

void perf_diag_wait(uint64_t ns, bool vsync) {
    if (!ns || !perf_diag_enabled()) return;
    t_wait.ns += ns;
    t_wait.calls++;
    if (vsync) t_wait.vsync_ns += ns, t_wait.vsync_calls++;
}

void perf_diag_present(uint64_t interval_ns, int64_t work_ns, uint64_t pacing_ns, uint64_t call_ns) {
    if (!perf_diag_enabled()) return;
    const uint64_t now = monotonic_ns();
    std::lock_guard lk(g_mu);
    Window& w = g_win;
    if (!w.from_ns) w.from_ns = now;
    w.frames++;
    if (interval_ns) {
        w.interval_sum += interval_ns;
        w.interval_max = std::max(w.interval_max, interval_ns);
        w.hist[interval_bin(interval_ns)]++;
    }
    if (work_ns > 0) w.work_sum += uint64_t(work_ns), w.work_max = std::max(w.work_max, uint64_t(work_ns));
    w.pacing_sum += pacing_ns;
    w.pacing_max = std::max(w.pacing_max, pacing_ns);
    w.call_sum += call_ns;
    w.call_max = std::max(w.call_max, call_ns);
    w.wait_ns += t_wait.ns;
    w.wait_calls += t_wait.calls;
    w.vsync_ns += t_wait.vsync_ns;
    w.vsync_calls += t_wait.vsync_calls;
    t_wait = {};
    if (w.frames >= 120 || now - w.from_ns >= 2'000'000'000ull) log_window(now);
}

void perf_diag_main_frame(uint64_t frame_ns, uint64_t cpu_ns) {
    if (!perf_diag_enabled()) return;
    std::lock_guard lk(g_mu);
    Window& w = g_win;
    w.main_frames++;
    w.main_sum += frame_ns;
    w.main_max = std::max(w.main_max, frame_ns);
    w.main_cpu_sum += cpu_ns;
    w.main_wait_ns += t_wait.ns;
    w.main_wait_calls += t_wait.calls;
    t_wait = {};
}

void perf_diag_gpu_present(uint64_t ns) {
    if (!perf_diag_enabled()) return;
    std::lock_guard lk(g_mu);
    g_win.gpu_calls++;
    g_win.gpu_sum += ns;
    g_win.gpu_max = std::max(g_win.gpu_max, ns);
}

void perf_diag_adpf(int64_t target_ns, int64_t reported_ns, size_t threads, bool session) {
    if (!perf_diag_enabled()) return;
    std::lock_guard lk(g_mu);
    Window& w = g_win;
    w.adpf_target = session ? target_ns : 0;
    w.adpf_threads = threads;
    if (!session || reported_ns <= 0) return;
    w.adpf_reports++;
    w.adpf_sum += reported_ns;
    w.adpf_max = std::max(w.adpf_max, reported_ns);
    w.adpf_over += reported_ns > target_ns;
}

}  // namespace nx::plat
