// NVN programs (docs/NVN.md, N5): the shader stages the game binds. Each stage arrives as Maxwell code at a GPU
// address in one of its memory pools, plus the NVN control section (docs/nvn/SHADERS.md). The code was translated
// to SPIR-V offline (docs/nvn/TRANSLATOR.md); nvn_shaders.cpp finds each stage's by the hash of its code block.
//
// nvnProgramSetShaders(NVNprogram*, int count, const NVNshaderData*): {NVNbufferAddress code; const void* control}
// per stage (0x10 bytes); the Ext variant's entries have a third, zero word (0x18 bytes).
//
// The stages are looked up when a program is first used, not in SetShaders: Persona 5 Royal sets up a program for
// each of the ~30,000 shaders of its GFD caches while it loads (one per few ms with the .spv read and reflected
// right away: minutes of black screen), and a scene uses a few hundred. SetShaders keeps the code address (the code
// stays in its pool while the program is in use) and the control fields read_control needs (the control section
// itself may be freed once SetShaders returns).
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nvn.h"
#include "nvn_shaders.h"

namespace nx::nvn {

namespace {

constexpr size_t kCtrlSize = 0x880, kCtrlFields = 0x6f8, kCtrlFieldsEnd = 0x758;  // read_control: +0, +0x6f8..+0x758

struct PendingStage {
    uint64_t code = 0;  // host address of the code block
    uint32_t magic = 0;
    std::array<uint8_t, kCtrlFieldsEnd - kCtrlFields> fields{};
};

struct Program : Object {
    std::vector<const ShaderInfo*> shaders;  // a stage with no translation is left out (and logged once)
    std::vector<PendingStage> pending;       // set, not looked up yet
    std::atomic<bool> resolved{true};
    std::mutex lock;
};

void resolve(Program& p, uint64_t program) {
    std::lock_guard lk(p.lock);
    if (p.resolved.load(std::memory_order_relaxed)) return;
    std::vector<const ShaderInfo*> shaders;
    for (const PendingStage& st : p.pending) {
        uint8_t ctrl[kCtrlSize] = {};
        std::memcpy(ctrl, &st.magic, 4);
        std::memcpy(ctrl + kCtrlFields, st.fields.data(), st.fields.size());
        if (const ShaderInfo* s = shader_for(st.code, reinterpret_cast<uint64_t>(ctrl))) shaders.push_back(s);
    }
    static const bool trace = std::getenv("NX_TRACE_PROGRAMS") != nullptr;  // each program's stages when set
    if (trace) {
        std::string line;
        for (const ShaderInfo* s : shaders) line += " " + s->hash;
        log("nvn: program %llx set:%s", (unsigned long long)program, line.c_str());
    }
    p.shaders = std::move(shaders);
    p.pending.clear();
    p.pending.shrink_to_fit();
    p.resolved.store(true, std::memory_order_release);
}

void ProgramInitialize(CPU& c) {  // NVNboolean (NVNprogram*, NVNdevice*)
    make<Program>(c.x[0]);
    c.x[0] = 1;
}

void ProgramFinalize(CPU& c) { destroy(c.x[0]); }

void set_shaders(CPU& c, size_t stride) {
    std::vector<PendingStage> pending;
    for (int k = 0; k < int(c.x[1]); k++) {
        uint64_t entry = c.x[2] + uint64_t(k) * stride;
        uint64_t code = ld<uint64_t>(entry), control = ld<uint64_t>(entry + 8);
        uint64_t offset = 0;
        MemoryPool* pool = pool_at_gpu(code, &offset);
        if (!pool || !control) {
            log("nvn: program %llx stage %d: code %llx control %llx not found", (unsigned long long)c.x[0], k,
                (unsigned long long)code, (unsigned long long)control);
            continue;
        }
        PendingStage st;
        st.code = pool->memory + offset;
        st.magic = ld<uint32_t>(control);
        std::memcpy(st.fields.data(), reinterpret_cast<const void*>(control + kCtrlFields), st.fields.size());
        pending.push_back(st);
    }
    if (auto* p = get<Program>(c.x[0])) {
        std::lock_guard lk(p->lock);
        p->pending = std::move(pending);
        p->shaders.clear();
        p->resolved.store(false, std::memory_order_release);
    }
    c.x[0] = 1;
}

void ProgramSetShaders(CPU& c) { set_shaders(c, 0x10); }
void ProgramSetShadersExt(CPU& c) { set_shaders(c, 0x18); }

}  // namespace

const std::vector<const ShaderInfo*>* program_shaders(uint64_t program) {
    auto* p = get<Program>(program);
    if (!p) return nullptr;
    if (!p->resolved.load(std::memory_order_acquire)) resolve(*p, program);
    return &p->shaders;
}

NX_HLE("nvnProgramInitialize", ProgramInitialize);
NX_HLE("nvnProgramFinalize", ProgramFinalize);
NX_HLE("nvnProgramSetShaders", ProgramSetShaders);
NX_HLE("nvnProgramSetShadersExt", ProgramSetShadersExt);

}  // namespace nx::nvn
