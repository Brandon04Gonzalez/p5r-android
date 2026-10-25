// ASTC LDR decoding on the CPU, for GPUs without textureCompressionASTC_LDR (NVIDIA and AMD desktop GPUs). Wonder
// ships some textures as ASTC (the title key art among them); on such a GPU they are decoded to RGBA8 at upload
// (nvn_upload.cpp), from the Khronos Data Format Specification's ASTC chapter. docs/nvn/WINDOWS-PLAN.md, DEBUG-2 #69.
//
// Self-contained (no nvn.h, no Vulkan): tests/astc_test.cpp compiles it on its own.
#pragma once

#include <cstdint>

namespace nx::nvn {

// One 16-byte block of bw x bh texels (4..12) into bw * bh RGBA8 texels, rows of bw. `srgb` selects the sRGB decode
// mode (endpoints expanded as e << 8 | 0x80). Blocks that are invalid or HDR decode to the error colour, magenta.
// Returns false for those.
bool astc_decode_block(const uint8_t* block, uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgba);

// A linear run of blocks (rows of ceil(width / bw) blocks, `slices` images one after the other) into tightly packed
// RGBA8 rows of `width` texels, `slices` images of width * height texels. Returns the number of invalid blocks.
uint64_t astc_decode_image(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t slices, uint32_t bw,
                           uint32_t bh, bool srgb, uint8_t* rgba);

}  // namespace nx::nvn
