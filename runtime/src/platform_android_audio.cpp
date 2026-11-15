#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "diagnostics.h"
#include "kernel.h"
#include "platform.h"
#include "platform_android.h"

// Android audio out: AAudio (split from platform_android.cpp).
namespace nx::plat {

// ---- audio: AAudio (callback pulls from a queue of submitted buffers) ------------------------------------------

namespace {
// The game's buffers are copied into a ring at submit and played from there. Each is handed back (done) when the
// device's read position reaches its end minus the A/V delay: with no delay, when it has played (as before); with a
// delay of D, D earlier, so the game queues D more sound ahead and what it mixes reaches the speaker D later, while
// its own audio clock keeps real time (audio_set_delay_ms, set from the frame rate: nvn_queue.cpp).
constexpr uint64_t kRingFrames = 1 << 16;  // ~1.4 s of 48 kHz stereo s16
struct Pending {
    uint64_t end;  // ring position (frames) after its last frame
    void* ctx;
};
std::mutex g_audio;
std::vector<uint32_t> g_ring(kRingFrames);  // one stereo s16 frame per element
uint64_t g_written = 0, g_read = 0;         // frames, since the stream opened
uint64_t g_device = 0;                      // frames the callback gave the device (silence included): AAudio's count
std::atomic<uint64_t> g_latency_ns{0};      // audio_latency_ns
std::deque<Pending> g_pending;
std::atomic<uint32_t> g_delay_frames{0};
std::atomic<uint64_t> g_occ_min{~uint64_t(0)}, g_occ_max{0};
std::atomic<uint64_t> g_truncations{0}, g_pending_max{0};
std::atomic<uint64_t> g_latency_ok{0}, g_latency_early{0};
AudioDone g_done = nullptr;
AAudioStream* g_stream = nullptr;
// Per ~2 s, logged from the callback: how the device pulls (callbacks, frames per call) and how often the game's
// buffers ran out (underruns: silence played, and the game's buffers come back late, its clock stalls).
struct CbStats {
    uint64_t from_ns = 0, calls = 0, frames = 0, underruns = 0, silent_frames = 0, max_gap_ns = 0, last_ns = 0;
    int32_t min_frames = 1 << 30, max_frames = 0;
} g_cb;

void cb_stats(int32_t frames, size_t missing_bytes) {
    CbStats& s = g_cb;
    const uint64_t now = monotonic_ns();
    if (!s.from_ns) s.from_ns = now;
    if (s.last_ns) s.max_gap_ns = std::max(s.max_gap_ns, now - s.last_ns);
    s.last_ns = now;
    s.calls++, s.frames += uint64_t(frames);
    s.min_frames = std::min(s.min_frames, frames), s.max_frames = std::max(s.max_frames, frames);
    if (missing_bytes) s.underruns++, s.silent_frames += missing_bytes / 4;
    if (now - s.from_ns < 2'000'000'000ull) return;
    if (s.underruns || std::getenv("NX_AUDIO_STATS"))
        nx::log("audio: device pulled %llu times in %.1f s (%d..%d frames, longest %.1f ms apart); %llu underruns, "
                "%.0f ms of silence", (unsigned long long)s.calls, double(now - s.from_ns) / 1e9, s.min_frames,
                s.max_frames, double(s.max_gap_ns) / 1e6, (unsigned long long)s.underruns,
                double(s.silent_frames) / 48.0);
    s = {};
    s.from_ns = s.last_ns = now;
}

aaudio_data_callback_result_t audio_cb(AAudioStream*, void*, void* out, int32_t frames) {
#if NX_DIAGNOSTICS_ON
    const uint64_t diag_start = monotonic_ns();
#endif
    auto* dst = static_cast<uint32_t*>(out);
    size_t missing = 0;
    uint64_t occupancy_now = 0, pending_now = 0;
    std::vector<void*> finished;
    {
        std::lock_guard lk(g_audio);
        const uint64_t occupancy = g_written - g_read;
        occupancy_now = occupancy;
        g_occ_min.store(std::min(g_occ_min.load(std::memory_order_relaxed), occupancy), std::memory_order_relaxed);
        g_occ_max.store(std::max(g_occ_max.load(std::memory_order_relaxed), occupancy), std::memory_order_relaxed);
        const uint64_t n = std::min<uint64_t>(uint64_t(frames), occupancy);
        const uint64_t rpos = g_read % kRingFrames;
        const uint64_t first = std::min(n, kRingFrames - rpos);
        std::memcpy(dst, &g_ring[rpos], first * 4);
        if (n > first) std::memcpy(dst + first, &g_ring[0], (n - first) * 4);
        g_read += n;
        g_device += uint64_t(frames);
        missing = size_t(frames) - size_t(n);
        if (missing) std::memset(dst + n, 0, missing * 4);  // underrun: silence
        const uint64_t due = g_read + g_delay_frames.load(std::memory_order_relaxed);
        while (!g_pending.empty() && g_pending.front().end <= due) {
            finished.push_back(g_pending.front().ctx);
            g_pending.pop_front();
        }
        pending_now = g_pending.size();
    }
    cb_stats(frames, missing * 4);
    for (void* c : finished) g_done(c);
#if NX_DIAGNOSTICS_ON
    diag::audio_callback(diag_start, monotonic_ns(), frames, uint64_t(missing), occupancy_now, pending_now);
#endif
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
void audio_error_cb(AAudioStream*, void*, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        nx::log("audio: AAudio stream disconnected (error %d)", int(error));
    }
}

bool audio_stream_create() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, audio_error_cb, nullptr);
    bool ok = AAudioStreamBuilder_openStream(b, &g_stream) == AAUDIO_OK && AAudioStream_requestStart(g_stream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(b);
    if (ok)
        nx::log("audio: AAudio stream: %d frames per burst, buffer %d of %d frames, performance mode %d",
                int(AAudioStream_getFramesPerBurst(g_stream)), int(AAudioStream_getBufferSizeInFrames(g_stream)),
                int(AAudioStream_getBufferCapacityInFrames(g_stream)), int(AAudioStream_getPerformanceMode(g_stream)));
    return ok;
}

void audio_reopen_if_disconnected() {
    if (!g_stream || AAudioStream_getState(g_stream) == AAUDIO_STREAM_STATE_DISCONNECTED) {
        if (g_stream) {
            AAudioStream_close(g_stream);
            g_stream = nullptr;
        }
        audio_stream_create();
    }
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // NX_MUTE=1 (and headless test runs): no device, the callers pace themselves with a timer (as on Windows and macOS).
    if (headless() || std::getenv("NX_MUTE")) return false;
    return audio_stream_create();
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused) {
        AAudioStream_requestPause(g_stream);
    } else {
        audio_reopen_if_disconnected();
        if (g_stream) {
            aaudio_result_t res = AAudioStream_requestStart(g_stream);
            if (res == AAUDIO_ERROR_DISCONNECTED) {
                audio_reopen_if_disconnected();
            }
        }
    }
}

// When the buffer starting at ring position `start` reaches the speaker: AAudio's last timestamp (a frame the device
// presented and when) moved by the frames between, the ring's queue ahead of it and the device's own buffer included.
// Every 8th buffer, smoothed (A/V sync, nvn_queue.cpp).
static void measure_latency(uint64_t start, uint64_t read, uint64_t device) {
    static uint32_t n = 0;
    if (n++ % 8) return;
    int64_t pos = 0, at = 0;
    if (!g_stream || AAudioStream_getTimestamp(g_stream, CLOCK_MONOTONIC, &pos, &at) != AAUDIO_OK || at <= 0) {
        g_latency_early.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t frame = int64_t(device + (start - read));  // the device's index of the buffer's first frame
    const int64_t when = at + (frame - pos) * 1000000000ll / 48000;
    const int64_t now = int64_t(monotonic_ns());
    if (when <= now || when - now > 2000000000ll) {
        g_latency_early.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t l = uint64_t(when - now), old = g_latency_ns.load(std::memory_order_relaxed);
    g_latency_ns.store(old ? old + (int64_t(l) - int64_t(old)) / 8 : l, std::memory_order_relaxed);
    g_latency_ok.fetch_add(1, std::memory_order_relaxed);
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    audio_reopen_if_disconnected();
    if (!g_stream) return false;
    uint64_t start, read, device;
#if NX_DIAGNOSTICS_ON
    uint64_t diag_pending = 0;
    bool diag_truncated = false;
#endif
    {
        std::lock_guard lk(g_audio);
        start = g_written, read = g_read, device = g_device;
        const uint64_t want = bytes / 4;
        const uint64_t n = std::min<uint64_t>(want, kRingFrames - (g_written - g_read));  // never more than a full ring
        if (n < want) g_truncations.fetch_add(1, std::memory_order_relaxed);
        const auto* src = static_cast<const uint8_t*>(data);
        const uint64_t wpos = g_written % kRingFrames;
        const uint64_t first = std::min(n, kRingFrames - wpos);
        std::memcpy(&g_ring[wpos], src, first * 4);
        if (n > first) std::memcpy(&g_ring[0], src + first * 4, (n - first) * 4);
        g_written += n;
        g_pending.push_back({g_written, ctx});
        g_pending_max.store(std::max(g_pending_max.load(std::memory_order_relaxed), uint64_t(g_pending.size())),
                            std::memory_order_relaxed);
#if NX_DIAGNOSTICS_ON
        diag_pending = g_pending.size();
        diag_truncated = n < want;
#endif
    }
    measure_latency(start, read, device);
#if NX_DIAGNOSTICS_ON
    diag::audio_submit(diag_pending, diag_truncated);
#endif
    return true;
}

uint64_t audio_latency_ns() { return g_latency_ns.load(std::memory_order_relaxed); }

AudioDiagnostics audio_diagnostics() {
    const uint64_t mn = g_occ_min.load(std::memory_order_relaxed);
    return {mn == ~uint64_t(0) ? 0 : mn,
            g_occ_max.load(std::memory_order_relaxed),
            g_truncations.load(std::memory_order_relaxed),
            g_pending_max.load(std::memory_order_relaxed),
            g_latency_ok.load(std::memory_order_relaxed),
            g_latency_early.load(std::memory_order_relaxed)};
}

void audio_set_delay_ms(uint32_t ms) { g_delay_frames.store(std::min<uint32_t>(ms, 500) * 48, std::memory_order_relaxed); }

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

}  // namespace nx::plat
