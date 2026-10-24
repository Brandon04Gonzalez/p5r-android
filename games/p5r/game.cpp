// Persona 5 Royal (01005CA01580E000, 1.0.0, BID 708BCACA873EA32B): what the shared runtime needs to know about the
// game, and what only P5R needs. The game draws through NVN (Atlus' GFD engine); its shaders live in the GFD shader
// caches BASE/GFDNXHHPRESET*.GSC of ALL_USEU.CPK (tools/gsc_shaders.py). Notes: games/p5r/PROGRESO.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"p5r", "Persona 5 Royal", 0x01005CA01580E000ull, "io.github.p5rrecomp",
                                "private/p5r", 0 /* main.npdm: no system resource (SDK 13) */};

namespace {

// glslc, NVN's run-time GLSL compiler, is linked against but the game ships its shaders precompiled (the GSC
// caches). Initialize fails, so nothing is ever compiled.
void glslc_fail(CPU& c) {
    log("p5r: %s called: run-time shader compilation is not supported", hle_current_name());
    c.x[0] = 0;
}

void glslc_nop(CPU& c) { c.x[0] = 0; }

// The engine's assertion handler (main+1028d70, "needle" Rsdx and friends) formats its message and fputs it to
// stderr right before it traps; the SDK's stdio goes nowhere. Copy every fputs line into the log, then do the real
// one.
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

// The frame rate. The game's target frame rate is [[main+0x2209f50]+0x84]: 30 on the Switch, read through main+f73730
// all over the game and set by main+f73740(fps), which also resets the engine's frame timer ([[main+0x220a9c0]]).
// Every frame the game's main thread (main+fd4140) sets the engine's frame interval to 60 / it through a virtual setter
// (main+1048ee0: vsyncs per frame, 1..4, stored at obj+0x30), which the present (main+1025c9c) hands to
// nvnWindowSetPresentInterval. The launcher's 60 fps (NX_FPS_CAP=60) raises the cap to 60 from that setter: on the PC
// the game then ran at 45-60 fps instead of 30. (The Atmosphere cheat that ships with the dump writes
// [[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14, a chain that is empty in this port.)
//
// Most of the game steps 1 / target per frame, so under the target it runs slow: at ~10 fps the ATLUS logos took 35 s
// instead of 15 (with a target of 10, 17 s). On the Adreno 610 (~8-13 fps in the 3D title and the casino's cutscenes)
// Joker's fall to the title menu and the police corner played at 30-45 % speed while their music and voices kept real
// time: the sound "ahead". So the target follows the frame rate the device reaches (paced_target), between
// NX_P5R_MIN_FPS (10) and the cap; the present interval stays the cap's, or a lower target would lower the cap too and
// never come back up. NX_P5R_AUTO_FPS=0: the old fixed target (the cap). NX_P5R_FPS=<5..60>: a fixed target with its
// own interval, to study. NX_P5R_TRACE_INTERVAL=1: the setter's calls are logged.
Fn g_interval_setter = nullptr;
uint64_t g_main_base = 0;
constexpr double kTicks = 19.2e6;  // the system tick the engine times its frames with

int target_fps() {
    uint64_t p = ld<uint64_t>(g_main_base + 0x2209f50);
    return p ? int(ld<uint32_t>(p + 0x84)) : 0;
}

void set_target_fps(CPU& c, int fps) {
    CPU saved = c;
    c.x[0] = uint32_t(fps);
    call(c, g_main_base + 0xf73740);
    c = saved;
}

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

int forced_fps() {
    static const int fps = [] {
        const int v = env_int("NX_P5R_FPS", 0);
        return v >= 5 && v <= 60 ? v : 0;
    }();
    return fps;
}

int cap_fps() {
    static const int fps = env_int("NX_FPS_CAP", 30) >= 60 ? 60 : 30;
    return fps;
}

bool auto_fps() {
    static const bool on = env_int("NX_P5R_AUTO_FPS", 1) != 0;
    return on;
}

// The target for the frames the device makes, chosen every frame so the game's steps (1 / target each) add up to the
// real time. A debt (real time minus steps; a frame counts at most the slowest step, 1 / NX_P5R_MIN_FPS, so a load's
// hitch is not made up) and the expected frame period (smoothed) give the next step: period + debt / 4, as a whole
// frame rate between NX_P5R_MIN_FPS (10) and the cap. The first version (a half-second window's mean with a dead band)
// left the target 0.5-1 fps over the device's on the tablet: the game ran at 94-97 % of real time at 15-24 fps and the
// sound still drifted ahead through a scene. `period`: the frame that just ended, in ticks; `used`: its target.
int paced_target(uint64_t period, int used) {
    static const int cap = cap_fps(), lo = std::clamp(env_int("NX_P5R_MIN_FPS", 10), 5, cap);
    static double debt = 0, expect = 0;
    const double r = std::min(double(period) / kTicks, 1.0 / lo);
    expect = expect > 0 ? expect + (r - expect) * 0.25 : r;
    if (used > 0) debt = std::clamp(debt + r - 1.0 / used, -0.15, 0.15);
    const double step = std::max(expect + debt * 0.25, 1.0 / cap);
    return std::clamp(int(1.0 / step + 0.5), lo, cap);
}

// The game clock. The engine's loop (main+fe75ac) times each frame with the system tick, clamps it to the frame
// timer's [[main+0x220a9c0]+0x34] seconds (0.05, set at boot by main+ce39ac) and stores it in [[main+0x2209f50]+0x50]
// (ticks; +0x48 is the frame's start), which main+f730c0 hands to the game's update as seconds. The parts of the game
// that use it ran slow under 20 fps whatever the target: the clamp is raised to NX_P5R_MAX_FRAME_MS (150 by default,
// real time down to ~7 fps; 50 = the game's own), longer frames (loading hitches) are still cut. The log's
// `p5r: game clock` says when the game ran under real time: by that clock, and by its steps (frames / target).
// NX_P5R_SLOW_MS=<ms>: every frame sleeps that long (a slow phone on the PC). Returns the real length of the frame
// that just started, in ticks, once per frame (0 otherwise).
float max_frame_s() {
    static const float s = float(std::clamp(env_int("NX_P5R_MAX_FRAME_MS", 150), 50, 500)) / 1000.0f;
    return s;
}

uint64_t game_clock() {
    if (uint64_t t = ld<uint64_t>(ld<uint64_t>(g_main_base + 0x220a9c0))) {
        uint32_t bits;
        const float want = max_frame_s();
        std::memcpy(&bits, &want, 4);
        if (const uint32_t old = ld<uint32_t>(t + 0x34); old != bits) {
            float was;
            std::memcpy(&was, &old, 4);
            log("p5r: longest frame the game clock follows %.0f -> %.0f ms", double(was) * 1e3, double(want) * 1e3);
            st<uint32_t>(t + 0x34, bits);
        }
    }
    static const int slow_ms = env_int("NX_P5R_SLOW_MS", 0);
    if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));

    // A frame is a new start in +0x48: its real length is the step between starts, the game's is +0x50.
    const uint64_t g = ld<uint64_t>(g_main_base + 0x2209f50);
    if (!g) return 0;
    static uint64_t last_start = 0, real = 0, game = 0, frames = 0, cut = 0, longest = 0;
    static double steps = 0;
    static int lowest = 0, highest = 0;
    const uint64_t start = ld<uint64_t>(g + 0x48);
    if (start == last_start) return 0;
    uint64_t r = 0;
    if (last_start && start > last_start) {
        const uint64_t d = ld<uint64_t>(g + 0x50);
        r = start - last_start;
        real += r, game += d, frames++;
        if (const int fps = target_fps(); fps > 0) {
            steps += kTicks / fps;
            lowest = lowest ? std::min(lowest, fps) : fps, highest = std::max(highest, fps);
        }
        if (d + 19200 < r) cut++;
        longest = std::max(longest, r);
    }
    last_start = start;
    if (real < 10 * uint64_t(kTicks)) return r;
    // Logged when the game is off real time or the device under the cap (the target then moves).
    const double fps = double(frames) * kTicks / double(real);
    if (game * 100 < real * 97 || std::abs(steps / double(real) - 1.0) > 0.02 || fps < cap_fps() * 0.95)
        log("p5r: game clock %.1f s, steps %.1f s in %.1f s real, %.1f fps (target %d-%d), %llu of %llu frames cut "
            "(longest %.0f ms)", double(game) / kTicks, steps / kTicks, double(real) / kTicks, fps, lowest, highest,
            (unsigned long long)cut, (unsigned long long)frames, double(longest) * 1e3 / kTicks);
    real = game = frames = cut = longest = 0, steps = 0, lowest = highest = 0;
    return r;
}

void interval_setter_hook(CPU& c) {
    const uint64_t period = game_clock();
    const int now = target_fps();
    int fps = forced_fps();
    const int interval = fps ? std::clamp(60 / fps, 1, 4) : 60 / cap_fps();
    if (!fps) fps = !auto_fps() ? cap_fps() : period ? paced_target(period, now) : now;
    if (fps > 0 && now > 0 && now != fps) {
        static int logged = 0;  // the automatic target moves every frame: only the first changes
        if (logged++ < 3) log("p5r: target frame rate %d -> %d%s", now, fps, auto_fps() && !forced_fps() ? " (follows the device)" : "");
        set_target_fps(c, fps);
    }
    c.x[1] = uint32_t(interval);
    static int traced = 0;
    if (std::getenv("NX_P5R_TRACE_INTERVAL") && traced++ < 40)
        log("p5r: frame interval set to %u on %llx from %s", uint32_t(c.x[1]), (unsigned long long)c.x[0], describe(c.x[30]).c_str());
    g_interval_setter(c);
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
    // P5R never binds a storage image: colour images without STORAGE keep Adreno's framebuffer compression
    // (nvn_image.cpp). config.txt's NX_IMAGE_STORAGE=1 brings it back.
#if defined(_WIN32)
    if (!std::getenv("NX_IMAGE_STORAGE")) _putenv_s("NX_IMAGE_STORAGE", "0");
#else
    setenv("NX_IMAGE_STORAGE", "0", 0);
#endif
    if (const LoadedModule* m = module_at(find_symbol("nnMain")); m && m->table[0x1048ee0 / 4]) {
        g_main_base = m->base;
        g_interval_setter = m->table[0x1048ee0 / 4];
        m->table[0x1048ee0 / 4] = interval_setter_hook;
    } else {
        log("p5r: nnMain or the frame interval setter not found: the game's own 30 fps");
    }
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
