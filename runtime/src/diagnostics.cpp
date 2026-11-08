#include "diagnostics.h"

#if NX_DIAGNOSTICS_ON

#include <algorithm>
#include <atomic>
#include <cstdio>

#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif

#include <nx/runtime.h>

namespace nx::diag {

namespace {

struct Counter {
    std::atomic<uint64_t> n{0}, total{0}, max{0}, max_at{0};
};

struct Snapshot {
    uint64_t n = 0, total = 0, max = 0, max_at = 0;
};

void add(Counter& c, uint64_t ns, uint64_t at) {
    c.n.fetch_add(1, std::memory_order_relaxed);
    c.total.fetch_add(ns, std::memory_order_relaxed);
    uint64_t old = c.max.load(std::memory_order_relaxed);
    while (ns > old) {
        if (c.max.compare_exchange_weak(old, ns, std::memory_order_relaxed)) {
            c.max_at.store(at, std::memory_order_relaxed);
            break;
        }
    }
}

Snapshot take(Counter& c) {
    Snapshot s;
    s.n = c.n.exchange(0, std::memory_order_relaxed);
    s.total = c.total.exchange(0, std::memory_order_relaxed);
    s.max = c.max.exchange(0, std::memory_order_relaxed);
    s.max_at = c.max_at.exchange(0, std::memory_order_relaxed);
    if (!s.n) s.max = s.max_at = 0;
    return s;
}

double seconds(uint64_t ns) { return double(ns) / 1e9; }
double millis(uint64_t ns) { return double(ns) / 1e6; }
double average_ms(const Snapshot& s) { return s.n ? millis(s.total) / double(s.n) : 0.0; }
double max_at_s(const Snapshot& s) { return s.max ? seconds(s.max_at) : 0.0; }

// ---- video -------------------------------------------------------------------------------------------------------
Counter g_pump, g_convert, g_copy, g_send_wait;
std::atomic<uint64_t> g_slots{0}, g_frame_bytes{0}, g_frame_capacity{0}, g_input_bytes{0}, g_stride{0}, g_rows{0};
std::atomic<uint64_t> g_queued{0}, g_ready{0}, g_busy{0};

// ---- render lock -------------------------------------------------------------------------------------------------
Counter g_lock_wait, g_lock_hold;

// ---- audio -------------------------------------------------------------------------------------------------------
Counter g_cb_gap, g_cb_exec;
std::atomic<uint64_t> g_callbacks{0}, g_underruns{0}, g_silent_frames{0}, g_last_underrun{0}, g_truncations{0};
std::atomic<uint64_t> g_occ_min{~uint64_t(0)}, g_occ_max{0}, g_occ_now{0};
std::atomic<uint64_t> g_pending_max{0}, g_pending_now{0};
std::atomic<uint64_t> g_last_callback_start{0};

// ---- memory ------------------------------------------------------------------------------------------------------
uint64_t g_mem_peak = 0;

uint64_t resident_bytes() {
#if defined(__ANDROID__) || defined(__linux__)
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    const int got = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? uint64_t(resident) * uint64_t(page) : 0;
#else
    return 0;
#endif
}

}  // namespace

void tick(uint64_t now) {
    static uint64_t last = 0;
    static bool started = false;
    if (!started) {
        started = true;
        last = now;
        return;
    }
    if (now - last < 2'000'000'000ull) return;
    const uint64_t span = now - last;
    last = now;

    const Snapshot pump = take(g_pump), convert = take(g_convert), copy = take(g_copy), send = take(g_send_wait);
    const Snapshot wait = take(g_lock_wait), hold = take(g_lock_hold);
    const Snapshot gap = take(g_cb_gap), exec = take(g_cb_exec);

    uint64_t occ_min = g_occ_min.exchange(~uint64_t(0), std::memory_order_relaxed);
    if (occ_min == ~uint64_t(0)) occ_min = 0;
    const uint64_t occ_max = g_occ_max.exchange(0, std::memory_order_relaxed);
    const uint64_t pending_max = g_pending_max.exchange(0, std::memory_order_relaxed);
    const uint64_t callbacks = g_callbacks.exchange(0, std::memory_order_relaxed);
    const uint64_t underruns = g_underruns.exchange(0, std::memory_order_relaxed);
    const uint64_t silent = g_silent_frames.exchange(0, std::memory_order_relaxed);
    const uint64_t last_underrun = g_last_underrun.exchange(0, std::memory_order_relaxed);
    const uint64_t truncations = g_truncations.exchange(0, std::memory_order_relaxed);

    const uint64_t resident = resident_bytes();
    g_mem_peak = std::max(g_mem_peak, resident);

    log("diag video: t=%.3f s window %.2f s frame %llu; pump %llu calls %.2f/%.2f ms avg/max at %.3f; to_nv12 %llu "
        "%.2f/%.2f; copy %llu %.2f/%.2f; send+wait %llu %.2f/%.2f; slots %llu, frame %.1f KB (cap %.1f KB), input "
        "%.1f KB, stride %llu rows %llu, queued %llu ready %llu busy %llu",
        seconds(now), seconds(span), (unsigned long long)g_frame.load(), (unsigned long long)pump.n, average_ms(pump),
        millis(pump.max),
        max_at_s(pump), (unsigned long long)convert.n, average_ms(convert), millis(convert.max),
        (unsigned long long)copy.n, average_ms(copy), millis(copy.max), (unsigned long long)send.n, average_ms(send),
        millis(send.max), (unsigned long long)g_slots.load(), double(g_frame_bytes.load()) / 1024.0,
        double(g_frame_capacity.load()) / 1024.0, double(g_input_bytes.load()) / 1024.0,
        (unsigned long long)g_stride.load(), (unsigned long long)g_rows.load(), (unsigned long long)g_queued.load(),
        (unsigned long long)g_ready.load(), (unsigned long long)g_busy.load());
    log("diag render: t=%.3f s; g_render.lock %llu acquisitions, wait %.2f/%.2f ms avg/max at %.3f; hold %.2f/%.2f "
        "ms avg/max at %.3f",
        seconds(now), (unsigned long long)wait.n, average_ms(wait), millis(wait.max), max_at_s(wait), average_ms(hold),
        millis(hold.max), max_at_s(hold));
    log("diag audio: t=%.3f s; %llu callbacks, interval %.2f/%.2f ms avg/max at %.3f; exec %.2f/%.2f ms avg/max at "
        "%.3f; %llu underruns (last at %.3f), %.1f ms silence; ring %llu..%llu now %llu frames, pending %llu max/%llu "
        "now, %llu truncated",
        seconds(now), (unsigned long long)callbacks, average_ms(gap), millis(gap.max), max_at_s(gap), average_ms(exec),
        millis(exec.max), max_at_s(exec), (unsigned long long)underruns,
        last_underrun ? seconds(last_underrun) : 0.0, double(silent) / 48.0, (unsigned long long)occ_min,
        (unsigned long long)occ_max, (unsigned long long)g_occ_now.load(), (unsigned long long)pending_max,
        (unsigned long long)g_pending_now.load(), (unsigned long long)truncations);
    log("diag memory: t=%.3f s; resident %.1f MB, peak %.1f MB", seconds(now), double(resident) / 1048576.0,
        double(g_mem_peak) / 1048576.0);
}

void movie_pump(uint64_t ns, uint64_t at) { add(g_pump, ns, at); }
void movie_convert(uint64_t ns, uint64_t at) { add(g_convert, ns, at); }
void movie_copy(uint64_t ns, uint64_t at) { add(g_copy, ns, at); }
void movie_send_wait(uint64_t ns, uint64_t at) { add(g_send_wait, ns, at); }

void movie_buffers(uint64_t output_slots, uint64_t frame_bytes, uint64_t input_bytes, uint64_t stride, uint64_t rows) {
    g_slots.store(output_slots, std::memory_order_relaxed);
    g_frame_bytes.store(frame_bytes, std::memory_order_relaxed);
    g_input_bytes.store(input_bytes, std::memory_order_relaxed);
    g_stride.store(stride, std::memory_order_relaxed);
    g_rows.store(rows, std::memory_order_relaxed);
}

void movie_frame_alloc(uint64_t size, uint64_t capacity) {
    g_frame_bytes.store(size, std::memory_order_relaxed);
    g_frame_capacity.store(capacity, std::memory_order_relaxed);
}

void movie_queues(uint64_t queued, uint64_t ready, uint64_t busy) {
    g_queued.store(queued, std::memory_order_relaxed);
    g_ready.store(ready, std::memory_order_relaxed);
    g_busy.store(busy, std::memory_order_relaxed);
}

void render_lock_wait(uint64_t ns, uint64_t at) { add(g_lock_wait, ns, at); }
void render_lock_hold(uint64_t ns, uint64_t at) { add(g_lock_hold, ns, at); }

void audio_callback(uint64_t start_ns, uint64_t end_ns, int32_t frames, uint64_t missing_frames, uint64_t occupancy,
                    uint64_t pending) {
    (void)frames;
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    const uint64_t previous = g_last_callback_start.exchange(start_ns, std::memory_order_relaxed);
    if (previous && start_ns > previous) add(g_cb_gap, start_ns - previous, start_ns);
    if (end_ns > start_ns) add(g_cb_exec, end_ns - start_ns, end_ns);

    uint64_t old = g_occ_min.load(std::memory_order_relaxed);
    while (occupancy < old && !g_occ_min.compare_exchange_weak(old, occupancy, std::memory_order_relaxed)) {}
    old = g_occ_max.load(std::memory_order_relaxed);
    while (occupancy > old && !g_occ_max.compare_exchange_weak(old, occupancy, std::memory_order_relaxed)) {}
    g_occ_now.store(occupancy, std::memory_order_relaxed);

    old = g_pending_max.load(std::memory_order_relaxed);
    while (pending > old && !g_pending_max.compare_exchange_weak(old, pending, std::memory_order_relaxed)) {}
    g_pending_now.store(pending, std::memory_order_relaxed);

    if (missing_frames) {
        g_underruns.fetch_add(1, std::memory_order_relaxed);
        g_silent_frames.fetch_add(missing_frames, std::memory_order_relaxed);
        g_last_underrun.store(end_ns, std::memory_order_relaxed);
    }
}

void audio_submit(uint64_t pending, bool truncated) {
    uint64_t old = g_pending_max.load(std::memory_order_relaxed);
    while (pending > old && !g_pending_max.compare_exchange_weak(old, pending, std::memory_order_relaxed)) {}
    g_pending_now.store(pending, std::memory_order_relaxed);
    if (truncated) g_truncations.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nx::diag

#endif  // NX_DIAGNOSTICS_ON
