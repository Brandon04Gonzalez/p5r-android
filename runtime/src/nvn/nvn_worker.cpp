// The replay thread (NX_REPLAY_THREAD=0 turns it off). nvnQueueSubmitCommands and nvnQueuePresentTexture used to
// replay into Vulkan and wait for the GPU on the game's render thread, and the game's main thread waits for that
// thread every frame: the frame was game logic + recording + replay + GPU, one after the other (~30 fps in a course
// with every part under 16 ms; docs/nvn/DEBUG.md #40). Like a real GPU, the work now goes to a queue that one host
// thread replays in order; the game only waits where NVN makes it wait: nvnSyncWait on a fenced sync, nvnQueueFinish,
// memory pool invalidation and finalization, and a present queue at most two presents deep.
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "nvn.h"
#include "platform.h"

namespace nx::nvn {

namespace {

std::mutex g_lock;
std::condition_variable g_cv, g_done_cv;
std::deque<std::pair<uint64_t, std::function<void()>>> g_jobs;
uint64_t g_enqueued = 0, g_done = 0;
bool g_started = false;

bool enabled() {
    static const bool on = [] {
        // Off unless NX_REPLAY_THREAD=1: with the replay a frame behind, the course's render passes multiplied
        // (1.3 M ended by target changes against 125 k) and it ran slower (docs/nvn/DEBUG.md #40).
        const char* e = std::getenv("NX_REPLAY_THREAD");
        return e && e[0] == '1';
    }();
    return on;
}

void run() {
    plat::set_thread_name("nvn replay");
    for (;;) {
        std::pair<uint64_t, std::function<void()>> job;
        {
            std::unique_lock lk(g_lock);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job.second();
        {
            std::lock_guard lk(g_lock);
            g_done = job.first;
        }
        g_done_cv.notify_all();
    }
}

}  // namespace

uint64_t worker_run(std::function<void()> job) {
    if (!enabled()) {
        job();
        return 0;
    }
    uint64_t serial;
    {
        std::lock_guard lk(g_lock);
        if (!g_started) {
            g_started = true;
            std::thread(run).detach();
        }
        serial = ++g_enqueued;
        g_jobs.push_back({serial, std::move(job)});
    }
    g_cv.notify_one();
    return serial;
}

void worker_wait(uint64_t serial) {
    if (!serial) return;
    std::unique_lock lk(g_lock);
    g_done_cv.wait(lk, [&] { return g_done >= serial; });
}

uint64_t worker_last() {
    std::lock_guard lk(g_lock);
    return g_enqueued;
}

void worker_finish() { worker_wait(worker_last()); }

}  // namespace nx::nvn
