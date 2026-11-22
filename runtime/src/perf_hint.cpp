// Performance hints (docs/nvn/ANDROID-PORT.md A5, DEBUG-2.md #84): on Android the threads that make a frame (the
// game's render thread, which also replays NVN, and the presenting one) join an ADPF session (APerformanceHint,
// Android 13+) with a 16.7 ms target, and each frame's duration is reported. The system then raises the big cores'
// clocks when a frame runs late instead of guessing from load, which on phones keeps a steady 60 fps cooler and faster
// than the default governor. The API is looked up with dlsym, so older Android versions just go without.
// NX_ADPF=0: off. Other platforms: nothing.
#include "platform.h"

#if defined(__ANDROID__)
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <nx/runtime.h>
#endif

namespace nx::plat {

#if defined(__ANDROID__)
namespace {

using GetManager = void* (*)();
using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
using ReportActual = int (*)(void*, int64_t);
using UpdateTarget = void (*)(void*, int64_t);
using SetThreads = int (*)(void*, const int32_t*, size_t);
using CloseSession = void (*)(void*);
using WdCreate = void* (*)();
using WdSetNs = void (*)(void*, int64_t);
using ReportActual2 = int (*)(void*, void*);
using ThermalAcquire = void* (*)();
using ThermalStatus = int (*)(void*);
using ThermalHeadroom = float (*)(void*, int);

struct Api {
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    ReportActual report = nullptr;
    UpdateTarget update_target = nullptr;
    SetThreads set_threads = nullptr;
    CloseSession close = nullptr;
    WdCreate wd_create = nullptr;
    WdSetNs wd_start = nullptr;
    WdSetNs wd_total = nullptr;
    WdSetNs wd_cpu = nullptr;
    WdSetNs wd_gpu = nullptr;
    ReportActual2 report2 = nullptr;
    void* wd = nullptr;
    void* thermal_mgr = nullptr;
    ThermalStatus thermal_status = nullptr;
    ThermalHeadroom thermal_headroom = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api x;
        const char* e = std::getenv("NX_ADPF");
        if (e && e[0] == '0') {
            __android_log_print(ANDROID_LOG_INFO, "P5R-ADPF", "ADPF disabled via NX_ADPF=0");
            return x;
        }
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) {
            __android_log_print(ANDROID_LOG_WARN, "P5R-ADPF", "dlopen(libandroid.so) failed");
            return x;
        }
        auto find_sym = [&](const char* n1, const char* n2) {
            void* s = dlsym(lib, n1);
            if (!s && n2) s = dlsym(lib, n2);
            return s;
        };
        x.get_manager = reinterpret_cast<GetManager>(find_sym("APerformanceHint_getManager", "APerformanceHintManager_getInstance"));
        x.create = reinterpret_cast<CreateSession>(find_sym("APerformanceHint_createSession", "APerformanceHintManager_createSession"));
        x.report = reinterpret_cast<ReportActual>(find_sym("APerformanceHint_reportActualWorkDuration", "APerformanceHintManager_reportActualWorkDuration"));
        x.update_target = reinterpret_cast<UpdateTarget>(find_sym("APerformanceHint_updateTargetWorkDuration", "APerformanceHintManager_updateTargetWorkDuration"));
        x.set_threads = reinterpret_cast<SetThreads>(find_sym("APerformanceHint_setThreads", "APerformanceHintManager_setThreads"));
        x.close = reinterpret_cast<CloseSession>(find_sym("APerformanceHint_closeSession", "APerformanceHintManager_closeSession"));
        x.wd_create = reinterpret_cast<WdCreate>(dlsym(lib, "AWorkDuration_create"));
        x.wd_start = reinterpret_cast<WdSetNs>(dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos"));
        x.wd_total = reinterpret_cast<WdSetNs>(dlsym(lib, "AWorkDuration_setActualTotalDurationNanos"));
        x.wd_cpu = reinterpret_cast<WdSetNs>(dlsym(lib, "AWorkDuration_setActualCpuDurationNanos"));
        x.wd_gpu = reinterpret_cast<WdSetNs>(dlsym(lib, "AWorkDuration_setActualGpuDurationNanos"));
        x.report2 = reinterpret_cast<ReportActual2>(dlsym(lib, "APerformanceHint_reportActualWorkDuration2"));
        if (x.wd_create && x.wd_start && x.wd_total && x.wd_cpu && x.wd_gpu && x.report2) x.wd = x.wd_create();
        auto tacq = reinterpret_cast<ThermalAcquire>(dlsym(lib, "AThermal_acquireManager"));
        x.thermal_status = reinterpret_cast<ThermalStatus>(dlsym(lib, "AThermal_getCurrentThermalStatus"));
        x.thermal_headroom = reinterpret_cast<ThermalHeadroom>(dlsym(lib, "AThermal_getThermalHeadroom"));
        if (tacq && (x.thermal_status || x.thermal_headroom)) x.thermal_mgr = tacq();
        x.ok = x.get_manager && x.create && x.report && x.close;
        if (x.ok) {
            __android_log_print(ANDROID_LOG_INFO, "P5R-ADPF", "ADPF active (API35 report2=%d, thermal=%d)",
                                x.wd ? 1 : 0, x.thermal_mgr ? 1 : 0);
        } else {
            __android_log_print(ANDROID_LOG_WARN, "P5R-ADPF", "ADPF symbols not available on this Android version");
        }
        return x;
    }();
    return a;
}

int read_batt_deci_c() {
    static const char* paths[] = {
        "/sys/class/power_supply/battery/temp",
        "/sys/class/power_supply/bms/temp",
    };
    for (const char* p : paths) {
        if (FILE* f = std::fopen(p, "r")) {
            int t = 0;
            if (std::fscanf(f, "%d", &t) == 1 && t > 0) {
                std::fclose(f);
                return t > 100 ? t : t * 10;  // tenths of deg C
            }
            std::fclose(f);
        }
    }
    return 0;
}

std::mutex g_lock;
std::vector<int32_t> g_tids;
void* g_session = nullptr;
bool g_dirty = false;
int64_t g_last_target = 0;
int g_thermal_level = 0;
uint64_t g_next_thermal_ns = 0;

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
    if (g_tids.size() < 8 && std::find(g_tids.begin(), g_tids.end(), tid) == g_tids.end())
        g_tids.push_back(tid), g_dirty = true;
}

void perf_hint_frame(int64_t ns) {
    const Api& a = api();
    if (!a.ok || ns <= 0) return;
    std::lock_guard lk(g_lock);
    const char* cap = std::getenv("NX_FPS_CAP");
    int64_t target = cap && std::atoi(cap) > 0 && std::atoi(cap) <= 30 ? 33'333'333 : 16'666'667;

    // 1 Hz thermal check (ColorOS SKIN cliff / BatteryManager temp guard from TOTKDroid v34)
    const uint64_t now_ns = monotonic_ns();
    if (now_ns >= g_next_thermal_ns) {
        g_next_thermal_ns = now_ns + 1'000'000'000ull;
        int ts = (a.thermal_mgr && a.thermal_status) ? a.thermal_status(a.thermal_mgr) : 0;
        float hr = (a.thermal_mgr && a.thermal_headroom) ? a.thermal_headroom(a.thermal_mgr, 0) : -1.0f;
        int batt = read_batt_deci_c();
        bool hot2 = (ts >= 3) || (ts >= 2 && (batt <= 0 || batt >= 380 || hr >= 0.70f)) || (batt >= 390);
        bool warm1 = (ts >= 1) || (batt >= 383);
        int new_lvl = hot2 ? 2 : warm1 ? 1 : 0;
        if (new_lvl != g_thermal_level) {
            g_thermal_level = new_lvl;
            log("adpf: thermal level -> %d (status %d, headroom %.2f, batt %.1fC)",
                g_thermal_level, ts, double(hr), batt / 10.0);
        }
    }
    if (g_thermal_level >= 2 && target < 25'000'000) target = 25'000'000;
    else if (g_thermal_level == 1 && target < 20'000'000) target = 20'000'000;

    if (g_dirty && !g_tids.empty()) {
        if (g_session && a.set_threads) {
            if (a.set_threads(g_session, g_tids.data(), g_tids.size()) == 0) {
                g_dirty = false;
            }
        }
        if (g_dirty) {
            if (g_session) a.close(g_session);
            g_session = a.create(a.get_manager(), g_tids.data(), g_tids.size(), target);
            g_last_target = target;
            g_dirty = false;
            log("adpf: %s for %zu threads", g_session ? "session made" : "no session", g_tids.size());
        }
    }
    if (g_session) {
        if (a.update_target && target != g_last_target) {
            a.update_target(g_session, target);
            g_last_target = target;
        }
        // Anti-death-spiral guard while warm/hot: cap spike reports to 1.05x target so ADPF PID doesn't slam clocks
        int64_t rep_ns = ns;
        if (g_thermal_level >= 2 && target > 0) {
            int64_t lim = target * 105 / 100;
            if (rep_ns > lim) rep_ns = lim;
        }
        if (a.wd && a.report2) {
            a.wd_start(a.wd, int64_t(now_ns) - rep_ns);
            a.wd_total(a.wd, rep_ns);
            a.wd_cpu(a.wd, rep_ns);
            a.wd_gpu(a.wd, rep_ns / 2);
            a.report2(g_session, a.wd);
        } else {
            a.report(g_session, rep_ns);
        }
        // ColorOS ADPF resets cpus_allowed to 0..7 on report/setThreads; re-pin presenting thread to X4 cluster.
        place_fast_cores();
    }
}
#else
void gpu_turbo() {}
void perf_hint_thread() {}
void perf_hint_frame(int64_t) {}
#endif

}  // namespace nx::plat
