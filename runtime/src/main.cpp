// Boot: map and link the recompiled modules (what rtld does), create the main thread and enter
// nn::init::Start(main thread handle, main module base, init, fini) like rtld's _start.
// The platform's entry point (platform_win.cpp / platform_android.cpp) calls nx::boot().
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

#if NX_NCE
#include "game_image.h"
#include "nce/nce.h"
#include "nce/nso.h"
#else
extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;
#endif

namespace nx {

// The game's two modules: the recompiled ones, or (native execution) the NSOs of the player's exefs.
static const ModuleInfo* g_main = nullptr;
static const ModuleInfo* g_sdk = nullptr;

uint64_t mrs(CPU& c, uint32_t sysreg) {
    switch (sysreg) {
        case 0xDF02:  // CNTVCT_EL0
        case 0xDF01:  // CNTPCT_EL0
            return system_tick();
        case 0xDF00:  // CNTFRQ_EL0
            return 19200000;
        case 0xD807:  // DCZID_EL0: DC ZVA allowed, 64-byte blocks
            return 4;
        case 0xC000:  // MIDR_EL1 (Cortex-A57)
            return 0x411FD071;
        case 0xD801:  // CTR_EL0 (Cortex-A57: 64-byte lines, PIPT instruction cache)
            return 0x8444C004;
        default:
            log("mrs %#x at lr %s", sysreg, describe(c.x[30]).c_str());
            return 0;
    }
}

void msr(CPU& c, uint32_t sysreg, uint64_t value) {
    log("msr %#x = %llx at lr %s", sysreg, (unsigned long long)value, describe(c.x[30]).c_str());
}

// ---- module initializers --------------------------------------------------------------------------------------
// Each module's DT_INIT (at +0x30 in both main and sdk, see .dynamic) registers the module with the libc
// (__nnmusl_init_dso: TLS, unwind info) and runs its init_array, unless the registration reports 1: then the
// constructors are left to the loader and we run them. Registration only works once the program has given nn::os
// a TLS allocator (SetMemoryAllocatorForThreadLocal, in nninitStartup), so rtld's initializer callback does nothing
// and the modules are initialized right before nnMain, SDK first. Static TLS itself comes from the kernel
// (kernel.cpp: a block per thread at TLS region +0x1F0, where nn::os loads TPIDR_EL0 from).
constexpr uint64_t kDtInit = 0x30;

static void call_preserving(CPU& c, uint64_t target) {
    uint64_t saved[13];
    for (int r = 19; r <= 31; r++) saved[r - 19] = c.x[r];
    call(c, target);
    for (int r = 19; r <= 31; r++) c.x[r] = saved[r - 19];
}

// DT_INIT loads the word where it keeps __nnmusl_init_dso's result with `adrp xN, page` ... `ldr w8, [xN, #off]`
// in its first instructions (SDK 9: adjacent; SDK 16: another adrp in between). Follow the adrp targets until
// that load.
static uint64_t init_status_addr(const ModuleInfo& m) {
    uint64_t page[32] = {};
    for (uint64_t pc = m.base + kDtInit; pc < m.base + kDtInit + 0x40; pc += 4) {
        uint32_t w = ld<uint32_t>(pc);
        if ((w & 0x9F000000) == 0x90000000) {  // adrp
            int64_t imm = int64_t(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 43 >> 31;  // sign-extended << 12
            page[w & 31] = (pc & ~0xFFFull) + uint64_t(imm);
        } else if ((w & 0xFFC0001F) == 0xB9400008 && page[(w >> 5) & 31]) {  // ldr w8, [xN, #imm]
            return page[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 4;
        }
    }
    fatal("%s: DT_INIT does not load its init status as expected", m.name);
}

// DT_INIT hands __nnmusl_init_dso the module's layout: consecutive GOT slots it loads with adrp + ldr. Slots 13
// and 14 are the start and end of the module's .tdata (checked on SDK 16: Wonder's main 0x359cc18..0x359cc28,
// sdk empty). Returns false when the pattern is not there (SDK 9 modules have no .tdata to worry about).
static bool tdata_range(const ModuleInfo& m, uint64_t& start, uint64_t& end) {
    uint64_t page[32] = {}, lo = ~0ull, hi = 0;
    for (uint64_t pc = m.base + kDtInit; pc < m.base + kDtInit + 0x100; pc += 4) {
        uint32_t w = ld<uint32_t>(pc);
        if ((w & 0x9F000000) == 0x90000000) {  // adrp
            int64_t imm = int64_t(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 43 >> 31;
            page[w & 31] = (pc & ~0xFFFull) + uint64_t(imm);
        } else if ((w & 0xFFC00000) == 0xF9400000 && page[(w >> 5) & 31]) {  // ldr xT, [xN, #imm]
            uint64_t slot = page[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 8;
            lo = std::min(lo, slot), hi = std::max(hi, slot);
        }
    }
    if (hi < lo || (hi - lo) / 8 + 1 < 15) return false;
    start = ld<uint64_t>(lo + 13 * 8), end = ld<uint64_t>(lo + 14 * 8);
    return start <= end && end - start <= 0x1000 && start >= m.base && end <= m.base + 0x10000000;
}

static void init_module(CPU& c, const ModuleInfo& m) {
    call_preserving(c, m.base + kDtInit);
    uint32_t status = ld<uint32_t>(init_status_addr(m));
    if (status == 1)
        for (size_t k = 0; k < m.init_count; k++) call_preserving(c, m.base + m.init_array[k]);
    log("%s constructors: %zu", m.name, status == 1 ? m.init_count : 0);
}

static void rtld_init(CPU&) {}
static void rtld_fini(CPU&) {}
static void rtld_mark(CPU&) {}

// nnMain, as called by nn::init::Start: the constructors need the program's allocator, which nninitStartup has
// set up by now, so they run here, right before the real nnMain.
static void nn_main(CPU& c) {
    init_module(c, *g_sdk);
    init_module(c, *g_main);
    game::after_init();
    log("entering nnMain");
    uint64_t real = 0;
    for (size_t k = 0; k < g_main->symbol_count; k++)
        if (!g_main->symbols[k].imported && std::string(g_main->symbols[k].name) == "nnMain")
            real = g_main->base + g_main->symbols[k].value;
    call(c, real);
}

// longjmp(buf, val): unwinds the host stack to the recompiled function that called setjmp on buf.
static void long_jmp(CPU& c) { throw LongJmp{c.x[0], uint32_t(c.x[1])}; }

static void process_return(CPU&) {
    log("nn::init::Start returned");
    plat::exit_process(0);
}

NX_HLE("nnMain", nn_main);
NX_HLE("longjmp", long_jmp);
NX_HLE("_longjmp", long_jmp);
NX_HLE("__rtld_init", rtld_init);
NX_HLE("__rtld_fini", rtld_fini);
NX_HLE("__rtld_mark", rtld_mark);
NX_HLE("__process_return", process_return);

// ---- boot -----------------------------------------------------------------------------------------------------

void boot() {
    if (const char* t = std::getenv("NX_TRACE")) g_trace = *t == '1';
    if (plat::setting("trace") == "1") g_trace = true;
    memory_init();
    kernel_init();
#if NX_NCE
    // The player's own ExeFS (main and sdk as they are on the cartridge or in the eShop download): from the game file
    // read in place (game_image.h), else from the data folder's exefs/.
    nce::init();
    static std::vector<std::unique_ptr<nce::NsoModule>> nso;
    const std::string exefs = plat::data_root() + "/exefs";
    const nxi::GameImage* image = game_image();

    auto find_mod_exefs = [](const std::string& name) -> std::string {
        std::error_code ec;
        fs::path root = fs::u8path(plat::data_root()) / "mods";
        if (fs::is_regular_file(root / "exefs" / fs::u8path(name), ec))
            return (root / "exefs" / fs::u8path(name)).string();
        if (fs::is_directory(root, ec)) {
            for (auto& e : fs::directory_iterator(root, ec)) {
                if (e.is_directory(ec)) {
                    fs::path candidate = e.path() / "exefs" / fs::u8path(name);
                    if (fs::is_regular_file(candidate, ec)) return candidate.string();
                }
            }
        }
        return {};
    };

    std::vector<std::string> mod_names = {"main", "sdk"};
    for (int k = 0; k <= 9; k++) mod_names.push_back("subsdk" + std::to_string(k));

    for (const auto& name : mod_names) {
        std::string mod_file = find_mod_exefs(name);
        std::string error;
        const uint64_t base = kModuleBase + uint64_t(nso.size()) * 0x10000000;
        if (!mod_file.empty()) {
            log("nce: loading mod exefs override: %s", mod_file.c_str());
            auto m = nce::load_nso(mod_file, name, base, error);
            if (m) nso.push_back(std::move(m));
            else log("nce: error loading mod %s: %s", mod_file.c_str(), error.c_str());
        } else if (image) {
            auto it = image->exefs().find(name);
            if (it != image->exefs().end()) {
                auto m = nce::load_nso_bytes(it->second, std::string("ExeFS/") + name, name, base, error);
                if (m) nso.push_back(std::move(m));
            } else if (name == "main" || name == "sdk") {
                fatal("the game file's ExeFS has no %s", name.c_str());
            }
        } else {
            std::error_code ec;
            if (fs::is_regular_file(fs::u8path(exefs) / fs::u8path(name), ec)) {
                auto m = nce::load_nso(exefs + "/" + name, name, base, error);
                if (m) nso.push_back(std::move(m));
            } else if (name == "main" || name == "sdk") {
                fatal("exefs/%s not found (copy the exefs folder of your own dump there)", name.c_str());
            }
        }
    }
    if (nso.size() < 2) fatal("failed to load base modules (main and sdk required)");

    if (const char* want = game::kInfo.main_build_id) {
        std::string have;
        for (uint8_t b : nso[0]->build_id) {
            char hex[3];
            std::snprintf(hex, sizeof hex, "%02X", b);
            have += hex;
        }
        const char* known_builds[] = {
            "708BCACA873EA32B", "B78A03E489E5BD92", "6E1F4CAE92A28C91", "2F2253ACD6BE5D87"
        };
        bool known = false;
        for (const char* k : known_builds) {
            if (have.compare(0, std::strlen(k), k) == 0) { known = true; break; }
        }
        bool allow_any = std::getenv("NX_ALLOW_ANY_BUILD") || std::getenv("NX_IGNORE_VERSION");
        if (known) {
            log("p5r: main build %s recognized and supported", have.c_str());
        } else if (allow_any) {
            log("p5r: main build %s allowed by NX_ALLOW_ANY_BUILD switch", have.c_str());
        } else {
            fatal("exefs/main is build %s, not %s: this port is made for Persona 5 Royal 1.0.0 (US, EU, JP, or Asia). Set NX_ALLOW_ANY_BUILD=1 to override.",
                  have.c_str(), want);
        }
    }

    g_main = &nso[0]->info, g_sdk = &nso[1]->info;
    std::vector<const ModuleInfo*> mods;
    std::vector<const uint8_t*> img_ptrs;
    for (const auto& m : nso) {
        mods.push_back(&m->info);
        img_ptrs.push_back(m->image.data());
    }
    load_modules(mods.data(), mods.size(), "", img_ptrs.data());
#else
    g_main = &nx_module_main, g_sdk = &nx_module_sdk;
    const ModuleInfo* mods[] = {g_main, g_sdk};
    load_modules(mods, 2, plat::data_root() + "/analysis");
#endif

    uint64_t tdata = 0, tdata_end = 0;
    if (tdata_range(*g_main, tdata, tdata_end) && tdata_end > tdata) {
        set_tls_template(tdata, tdata_end);
        log("main .tdata: %llu bytes", (unsigned long long)(tdata_end - tdata));
    }
    uint32_t main_thread = create_main_thread(16ull << 20);
    CPU& c = current_cpu();
    c.x[0] = main_thread;
    c.x[1] = g_main->base;
    // SDK 9: Start(thread, main base, init, fini). SDK 16 adds a first callback: rtld's sets a word to 1 (rtld+0x1330
    // in Wonder), before its init and fini that walk the module list.
    uint64_t start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (start) {
        c.x[2] = hle_address("__rtld_init");
        c.x[3] = hle_address("__rtld_fini");
    } else if ((start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_"))) {
        c.x[2] = hle_address("__rtld_mark");
        c.x[3] = hle_address("__rtld_init");
        c.x[4] = hle_address("__rtld_fini");
    } else {
        fatal("nn::init::Start not found");
    }
    c.x[30] = hle_address("__process_return");
    log("entering nn::init::Start at %s", describe(start).c_str());
    call(c, start);
    log("returned from nn::init::Start");
}

}  // namespace nx
