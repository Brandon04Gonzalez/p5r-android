// Unified, low-overhead diagnostics for the Android performance/audio investigation (NX_DIAGNOSTICS builds).
//
// All producers only update atomics or plain counters; the periodic summary is logged from the presenting thread
// (nvn_queue.cpp), never from the real-time AAudio callback. Timestamps come from plat::monotonic_ns(), the same clock
// used by the audio, movie and render paths, so the maxima in one summary line can be lined up with the others.
#pragma once

#include <cstdint>

#if defined(NX_DIAGNOSTICS) && NX_DIAGNOSTICS
#define NX_DIAGNOSTICS_ON 1
#else
#define NX_DIAGNOSTICS_ON 0
#endif

namespace nx::diag {

#if NX_DIAGNOSTICS_ON

constexpr bool kEnabled = true;

// Periodic summary (every ~2 s) from the presenting thread. Samples process memory cheaply (statm) when it is due.
void tick(uint64_t now);

// Video path (hle_movie.cpp). Durations in ns, `at` is the monotonic timestamp of the sample.
void movie_pump(uint64_t ns, uint64_t at);
void movie_convert(uint64_t ns, uint64_t at);
void movie_copy(uint64_t ns, uint64_t at);
void movie_send_wait(uint64_t ns, uint64_t at);
void movie_buffers(uint64_t output_slots, uint64_t frame_bytes, uint64_t input_bytes, uint64_t stride, uint64_t rows);
void movie_frame_alloc(uint64_t size, uint64_t capacity);
void movie_queues(uint64_t queued, uint64_t ready, uint64_t busy);

// g_render.lock (hle_audio.cpp). Every acquisition records its wait and its hold time.
void render_lock_wait(uint64_t ns, uint64_t at);
void render_lock_hold(uint64_t ns, uint64_t at);

// AAudio callback (platform_android_audio.cpp). Called after the callback's work; no logging happens there.
void audio_callback(uint64_t start_ns, uint64_t end_ns, int32_t frames, uint64_t missing_frames, uint64_t occupancy,
                    uint64_t pending);
void audio_submit(uint64_t pending, bool truncated);

#else

constexpr bool kEnabled = false;

inline void tick(uint64_t) {}
inline void movie_pump(uint64_t, uint64_t) {}
inline void movie_convert(uint64_t, uint64_t) {}
inline void movie_copy(uint64_t, uint64_t) {}
inline void movie_send_wait(uint64_t, uint64_t) {}
inline void movie_buffers(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
inline void movie_frame_alloc(uint64_t, uint64_t) {}
inline void movie_queues(uint64_t, uint64_t, uint64_t) {}
inline void render_lock_wait(uint64_t, uint64_t) {}
inline void render_lock_hold(uint64_t, uint64_t) {}
inline void audio_callback(uint64_t, uint64_t, int32_t, uint64_t, uint64_t, uint64_t) {}
inline void audio_submit(uint64_t, bool) {}

#endif

}  // namespace nx::diag
