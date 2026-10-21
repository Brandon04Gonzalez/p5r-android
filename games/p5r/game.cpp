// Persona 5 Royal (01005CA01580E000, 1.0.0, BID 708BCACA873EA32B): what the shared runtime needs to know about the
// game, and what only P5R needs. The game draws through NVN (Atlus' GFD engine); its shaders live in the GFD shader
// caches BASE/GFDNXHHPRESET*.GSC of ALL_USEU.CPK (tools/gsc_shaders.py). Notes: games/p5r/PROGRESO.md.
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

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
