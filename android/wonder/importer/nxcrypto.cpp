// AES-128 / SHA-256 / CRC-32 (nxcrypto.h). Encryption (CTR, the bulk of the work) uses T-tables built at start-up
// from the S-box, which is itself computed from GF(2^8) arithmetic (no hand-typed tables to get wrong); decryption is
// only used on a few kilobytes (NCA headers, key unwrapping) and is the plain byte-wise inverse cipher.
#include "nxcrypto.h"

#include <cstring>

namespace nxi {

namespace {

uint8_t g_sbox[256], g_inv_sbox[256];
uint32_t g_te[4][256];

inline uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}
inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint8_t rotl8(uint8_t x, int n) { return uint8_t((x << n) | (x >> (8 - n))); }

struct TableInit {
    TableInit() {
        // S-box: multiplicative inverse (walking generator 3 and its inverse) + the affine map.
        uint8_t p = 1, q = 1;
        do {
            p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
            q ^= uint8_t(q << 1);
            q ^= uint8_t(q << 2);
            q ^= uint8_t(q << 4);
            if (q & 0x80) q ^= 0x09;
            g_sbox[p] = uint8_t(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
        } while (p != 1);
        g_sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) g_inv_sbox[g_sbox[i]] = uint8_t(i);
        for (int i = 0; i < 256; i++) {
            uint8_t s = g_sbox[i];
            // column contribution of a row-0 byte, little-endian word: (2s, s, s, 3s)
            uint32_t t = uint32_t(xtime(s)) | uint32_t(s) << 8 | uint32_t(s) << 16 | uint32_t(xtime(s) ^ s) << 24;
            g_te[0][i] = t;
            g_te[1][i] = rotl32(t, 8);
            g_te[2][i] = rotl32(t, 16);
            g_te[3][i] = rotl32(t, 24);
        }
    }
};
const TableInit g_tables;

inline uint32_t load_le(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void store_le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
}
inline uint32_t sub_word(uint32_t w) {
    return uint32_t(g_sbox[w & 0xff]) | uint32_t(g_sbox[(w >> 8) & 0xff]) << 8 | uint32_t(g_sbox[(w >> 16) & 0xff]) << 16 |
           uint32_t(g_sbox[w >> 24]) << 24;
}

}  // namespace

void secure_zero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}

Aes128::~Aes128() { secure_zero(rk_, sizeof rk_); }

void Aes128::set_key(const uint8_t key[16]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    for (int i = 0; i < 4; i++) rk_[i] = load_le(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk_[i - 1];
        if (i % 4 == 0) t = sub_word((t >> 8) | (t << 24)) ^ rcon[i / 4 - 1];
        rk_[i] = rk_[i - 4] ^ t;
    }
}

void Aes128::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint32_t s0 = load_le(in) ^ rk_[0], s1 = load_le(in + 4) ^ rk_[1], s2 = load_le(in + 8) ^ rk_[2],
             s3 = load_le(in + 12) ^ rk_[3];
    const uint32_t* rk = rk_ + 4;
    for (int r = 1; r < 10; r++, rk += 4) {
        uint32_t t0 = g_te[0][s0 & 0xff] ^ g_te[1][(s1 >> 8) & 0xff] ^ g_te[2][(s2 >> 16) & 0xff] ^ g_te[3][s3 >> 24] ^ rk[0];
        uint32_t t1 = g_te[0][s1 & 0xff] ^ g_te[1][(s2 >> 8) & 0xff] ^ g_te[2][(s3 >> 16) & 0xff] ^ g_te[3][s0 >> 24] ^ rk[1];
        uint32_t t2 = g_te[0][s2 & 0xff] ^ g_te[1][(s3 >> 8) & 0xff] ^ g_te[2][(s0 >> 16) & 0xff] ^ g_te[3][s1 >> 24] ^ rk[2];
        uint32_t t3 = g_te[0][s3 & 0xff] ^ g_te[1][(s0 >> 8) & 0xff] ^ g_te[2][(s1 >> 16) & 0xff] ^ g_te[3][s2 >> 24] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return uint32_t(g_sbox[a & 0xff]) | uint32_t(g_sbox[(b >> 8) & 0xff]) << 8 | uint32_t(g_sbox[(c >> 16) & 0xff]) << 16 |
               uint32_t(g_sbox[d >> 24]) << 24;
    };
    store_le(out, last(s0, s1, s2, s3) ^ rk[0]);
    store_le(out + 4, last(s1, s2, s3, s0) ^ rk[1]);
    store_le(out + 8, last(s2, s3, s0, s1) ^ rk[2]);
    store_le(out + 12, last(s3, s0, s1, s2) ^ rk[3]);
}

void Aes128::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    auto add_rk = [&](int round) {
        for (int c = 0; c < 4; c++) {
            uint32_t k = rk_[round * 4 + c];
            for (int r = 0; r < 4; r++) s[4 * c + r] ^= uint8_t(k >> (8 * r));
        }
    };
    auto inv_shift_sub = [&]() {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = g_inv_sbox[s[4 * c + r]];
        std::memcpy(s, t, 16);
    };
    std::memcpy(s, in, 16);
    add_rk(10);
    for (int round = 9; round >= 1; round--) {
        inv_shift_sub();
        add_rk(round);
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
            s[4 * c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
            s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
            s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
            s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
        }
    }
    inv_shift_sub();
    add_rk(0);
    std::memcpy(out, s, 16);
    secure_zero(s, sizeof s);
}

void aes_ecb_decrypt(const Aes128& k, uint8_t* data, size_t n) {
    for (size_t i = 0; i + 16 <= n; i += 16) k.decrypt_block(data + i, data + i);
}

void aes_ctr(const Aes128& k, const uint8_t iv[16], uint8_t* data, size_t n) {
    uint8_t ctr[16], ks[16];
    std::memcpy(ctr, iv, 16);
    for (size_t i = 0; i < n; i += 16) {
        k.encrypt_block(ctr, ks);
        size_t m = n - i < 16 ? n - i : 16;
        for (size_t j = 0; j < m; j++) data[i + j] ^= ks[j];
        for (int j = 15; j >= 0 && ++ctr[j] == 0; j--) {
        }
    }
}

void aes_xts_decrypt(const Aes128& data_key, const Aes128& tweak_key, uint64_t sector, size_t sector_size, uint8_t* data,
                     size_t n) {
    for (size_t off = 0; off + sector_size <= n; off += sector_size, sector++) {
        uint8_t tw[16] = {};
        for (int j = 0; j < 8; j++) tw[15 - j] = uint8_t(sector >> (8 * j));  // Nintendo: big-endian sector number
        tweak_key.encrypt_block(tw, tw);
        for (size_t b = 0; b < sector_size; b += 16) {
            uint8_t* p = data + off + b;
            for (int j = 0; j < 16; j++) p[j] ^= tw[j];
            data_key.decrypt_block(p, p);
            for (int j = 0; j < 16; j++) p[j] ^= tw[j];
            uint8_t carry = tw[15] >> 7;  // multiply the tweak by x (little-endian GF(2^128))
            for (int j = 15; j > 0; j--) tw[j] = uint8_t((tw[j] << 1) | (tw[j - 1] >> 7));
            tw[0] = uint8_t((tw[0] << 1) ^ (carry ? 0x87 : 0));
        }
    }
}

// ---- SHA-256 ----

namespace {
const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    if (used_) {
        size_t m = 64 - used_ < n ? 64 - used_ : n;
        std::memcpy(buf_ + used_, p, m);
        used_ += m, p += m, n -= m;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(p);
    std::memcpy(buf_, p, n);
    used_ = n;
}

void Sha256::final(uint8_t out[32]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (used_ != 56) update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = uint8_t(bits >> (56 - 8 * i));
    update(lenb, 8);
    for (int i = 0; i < 8; i++)
        out[4 * i] = uint8_t(h_[i] >> 24), out[4 * i + 1] = uint8_t(h_[i] >> 16), out[4 * i + 2] = uint8_t(h_[i] >> 8),
                out[4 * i + 3] = uint8_t(h_[i]);
}

void sha256(const void* data, size_t n, uint8_t out[32]) {
    Sha256 s;
    s.update(data, n);
    s.final(out);
}

// ---- CRC-32 (zlib polynomial), slicing by 8 ----

namespace {
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (int s = 1; s < 8; s++)
            for (int i = 0; i < 256; i++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
    }
};
const CrcTables g_crc;
}  // namespace

uint32_t crc32_update(uint32_t crc, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (n >= 8) {
        uint32_t a = crc ^ load_le(p), b = load_le(p + 4);
        crc = g_crc.t[7][a & 0xff] ^ g_crc.t[6][(a >> 8) & 0xff] ^ g_crc.t[5][(a >> 16) & 0xff] ^ g_crc.t[4][a >> 24] ^
              g_crc.t[3][b & 0xff] ^ g_crc.t[2][(b >> 8) & 0xff] ^ g_crc.t[1][(b >> 16) & 0xff] ^ g_crc.t[0][b >> 24];
        p += 8, n -= 8;
    }
    while (n--) crc = g_crc.t[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// ---- known answers ----

bool crypto_self_test() {
    // FIPS-197 appendix C.1
    const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    Aes128 a;
    a.set_key(key);
    uint8_t out[16], back[16];
    a.encrypt_block(pt, out);
    a.decrypt_block(out, back);
    if (std::memcmp(out, ct, 16) || std::memcmp(back, pt, 16)) return false;
    // SHA-256("abc")
    const uint8_t abc_hash[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    uint8_t h[32];
    sha256("abc", 3, h);
    if (std::memcmp(h, abc_hash, 32)) return false;
    // CRC-32("123456789") = cbf43926
    return crc32_update(0, "123456789", 9) == 0xCBF43926u;
}

}  // namespace nxi
