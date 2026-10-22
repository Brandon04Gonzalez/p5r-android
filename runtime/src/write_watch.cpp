// Write watches (docs/nvn/android-perf/PLAN.md R4): whether the CPU wrote a range since it was last asked, from page
// protection instead of hashing. A watched range is mapped read-only; the first write into one of its pages faults,
// the crash handler (platform_android.cpp, platform_mac.cpp) hands the address here first, and the page is made
// writable again with the range marked written, so the store retries and succeeds. write_watch_take re-protects the
// range and then reads the mark: a store before the re-protection found the mark already set (its page was writable
// only because a fault set it), one after it faults again.
//
// For Wonder's glyph cache (a 16 MB pitch-linear texture the CPU writes without a flush), which was hashed in full
// every 4th frame (~5 ms on a phone, a spike every 4 frames). Only for game code's stores: the kernel writing into a
// read-only page fails the syscall instead (EFAULT), so it is kept to textures, and dropped with them. POSIX only;
// elsewhere (and with NX_WRITE_WATCH=0) write_watch_add fails and callers hash as before.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "platform.h"

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace nx::plat {

#if !defined(_WIN32)
namespace {

struct Watch {
    std::atomic<uint64_t> addr{0};  // the caller's address (0: a free slot)
    std::atomic<uint64_t> begin{0}, end{0};  // the pages it covers
    uint64_t size = 0;                       // the caller's size
    std::atomic<bool> written{false};
    // The pages written since the last take, as the first and the end of them (UINT64_MAX / 0: none known). Set before
    // `written`, so a take that sees the flag also sees the range; one that races a fault finds the range alone.
    std::atomic<uint64_t> lo{UINT64_MAX}, hi{0};
};
constexpr int kWatches = 16;
Watch g_watches[kWatches];
// Add, remove and take one at a time. A take on the replay thread re-protected a range the game's thread had just
// removed (its texture finalized): the pages stayed read-only with no watch, and the game's next store into them
// was a crash (P5R's video frames: a texture over the same memory made and dropped every frame). The fault handler
// takes no lock: remove makes the pages writable before it forgets the watch, so a fault still finds it.
std::mutex g_watch_lock;
// Ranges whose watch was removed lately. A store can fault on a watched page and reach the handler only after another
// thread removed that watch (pages made writable, watch forgotten): the handler finds no watch, but the address is
// in a retired range, so the store is retried instead of being a crash.
struct Retired {
    std::atomic<uint64_t> begin{0}, end{0};
};
constexpr uint32_t kRetired = 64;
Retired g_retired[kRetired];
std::atomic<uint32_t> g_retired_next{0};

uint64_t page_size() {
    static const uint64_t p = uint64_t(sysconf(_SC_PAGESIZE));
    return p;
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_WRITE_WATCH");
        return !(e && e[0] == '0');
    }();
    return on;
}

}  // namespace

bool write_watch_add(uint64_t addr, uint64_t size) {
    if (!enabled() || !addr || !size) return false;
    std::lock_guard lk(g_watch_lock);
    uint64_t ps = page_size(), b = addr & ~(ps - 1), e = (addr + size + ps - 1) & ~(ps - 1);
    for (Watch& w : g_watches) {
        uint64_t expected = 0;
        if (!w.addr.compare_exchange_strong(expected, addr)) continue;
        w.begin = b, w.end = e, w.size = size, w.written = true;  // unknown until the first take: written
        w.lo = UINT64_MAX, w.hi = 0;
        if (mprotect(reinterpret_cast<void*>(b), e - b, PROT_READ) != 0) {
            w.addr = 0;
            return false;
        }
        return true;
    }
    return false;
}

void write_watch_remove(uint64_t addr) {
    std::lock_guard lk(g_watch_lock);
    for (Watch& w : g_watches)
        if (w.addr == addr) {
            mprotect(reinterpret_cast<void*>(w.begin.load()), w.end - w.begin, PROT_READ | PROT_WRITE);
            Retired& r = g_retired[g_retired_next++ % kRetired];
            r.begin = 0, r.end = w.end.load(), r.begin = w.begin.load();
            w.begin = w.end = 0;
            w.addr = 0;
        }
}

// The host is about to store into [addr, addr + size) (hle_movie.cpp: a decoded picture copied into the game's
// memory): the watches over it are marked written and those pages made writable with one call each, instead of one
// fault per page (760 for a 1080p NV12 frame, every frame of a film). A take between this and the stores re-protects
// them: the rest of the stores fault as usual.
void write_watch_will_write(uint64_t addr, uint64_t size) {
    if (!enabled() || !addr || !size) return;
    std::lock_guard lk(g_watch_lock);
    uint64_t ps = page_size(), b = addr & ~(ps - 1), e = (addr + size + ps - 1) & ~(ps - 1);
    for (Watch& w : g_watches) {
        if (!w.addr) continue;
        uint64_t lo = std::max<uint64_t>(b, w.begin), hi = std::min<uint64_t>(e, w.end);
        if (lo >= hi) continue;
        for (uint64_t cur = w.lo; lo < cur && !w.lo.compare_exchange_weak(cur, lo);) {}
        for (uint64_t cur = w.hi; hi > cur && !w.hi.compare_exchange_weak(cur, hi);) {}
        w.written = true;
        mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE);
    }
}

bool write_watch_take(uint64_t addr, uint64_t* lo, uint64_t* hi) {
    if (lo) *lo = 0, *hi = UINT64_MAX;  // unknown: all of it
    std::lock_guard lk(g_watch_lock);
    for (Watch& w : g_watches)
        if (w.addr == addr) {
            mprotect(reinterpret_cast<void*>(w.begin.load()), w.end - w.begin, PROT_READ);
            bool was = w.written.exchange(false);
            uint64_t a = w.lo.exchange(UINT64_MAX), b = w.hi.exchange(0);
            if (a < b) {  // the written pages, clipped to the caller's range and made relative to it
                a = a > addr ? a - addr : 0;
                b = std::min<uint64_t>(b - addr, w.size);
                if (lo) *lo = a, *hi = std::max(a, b);
            } else if (lo && !was) {
                *lo = *hi = 0;  // nothing written
            }
            return was || a < b;
        }
    return true;
}

bool write_watch_fault(void* at) {
    uint64_t a = reinterpret_cast<uint64_t>(at);
    bool hit = false;
    uint64_t page = a & ~(page_size() - 1);
    for (Watch& w : g_watches)
        if (w.addr && a >= w.begin && a < w.end) {
            for (uint64_t cur = w.lo; page < cur && !w.lo.compare_exchange_weak(cur, page);) {}
            for (uint64_t cur = w.hi; page + page_size() > cur && !w.hi.compare_exchange_weak(cur, page + page_size());) {}
            w.written = true, hit = true;
        }
    if (!hit)
        for (Retired& r : g_retired)
            if (a >= r.begin && a < r.end) hit = true;
    if (hit) mprotect(reinterpret_cast<void*>(a & ~(page_size() - 1)), page_size(), PROT_READ | PROT_WRITE);
    return hit;
}
#else
bool write_watch_add(uint64_t, uint64_t) { return false; }
void write_watch_remove(uint64_t) {}
void write_watch_will_write(uint64_t, uint64_t) {}
bool write_watch_take(uint64_t, uint64_t* lo, uint64_t* hi) {
    if (lo) *lo = 0, *hi = UINT64_MAX;
    return true;
}
bool write_watch_fault(void*) { return false; }
#endif

}  // namespace nx::plat
