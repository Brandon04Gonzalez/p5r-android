// Tables the recompiler emits for a module.
#pragma once

#include <cstddef>
#include <cstdint>

#include <nx/cpu.h>

namespace nx {

struct FuncEntry {
    uint64_t offset;  // module-relative guest address
    Fn fn;
};

struct Reloc {
    uint64_t offset;
    uint32_t type;
    uint32_t sym;
    int64_t addend;
};

struct Symbol {
    const char* name;
    uint64_t value;
    uint64_t size;
    uint8_t type;
    uint8_t bind;
    uint8_t imported;
};

struct ModuleInfo {
    const char* name;
    uint64_t base;  // load address the code was generated for
    uint64_t text, text_size, ro, ro_size, data, data_size, bss, bss_size;
    const FuncEntry* funcs;
    size_t func_count;
    const Reloc* relocs;
    size_t reloc_count;
    const Symbol* symbols;
    size_t symbol_count;
    const uint64_t* init_array;
    size_t init_count;
    const uint64_t* fini_array;
    size_t fini_count;
};

constexpr uint32_t R_AARCH64_ABS64 = 257;
constexpr uint32_t R_AARCH64_GLOB_DAT = 1025;
constexpr uint32_t R_AARCH64_JUMP_SLOT = 1026;
constexpr uint32_t R_AARCH64_RELATIVE = 1027;

}  // namespace nx
