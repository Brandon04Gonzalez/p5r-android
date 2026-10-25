// ASTC decoder test (runtime/src/nvn/nvn_astc.cpp). Built by CMake with -DNX_TESTS=ON.
//
//   astc_test --selftest
//       void-extent blocks (LDR colour, HDR -> error colour), reserved block modes -> error colour, and a sweep of
//       random blocks through every block size (no crash, every texel written).
//   astc_test <blocks.bin> <width> <height> <bw> <bh> <srgb 0|1> <out.rgba>
//       decodes a linear run of blocks (tiling_test output) to RGBA8; tests/astc_check.py compares it with an
//       independent decoder on the game's own textures.
#define _CRT_SECURE_NO_WARNINGS  // fopen: the same code builds on every host
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "nvn_astc.h"

using nx::nvn::astc_decode_block;
using nx::nvn::astc_decode_image;

namespace {

int g_failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), g_failed++;
}

bool all_texels(const uint8_t* rgba, int n, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int k = 0; k < n; k++)
        if (rgba[4 * k] != r || rgba[4 * k + 1] != g || rgba[4 * k + 2] != b || rgba[4 * k + 3] != a) return false;
    return true;
}

int selftest() {
    uint8_t block[16], out[12 * 12 * 4];
    // Void extent, LDR: block mode 0x1FC, extent all ones, colour (R, G, B, A) as UNORM16 in bits 64..127.
    std::memset(block, 0xFF, 8);
    block[0] = 0xFC, block[1] = 0xFD;  // bits 0..8 = 0x1FC, bit 9 (HDR) = 0, bits 10..11 = 1
    const uint16_t c[4] = {0x1234, 0xABCD, 0x8000, 0xFFFF};
    std::memcpy(block + 8, c, 8);
    expect(astc_decode_block(block, 4, 4, false, out), "void extent decodes");
    expect(all_texels(out, 16, 0x12, 0xAB, 0x80, 0xFF), "void extent colour is the top byte of each channel");
    // The same with the HDR bit: the error colour.
    block[1] = 0xFF;
    expect(!astc_decode_block(block, 6, 6, false, out), "HDR void extent refused");
    expect(all_texels(out, 36, 0xFF, 0, 0xFF, 0xFF), "HDR void extent is magenta");
    // Block mode with bits 0..3 all zero is reserved.
    std::memset(block, 0, 16);
    expect(!astc_decode_block(block, 4, 4, true, out), "reserved block mode refused");
    expect(all_texels(out, 16, 0xFF, 0, 0xFF, 0xFF), "reserved block mode is magenta");
    // Random blocks at every legal footprint: never crash, never leave a texel unwritten.
    static const int sizes[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5},
                                   {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    std::mt19937 rng(1234);
    int valid = 0, total = 0;
    for (const auto& s : sizes)
        for (int k = 0; k < 20000; k++) {
            for (uint8_t& v : block) v = uint8_t(rng());
            // Decoded over two different fills: the results must be the same (every texel written, deterministic).
            uint8_t again[12 * 12 * 4];
            std::memset(out, 0x5A, sizeof out), std::memset(again, 0xA5, sizeof again);
            valid += astc_decode_block(block, uint32_t(s[0]), uint32_t(s[1]), (k & 1) != 0, out);
            astc_decode_block(block, uint32_t(s[0]), uint32_t(s[1]), (k & 1) != 0, again);
            total++;
            if (std::memcmp(out, again, size_t(s[0]) * s[1] * 4) != 0) {
                std::printf("block %dx%d:", s[0], s[1]);
                for (uint8_t v : block) std::printf(" %02x", v);
                std::printf("\n");
                expect(false, "every texel written");
                break;
            }
        }
    std::printf("astc_test: %d of %d random blocks were valid encodings\n", valid, total);
    expect(valid > total / 20, "a fair share of random blocks decode (the decoder is not refusing everything)");
    // An image whose size is not a multiple of the block: edge blocks are clipped.
    std::vector<uint8_t> blocks(2 * 2 * 16), img(7 * 5 * 4);
    for (int b = 0; b < 4; b++) {
        std::memset(blocks.data() + 16 * b, 0xFF, 8);
        blocks[16 * b] = 0xFC, blocks[16 * b + 1] = 0xFD;
        const uint16_t cc[4] = {uint16_t(0x1000 * (b + 1)), 0, 0, 0xFFFF};
        std::memcpy(blocks.data() + 16 * b + 8, cc, 8);
    }
    expect(astc_decode_image(blocks.data(), 7, 5, 1, 4, 4, false, img.data()) == 0, "image decodes");
    expect(img[(4 * 7 + 6) * 4] == 0x40, "bottom-right texel comes from the fourth block");
    std::printf("astc_test: %s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc != 8) {
        std::printf("usage: astc_test --selftest | <blocks.bin> <width> <height> <bw> <bh> <srgb 0|1> <out.rgba>\n");
        return 2;
    }
    const uint32_t w = uint32_t(std::atoi(argv[2])), h = uint32_t(std::atoi(argv[3]));
    const uint32_t bw = uint32_t(std::atoi(argv[4])), bh = uint32_t(std::atoi(argv[5]));
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    const uint64_t need = uint64_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    std::vector<uint8_t> blocks(need);
    const size_t got = std::fread(blocks.data(), 1, need, f);
    std::fclose(f);
    if (got != need) return std::printf("short file: %zu of %llu bytes\n", got, (unsigned long long)need), 2;
    std::vector<uint8_t> rgba(uint64_t(w) * h * 4);
    const uint64_t bad = astc_decode_image(blocks.data(), w, h, 1, bw, bh, argv[6][0] == '1', rgba.data());
    f = std::fopen(argv[7], "wb");
    if (!f) return 2;
    std::fwrite(rgba.data(), 1, rgba.size(), f);
    std::fclose(f);
    std::printf("%llu invalid blocks\n", (unsigned long long)bad);
    return 0;
}
