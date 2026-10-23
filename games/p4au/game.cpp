// Persona 4 Arena Ultimax (010075A016A3A000, 1.0.0 USA): what the shared runtime needs to know about the game, and
// what only P4AU needs. Arc System Works' engine on nn::gfx over NVN (SDK 12.3.3); one BNSH per shader stage in
// romfs data/shader (games/p4au/tools/bnsh_shaders.py); films are data/movie*/*.mp4 through nn::movie (subsdk0).
// Notes: games/p4au/PROGRESO.md.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <nx/game.h>
#include <nx/runtime.h>

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
    // P4AU never sets the window origin: NVN's default, lower-left (nvn_device.cpp).
#if defined(_WIN32)
    if (!std::getenv("NX_ORIGIN_MODE")) _putenv_s("NX_ORIGIN_MODE", "0");
#else
    setenv("NX_ORIGIN_MODE", "0", 0);
#endif
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    if (!m) return;
    g_main = m->base;
    if (std::getenv("NX_P4AU_DEBUG")) {
        hook(m, 0xb1470, g_orig_b1470, hook_b1470);
        hook(m, 0x563c, g_orig_563c, hook_563c);
    }
}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
