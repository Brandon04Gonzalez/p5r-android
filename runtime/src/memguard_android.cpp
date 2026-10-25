// Memory guard (Android), as memguard_mac.cpp: the process's memory is checked every 250 ms, logged every 30 s, and
// past the limit the process ends at once, before the low-memory killer takes it (or the phone thrashes). The limit is
// NX_MEM_LIMIT_GB (config.txt), else 60% of the phone's RAM.
//
// What it reads (NX_MEMGUARD, docs/nvn/android-perf/PHASE4.md H0/H1):
//   statm (default)  resident pages from /proc/self/statm: the kernel's counters, no page-table walk. Resident >= Pss,
//                    so the guard stays on the safe side.
//   smaps            Pss from /proc/self/smaps_rollup, as before. That read walks every page-table entry of the
//                    multi-GB guest heap under the mm lock: 38-63 ms on a Galaxy S23, and every mprotect, mmap,
//                    munmap and page fault of the game waits behind it. It was the course's 45-60 ms main-thread
//                    hitch every ~300 ms. Never read it periodically on Android; this mode is for memory studies.
//   ab               alternates smaps and statm every 160 reads (~40 s), for an A/B inside one run.
//   off              no guard.
// Each read is timed; memguard_overlap_ns tells how much of a time window a read covered (the hitch lines).
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <nx/runtime.h>

#include "platform.h"

namespace nx::plat {

namespace {

uint64_t smaps_pss_bytes() {
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "Pss:", 4) == 0) {
            kb = std::strtoull(line + 4, nullptr, 10);
            break;
        }
    std::fclose(f);
    return kb << 10;
}

uint64_t statm_resident_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    if (std::fscanf(f, "%llu %llu", &size, &resident) != 2) resident = 0;
    std::fclose(f);
    return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
}

// Cheap counters (no page-table walk): VmSwap/RssAnon/RssFile from /proc/self/status, MemAvailable/SwapFree from
// /proc/meminfo. Logged every 30 s next to the resident figure (MEMORY.md item 11).
uint64_t kb_field(const char* path, const char* key) {
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    char line[256];
    size_t n = std::strlen(key);
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, key, n) == 0) {
            kb = std::strtoull(line + n, nullptr, 10);
            break;
        }
    std::fclose(f);
    return kb;
}

struct Read {
    std::atomic<uint64_t> begin{0}, end{0};
};
Read g_reads[16];
std::atomic<uint32_t> g_read_next{0};

}  // namespace

uint64_t memguard_overlap_ns(uint64_t from, uint64_t to) {
    uint64_t total = 0;
    for (Read& r : g_reads) {
        uint64_t b = r.begin.load(std::memory_order_relaxed), e = r.end.load(std::memory_order_relaxed);
        if (!e || e <= from || b >= to) continue;
        total += (e < to ? e : to) - (b > from ? b : from);
    }
    return total;
}

void start_memory_guard() {
    const char* e = std::getenv("NX_MEM_LIMIT_GB");
    double ram = double(sysconf(_SC_PHYS_PAGES)) * double(sysconf(_SC_PAGESIZE)) / double(1ull << 30);
    double gb = e ? std::atof(e) : 0.0;
    if (gb <= 0) gb = ram > 0 ? ram * 0.6 : 6.0;
    const uint64_t limit = uint64_t(gb * double(1ull << 30));
    const char* m = std::getenv("NX_MEMGUARD");
    const std::string mode = m ? m : "statm";
    if (mode == "off") {
        log("memory: guard off (NX_MEMGUARD=off)");
        return;
    }
    log("memory: guard at %.1f GB (the phone has %.1f GB), source %s", gb, ram, mode.c_str());
    std::thread([limit, gb, mode] {
        uint64_t ticks = 0, peak = 0, read_ns = 0, read_max = 0;
        bool smaps = mode == "smaps" || mode == "ab";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (mode == "ab" && ticks && ticks % 160 == 0) {
                smaps = !smaps;
                log("memory: guard source %s -> %s", smaps ? "statm" : "smaps", smaps ? "smaps" : "statm");
            }
            Read& r = g_reads[g_read_next.fetch_add(1) % 16];
            uint64_t t0 = monotonic_ns();
            r.end = 0, r.begin = t0;
            uint64_t f = smaps ? smaps_pss_bytes() : statm_resident_bytes();
            uint64_t t1 = monotonic_ns();
            r.end = t1;
            read_ns += t1 - t0, read_max = std::max(read_max, t1 - t0);
            if (f > peak) peak = f;
            if (++ticks % 120 == 0) {
                log("memory: %.2f GB %s (peak %.2f GB, limit %.1f GB), read avg %.2f ms, max %.2f ms",
                    double(f) / double(1ull << 30), smaps ? "Pss (smaps)" : "resident (statm)",
                    double(peak) / double(1ull << 30), gb, double(read_ns) / 120e6, double(read_max) / 1e6);
                read_ns = read_max = 0;
                log("memory: anon %llu MB, file %llu MB, swapped %llu MB (VmSwap) | system: MemAvailable %llu MB, SwapFree %llu MB",
                    (unsigned long long)(kb_field("/proc/self/status", "RssAnon:") >> 10),
                    (unsigned long long)(kb_field("/proc/self/status", "RssFile:") >> 10),
                    (unsigned long long)(kb_field("/proc/self/status", "VmSwap:") >> 10),
                    (unsigned long long)(kb_field("/proc/meminfo", "MemAvailable:") >> 10),
                    (unsigned long long)(kb_field("/proc/meminfo", "SwapFree:") >> 10));
            }
            if (f > limit) {
                log("memory: %.2f GB is over the %.1f GB limit (NX_MEM_LIMIT_GB): stopping",
                    double(f) / double(1ull << 30), gb);
                _exit(3);
            }
        }
    }).detach();
}

}  // namespace nx::plat
