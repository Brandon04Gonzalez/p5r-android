// Persona 4 Arena Ultimax (010075A016A3A000, 1.0.0 USA): what the shared runtime needs to know about the game, and
// what only P4AU needs. Arc System Works' engine on nn::gfx over NVN (SDK 12.3.3); one BNSH per shader stage in
// romfs data/shader (games/p4au/tools/bnsh_shaders.py); films are data/movie*/*.mp4 through nn::movie (subsdk0).
// Notes: games/p4au/PROGRESO.md.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#include <nx/game.h>
#include <nx/runtime.h>

#include "platform.h"

namespace nx {

const game::Info game::kInfo = {"p4au", "Persona 4 Arena Ultimax", 0x010075A016A3A000ull, "io.github.p4aurecomp",
                                "private/p4au", 0 /* main.npdm: no system resource (SDK 12) */};

namespace {

// glslc, NVN's run-time GLSL compiler, may be linked but the game ships its shaders precompiled.
void glslc_fail(CPU& c) {
    log("p4au: %s called: run-time shader compilation is not supported", hle_current_name());
    c.x[0] = 0;
}

void glslc_nop(CPU& c) { c.x[0] = 0; }

// Copy every fputs line into the log (assertion messages), then do the real one.
void fputs_logged(CPU& c) {
    static const uint64_t real = find_symbol("fputs");
    const char* s = reinterpret_cast<const char*>(c.x[0]);
    if (s) {
        std::string line(s, strnlen(s, 1024));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        log("[guest fputs] %s", line.c_str());
    }
    if (real) call(c, real);
    else c.x[0] = 0;
}

// ---- debugging hooks (NX_P4AU_DEBUG=1) ----
uint64_t g_main = 0;
Fn g_orig_b1470 = nullptr, g_orig_563c = nullptr;

void hook_b1470(CPU& c) {
    uint64_t single = g_main + 0x1298908, obj = ld<uint64_t>(single), vt = obj ? ld<uint64_t>(obj) : 0;
    log("p4au dbg: b1470(x0 %llx x1 %llx w2 %u) singleton+0 %llx vt %s [1] %s", (unsigned long long)c.x[0],
        (unsigned long long)c.x[1], uint32_t(c.x[2]), (unsigned long long)obj, describe(vt).c_str(),
        vt ? describe(ld<uint64_t>(vt + 8)).c_str() : "-");
    g_orig_b1470(c);
}

void hook_563c(CPU& c) {
    uint64_t o = c.x[2], vt = o ? ld<uint64_t>(o) : 0;
    log("p4au dbg: 563c(x0 %llx x1 %llx x2 %llx) [x2] %s [[x2]] %s", (unsigned long long)c.x[0],
        (unsigned long long)c.x[1], (unsigned long long)o, describe(vt).c_str(),
        vt ? describe(ld<uint64_t>(vt)).c_str() : "-");
    g_orig_563c(c);
}

void hook(const LoadedModule* m, uint32_t off, Fn& orig, Fn fn) {
    orig = m->table[off / 4];
    if (orig) m->table[off / 4] = fn;
    else log("p4au: no function at main+%x to hook", off);
}

// ---- call counting (NX_P4AU_HOOKS="29868,cc424,..."): who calls these main functions, with what, every 3 s ----
// Only calls through the function table (virtual and other indirect calls) are seen. Counted by (function, caller,
// x0, x1, result). Input chain: the npad device (vtable main+6489c8) has +0x18 Update (main+29418), +0x20
// IsPressed(bit) (main+29868) and +0x28 IsConnected (main+298a0); the button mappers above it (vtables main+64cb88,
// main+64cbf0) have IsPressed(logical button) at main+cc424 and main+cc740.
constexpr int kMaxHooks = 16;
uint32_t g_hook_off[kMaxHooks];
Fn g_hook_orig[kMaxHooks];
std::mutex g_in_m;
std::map<std::tuple<int, uint64_t, uint64_t, uint64_t, uint64_t>, uint64_t> g_in_counts;
uint64_t g_in_last = 0;

void in_count(int k, CPU& c, uint64_t lr, uint64_t obj, uint64_t arg) {
    std::lock_guard lk(g_in_m);
    g_in_counts[{k, lr, obj, arg, c.x[0]}]++;
    uint64_t now = plat::monotonic_ns();
    if (now - g_in_last < 3'000'000'000ull) return;
    g_in_last = now;
    log("p4au calls: %zu kinds in the last 3 s", g_in_counts.size());
    for (const auto& [key, n] : g_in_counts)
        log("  main+%x x0 %llx x1 %#llx -> %#llx  x%llu  from %s", g_hook_off[std::get<0>(key)],
            (unsigned long long)std::get<2>(key), (unsigned long long)std::get<3>(key),
            (unsigned long long)std::get<4>(key), (unsigned long long)n, describe(std::get<1>(key)).c_str());
    g_in_counts.clear();
}

template <int K>
void counted(CPU& c) {
    uint64_t lr = c.x[30], x0 = c.x[0], x1 = c.x[1];
    g_hook_orig[K](c);
    in_count(K, c, lr, x0, x1);
}

template <int... K>
constexpr std::array<Fn, sizeof...(K)> counted_fns(std::integer_sequence<int, K...>) {
    return {&counted<K>...};
}

void hook_counted(const LoadedModule* m, const char* list) {
    static constexpr auto fns = counted_fns(std::make_integer_sequence<int, kMaxHooks>{});
    int k = 0;
    for (const char* p = list; *p && k < kMaxHooks;) {
        char* end = nullptr;
        uint32_t off = uint32_t(std::strtoul(p, &end, 16));
        if (end == p) break;
        g_hook_off[k] = off;
        hook(m, off, g_hook_orig[k], fns[k]);
        if (g_hook_orig[k]) log("p4au: counting calls to main+%x", off), k++;
        p = *end ? end + 1 : end;
    }
}

// ---- menu focus (NX_P4AU_FOCUS=1): log the menu input gates whenever they change ----
// The menu input helpers (main+8b500 and its siblings) ignore a button unless the "input lock" word (singleton
// main+1ade8a8, +0x30) is 0 and strcmp(focus, caller's screen name) == 0. The focus is a stack of 32-byte names at
// main+1ade3c0 indexed by the int at main+1ade500, or the name at main+1ade508 when the int at main+1ade504 is set.
// Checked once a frame from the first npad device's Update (main+29418).
Fn g_focus_orig = nullptr;

void focus_check(CPU& c) {
    g_focus_orig(c);
    auto str = [](uint64_t a) { return std::string(reinterpret_cast<const char*>(a), strnlen(reinterpret_cast<const char*>(a), 32)); };
    int32_t idx = ld<int32_t>(g_main + 0x1ade500), flag = ld<int32_t>(g_main + 0x1ade504);
    std::string s = "index " + std::to_string(idx) + " override " + std::to_string(flag) + " [" + str(g_main + 0x1ade508) +
                    "] lock " + std::to_string(ld<int32_t>(g_main + 0x1ade8a8 + 0x30)) + " stack:";
    for (int k = 0; k <= idx && k < 10; k++) s += " [" + str(g_main + 0x1ade3c0 + 32ull * k) + "]";
    static std::string last;
    static const uint64_t t0 = plat::monotonic_ns();
    if (s != last) log("p4au focus at %.1f s: %s", (plat::monotonic_ns() - t0) / 1e9, s.c_str()), last = s;
    // The story select (object main+23bf0b8): state, chapter tab, cursor, zoom, the card under the cursor per tab,
    // and the first cards' screen positions (main+1c28c9c, 0xa8 apart) that the hover test (main+2a9df0) uses.
    if (std::getenv("NX_P4AU_STORY")) {
        uint64_t o = g_main + 0x23bf0b8, cards = g_main + 0x1c28c9c;
        char b[512];
        int n = std::snprintf(b, sizeof b, "state %d tab %d cursor %.0f,%.0f zoom %.2f hover %d %d %d %d cards:",
                              ld<int32_t>(o), ld<int32_t>(o + 0x3c), ld<float>(o + 0x78), ld<float>(o + 0x7c),
                              ld<float>(o + 0x1494), ld<int32_t>(o + 0x58), ld<int32_t>(o + 0x5c), ld<int32_t>(o + 0x60),
                              ld<int32_t>(o + 0x64));
        for (int k = 0; k < 4 && n < int(sizeof b) - 40; k++)
            n += std::snprintf(b + n, sizeof b - n, " %.0f,%.0f", ld<float>(cards + 0xa8ull * k), ld<float>(cards + 0xa8ull * k + 4));
        static std::string story;
        if (story != b) log("p4au story at %.1f s: %s", (plat::monotonic_ns() - t0) / 1e9, b), story = b;
    }
}

}  // namespace

NX_HLE("fputs", fputs_logged);

NX_HLE("glslcInitialize", glslc_fail);
NX_HLE("glslcCompile", glslc_fail);
NX_HLE("glslcCompilePreSpecialized", glslc_fail);
NX_HLE("glslcCompileSpecialized", glslc_fail);
NX_HLE("glslcFinalize", glslc_nop);
NX_HLE("glslcSetAllocator", glslc_nop);
NX_HLE("glslcGetDefaultOptions", glslc_nop);
NX_HLE("glslcGetVersion", glslc_nop);

void game::after_init() {
    // P4AU never sets the window origin: NVN's default, lower-left (nvn_device.cpp). In a fight it draws ~7 MB of
    // vertices a frame that are all the same as the frame before: the buffer cache across submits
    // (nvn_buffer_cache.cpp) keeps them on the GPU (7.3 -> 0.03 MB copied a frame). config.txt's
    // NX_BUFFER_CACHE=0 turns it off.
#if defined(_WIN32)
    if (!std::getenv("NX_ORIGIN_MODE")) _putenv_s("NX_ORIGIN_MODE", "0");
    if (!std::getenv("NX_BUFFER_CACHE")) _putenv_s("NX_BUFFER_CACHE", "1");
#else
    setenv("NX_ORIGIN_MODE", "0", 0);
    setenv("NX_BUFFER_CACHE", "1", 0);
#endif
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    if (!m) return;
    g_main = m->base;
    if (std::getenv("NX_P4AU_DEBUG")) {
        hook(m, 0xb1470, g_orig_b1470, hook_b1470);
        hook(m, 0x563c, g_orig_563c, hook_563c);
    }
    if (const char* list = std::getenv("NX_P4AU_HOOKS")) hook_counted(m, list);
    if (std::getenv("NX_P4AU_FOCUS")) hook(m, 0x29418, g_focus_orig, focus_check);
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
