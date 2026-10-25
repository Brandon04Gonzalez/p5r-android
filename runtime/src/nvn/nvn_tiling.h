// Maxwell block-linear -> linear conversion for texture uploads (docs/nvn/TILING.md). Games load pre-tiled BNTX data
// straight into NVN memory pools; Vulkan's vkCmdCopyBufferToImage wants rows of texels, so every texture the game
// fills this way is untiled once on its way to the host GPU.
//
// Self-contained on purpose (no nvn.h, no Vulkan): tests/tiling_test.cpp compiles it on its own.
#pragma once

#include <cstdint>
#include <vector>

namespace nx::nvn {

// A GOB ("group of bytes") is 64 bytes x 8 rows = 512 bytes. GOBs stack vertically into blocks of 1..32 GOBs (the
// block height) and, for 3D textures, in depth (the block depth); blocks are laid out left to right, then top to
// bottom, then front to back.
constexpr uint32_t kGobWidth = 64, kGobHeight = 8, kGobSize = 512;

// Copies one mip level / array layer of a block-linear surface into a tightly packed linear buffer (rows of
// width_blocks * bytes_per_block, then depth slices of height_blocks rows). All sizes are in blocks: a compressed
// format's 4x4 (or ASTC NxM) block counts as one. block_height_gobs and block_depth_gobs are powers of two.
// `src` must hold the whole level: div_up(height_blocks, 8 * block_height_gobs) rows of blocks, each
// div_up(width_blocks * bytes_per_block, 64) blocks of 512 * block_height_gobs * block_depth_gobs bytes, times
// div_up(depth, block_depth_gobs); block_linear_size() computes it.
void deswizzle_block_linear(const uint8_t* src, uint8_t* dst, uint32_t width_blocks, uint32_t height_blocks,
                            uint32_t depth, uint32_t bytes_per_block, uint32_t block_height_gobs,
                            uint32_t block_depth_gobs);

// Bytes deswizzle_block_linear reads for such a level (GOB-aligned width, block-aligned height and depth).
uint64_t block_linear_size(uint32_t width_blocks, uint32_t height_blocks, uint32_t depth, uint32_t bytes_per_block,
                           uint32_t block_height_gobs, uint32_t block_depth_gobs);

// Block height of level 0 as nvn_texture_layout.cpp picks it: the smallest power of two >= rows / 8, at most 16.
// Packaged (BNTX) textures carry their own, often smaller, value; see docs/nvn/TILING.md.
uint32_t default_block_height_gobs(uint32_t rows);

// Block height of a mip level `rows` block rows tall, given level 0's: the hardware halves it while the level fits
// in half a block, so it is min(level 0's, the smallest power of two >= rows / 8).
uint32_t mip_block_height_gobs(uint32_t rows, uint32_t block_height_gobs0);

// One mip level of a texture in the linear buffer. The level's layers (or 2D-array slices) follow each other,
// layer_size apart, so one VkBufferImageCopy per level covers all layers (bufferRowLength/ImageHeight = 0).
struct LinearLevel {
    uint64_t offset = 0;         // in the linear buffer; a multiple of 16 and of bytes_per_block
    uint64_t layer_size = 0;     // width_blocks * height_blocks * bytes_per_block
    uint64_t tiled_offset = 0;   // of layer 0 of this level in the tiled data
    uint32_t width = 0, height = 0;               // in texels (the VkExtent3D of the copy)
    uint32_t width_blocks = 0, height_blocks = 0;
    uint32_t block_height_gobs = 1;               // the level's own, after mip reduction
};

struct TextureLayout {
    std::vector<LinearLevel> levels;
    uint32_t layers = 1;
    uint64_t tiled_layer_stride = 0;  // mip chain size aligned to one level-0 block (512 * block height)
    uint64_t tiled_size = 0;          // bytes of tiled data read: (layers - 1) strides + the last chain unaligned
    uint64_t linear_size = 0;
};

// Walks a mip chain / array exactly like nvn_texture_layout.cpp computes storage sizes: level 0's block height (from
// default_block_height_gobs() unless block_height_gobs0 is given, as BNTX textures require), levels back to back
// each with its own reduced block height, rows padded to 64 bytes and to the block height, layers one aligned chain
// apart. depth_or_layers counts layers: 3D textures are treated as arrays of slices, as nvn_texture_layout.cpp does.
TextureLayout block_linear_layout(uint32_t width, uint32_t height, uint32_t depth_or_layers, uint32_t levels,
                                  uint32_t block_w, uint32_t block_h, uint32_t bytes_per_block,
                                  uint32_t block_height_gobs0 = 0);

// Untiles a whole texture (every level of every layer) into linear_out, resized to fit. Returns the levels, or an
// empty vector if the parameters are invalid or `tiled_size` is smaller than the layout needs (nothing is written).
std::vector<LinearLevel> deswizzle_texture(const uint8_t* tiled, uint64_t tiled_size, uint32_t width, uint32_t height,
                                           uint32_t depth_or_layers, uint32_t levels, uint32_t block_w,
                                           uint32_t block_h, uint32_t bytes_per_block,
                                           std::vector<uint8_t>& linear_out, uint32_t block_height_gobs0 = 0);

}  // namespace nx::nvn
