// ASTC LDR decoder (nvn_astc.h), written from the Khronos Data Format Specification, "ASTC Compressed Texture Image
// Formats": block mode, partitions, colour endpoint modes, integer sequence encoding, weight infill, void extent.
// HDR endpoint modes and HDR void extents decode to the error colour: Wonder's textures are LDR.
#include "nvn_astc.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace nx::nvn {

namespace {

struct Block {
    uint64_t w[2];
    // Bits [pos, pos + n), n <= 32, LSB first.
    uint32_t get(int pos, int n) const {
        if (n <= 0) return 0;
        uint64_t v = pos >= 64 ? w[1] >> (pos - 64) : pos + n <= 64 ? w[0] >> pos : (w[0] >> pos) | (w[1] << (64 - pos));
        return uint32_t(v & ((1ull << n) - 1));
    }
};

uint64_t reverse64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    v = ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFull) | ((v & 0x0000FFFF0000FFFFull) << 16);
    return (v >> 32) | (v << 32);
}

// The 21 quantization ranges, in order: levels = (3 if trits, 5 if quints, else 1) << bits.
struct Range {
    uint8_t trits, quints, bits;
};
constexpr Range kRanges[21] = {{0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3}, {0, 1, 1},
                               {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3}, {0, 0, 5}, {0, 1, 3}, {1, 0, 4},
                               {0, 0, 6}, {0, 1, 4}, {1, 0, 5}, {0, 0, 7}, {0, 1, 5}, {1, 0, 6}, {0, 0, 8}};

int ise_bits(int count, int r) {
    const Range& q = kRanges[r];
    return count * q.bits + (q.trits ? (8 * count + 4) / 5 : q.quints ? (7 * count + 2) / 3 : 0);
}

struct IseValue {
    uint8_t digit = 0;  // the trit or quint
    uint32_t low = 0;   // the plain bits below it
};

// Reads bits of one encoded sequence; bits past its end read as zero (a truncated last trit/quint block).
struct Reader {
    const Block& b;
    int pos, end;
    uint32_t read(int n) {
        int have = std::max(0, std::min(n, end - pos));
        uint32_t v = b.get(pos, have);
        pos += n;
        return v;
    }
};

void trits_of(uint32_t t, uint8_t* out) {
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1u; };
    uint32_t c, t3, t4;
    if (((t >> 2) & 7) == 7) {
        c = ((t >> 5) & 7) << 2 | (t & 3), t4 = 2, t3 = 2;
    } else {
        c = t & 0x1F;
        if (((t >> 5) & 3) == 3) t4 = 2, t3 = bit(t, 7);
        else t4 = bit(t, 7), t3 = (t >> 5) & 3;
    }
    uint32_t t0, t1, t2;
    if ((c & 3) == 3) t2 = 2, t1 = bit(c, 4), t0 = bit(c, 3) << 1 | (bit(c, 2) & ~bit(c, 3) & 1u);
    else if (((c >> 2) & 3) == 3) t2 = 2, t1 = 2, t0 = c & 3;
    else t2 = bit(c, 4), t1 = (c >> 2) & 3, t0 = bit(c, 1) << 1 | (bit(c, 0) & ~bit(c, 1) & 1u);
    out[0] = uint8_t(t0), out[1] = uint8_t(t1), out[2] = uint8_t(t2), out[3] = uint8_t(t3), out[4] = uint8_t(t4);
}

void quints_of(uint32_t q, uint8_t* out) {
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1u; };
    uint32_t q0, q1, q2;
    if (((q >> 1) & 3) == 3 && ((q >> 5) & 3) == 0) {
        q2 = bit(q, 0) << 2 | (bit(q, 4) & ~bit(q, 0) & 1u) << 1 | (bit(q, 3) & ~bit(q, 0) & 1u), q1 = 4, q0 = 4;
    } else {
        uint32_t c;
        if (((q >> 1) & 3) == 3) q2 = 4, c = ((q >> 3) & 3) << 3 | ((~q >> 5) & 3) << 1 | (q & 1);
        else q2 = (q >> 5) & 3, c = q & 0x1F;
        if ((c & 7) == 5) q1 = 4, q0 = (c >> 3) & 3;
        else q1 = (c >> 3) & 3, q0 = c & 7;
    }
    out[0] = uint8_t(q0), out[1] = uint8_t(q1), out[2] = uint8_t(q2);
}

void ise_decode(const Block& b, int pos, int count, int r, IseValue* out) {
    const Range& q = kRanges[r];
    const int m = q.bits;
    Reader rd{b, pos, pos + ise_bits(count, r)};
    if (q.trits) {
        for (int i = 0; i < count; i += 5) {
            uint32_t low[5], t = 0;
            low[0] = rd.read(m), t |= rd.read(2);
            low[1] = rd.read(m), t |= rd.read(2) << 2;
            low[2] = rd.read(m), t |= rd.read(1) << 4;
            low[3] = rd.read(m), t |= rd.read(2) << 5;
            low[4] = rd.read(m), t |= rd.read(1) << 7;
            uint8_t d[5];
            trits_of(t, d);
            for (int j = 0; j < 5 && i + j < count; j++) out[i + j] = {d[j], low[j]};
        }
    } else if (q.quints) {
        for (int i = 0; i < count; i += 3) {
            uint32_t low[3], v = 0;
            low[0] = rd.read(m), v |= rd.read(3);
            low[1] = rd.read(m), v |= rd.read(2) << 3;
            low[2] = rd.read(m), v |= rd.read(2) << 5;
            uint8_t d[3];
            quints_of(v, d);
            for (int j = 0; j < 3 && i + j < count; j++) out[i + j] = {d[j], low[j]};
        }
    } else {
        for (int i = 0; i < count; i++) out[i] = {0, rd.read(m)};
    }
}

uint32_t replicate(uint32_t v, int from, int to) {
    if (!from) return 0;
    uint32_t r = 0;
    for (int n = 0; n < to;) {
        int take = std::min(from, to - n);
        r = r << take | v >> (from - take);
        n += take;
    }
    return r;
}

uint8_t unquantize_color(const IseValue& v, int r) {
    const Range& q = kRanges[r];
    const int m = q.bits;
    if (!q.trits && !q.quints) return uint8_t(replicate(v.low, m, 8));
    const uint32_t a = v.low & 1 ? 0x1FF : 0, bits = v.low >> 1;
    uint32_t b = 0, c = 0;
    if (q.trits) {
        static constexpr uint32_t kC[7] = {0, 204, 93, 44, 22, 11, 5};
        c = kC[m];
        if (m == 2) b = (bits & 1) * 0x116;  // b000b0bb0
        else if (m == 3) b = bits << 7 | bits << 2 | bits;
        else if (m == 4) b = bits << 6 | bits;
        else if (m == 5) b = bits << 5 | bits >> 2;
        else if (m == 6) b = bits << 4 | bits >> 4;
    } else {
        static constexpr uint32_t kC[6] = {0, 113, 54, 26, 13, 6};
        c = kC[m];
        if (m == 2) b = (bits & 1) * 0x10C;  // b0000bb00
        else if (m == 3) b = bits << 7 | bits << 1 | bits >> 1;
        else if (m == 4) b = bits << 6 | bits >> 1;
        else if (m == 5) b = bits << 5 | bits >> 3;
    }
    uint32_t t = (v.digit * c + b) ^ a;
    return uint8_t((a & 0x80) | (t >> 2));
}

uint8_t unquantize_weight(const IseValue& v, int r) {
    const Range& q = kRanges[r];
    const int m = q.bits;
    uint32_t w;
    if (!q.trits && !q.quints) {
        w = replicate(v.low, m, 6);
    } else if (m == 0) {
        static constexpr uint8_t kT[3] = {0, 32, 63}, kQ[5] = {0, 16, 32, 47, 63};
        w = q.trits ? kT[v.digit] : kQ[v.digit];
    } else {
        const uint32_t a = v.low & 1 ? 0x7F : 0, bits = v.low >> 1;
        uint32_t b = 0, c;
        if (q.trits) {
            c = m == 1 ? 50 : m == 2 ? 23 : 11;
            if (m == 2) b = (bits & 1) * 0x45;  // b000b0b
            else if (m == 3) b = bits << 5 | bits;
        } else {
            c = m == 1 ? 28 : 13;
            if (m == 2) b = (bits & 1) * 0x42;  // b0000b0
        }
        uint32_t t = (v.digit * c + b) ^ a;
        w = (a & 0x20) | (t >> 2);
    }
    return uint8_t(w > 32 ? w + 1 : w);
}

bool decode_mode(uint32_t mode, int& x, int& y, bool& dual, int& range) {
    uint32_t r = (mode >> 4) & 1, h = (mode >> 9) & 1, d = (mode >> 10) & 1, a = (mode >> 5) & 3;
    if (mode & 3) {
        r |= (mode & 3) << 1;
        uint32_t b = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
            case 0: x = int(b + 4), y = int(a + 2); break;
            case 1: x = int(b + 8), y = int(a + 2); break;
            case 2: x = int(a + 2), y = int(b + 8); break;
            default:
                b &= 1;
                if (mode & 0x100) x = int(b + 2), y = int(a + 2);
                else x = int(a + 2), y = int(b + 6);
        }
    } else {
        if (((mode >> 2) & 3) == 0) return false;
        r |= ((mode >> 2) & 3) << 1;
        uint32_t b = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
            case 0: x = 12, y = int(a + 2); break;
            case 1: x = int(a + 2), y = 12; break;
            case 2: x = int(a + 6), y = int(b + 6), d = 0, h = 0; break;
            default:
                switch ((mode >> 5) & 3) {
                    case 0: x = 6, y = 10; break;
                    case 1: x = 10, y = 6; break;
                    default: return false;
                }
        }
    }
    dual = d != 0;
    range = int(r) - 2 + 6 * int(h);
    return r >= 2 && range <= 11;
}

uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// bit_transfer_signed: `a` becomes a signed 6-bit offset, `b` the base with a's top bit.
void transfer(int& a, int& b) {
    b = (b >> 1) | (a & 0x80);
    a = (a >> 1) & 0x3F;
    if (a & 0x20) a -= 0x40;
}

void set(uint8_t* e, int r, int g, int b, int a) { e[0] = clamp8(r), e[1] = clamp8(g), e[2] = clamp8(b), e[3] = clamp8(a); }
void blue_contract(uint8_t* e, int r, int g, int b, int a) { set(e, (r + b) >> 1, (g + b) >> 1, b, a); }

// Endpoints of one partition from its colour values (0..255). False for the HDR modes.
bool endpoints(uint32_t cem, const uint8_t* c, uint8_t* e0, uint8_t* e1) {
    int v[8];
    for (int k = 0; k < 8; k++) v[k] = k < int((cem >> 2) + 1) * 2 ? c[k] : 0;
    switch (cem) {
        case 0: set(e0, v[0], v[0], v[0], 255), set(e1, v[1], v[1], v[1], 255); return true;
        case 1: {
            int l0 = (v[0] >> 2) | (v[1] & 0xC0), l1 = std::min(255, l0 + (v[1] & 0x3F));
            set(e0, l0, l0, l0, 255), set(e1, l1, l1, l1, 255);
            return true;
        }
        case 4: set(e0, v[0], v[0], v[0], v[2]), set(e1, v[1], v[1], v[1], v[3]); return true;
        case 5:
            transfer(v[1], v[0]), transfer(v[3], v[2]);
            set(e0, v[0], v[0], v[0], v[2]), set(e1, v[0] + v[1], v[0] + v[1], v[0] + v[1], v[2] + v[3]);
            return true;
        case 6:
            set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255), set(e1, v[0], v[1], v[2], 255);
            return true;
        case 10:
            set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]), set(e1, v[0], v[1], v[2], v[5]);
            return true;
        case 8:
        case 12: {
            int a0 = cem == 12 ? v[6] : 255, a1 = cem == 12 ? v[7] : 255;
            if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4])
                set(e0, v[0], v[2], v[4], a0), set(e1, v[1], v[3], v[5], a1);
            else
                blue_contract(e0, v[1], v[3], v[5], a1), blue_contract(e1, v[0], v[2], v[4], a0);
            return true;
        }
        case 9:
        case 13: {
            transfer(v[1], v[0]), transfer(v[3], v[2]), transfer(v[5], v[4]);
            if (cem == 13) transfer(v[7], v[6]);
            else v[6] = 255, v[7] = 0;
            if (v[1] + v[3] + v[5] >= 0)
                set(e0, v[0], v[2], v[4], v[6]), set(e1, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]);
            else
                blue_contract(e0, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]),
                    blue_contract(e1, v[0], v[2], v[4], v[6]);
            return true;
        }
        default: return false;  // 2, 3, 7, 11, 14, 15: HDR
    }
}

uint32_t hash52(uint32_t p) {
    p ^= p >> 15, p -= p << 17, p += p << 7, p += p << 4, p ^= p >> 5, p += p << 16, p ^= p >> 7, p ^= p >> 3;
    p ^= p << 6, p ^= p >> 17;
    return p;
}

int partition_of(uint32_t seed, uint32_t x, uint32_t y, uint32_t z, int count, bool small) {
    if (small) x <<= 1, y <<= 1, z <<= 1;
    seed += uint32_t(count - 1) * 1024;
    uint32_t rnum = hash52(seed);
    uint32_t s[12] = {rnum & 0xF,          (rnum >> 4) & 0xF,  (rnum >> 8) & 0xF,  (rnum >> 12) & 0xF,
                      (rnum >> 16) & 0xF,  (rnum >> 20) & 0xF, (rnum >> 24) & 0xF, (rnum >> 28) & 0xF,
                      (rnum >> 18) & 0xF,  (rnum >> 22) & 0xF, (rnum >> 26) & 0xF, ((rnum >> 30) | (rnum << 2)) & 0xF};
    for (uint32_t& v : s) v *= v;
    int sh1, sh2;
    if (seed & 1) sh1 = seed & 2 ? 4 : 5, sh2 = count == 3 ? 6 : 5;
    else sh1 = count == 3 ? 6 : 5, sh2 = seed & 2 ? 4 : 5;
    int sh3 = seed & 0x10 ? sh1 : sh2;
    for (int k = 0; k < 8; k++) s[k] >>= k & 1 ? sh2 : sh1;
    for (int k = 8; k < 12; k++) s[k] >>= sh3;
    uint32_t a = (s[0] * x + s[1] * y + s[10] * z + (rnum >> 14)) & 0x3F;
    uint32_t b = (s[2] * x + s[3] * y + s[11] * z + (rnum >> 10)) & 0x3F;
    uint32_t c = count < 3 ? 0 : (s[4] * x + s[5] * y + s[8] * z + (rnum >> 6)) & 0x3F;
    uint32_t d = count < 4 ? 0 : (s[6] * x + s[7] * y + s[9] * z + (rnum >> 2)) & 0x3F;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    return c >= d ? 2 : 3;
}

bool error_block(uint32_t bw, uint32_t bh, uint8_t* rgba) {
    for (uint32_t k = 0; k < bw * bh; k++) rgba[4 * k] = 0xFF, rgba[4 * k + 1] = 0, rgba[4 * k + 2] = 0xFF, rgba[4 * k + 3] = 0xFF;
    return false;
}

}  // namespace

bool astc_decode_block(const uint8_t* src, uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba) {
    Block b;
    std::memcpy(b.w, src, 16);
    const uint32_t mode = b.get(0, 11);
    if ((mode & 0x1FF) == 0x1FC) {  // void extent: one colour (the extent coordinates are only a hint)
        if (mode & 0x200) return error_block(bw, bh, rgba);  // HDR
        uint8_t c[4];
        for (int k = 0; k < 4; k++) c[k] = uint8_t(b.get(64 + 16 * k, 16) >> 8);
        for (uint32_t k = 0; k < bw * bh; k++) std::memcpy(rgba + 4 * k, c, 4);
        return true;
    }
    int wx = 0, wy = 0, wrange = 0;
    bool dual = false;
    if (!decode_mode(mode, wx, wy, dual, wrange) || wx > int(bw) || wy > int(bh)) return error_block(bw, bh, rgba);
    const int partitions = int(b.get(11, 2)) + 1, planes = dual ? 2 : 1, nweights = wx * wy * planes;
    const int wbits = ise_bits(nweights, wrange);
    if (nweights > 64 || wbits < 24 || wbits > 96 || (dual && partitions == 4)) return error_block(bw, bh, rgba);
    int below = 128 - wbits, color_start = 17;
    uint32_t cem[4] = {}, seed = 0;
    if (partitions == 1) {
        cem[0] = b.get(13, 4);
    } else {
        seed = b.get(13, 10), color_start = 29;
        uint32_t enc = b.get(23, 6);
        if ((enc & 3) == 0) {
            for (int p = 0; p < partitions; p++) cem[p] = enc >> 2;
        } else {
            const int extra = 3 * partitions - 4;
            below -= extra;
            uint32_t all = enc >> 2 | b.get(below, extra) << 4, base = (enc & 3) - 1;
            for (int p = 0; p < partitions; p++)
                cem[p] = (((all >> p) & 1) + base) << 2 | ((all >> (partitions + 2 * p)) & 3);
        }
    }
    uint32_t ccs = 0;
    if (dual) below -= 2, ccs = b.get(below, 2);
    int ncolors = 0;
    for (int p = 0; p < partitions; p++) ncolors += int((cem[p] >> 2) + 1) * 2;
    if (ncolors > 18 || below < color_start) return error_block(bw, bh, rgba);
    int crange = -1;
    for (int r = 20; r >= 0 && crange < 0; r--)
        if (ise_bits(ncolors, r) <= below - color_start) crange = r;
    if (crange < 4) return error_block(bw, bh, rgba);  // fewer than 6 levels: not a legal encoding
    IseValue cv[18];
    ise_decode(b, color_start, ncolors, crange, cv);
    uint8_t colors[18], ep[4][2][4];
    for (int k = 0; k < ncolors; k++) colors[k] = unquantize_color(cv[k], crange);
    for (int p = 0, at = 0; p < partitions; at += int((cem[p] >> 2) + 1) * 2, p++)
        if (!endpoints(cem[p], colors + at, ep[p][0], ep[p][1])) return error_block(bw, bh, rgba);

    const Block rev{{reverse64(b.w[1]), reverse64(b.w[0])}};  // the weights are stored from bit 127 down
    IseValue wv[64];
    ise_decode(rev, 0, nweights, wrange, wv);
    uint8_t w[64];
    for (int k = 0; k < nweights; k++) w[k] = unquantize_weight(wv[k], wrange);

    const int ds = (1024 + int(bw) / 2) / int(bw - 1), dt = (1024 + int(bh) / 2) / int(bh - 1);
    const bool small = bw * bh < 31;
    for (uint32_t t = 0; t < bh; t++)
        for (uint32_t s = 0; s < bw; s++) {
            const int gs = (ds * int(s) * (wx - 1) + 32) >> 6, gt = (dt * int(t) * (wy - 1) + 32) >> 6;
            const int js = std::min(gs >> 4, wx - 1), fs = gs & 15, jt = std::min(gt >> 4, wy - 1), ft = gt & 15;
            const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            const int s1 = std::min(js + 1, wx - 1), t1 = std::min(jt + 1, wy - 1);
            int pw[2];
            for (int pl = 0; pl < planes; pl++) {
                auto at = [&](int x, int y) { return int(w[(y * wx + x) * planes + pl]); };
                pw[pl] = (at(js, jt) * w00 + at(s1, jt) * w01 + at(js, t1) * w10 + at(s1, t1) * w11 + 8) >> 4;
            }
            const int part = partitions > 1 ? partition_of(seed, s, t, 0, partitions, small) : 0;
            uint8_t* o = rgba + 4 * (t * bw + s);
            for (int c = 0; c < 4; c++) {
                const int wt = dual && uint32_t(c) == ccs ? pw[1] : pw[0];
                const uint32_t e0 = ep[part][0][c], e1 = ep[part][1][c];
                const uint32_t c0 = srgb ? e0 << 8 | 0x80 : e0 * 257, c1 = srgb ? e1 << 8 | 0x80 : e1 * 257;
                o[c] = uint8_t(((c0 * uint32_t(64 - wt) + c1 * uint32_t(wt) + 32) >> 6) >> 8);
            }
        }
    return true;
}

uint64_t astc_decode_image(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices, uint32_t bw,
                           uint32_t bh, bool srgb, uint8_t* rgba) {
    const uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    const uint64_t rows = uint64_t(slices) * by;  // block rows over all slices
    auto decode_rows = [&](uint64_t first, uint64_t last) {
        uint64_t bad = 0;
        uint8_t texels[12 * 12 * 4];
        for (uint64_t row = first; row < last; row++) {
            const uint32_t z = uint32_t(row / by), y = uint32_t(row % by);
            for (uint32_t x = 0; x < bx; x++) {
                const uint8_t* src = blocks + (row * bx + x) * 16;
                bad += !astc_decode_block(src, bw, bh, srgb, texels);
                const uint32_t w = std::min(bw, width - x * bw), h = std::min(bh, height - y * bh);
                for (uint32_t r = 0; r < h; r++)
                    std::memcpy(rgba + ((uint64_t(z) * height + y * bh + r) * width + x * bw) * 4,
                                texels + r * bw * 4, w * 4);
            }
        }
        return bad;
    };
    // Big images over a few threads, by rows of blocks (each writes its own texel rows). The replay waits for them:
    // a 2048x2048 6x5 texture took ~40 ms on one core (DEBUG-2 #75).
    const uint64_t total = rows * bx;
    unsigned n = total >= 8192 ? std::min(6u, std::max(1u, std::thread::hardware_concurrency() / 2)) : 1u;
    if (n <= 1) return decode_rows(0, rows);
    std::vector<std::thread> pool;
    std::vector<uint64_t> bad(n, 0);
    for (unsigned k = 1; k < n; k++)
        pool.emplace_back([&, k] { bad[k] = decode_rows(rows * k / n, rows * (k + 1) / n); });
    bad[0] = decode_rows(0, rows / n);
    for (auto& t : pool) t.join();
    uint64_t sum = 0;
    for (uint64_t b : bad) sum += b;
    return sum;
}

}  // namespace nx::nvn
