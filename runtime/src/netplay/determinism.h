// Deterministic mode for lockstep netplay. Lockstep feeds both devices the same input
// for the same presented frame (nx::g_frame) and expects the same game state on that frame, so nothing the game's
// logic reads may depend on the host's speed: the wall clock, the order threads finish in, audio played in real time.
//
// NX_DETERMINISTIC=1 (off by default; the netplay session will turn it on) turns on what this needs: the main
// thread's clock follows the presented frames, loads finish before the next frame, the calendar clock and
// GenerateRandomBytes are fixed per session (NX_DET_SEED, the same on both peers). With it off, nothing here changes
// how the game runs. Tools, in either mode: NX_DET_TRACE=1 counts the guest callers of the host-dependent sources (the
// audit), NX_DET_HASH_EVERY=<n> logs the state hash every n frames, NX_DET_SNAP=<frame>,... writes main's image to
// <data root>/snap_<frame>.bin (to find state by comparing snapshots).
#pragma once

#include <cstdint>

#include <nx/cpu.h>

namespace nx {
struct KThread;
}

namespace nx::det {

bool enabled();  // NX_DETERMINISTIC=1, or a netplay session (start_session)
// Whether deterministic mode may be turned on in this process: NX_DETERMINISTIC=1, or a netplay session configured
// (net_host / net_join). Known before any guest code runs, unlike enabled(): native execution (nce.cpp) decides with it
// at load time whether the game's own counter reads go through system_tick().
bool requested();
bool tracing();  // NX_DET_TRACE=1
// A netplay session runs (netplay.cpp): deterministic mode on with the session's seed, whatever NX_DETERMINISTIC and
// NX_DET_SEED say. Call before the guest starts (the seed moves the boot clock and seeds GenerateRandomBytes).
void start_session(uint64_t seed);

// Census: calls of `source` per thread and guest caller chain (lr and three frame-pointer levels); each new chain is
// logged with its frame, and the counts every 600 frames.
void note(const char* source, CPU& c);

// The system tick the guest reads (nn::os::GetSystemTick, svcGetSystemTick, CNTPCT/CNTVCT): in deterministic mode,
// on the game's main thread, a clock of the presented frames (1/60 s per frame, plus one tick per read so a loop
// polling it still sees it move). Otherwise, and on other threads, `host_tick`.
uint64_t system_tick(uint64_t host_tick);
void set_main_thread(KThread* t);  // the first guest thread (kernel.cpp)

// A guest thread opens or reads a file (hle_fs.cpp): in deterministic mode it becomes a loader, which each present
// waits for until it is idle (NX_DET_LOAD_WAIT_MS, 5000, bounds one wait).
void file_io();
// svcStartThread started guest thread `id` (kernel.cpp): in deterministic mode, when the main thread started it, the
// main thread waits until the new thread is idle, so its first run (P4AU's save workers mount the save and open
// bbsave.dat) ends at the same point of the main thread's frame on every device (NX_DET_START_WAIT_MS, 200).
void thread_started(uint64_t id);
// Whether guest thread `id` is idle: blocked with nothing pending that would wake it, or exited (kernel.cpp).
bool thread_idle(uint64_t id);

// After each present, once g_frame has its new value (nvnQueuePresentTexture, on the game thread).
void on_present();

}  // namespace nx::det

namespace nx::netplay {

// The game's state that both peers must agree on at the same presented frame, as 32-bit words: the game layer
// registers a reader that fills up to `max` words and returns how many (games/p4au/game.cpp: the round clock, both
// characters' positions and health, the random number generators). Call on the game thread, between frames (from
// a present), so the words belong to one frame.
constexpr int kMaxStateWords = 256;
using StateReader = int (*)(uint32_t* words, int max);
void set_state_reader(StateReader r);
int state_words(uint32_t* words, int max);  // 0 when no reader is registered

// FNV-1a (64-bit) of the state words: what N3 exchanges between peers to detect a desync. 0 without a reader.
// NX_DET_HASH_EVERY=<n> logs it every n presented frames: "det: frame F state H (w0 w1 w2 w3) w4 ... w15"
// (games/p4au/tools/run_pc.ps1 -CompareTo compares two runs' lines); NX_DET_HASH_WORDS=1 adds every word in hex
// ("det: frame F words N: ...", to see which word a desync is in).
uint64_t state_hash();

}  // namespace nx::netplay
