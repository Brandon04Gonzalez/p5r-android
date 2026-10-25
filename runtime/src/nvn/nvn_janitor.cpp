// A janitor thread for work that need not hold up the replay (docs/nvn/android-perf/PHASE4.md C2b): resetting a
// retired slot's command pool and descriptor pool. On Adreno vkResetCommandPool frees the driver's per-pass objects,
// ~1.2 ms per course frame on the replay thread. A retired slot is not touched again until frame_begin takes it, so
// its pools can be reset here meanwhile (Vulkan's external synchronisation holds: one thread at a time), and
// frame_begin waits for the slot's flag, which is normally set long before. NX_JANITOR=0: the resets stay inline.
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

namespace {

std::mutex g_lock;
std::condition_variable g_cv, g_done_cv;
std::deque<std::function<void()>> g_jobs;
bool g_started = false;

void run() {
    plat::set_thread_name("nvn janitor");
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(g_lock);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
        {
            std::lock_guard lk(g_lock);  // pairs with janitor_wait's predicate check
        }
        g_done_cv.notify_all();
    }
}

}  // namespace

bool janitor_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_JANITOR");
        return !(e && e[0] == '0');
    }();
    return on;
}

void janitor_run(std::function<void()> job) {
    {
        std::lock_guard lk(g_lock);
        if (!g_started) {
            g_started = true;
            std::thread(run).detach();
        }
        g_jobs.push_back(std::move(job));
    }
    g_cv.notify_one();
}

void janitor_wait(const std::atomic<bool>& done) {
    for (int k = 0; k < 200 && !done.load(std::memory_order_acquire); k++) plat::yield();  // normally done already
    if (done.load(std::memory_order_acquire)) return;
    std::unique_lock lk(g_lock);
    g_done_cv.wait(lk, [&] { return done.load(std::memory_order_acquire); });
}

}  // namespace nx::nvn
