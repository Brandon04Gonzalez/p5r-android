// Persona 5 Royal (01005CA01580E000, 1.0.0, BID 708BCACA873EA32B): what the shared runtime needs to know about the
// game, and what only P5R needs. The game draws through NVN (Atlus' GFD engine); its shaders live in the GFD shader
// caches BASE/GFDNXHHPRESET*.GSC of ALL_USEU.CPK (tools/gsc_shaders.py). Notes: games/p5r/PROGRESO.md.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
// nvnWindowSetPresentInterval. The launcher's 60 fps (NX_FPS_CAP=60) sets the target to 60 from that setter: on the PC
// the game then ran at 45-60 fps instead of 30. (The Atmosphere cheat that ships with the dump writes
// [[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14, a chain that is empty in this port.) NX_P5R_FPS=<15..60>: any target, to
// study. NX_P5R_TRACE_INTERVAL=1: the setter's calls are logged.
Fn g_interval_setter = nullptr;
uint64_t g_main_base = 0;

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

int wanted_fps() {
    static const int fps = [] {
        const char* forced = std::getenv("NX_P5R_FPS");
        const char* cap = std::getenv("NX_FPS_CAP");
        int v = forced ? std::atoi(forced) : cap && std::atoi(cap) >= 60 ? 60 : 0;
        return v >= 15 && v <= 60 && 60 % v == 0 ? v : 0;  // 15, 20, 30 or 60: whole vsyncs per frame
    }();
    return fps;
}

void interval_setter_hook(CPU& c) {
    if (const int fps = wanted_fps()) {
        if (target_fps() != fps) {
            log("p5r: target frame rate %d -> %d", target_fps(), fps);
            set_target_fps(c, fps);
        }
        c.x[1] = uint32_t(60 / fps);
    }
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
