// p5r_shaders.h. The formats, as games/p5r/tools/cpk.py, gsc_shaders.py and tools/maxwell/exefs_shaders.py read them.
#include "p5r_shaders.h"

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

namespace p5r {

namespace {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// ---- SHA-1 (the harness names blocks by the first 16 hex digits of their SHA-1) ----

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

// ---- CRI: @UTF tables, CPK, CRILAYLA ----

struct Value {
    int type = -1;
    uint64_t u = 0;
    std::string s;
};
using Row = std::map<std::string, Value>;

std::vector<Row> read_utf(std::vector<uint8_t> d) {
    if (d.size() < 0x20) throw Fail("CPK: table too short");
    if (std::memcmp(d.data(), "@UTF", 4) != 0) {  // obfuscated
        uint32_t m = 0x655F;
        for (uint8_t& b : d) b ^= uint8_t(m), m = m * 0x4115;
        if (std::memcmp(d.data(), "@UTF", 4) != 0) throw Fail("CPK: not an @UTF table");
    }
    const uint8_t* p = d.data();
    const size_t base = 8;
    size_t rows_off = base + be16(p + 10), strings_off = base + be32(p + 12), data_off = base + be32(p + 16);
    uint16_t ncols = be16(p + 24), row_len = be16(p + 26);
    uint32_t nrows = be32(p + 28);
    auto at = [&](size_t o, size_t n) {
        if (o + n > d.size()) throw Fail("CPK: table data out of range");
        return p + o;
    };
    auto cstr = [&](uint32_t o) {
        size_t s = strings_off + o;
        at(s, 1);
        size_t e = s;
        while (e < d.size() && p[e]) e++;
        return std::string(reinterpret_cast<const char*>(p + s), e - s);
    };
    auto value = [&](int t, size_t pos, Value& v) -> size_t {
        v.type = t;
        switch (t) {
            case 0: case 1: v.u = *at(pos, 1); return 1;
            case 2: case 3: v.u = be16(at(pos, 2)); return 2;
            case 4: case 5: case 8: v.u = be32(at(pos, 4)); return 4;
            case 6: case 7: case 9: v.u = be64(at(pos, 8)); return 8;
            case 0xA: v.s = cstr(be32(at(pos, 4))); return 4;
            case 0xB: {  // data: offset, size (not needed here)
                at(pos, 8);
                v.u = be32(p + pos + 4);
                return 8;
            }
        }
        throw Fail("CPK: unknown @UTF column type");
    };
    struct Col {
        std::string name;
        int storage, type;
        Value constant;
    };
    std::vector<Col> cols;
    size_t pos = 0x20;
    for (int i = 0; i < ncols; i++) {
        uint8_t flags = *at(pos, 1);
        Col c{cstr(be32(at(pos + 1, 4))), flags & 0xF0, flags & 0x0F, {}};
        pos += 5;
        if (c.storage == 0x30) pos += value(c.type, pos, c.constant);
        cols.push_back(std::move(c));
    }
    std::vector<Row> rows(nrows);
    for (uint32_t r = 0; r < nrows; r++) {
        size_t q = rows_off + size_t(r) * row_len;
        for (const Col& c : cols) {
            if (c.storage == 0x50) q += value(c.type, q, rows[r][c.name]);
            else if (c.storage == 0x30) rows[r][c.name] = c.constant;
        }
    }
    (void)data_off;
    return rows;
}

std::vector<uint8_t> crilayla(const std::vector<uint8_t>& src) {
    if (src.size() < 0x110 || std::memcmp(src.data(), "CRILAYLA", 8) != 0) return src;
    uint32_t usize = le32(src.data() + 8), hoff = le32(src.data() + 12);
    if (0x10 + uint64_t(hoff) + 0x100 > src.size() || usize > (1u << 30)) throw Fail("CRILAYLA: bad header");
    std::vector<uint8_t> out(size_t(usize) + 0x100);
    std::memcpy(out.data(), src.data() + 0x10 + hoff, 0x100);
    int64_t pos = int64_t(src.size()) - 0x100 - 1;
    uint32_t bits = 0;
    int nbits = 0;
    auto get = [&](int n) {
        uint32_t v = 0;
        while (n) {
            if (nbits == 0) {
                if (pos < 0) throw Fail("CRILAYLA: stream ends early");
                bits = src[size_t(pos--)];
                nbits = 8;
            }
            int take = std::min(n, nbits);
            v = (v << take) | ((bits >> (nbits - take)) & ((1u << take) - 1));
            nbits -= take, n -= take;
        }
        return v;
    };
    int64_t w = int64_t(usize) + 0x100 - 1;
    while (w >= 0x100) {
        if (get(1)) {
            int64_t back = int64_t(get(13)) + 3;
            uint32_t length = 3;
            static const int lbits[4] = {2, 3, 5, 8};
            bool more = true;
            for (int lb : lbits) {
                uint32_t v = get(lb);
                length += v;
                if (v != (1u << lb) - 1) {
                    more = false;
                    break;
                }
            }
            while (more) {
                uint32_t v = get(8);
                length += v;
                more = v == 0xFF;
            }
            for (uint32_t k = 0; k < length; k++, w--) {
                if (w < 0 || w + back >= int64_t(out.size())) throw Fail("CRILAYLA: bad back reference");
                out[size_t(w)] = out[size_t(w + back)];
            }
        } else {
            out[size_t(w--)] = uint8_t(get(8));
        }
    }
    return out;
}

struct CpkFile {
    std::string path;
    uint64_t offset, size, extract;
};

std::vector<CpkFile> cpk_files(const nxi::GameImage& g, const nxi::GameImage::Entry& cpk) {
    auto block = [&](uint64_t off, const char* magic) {
        uint8_t head[16];
        if (g.read(cpk, off, head, 16) != 16 || std::memcmp(head, magic, 4) != 0)
            throw Fail(std::string("CPK: no ") + magic + " block");
        std::vector<uint8_t> d(size_t(le64(head + 8)));
        if (g.read(cpk, off + 16, d.data(), d.size()) != d.size()) throw Fail("CPK: truncated block");
        return d;
    };
    std::vector<Row> header = read_utf(block(0, "CPK "));
    if (header.empty()) throw Fail("CPK: empty header");
    uint64_t toc = header[0]["TocOffset"].u, content = header[0]["ContentOffset"].u;
    if (!toc) throw Fail("CPK: no table of contents");
    uint64_t base = content ? std::min(toc, content) : toc;
    std::vector<CpkFile> out;
    for (Row& r : read_utf(block(toc, "TOC "))) {
        std::string dir = r["DirName"].s, name = r["FileName"].s;
        uint64_t size = r["FileSize"].u, ext = r["ExtractSize"].u;
        out.push_back({dir.empty() ? name : dir + "/" + name, base + r["FileOffset"].u, size, ext ? ext : size});
    }
    return out;
}

// ---- the blocks ----

constexpr uint32_t kCtrlMagic = 0x98761234, kCode = 0x12345678, kCompute = 0x12345679;
constexpr uint64_t kExit = 0xe30000000007000full;

struct Data {
    const std::vector<uint8_t>& d;
    uint32_t u32(uint64_t o) const { return o + 4 <= d.size() ? le32(d.data() + o) : 0; }
    bool in(uint64_t o, uint64_t n) const { return o + n <= d.size(); }
    bool has_exit(uint64_t a, uint64_t b) const {  // the 8-byte EXIT anywhere in [a, b)
        uint8_t pat[8];
        for (int i = 0; i < 8; i++) pat[i] = uint8_t(kExit >> (8 * i));
        b = std::min<uint64_t>(b, d.size());
        if (a >= b) return false;
        return std::search(d.begin() + ptrdiff_t(a), d.begin() + ptrdiff_t(b), pat, pat + 8) != d.begin() + ptrdiff_t(b);
    }
    bool zero(uint64_t a, uint64_t b) const {
        b = std::min<uint64_t>(b, d.size());
        for (uint64_t i = a; i < b; i++)
            if (d[size_t(i)]) return false;
        return true;
    }
};

uint64_t up(uint64_t x) { return (x + 0xff) & ~uint64_t(0xff); }

std::string stage_of(const Data& x, uint64_t p, uint64_t c) {
    if (x.u32(p) == kCompute || x.u32(c + 0x708) == 0) return "compute";
    uint32_t t = (x.u32(p + 0x30) >> 10) & 0xF;
    return t == 1 ? "vertex" : t == 5 ? "pixel" : "other";
}

std::vector<std::pair<uint64_t, uint64_t>> hnsh_records(const Data& x) {
    std::vector<std::pair<uint64_t, uint64_t>> found;
    static const char kMagic[] = "HNSHV002";
    auto it = x.d.begin();
    while (true) {
        it = std::search(it, x.d.end(), kMagic, kMagic + 8);
        if (it == x.d.end()) break;
        uint64_t h = uint64_t(it - x.d.begin());
        it += 8;
        uint64_t ctrl = h + 0x24 + x.u32(h + 0x54), code = h + 0x24 + x.u32(h + 0x58), size = x.u32(h + 0x5c);
        if (!x.in(h, 0x60) || x.u32(ctrl) != kCtrlMagic || (x.u32(code) != kCode && x.u32(code) != kCompute) ||
            x.u32(ctrl + 0x704) != size)
            continue;
        found.push_back({code, ctrl});
    }
    return found;
}

// tools/maxwell/exefs_shaders.py find(): code blocks by magic paired with the control section that describes them.
std::vector<std::pair<uint64_t, uint64_t>> exefs_blocks(const Data& x) {
    const auto& d = x.d;
    std::vector<uint64_t> ctrls, codes;
    for (uint64_t o = 0; o + 0x880 <= d.size(); o += 4)
        if (x.u32(o) == kCtrlMagic && x.u32(o + 0x704) > 0 && x.u32(o + 0x704) < (1u << 24)) ctrls.push_back(o);
    for (uint64_t o = 0; o + 0x30 <= d.size();) {
        bool m = (d[o] == 0x78 || d[o] == 0x79) && d[o + 1] == 0x56 && d[o + 2] == 0x34 && d[o + 3] == 0x12 && x.zero(o + 4, o + 0x30);
        if (m) codes.push_back(o), o += 0x30;
        else o++;
    }
    auto fits = [&](uint64_t p, uint64_t c) {
        uint64_t s = x.u32(c + 0x704), start = x.u32(c + 0x708), plen = x.u32(c + 0x6f8), csize = x.u32(c + 0x6fc),
                 coff = x.u32(c + 0x700);
        bool compute = x.u32(p) == kCompute;
        if (p + s > d.size() || start != (compute ? 0x100u : 0x30u) || start + plen > s) return false;
        if (!x.has_exit(p + start, p + start + plen) || x.zero(p + start + plen - 8, p + start + plen)) return false;
        if (csize) {
            if (coff != up(start + plen) || s != up(coff + csize) || !x.zero(p + coff + csize, p + s)) return false;
        } else if (s != up(start + plen)) {
            return false;
        }
        if (!x.zero(p + start + plen, p + up(start + plen))) return false;
        uint32_t t = (x.u32(p + start) >> 10) & 0xF;
        return compute || t == 1 || t == 5;
    };
    auto key = [&](uint64_t c) {
        return std::string(reinterpret_cast<const char*>(d.data() + c + 0x6f8), 0x14) +
               std::string(reinterpret_cast<const char*>(d.data() + c + 0x748), 0x10);
    };
    std::vector<std::pair<uint64_t, uint64_t>> found;
    for (size_t i = 0; i < codes.size(); i++) {
        uint64_t p = codes[i], room = (i + 1 < codes.size() ? codes[i + 1] : d.size()) - p;
        std::vector<uint64_t> cands;
        for (uint64_t c : ctrls)
            if (x.u32(c + 0x704) <= room && fits(p, c)) cands.push_back(c);
        std::vector<uint64_t> adjacent;
        for (uint64_t c : cands)
            if (p >= c && p - c == 0x850) adjacent.push_back(c);
        if (adjacent.size() == 1) cands = adjacent;
        if (!cands.empty()) {
            uint32_t size = 0;
            for (uint64_t c : cands) size = std::max(size, x.u32(c + 0x704));
            cands.erase(std::remove_if(cands.begin(), cands.end(), [&](uint64_t c) { return x.u32(c + 0x704) != size; }),
                        cands.end());
        }
        std::set<std::string> keys;
        for (uint64_t c : cands) keys.insert(key(c));
        if (keys.size() == 1) found.push_back({p, cands[0]});
    }
    std::set<uint64_t> paired;
    for (auto& f : found) paired.insert(f.first);
    for (uint64_t c : ctrls) {
        uint64_t p = c + 0x850, s = x.u32(c + 0x704), plen = x.u32(c + 0x6f8);
        if (x.u32(c + 0x708) || !x.u32(c + 0x748) || paired.count(p) || p + s > d.size() || !(plen > 0 && plen <= s)) continue;
        if (x.u32(p) == kCode || x.u32(p) == kCompute || !x.has_exit(p, p + plen)) continue;
        found.push_back({p, c});
    }
    return found;
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

// gsc_shaders.py extract(): one harness group.
long extract(const std::string& group, const std::vector<uint8_t>& data, bool gsc, const std::string& out,
             std::set<std::string>& seen) {
    Data x{data};
    auto found = gsc ? hnsh_records(x) : exefs_blocks(x);
    std::string gdir = out + "/" + group;
    make_dir(gdir);
    std::string manifest =
        "id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\t"
        "code_sha1_16\tcode_file\tctrl_file\n";
    long n = 0;
    char line[512];
    for (auto [p, c] : found) {
        uint64_t s = x.u32(c + 0x704);
        if (!x.in(p, s)) continue;
        std::string h = sha1_16(data.data() + p, size_t(s));
        if (!seen.insert(h).second) continue;
        size_t clen = size_t(std::min<uint64_t>(0x880, data.size() - c));
        std::string cf = "ctrl_" + sha1_16(data.data() + c, clen) + ".bin";
        std::string stage = stage_of(x, p, c);
        write_file(gdir + "/code_" + h + ".bin", data.data() + p, size_t(s));
        write_file(gdir + "/" + cf, data.data() + c, clen);
        std::snprintf(line, sizeof line, "%s/0x%llx/%s\t%s\t0x%llx\t0x%llx\t0x%llx\t0x%x\t0x%x\t0x%x\t0x%x\t0x%x\t%s\tcode_%s.bin\t%s\n",
                      group.c_str(), (unsigned long long)p, stage.c_str(), stage.c_str(), (unsigned long long)p,
                      (unsigned long long)s, (unsigned long long)c, 0x880, x.u32(c + 0x708), x.u32(c + 0x6f8),
                      x.u32(c + 0x700), x.u32(c + 0x6fc), h.c_str(), h.c_str(), cf.c_str());
        manifest += line;
        n++;
    }
    write_file(gdir + "/manifest.tsv", reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    return n;
}

}  // namespace

long extract_shaders(const nxi::GameImage& game, const std::string& out_dir,
                     const std::function<void(int, int)>& progress, std::string& error) {
    try {
        make_dir(out_dir);
        const nxi::GameImage::Entry* cpk = game.find("CPK/ALL_USEU.CPK");
        if (!cpk) cpk = game.find("CPK/ALL_JP.CPK");
        if (!cpk) cpk = game.find("CPK/ALL_ASIA.CPK");
        if (!cpk) cpk = game.find("CPK/ALL.CPK");
        if (!cpk) throw Fail("the game file has no CPK/ALL_USEU.CPK or ALL_JP.CPK (is it Persona 5 Royal?)");
        auto main_it = game.exefs().find("main");
        if (main_it == game.exefs().end()) throw Fail("the game file's ExeFS has no main");
        std::vector<CpkFile> gscs;
        for (CpkFile& f : cpk_files(game, *cpk)) {
            std::string up_path = f.path;
            for (char& ch : up_path) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if (up_path.size() > 4 && up_path.compare(up_path.size() - 4, 4, ".GSC") == 0) gscs.push_back(f);
        }
        if (gscs.empty()) throw Fail("no shader caches (*.GSC) in CPK/ALL_USEU.CPK");
        std::set<std::string> seen;
        long total = 0;
        const int steps = int(gscs.size()) + 1;
        for (size_t i = 0; i < gscs.size(); i++) {
            if (progress) progress(int(i), steps);
            const CpkFile& f = gscs[i];
            std::vector<uint8_t> raw(size_t(f.size));
            if (game.read(*cpk, f.offset, raw.data(), raw.size()) != raw.size()) throw Fail("cannot read " + f.path);
            std::vector<uint8_t> data = f.extract != f.size ? crilayla(raw) : std::move(raw);
            // group: the path without its extension, non-alphanumerics as '_', lower case (gsc_shaders.py)
            std::string stem = f.path.substr(0, f.path.rfind('.')), group;
            for (char ch : stem) {
                bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
                if (ok) group += char(std::tolower(static_cast<unsigned char>(ch)));
                else if (group.empty() || group.back() != '_') group += '_';
            }
            while (!group.empty() && group.front() == '_') group.erase(group.begin());
            while (!group.empty() && group.back() == '_') group.pop_back();
            total += extract(group, data, true, out_dir, seen);
        }
        if (progress) progress(steps - 1, steps);
        std::vector<uint8_t> flat;
        if (!nxi::nso_to_flat_image(main_it->second, flat, error)) return -1;
        total += extract("exefs", flat, false, out_dir, seen);
        if (progress) progress(steps, steps);
        return total;
    } catch (const Fail& e) {
        error = e.what();
    } catch (const std::bad_alloc&) {
        error = "out of memory";
    }
    return -1;
}

}  // namespace p5r
