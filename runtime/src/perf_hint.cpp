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
using CloseSession = void (*)(void*);

struct Api {
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    ReportActual report = nullptr;
    UpdateTarget update_target = nullptr;
    CloseSession close = nullptr;
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
        x.close = reinterpret_cast<CloseSession>(find_sym("APerformanceHint_closeSession", "APerformanceHintManager_closeSession"));
        x.ok = x.get_manager && x.create && x.report && x.close;
        if (x.ok) {
            __android_log_print(ANDROID_LOG_INFO, "P5R-ADPF", "ADPF dynamic symbols resolved successfully");
        } else {
            __android_log_print(ANDROID_LOG_WARN, "P5R-ADPF", "ADPF symbols not available on this Android version");
        }
        return x;
    }();
    return a;
}

std::mutex g_lock;
std::vector<int32_t> g_tids;
void* g_session = nullptr;
bool g_dirty = false;

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
    if (g_dirty && !g_tids.empty()) {  // a new thread made part of the frame: a session with all of them
        if (g_session) a.close(g_session);
        g_session = a.create(a.get_manager(), g_tids.data(), g_tids.size(), target);
        g_dirty = false;
        log("adpf: %s for %zu threads", g_session ? "session made" : "no session", g_tids.size());
        __android_log_print(ANDROID_LOG_INFO, "P5R-ADPF", "ADPF hint session %s for %zu threads (target %lld ns)",
                            g_session ? "created" : "failed to create", g_tids.size(), (long long)target);
    }
    if (g_session) {
        a.report(g_session, ns);
    }
}
#else
void gpu_turbo() {}
void perf_hint_thread() {}
void perf_hint_frame(int64_t) {}
#endif

}  // namespace nx::plat
