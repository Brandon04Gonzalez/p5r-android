// NSO modules read from the player's own dump at run time (the loader path: no recompiled game code in the build).
// The same parse as recomp/nso.py: the three LZ4 segments, MOD0, .dynamic, the symbols (symtab up to strtab), the
// RELA and JMPREL relocations, and the init/fini arrays read from their RELATIVE addends. The result is a ModuleInfo
// as the recompiler generates it, without functions (funcs is null): load_modules and its linking take it as is.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nx/module.h"

namespace nx::nce {

struct NsoModule {
    std::string name;
    std::vector<uint8_t> image;  // text, ro and data at their offsets (module base 0, relocations not applied)
    ModuleInfo info{};           // points into the vectors below
    std::vector<Symbol> symbols;
    std::vector<std::string> names;  // the symbols' names (info.symbols[k].name points here)
    std::vector<Reloc> relocs;
    std::vector<uint64_t> init_array, fini_array;
    std::vector<uint8_t> build_id;   // the NSO header's module id (32 bytes)
};

// LZ4 block decompression into exactly `out_size` bytes; false on a malformed block.
bool lz4_block_decompress(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);

// Parses an NSO file (or its bytes, `path` naming it in errors); `base` is where it will be loaded (stored in
// info.base). Null with `error` set on failure.
std::unique_ptr<NsoModule> load_nso_bytes(const std::vector<uint8_t>& raw, const std::string& path,
                                          const std::string& name, uint64_t base, std::string& error);
std::unique_ptr<NsoModule> load_nso(const std::string& path, const std::string& name, uint64_t base,
                                    std::string& error);

}  // namespace nx::nce
