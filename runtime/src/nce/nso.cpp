#include "nso.h"

#include <cstdio>
#include <cstring>
#include <map>

#include "platform.h"

namespace nx::nce {

namespace {

constexpr int64_t DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
                  DT_SYMENT = 11, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27,
                  DT_FINI_ARRAYSZ = 28;

template <typename T>
bool rd(const std::vector<uint8_t>& v, uint64_t at, T& out) {
    if (at + sizeof(T) > v.size()) return false;
    std::memcpy(&out, v.data() + at, sizeof(T));
    return true;
}

}  // namespace

bool lz4_block_decompress(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
    size_t ip = 0, op = 0;
    while (ip < in_size) {
        const uint8_t token = in[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= in_size) return false;
                b = in[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > in_size || op + lit > out_size) return false;
        std::memcpy(out + op, in + ip, lit);
        ip += lit, op += lit;
        if (ip >= in_size) break;  // the last sequence has literals only
        if (ip + 2 > in_size) return false;
        const size_t offset = size_t(in[ip]) | size_t(in[ip + 1]) << 8;
        ip += 2;
        if (!offset || offset > op) return false;
        size_t len = token & 15;
        if (len == 15) {
            uint8_t b;
            do {
                if (ip >= in_size) return false;
                b = in[ip++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (op + len > out_size) return false;
        for (size_t k = 0; k < len; k++, op++) out[op] = out[op - offset];  // overlapping copies repeat bytes
    }
    return op == out_size;
}

std::unique_ptr<NsoModule> load_nso(const std::string& path, const std::string& name, uint64_t base,
                                    std::string& error) {
    std::vector<uint8_t> raw;
    if (FILE* f = plat::fopen_utf8(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            raw.resize(size_t(n));
            if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
        }
        std::fclose(f);
    }
    if (raw.size() < 0x100 || std::memcmp(raw.data(), "NSO0", 4) != 0) {
        error = path + ": not an NSO file";
        return nullptr;
    }
    auto m = std::make_unique<NsoModule>();
    m->name = name;
    uint32_t flags = 0;
    rd(raw, 0x0C, flags);
    uint32_t file_off[3], mem_off[3], size[3], comp[3];
    for (int i = 0; i < 3; i++) {
        rd(raw, 0x10 + i * 0x10, file_off[i]);
        rd(raw, 0x14 + i * 0x10, mem_off[i]);
        rd(raw, 0x18 + i * 0x10, size[i]);
        rd(raw, 0x60 + i * 4, comp[i]);
    }
    m->build_id.assign(raw.begin() + 0x40, raw.begin() + 0x60);
    const uint64_t data_end = uint64_t(mem_off[2]) + size[2];
    m->image.assign(data_end, 0);
    for (int i = 0; i < 3; i++) {
        if (uint64_t(file_off[i]) + comp[i] > raw.size() || uint64_t(mem_off[i]) + size[i] > data_end) {
            error = path + ": segment out of range";
            return nullptr;
        }
        const uint8_t* src = raw.data() + file_off[i];
        uint8_t* dst = m->image.data() + mem_off[i];
        if (flags & (1u << i)) {
            if (!lz4_block_decompress(src, comp[i], dst, size[i])) {
                error = path + ": bad LZ4 data";
                return nullptr;
            }
        } else {
            std::memcpy(dst, src, std::min(comp[i], size[i]));
        }
    }
    ModuleInfo& mi = m->info;
    mi.base = base;
    mi.text = mem_off[0], mi.text_size = size[0];
    mi.ro = mem_off[1], mi.ro_size = size[1];
    mi.data = mem_off[2], mi.data_size = size[2];

    uint32_t mod0 = 0;
    char magic[4] = {};
    if (!rd(m->image, 4, mod0) || mod0 + 0x1c > m->image.size() ||
        (std::memcpy(magic, m->image.data() + mod0, 4), std::memcmp(magic, "MOD0", 4) != 0)) {
        error = path + ": MOD0 not found";
        return nullptr;
    }
    int32_t rel[5];
    for (int i = 0; i < 5; i++) rd(m->image, mod0 + 4 + i * 4, rel[i]);
    const uint64_t dyn_off = mod0 + int64_t(rel[0]);
    mi.bss = mod0 + int64_t(rel[1]);
    mi.bss_size = uint64_t(int64_t(rel[2]) - rel[1]);

    std::map<int64_t, uint64_t> dyn;
    for (uint64_t a = dyn_off;; a += 16) {
        int64_t tag = 0;
        uint64_t val = 0;
        if (!rd(m->image, a, tag) || !rd(m->image, a + 8, val)) {
            error = path + ": .dynamic runs past the image";
            return nullptr;
        }
        if (tag == DT_NULL) break;
        dyn.emplace(tag, val);  // the first one wins, as nso.py
    }
    if (!dyn.count(DT_STRTAB) || !dyn.count(DT_SYMTAB)) {
        error = path + ": no symbol table";
        return nullptr;
    }
    const uint64_t strtab = dyn[DT_STRTAB], symtab = dyn[DT_SYMTAB];
    const uint64_t syment = dyn.count(DT_SYMENT) ? dyn[DT_SYMENT] : 24;
    const uint64_t nsyms = strtab > symtab ? (strtab - symtab) / syment : 0;  // symtab sits right before strtab
    m->symbols.resize(nsyms);
    m->names.resize(nsyms);
    for (uint64_t i = 0; i < nsyms; i++) {
        const uint64_t at = symtab + i * syment;
        uint32_t st_name = 0;
        uint8_t st_info = 0;
        uint16_t st_shndx = 0;
        uint64_t st_value = 0, st_size = 0;
        rd(m->image, at, st_name);
        rd(m->image, at + 4, st_info);
        rd(m->image, at + 6, st_shndx);
        rd(m->image, at + 8, st_value);
        rd(m->image, at + 16, st_size);
        const uint64_t s = strtab + st_name;
        if (s < m->image.size()) {
            const char* p = reinterpret_cast<const char*>(m->image.data() + s);
            m->names[i].assign(p, strnlen(p, m->image.size() - s));
        }
        Symbol& sym = m->symbols[i];
        sym.value = st_value, sym.size = st_size;
        sym.type = st_info & 0xF, sym.bind = st_info >> 4, sym.imported = st_shndx == 0;
    }
    for (uint64_t i = 0; i < nsyms; i++) m->symbols[i].name = m->names[i].c_str();

    auto relocs = [&](uint64_t start, uint64_t bytes) {
        for (uint64_t a = start; a + 24 <= start + bytes; a += 24) {
            uint64_t off = 0, info = 0;
            int64_t addend = 0;
            rd(m->image, a, off), rd(m->image, a + 8, info), rd(m->image, a + 16, addend);
            m->relocs.push_back({off, uint32_t(info), uint32_t(info >> 32), addend});
        }
    };
    if (dyn.count(DT_RELA)) relocs(dyn[DT_RELA], dyn.count(DT_RELASZ) ? dyn[DT_RELASZ] : 0);
    if (dyn.count(DT_JMPREL)) relocs(dyn[DT_JMPREL], dyn.count(DT_PLTRELSZ) ? dyn[DT_PLTRELSZ] : 0);

    std::map<uint64_t, const Reloc*> rel_at;
    for (const Reloc& r : m->relocs) rel_at.emplace(r.offset, &r);
    auto ptr_array = [&](int64_t tag, int64_t size_tag, std::vector<uint64_t>& out) {
        if (!dyn.count(tag)) return;
        const uint64_t start = dyn[tag], bytes = dyn.count(size_tag) ? dyn[size_tag] : 0;
        for (uint64_t a = start; a + 8 <= start + bytes; a += 8) {
            auto it = rel_at.find(a);
            uint64_t v = 0;
            if (it != rel_at.end() && it->second->type == R_AARCH64_RELATIVE)
                v = uint64_t(it->second->addend);
            else
                rd(m->image, a, v);
            out.push_back(v);
        }
    };
    ptr_array(DT_INIT_ARRAY, DT_INIT_ARRAYSZ, m->init_array);
    ptr_array(DT_FINI_ARRAY, DT_FINI_ARRAYSZ, m->fini_array);

    mi.name = m->name.c_str();
    mi.funcs = nullptr, mi.func_count = 0;
    mi.relocs = m->relocs.data(), mi.reloc_count = m->relocs.size();
    mi.symbols = m->symbols.data(), mi.symbol_count = m->symbols.size();
    mi.init_array = m->init_array.data(), mi.init_count = m->init_array.size();
    mi.fini_array = m->fini_array.data(), mi.fini_count = m->fini_array.size();
    return m;
}

}  // namespace nx::nce
