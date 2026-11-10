// Performance hints (docs/nvn/ANDROID-PORT.md A5, DEBUG-2.md #84): on Android the threads that make a frame (the
// game's render thread, which also replays NVN, and the presenting one) join an ADPF session (APerformanceHint,
// Android 13+) with the frame period as the target, and each frame's duration is reported. The system then raises the
// big cores' clocks when a frame runs late instead of guessing from load, which on phones keeps a steady 60 fps cooler
// and faster than the default governor. The API is looked up with dlsym, so older Android versions just go without.
//
// NX_ADPF=0: off. NX_ADPF_REPORT: interval (default: present-to-present, includes pacing; a missed deadline makes the
// report over target and asks for a boost), active (work since the previous present's pacing), or cpu (the session
// threads' own CPU time from /proc/self/task/<tid>/schedstat; honest work, but it under-reports a frame-limited game).
// NX_ADPF_TARGET_SCALE=0.5..1.0 (default 1) or NX_ADPF_TARGET_MS: headroom in the reported target.
// Other platforms: nothing.
#include "platform.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include <nx/runtime.h>
#endif

namespace nx::plat {

#if defined(__ANDROID__)
namespace {

using GetManager = void* (*)();
using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
using SetThreads = int (*)(void*, const int32_t*, size_t);
using ReportActual = int (*)(void*, int64_t);
using CloseSession = void (*)(void*);

struct Api {
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    SetThreads set_threads = nullptr;
    ReportActual report = nullptr;
    CloseSession close = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api x;
        const char* e = std::getenv("NX_ADPF");
        if (e && e[0] == '0') return x;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return x;
        x.get_manager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
        x.create = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
        x.set_threads = reinterpret_cast<SetThreads>(dlsym(lib, "APerformanceHint_setThreads"));
        x.report = reinterpret_cast<ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        x.close = reinterpret_cast<CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
        x.ok = x.get_manager && x.create && x.report && x.close;
        return x;
    }();
    return a;
}

enum class ReportMode { interval, active, cpu };

struct Config {
    ReportMode mode = ReportMode::interval;
    int64_t target_ns = 16'666'667;
};

int64_t frame_period_ns() {
    // The ADPF target is the frame period: 33.3 ms when NX_FPS_CAP holds the game at 30 fps (nvn_queue.cpp).
    const char* cap = std::getenv("NX_FPS_CAP");
    return cap && std::atoi(cap) > 0 && std::atoi(cap) <= 30 ? 33'333'333 : 16'666'667;
}

const Config& config() {
    static const Config c = [] {
        Config x;
        x.target_ns = frame_period_ns();
        double scale = 1.0;
        if (const char* e = std::getenv("NX_ADPF_TARGET_SCALE")) {
            const double v = std::atof(e);
            if (v > 0) scale = std::clamp(v, 0.5, 1.0);
        }
        x.target_ns = int64_t(double(x.target_ns) * scale);
        if (const char* e = std::getenv("NX_ADPF_TARGET_MS")) {
            const double ms = std::atof(e);
            if (ms > 0) x.target_ns = int64_t(ms * 1e6);
        }
        const char* mode = std::getenv("NX_ADPF_REPORT");
        if (mode && !std::strcmp(mode, "active")) x.mode = ReportMode::active;
        else if (mode && !std::strcmp(mode, "cpu")) x.mode = ReportMode::cpu;
        else if (mode && std::strcmp(mode, "interval"))
            log("adpf: unknown NX_ADPF_REPORT=%s: interval", mode);
        const char* name = x.mode == ReportMode::active ? "active" : x.mode == ReportMode::cpu ? "cpu" : "interval";
        log("adpf: target %.2f ms (scale %.2f), report %s%s", double(x.target_ns) / 1e6, scale, name,
            mode ? "" : " (default)");
        return x;
    }();
    return c;
}

std::mutex g_lock;
std::vector<int32_t> g_tids;
std::vector<uint64_t> g_last_cpu;
void* g_session = nullptr;
bool g_dirty = false;

uint64_t schedstat_ns(int32_t tid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", int(tid));
    if (FILE* f = std::fopen(path, "r")) {
        unsigned long long run = 0, runq = 0, slices = 0;
        const bool ok = std::fscanf(f, "%llu %llu %llu", &run, &runq, &slices) >= 1;
        std::fclose(f);
        (void)runq;
        (void)slices;
        if (ok) return run;
    }
    if (tid == int32_t(syscall(SYS_gettid))) {
        timespec ts{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return uint64_t(ts.tv_sec) * 1'000'000'000ull + ts.tv_nsec;
    }
    return 0;
}

uint64_t cpu_since_last() {
    uint64_t sum = 0;
    for (size_t k = 0; k < g_tids.size() && k < g_last_cpu.size(); k++) {
        const uint64_t now = schedstat_ns(g_tids[k]);
        if (now >= g_last_cpu[k]) sum += now - g_last_cpu[k];
        g_last_cpu[k] = now;
    }
    return sum;
}

}  // namespace

// NX_GPU_TURBO=1 (an experiment, off by default): the Adreno kernel driver's power control off, so the GPU runs at its
// highest clock (thermal limits still apply). The idea is libadrenotools' adrenotools_set_turbo (BSD-2-Clause); this
// is written from the kernel's msm_kgsl UAPI: IOCTL_KGSL_SETPROPERTY (type 0x09, nr 0x32) with KGSL_PROP_PWRCTRL (0xE)
// and the value 0. The 30-fps lock lowered the GPU clock and frames then took longer (MEASUREMENTS.md, phase 4).
// The setting outlives the process (the GPU stayed at its top clock with the game closed), so every start sets it:
// off with NX_GPU_TURBO=1, back on (the driver's normal clock control) otherwise.
void gpu_turbo() {
    const char* e = std::getenv("NX_GPU_TURBO");
    const bool turbo = e && e[0] == '1';
    struct Property {
        unsigned int type;
        void* value;
        size_t sizebytes;
    };
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        log("gpu turbo: /dev/kgsl-3d0 not opened (errno %d)", errno);
        return;
    }
    unsigned int enable = turbo ? 0 : 1;
    Property p{0xE, &enable, sizeof enable};
    int r = ioctl(fd, _IOW(0x09, 0x32, Property), &p);
    log("gpu turbo: KGSL power control %s: %s (errno %d)", turbo ? "off (NX_GPU_TURBO=1)" : "on (normal)",
        r == 0 ? "done" : "refused", r == 0 ? 0 : errno);
    if (!turbo) close(fd);
    // The descriptor stays open: the driver ties the setting to it.
}

void perf_hint_thread() {
    if (!api().ok) return;
    thread_local bool joined = false;
    if (joined) return;
    joined = true;
    std::lock_guard lk(g_lock);
    int32_t tid = int32_t(syscall(SYS_gettid));
    if (g_tids.size() < 8 && std::find(g_tids.begin(), g_tids.end(), tid) == g_tids.end()) {
        g_tids.push_back(tid);
        g_last_cpu.push_back(schedstat_ns(tid));
        g_dirty = true;
    }
}

void perf_hint_frame(int64_t ns, int64_t interval_ns) {
    const Api& a = api();
    if (!a.ok) return;
    const Config& cfg = config();
    int64_t reported = 0;
    size_t threads = 0;
    bool session = false;
    {
        std::lock_guard lk(g_lock);
        if (g_dirty && !g_tids.empty()) {  // a new thread made part of the frame: update the session
            bool updated = false;
            if (g_session && a.set_threads) updated = a.set_threads(g_session, g_tids.data(), g_tids.size()) == 0;
            if (!updated) {
                if (g_session) a.close(g_session);
                g_session = a.create(a.get_manager(), g_tids.data(), g_tids.size(), cfg.target_ns);
            }
            g_dirty = false;
            log("adpf: session %s for %zu threads%s", !g_session ? "not made" : updated ? "updated" : "made",
                g_tids.size(), updated ? " (setThreads)" : "");
        }
        if (g_session) {
            int64_t value = ns;
            if (cfg.mode == ReportMode::interval) value = interval_ns > 0 ? interval_ns : ns;
            else if (cfg.mode == ReportMode::cpu) value = int64_t(cpu_since_last());
            if (value > 0) {
                if (a.report(g_session, value) == 0) reported = value;
                session = true;
            }
        }
        threads = g_tids.size();
    }
    if (session) perf_diag_adpf(cfg.target_ns, reported, threads, true);
}
#else
void gpu_turbo() {}
void perf_hint_thread() {}
void perf_hint_frame(int64_t, int64_t) {}
#endif

}  // namespace nx::plat
