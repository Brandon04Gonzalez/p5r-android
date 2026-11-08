#include "p4au_shaders.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

namespace p4au {

namespace {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// SHA-1 (16 hex chars)
std::string sha1_16(const uint8_t* data, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int s) { return (x << s) | (x >> (32 - s)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) block(data + i);
    uint8_t tail[128] = {};
    size_t rest = n - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t len = rest + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(n) * 8;
    for (int j = 0; j < 8; j++) tail[len - 1 - j] = uint8_t(bits >> (8 * j));
    block(tail);
    if (len == 128) block(tail + 64);
    char out[41];
    for (int j = 0; j < 5; j++) std::snprintf(out + 8 * j, 9, "%08x", h[j]);
    return std::string(out, 16);
}

void make_dir(const std::string& p) {
#if defined(_WIN32)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

void write_file(const std::string& path, const uint8_t* p, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(p, 1, n, f) != n) {
        if (f) std::fclose(f);
        throw Fail("cannot write " + path + " (" + std::strerror(errno) + ")");
    }
    if (std::fclose(f) != 0) throw Fail("cannot write " + path);
}

const char* kStages[] = {"vertex", "hull", "domain", "geometry", "pixel", "compute"};

struct ExtractedStage {
    std::string id;
    std::string kind;
    uint64_t code_off;
    uint32_t code_size;
    uint64_t ctrl_off;
    uint32_t ctrl_size;
    uint32_t prog_start;
    uint32_t prog_bytes;
    uint32_t const_off;
    uint32_t const_size;
    std::string code_sha1;
    std::string ctrl_sha1;
};

void parse_bnsh(const std::string& name, const std::vector<uint8_t>& data,
                const std::string& gdir, std::set<std::string>& seen,
                std::vector<ExtractedStage>& stages) {
    if (data.size() < 0x40 || std::memcmp(data.data(), "BNSH\0\0\0\0", 8) != 0) return;
    uint16_t first_block = le16(data.data() + 0x16);
    if (first_block + 0x30 > data.size()) return;
    const uint8_t* g = data.data() + first_block;
    if (std::memcmp(g, "grsc", 4) != 0) return;

    uint32_t nvar = le32(g + 0x1C);
    uint64_t var_array = le64(g + 0x20);
    if (var_array + uint64_t(nvar) * 0x40 > data.size()) return;

    for (uint32_t k = 0; k < nvar; k++) {
        uint64_t binary = le64(data.data() + var_array + k * 0x40 + 0x10);
        if (!binary || binary + 0x40 > data.size()) continue;

        const uint8_t* p = data.data() + binary;
        for (int s = 0; s < 6; s++) {
            uint64_t sc = le64(p + 8 + s * 8);
            if (!sc || sc + 0x40 > data.size()) continue;

            uint64_t ctrl_off = le64(data.data() + sc + 8);
            uint64_t code_off = le64(data.data() + sc + 0x10);
            uint32_t code_size = le32(data.data() + sc + 0x18);
            uint32_t ctrl_size = le32(data.data() + sc + 0x1C);

            if (code_off + code_size > data.size() || ctrl_off + ctrl_size > data.size()) continue;

            std::string code_hash = sha1_16(data.data() + code_off, code_size);
            std::string ctrl_hash = sha1_16(data.data() + ctrl_off, ctrl_size);

            if (seen.insert(code_hash).second) {
                write_file(gdir + "/code_" + code_hash + ".bin", data.data() + code_off, code_size);
            }
            if (seen.insert(ctrl_hash).second) {
                write_file(gdir + "/ctrl_" + ctrl_hash + ".bin", data.data() + ctrl_off, ctrl_size);
            }

            ExtractedStage st;
            char vbuf[32];
            std::snprintf(vbuf, sizeof(vbuf), "v%05u", k);
            st.id = name + "/" + vbuf + "/" + kStages[s];
            st.kind = kStages[s];
            st.code_off = code_off;
            st.code_size = code_size;
            st.ctrl_off = ctrl_off;
            st.ctrl_size = ctrl_size;
            if (ctrl_size >= 0x710) {
                st.prog_start = le32(data.data() + ctrl_off + 0x708);
                st.prog_bytes = le32(data.data() + ctrl_off + 0x6F8);
                st.const_off = le32(data.data() + ctrl_off + 0x700);
                st.const_size = le32(data.data() + ctrl_off + 0x6FC);
            } else {
                st.prog_start = 0;
                st.prog_bytes = 0;
                st.const_off = 0;
                st.const_size = 0;
            }
            st.code_sha1 = code_hash;
            st.ctrl_sha1 = ctrl_hash;
            stages.push_back(std::move(st));
        }
    }
}

}  // namespace

long extract_shaders(const nxi::GameImage& game, const std::string& out_dir,
                     const std::function<void(int, int)>& progress, std::string& error) {
    try {
        make_dir(out_dir);

        // In P4AU, shaders are stored in RomFS under data/shader/
        std::vector<std::pair<std::string, nxi::GameImage::Entry>> shader_entries;
        auto list = game.list("data/shader");
        if (list.empty()) list = game.list("DATA/SHADER");
        if (list.empty()) list = game.list("data/Shader");
        for (const auto& item : list) {
            std::string lower = item.first;
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".bnsh") == 0) {
                shader_entries.push_back(item);
            }
        }
        if (shader_entries.empty()) {
            // Fallback: list all RomFS files
            auto all = game.list("");
            for (const auto& item : all) {
                std::string lower = item.first;
                for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".bnsh") == 0) {
                    shader_entries.push_back(item);
                }
            }
        }
        if (shader_entries.empty()) throw Fail("no .bnsh files found in data/shader (is it Persona 4 Arena Ultimax?)");

        std::string gdir = out_dir + "/bnsh";
        make_dir(gdir);

        std::set<std::string> seen;
        std::vector<ExtractedStage> stages;

        for (size_t i = 0; i < shader_entries.size(); i++) {
            if (progress) progress(int(i), int(shader_entries.size()));
            const auto& item = shader_entries[i];
            std::vector<uint8_t> data;
            data.resize(size_t(item.second.size));
            if (game.read(item.second, 0, data.data(), data.size()) != data.size()) {
                throw Fail("cannot read " + item.first);
            }
            parse_bnsh(item.first, data, gdir, seen, stages);
        }

        if (progress) progress(int(shader_entries.size()), int(shader_entries.size()));

        // Write manifest.tsv
        std::string manifest = "id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\tcode_sha1_16\tcode_file\tctrl_file\n";
        char line[512];
        for (const auto& s : stages) {
            std::snprintf(line, sizeof(line),
                          "%s\t%s\t0x%llx\t0x%x\t0x%llx\t0x%x\t0x%x\t0x%x\t0x%x\t0x%x\t%s\tcode_%s.bin\tctrl_%s.bin\n",
                          s.id.c_str(), s.kind.c_str(),
                          (unsigned long long)s.code_off, s.code_size,
                          (unsigned long long)s.ctrl_off, s.ctrl_size,
                          s.prog_start, s.prog_bytes, s.const_off, s.const_size,
                          s.code_sha1.c_str(), s.code_sha1.c_str(), s.ctrl_sha1.c_str());
            manifest += line;
        }
        write_file(gdir + "/manifest.tsv", reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());

        std::set<std::string> code_blocks;
        for (const auto& s : stages) code_blocks.insert(s.code_sha1);
        return long(code_blocks.size());
    } catch (const Fail& e) {
        error = e.what();
    } catch (const std::exception& e) {
        error = e.what();
    }
    return -1;
}

}  // namespace p4au
