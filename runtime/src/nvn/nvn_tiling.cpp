// Maxwell block-linear -> linear (docs/nvn/TILING.md).
//
// Inside a GOB (64 bytes x 8 rows) the byte at (x, y) sits at
//   (x / 32) * 256 + (y / 2) * 64 + ((x / 16) % 2) * 32 + (y % 2) * 16 + x % 16
// so every aligned 16-byte piece of a GOB row is contiguous in memory: a row is 4 copies of 16 bytes, from offsets
// 0, 32, 256 and 288 past the row's start ((y / 2) * 64 + (y % 2) * 16). The copies below work on those pieces
// (one NEON load/store pair each on the M1), GOB by GOB, so the source is read in order, 512 bytes at a time, and
// nothing divides per texel: block sizes are powers of two, handled with shifts.
#include "nvn_tiling.h"

#include <algorithm>
#include <cstring>

namespace nx::nvn {

namespace {

constexpr uint32_t kPiece[4] = {0, 32, 256, 288};  // where the 16-byte pieces of a GOB row start

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// log2 of a GOB count, rounding a bad (non power of two) value down and clamping to 1..32, so a corrupt parameter
// cannot make the walk read outside what block_linear_size() promised.
uint32_t gob_shift(uint32_t gobs) {
    uint32_t s = 0;
    while (s < 5 && (2u << s) <= gobs) s++;
    return s;
}

uint32_t row_start(uint32_t y) { return (y >> 1) * 64 + (y & 1) * 16; }

inline void copy16(uint8_t* d, const uint8_t* s) { std::memcpy(d, s, 16); }

// A whole GOB: 8 rows of 64 bytes. The common case, fully unrolled by the compiler.
inline void copy_gob(const uint8_t* gob, uint8_t* dst, uint64_t pitch) {
    for (uint32_t y = 0; y < kGobHeight; y++, dst += pitch) {
        const uint8_t* s = gob + row_start(y);
        copy16(dst, s);
        copy16(dst + 16, s + 32);
        copy16(dst + 32, s + 256);
        copy16(dst + 48, s + 288);
    }
}

// The edges: the bottom GOB row of a level whose height is not a multiple of 8, and the rightmost GOB of a level whose
// row is not a multiple of 64 bytes (`bytes` < 64; not always a multiple of 16, e.g. one 8-byte BC1 block).
void copy_gob_part(const uint8_t* gob, uint8_t* dst, uint64_t pitch, uint32_t rows, uint32_t bytes) {
    for (uint32_t y = 0; y < rows; y++, dst += pitch) {
        const uint8_t* s = gob + row_start(y);
        for (uint32_t p = 0; p < 4 && p * 16 < bytes; p++) {
            std::memcpy(dst + p * 16, s + kPiece[p], std::min<uint32_t>(16, bytes - p * 16));
        }
    }
}

}  // namespace

uint64_t block_linear_size(uint32_t width_blocks, uint32_t height_blocks, uint32_t depth, uint32_t bytes_per_block,
                           uint32_t block_height_gobs, uint32_t block_depth_gobs) {
    uint32_t bh = 1u << gob_shift(block_height_gobs), bd = 1u << gob_shift(block_depth_gobs);
    uint64_t gobs_x = div_up(uint64_t(width_blocks) * bytes_per_block, kGobWidth);
    return gobs_x * div_up(height_blocks, kGobHeight * bh) * div_up(depth, bd) * kGobSize * bh * bd;
}

void deswizzle_block_linear(const uint8_t* src, uint8_t* dst, uint32_t width_blocks, uint32_t height_blocks,
                            uint32_t depth, uint32_t bytes_per_block, uint32_t block_height_gobs,
                            uint32_t block_depth_gobs) {
    if (!src || !dst || !width_blocks || !height_blocks || !depth || !bytes_per_block) return;
    const uint32_t bh_shift = gob_shift(block_height_gobs), bd_shift = gob_shift(block_depth_gobs);
    const uint32_t bh_mask = (1u << bh_shift) - 1, bd_mask = (1u << bd_shift) - 1;
    const uint64_t pitch = uint64_t(width_blocks) * bytes_per_block;  // linear row
    const uint64_t full_gobs = pitch / kGobWidth;
    const uint32_t tail = uint32_t(pitch % kGobWidth);
    const uint64_t gobs_x = full_gobs + (tail != 0);
    const uint64_t block = uint64_t(kGobSize) << (bh_shift + bd_shift);           // one block of GOBs
    const uint64_t block_row = block * gobs_x;                                     // a row of blocks
    const uint64_t slab = block_row * div_up(height_blocks, kGobHeight << bh_shift);  // block_depth slices
    const uint64_t slice = pitch * height_blocks;                                  // linear slice

    for (uint32_t z = 0; z < depth; z++) {
        // Slices inside a block are whole column-of-GOBs apart: GOB (gy, gz) of a block is at (gz * bh + gy) * 512.
        const uint8_t* zsrc = src + (z >> bd_shift) * slab + (uint64_t(z & bd_mask) << (9 + bh_shift));
        uint8_t* zdst = dst + z * slice;
        for (uint32_t y = 0; y < height_blocks; y += kGobHeight) {
            const uint32_t gy = y / kGobHeight;
            const uint8_t* s = zsrc + (gy >> bh_shift) * block_row + (uint64_t(gy & bh_mask) << 9);
            uint8_t* d = zdst + y * pitch;
            const uint32_t rows = std::min<uint32_t>(kGobHeight, height_blocks - y);
            if (rows == kGobHeight) {
                for (uint64_t gx = 0; gx < full_gobs; gx++) copy_gob(s + gx * block, d + gx * kGobWidth, pitch);
            } else {
                for (uint64_t gx = 0; gx < full_gobs; gx++) {
                    copy_gob_part(s + gx * block, d + gx * kGobWidth, pitch, rows, kGobWidth);
                }
            }
            if (tail) copy_gob_part(s + full_gobs * block, d + full_gobs * kGobWidth, pitch, rows, tail);
        }
    }
}

uint32_t default_block_height_gobs(uint32_t rows) {
    uint32_t gobs = uint32_t(div_up(rows, kGobHeight)), h = 1;
    while (h < gobs && h < 16) h <<= 1;
    return h;
}

uint32_t mip_block_height_gobs(uint32_t rows, uint32_t block_height_gobs0) {
    uint32_t gobs = uint32_t(div_up(rows, kGobHeight)), h = 1;
    while (h < gobs && h < 32) h <<= 1;
    return std::min(h, 1u << gob_shift(block_height_gobs0));
}

TextureLayout block_linear_layout(uint32_t width, uint32_t height, uint32_t depth_or_layers, uint32_t levels,
                                  uint32_t block_w, uint32_t block_h, uint32_t bytes_per_block,
                                  uint32_t block_height_gobs0) {
    TextureLayout t;
    if (!width || !height || !block_w || !block_h || !bytes_per_block) return t;
    levels = std::max(1u, levels);
    t.layers = std::max(1u, depth_or_layers);
    uint32_t bh0 = block_height_gobs0 ? 1u << gob_shift(block_height_gobs0)
                                      : default_block_height_gobs(uint32_t(div_up(height, block_h)));
    // vkCmdCopyBufferToImage wants bufferOffset a multiple of the texel block size (and of 4 on some drivers);
    // 16 and bytes_per_block together (12 for RGB32 makes 48) satisfy both.
    uint64_t align = bytes_per_block;
    while (align % 16) align += bytes_per_block;

    uint64_t chain = 0, linear = 0;
    t.levels.reserve(levels);
    for (uint32_t level = 0; level < levels; level++) {
        LinearLevel l;
        l.width = std::max(1u, width >> level);
        l.height = std::max(1u, height >> level);
        l.width_blocks = uint32_t(div_up(l.width, block_w));
        l.height_blocks = uint32_t(div_up(l.height, block_h));
        l.block_height_gobs = mip_block_height_gobs(l.height_blocks, bh0);
        l.tiled_offset = chain;
        l.layer_size = uint64_t(l.width_blocks) * l.height_blocks * bytes_per_block;
        l.offset = div_up(linear, align) * align;
        linear = l.offset + l.layer_size * t.layers;
        chain += block_linear_size(l.width_blocks, l.height_blocks, 1, bytes_per_block, l.block_height_gobs, 1);
        t.levels.push_back(l);
    }
    uint64_t block = uint64_t(kGobSize) * bh0;
    t.tiled_layer_stride = div_up(chain, block) * block;
    t.tiled_size = t.tiled_layer_stride * (t.layers - 1) + chain;
    t.linear_size = linear;
    return t;
}

std::vector<LinearLevel> deswizzle_texture(const uint8_t* tiled, uint64_t tiled_size, uint32_t width, uint32_t height,
                                           uint32_t depth_or_layers, uint32_t levels, uint32_t block_w,
                                           uint32_t block_h, uint32_t bytes_per_block,
                                           std::vector<uint8_t>& linear_out, uint32_t block_height_gobs0) {
    TextureLayout t = block_linear_layout(width, height, depth_or_layers, levels, block_w, block_h, bytes_per_block,
                                          block_height_gobs0);
    if (!tiled || t.levels.empty() || t.tiled_size > tiled_size) return {};
    linear_out.resize(t.linear_size);
    // Level-major output (all layers of level 0, then of level 1, ...) while the source is layer-major: each
    // (level, layer) is still one contiguous source range, so the order of the walk does not matter for reads.
    for (const LinearLevel& l : t.levels) {
        for (uint32_t layer = 0; layer < t.layers; layer++) {
            deswizzle_block_linear(tiled + layer * t.tiled_layer_stride + l.tiled_offset,
                                   linear_out.data() + l.offset + layer * l.layer_size, l.width_blocks,
                                   l.height_blocks, 1, bytes_per_block, l.block_height_gobs, 1);
        }
    }
    return t.levels;
}

}  // namespace nx::nvn
