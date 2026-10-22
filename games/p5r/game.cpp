// Persona 5 Royal (01005CA01580E000, 1.0.0, BID 708BCACA873EA32B): what the shared runtime needs to know about the
// game, and what only P5R needs. The game draws through NVN (Atlus' GFD engine); its shaders live in the GFD shader
// caches BASE/GFDNXHHPRESET*.GSC of ALL_USEU.CPK (tools/gsc_shaders.py). Notes: games/p5r/PROGRESO.md.
#include <chrono>
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

// The frame rate (the launcher's "FPS": NX_FPS_CAP=60; 30 is the game's own and the default). The engine keeps its
// frame interval at [[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14: 1 = 60 fps, 2 = 30 (the Atmosphere cheat that ships
// with the dump: Breeze beta53, 1.0.0 BID 708BCACA873EA32B). The chain only exists once the engine is up and the game
// may write it back on scene changes, so a host thread rewrites it twice a second.
bool readable(uint64_t p) {
    if (!p || (p & 7)) return false;
    VmInfo v = vm_query(p);
    return v.state != 0 && (v.perm & 1);
}

void fps60_thread(uint64_t main_base) {
    log("p5r: 60 fps (NX_FPS_CAP=60): patching the frame interval");
    bool said = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint64_t p = main_base + 0x2327B60;
        bool ok = true;
        static int dbg = 0;
        std::string trace;
        for (uint64_t off : {0ull, 0x10ull, 0x60ull, 0x8ull}) {
            VmInfo v = vm_query(p + off);
            char b[96];
            std::snprintf(b, sizeof b, " [%llx st %u perm %u]", (unsigned long long)(p + off), v.state, v.perm);
            trace += b;
            if (!readable(p + off)) {
                ok = false;
                break;
            }
            p = ld<uint64_t>(p + off);
        }
        if (dbg++ % 20 == 0) log("p5r dbg: chain%s -> %llx", trace.c_str(), (unsigned long long)p);
        if (!ok || !p || !readable((p + 0xF14) & ~7ull)) continue;
        if (ld<uint32_t>(p + 0xF14) != 1) {
            if (!said) log("p5r: frame interval %u -> 1 at %llx", ld<uint32_t>(p + 0xF14), (unsigned long long)(p + 0xF14)), said = true;
            st<uint32_t>(p + 0xF14, 1);
        }
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
    const char* cap = std::getenv("NX_FPS_CAP");
    if (cap && std::atoi(cap) >= 60) {
        const LoadedModule* m = module_at(find_symbol("nnMain"));
        if (m) std::thread(fps60_thread, m->base).detach();
        else log("p5r: nnMain not found: no 60 fps patch");
    }
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
