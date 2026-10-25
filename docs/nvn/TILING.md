# Block-linear textures: untiling for Vulkan

The Switch GPU (Maxwell, in the Tegra X1) does not store textures row by row. It uses a tiled layout called
**block linear**, which keeps texels that are close in 2D close in memory. Nintendo games ship their textures
already tiled (BNTX files) and copy them into NVN memory pools as is. Vulkan's `vkCmdCopyBufferToImage` wants plain
rows, so every such texture is untiled on its way to the host GPU. Code: `runtime/src/nvn/nvn_tiling.{h,cpp}`.

## The layout

Everything is counted in **blocks**: one texel for plain formats, one 4x4 (or ASTC NxM) block for compressed ones.
A row of blocks is `width_blocks * bytes_per_block` bytes.

- **GOB** ("group of bytes"): 64 bytes wide x 8 rows = 512 bytes. Inside a GOB, the byte at (x, y) is at
  `(x/32)*256 + (y/2)*64 + ((x/16)%2)*32 + (y%2)*16 + x%16`. Each aligned 16-byte piece of a row is contiguous, so a
  GOB row is four 16-byte copies from offsets 0, 32, 256 and 288 (plus the row start `(y/2)*64 + (y%2)*16`).
- **Block**: GOBs stacked vertically, 1..32 of them: the **block height** (a power of two). For 3D textures blocks
  are also stacked in depth (block depth).
- Blocks go left to right across the image (the row is padded to a whole GOB, 64 bytes), then down (the height is
  padded to a whole block), then, for 3D, back.

A level's size is `div_up(row bytes, 64) * div_up(rows, 8 * block height) * 512 * block height`.

## Mip chains and arrays

Walked the way `runtime/src/nvn/nvn_texture_layout.cpp` computes storage sizes:

- Level 0's block height comes from the texture (see below).
- Each smaller level uses `min(level 0's, smallest power of two >= level rows / 8)`: the hardware halves the block
  height while the level fits in half a block.
- Levels lie back to back, each padded as above, no extra alignment between them.
- Array layers are one mip chain apart, the chain being aligned to one level-0 block (`512 * block height`).

## API

```cpp
// One level / layer (or a 3D level with depth and block depth) into tight rows.
void deswizzle_block_linear(src, dst, width_blocks, height_blocks, depth, bytes_per_block,
                            block_height_gobs, block_depth_gobs);
uint64_t block_linear_size(...);            // bytes that call reads
TextureLayout block_linear_layout(...);     // offsets only (tiled and linear), no copy
std::vector<LinearLevel> deswizzle_texture(tiled, tiled_size, width, height, layers, levels,
                                           block_w, block_h, bytes_per_block, linear_out,
                                           block_height_gobs0 = 0);
```

The linear output is **level-major**: all layers of level 0, then all layers of level 1, ... Each `LinearLevel`
gives the offset (a multiple of 16 and of the block size), the texel extent and the block counts, so one
`VkBufferImageCopy` per level (with `bufferRowLength = bufferImageHeight = 0`, `layerCount = layers`) uploads it.
`deswizzle_texture` returns an empty vector, without writing, if `tiled_size` is too small for the layout.

Speed on the M1: a 1920x1080 RGBA8 level in about 0.4 ms (~20 GB/s), a 2048x2048 BC7 chain with 12 levels in
about 2 ms. The copy walks the source in order, one GOB at a time; no division per texel.

## Block height: take it from the texture

Level 0's block height is a choice made when the texture is created, and the data only reads back correctly with
the same value. `default_block_height_gobs()` is `nvn_texture_layout.cpp`'s choice (smallest power of two >= rows / 8,
at most 16). **BNTX textures carry their own**, and it is often smaller:

- BRTI `+0x34` ("texture layout", the value some tools call `sizeRange` or `blockHeightLog2`): bits 0..2 are
  **log2 of the block height in GOBs**, so `block_height_gobs = 1 << (layout & 7)`. `+0x38` holds `0x10007` in every
  Wonder texture. These 8 bytes are presumably what nn::gfx hands to `nvnTextureBuilderSetPackagedTextureLayout`
  (the game loads that function); to verify on the trace.
- Over all 13,171 block-linear textures in Wonder's `Model/`, `UI/` and `Layout/` (`tools/maxwell/bntx.py --survey`),
  the file's value equals `default_block_height_gobs()` for 12,731 and is **one step smaller** for 440, e.g. 300x300
  ASTC 4x4 (75 rows): file 8, `nvn_texture_layout.cpp` 16. Untiling those with 16 garbles them. So packaged textures
  must be untiled with the file's value (`block_height_gobs0`).
- The file's value fits, with no exception in the game, "smallest power of two >= rows * 3 / 32, at most 16" (rows
  1-10 give 1, 11-21 give 2, 22-42 give 4, 43-85 give 8, then 16). This is a fit, not the driver's code.

## BNTX sizes vs. nvn_texture_layout.cpp

With the file's block height, the formula matches the data exactly: every mip offset of all 13,171 textures, and
the image size of all 442 arrays (layers x aligned chain). Two differences remain:

- Single-layer textures: the BNTX image size is the **unaligned** chain; `nvn_texture_layout.cpp` rounds it up to a
  block (e.g. 0x55e00 vs 0x56000 for 512x512 BC5 with mips). The formula is never smaller than the file.
- The BNTX alignment field is 0x200 everywhere; `nvn_texture_layout.cpp` returns 512 * block height.

Where the file's block height is smaller, the formula with its own choice still gives a size >= the file's
(checked on all textures), so allocations are safe; only the untiling must use the file's value.

## Not covered

- True 3D textures through `deswizzle_texture`: like `nvn_texture_layout.cpp` it treats depth as layers. The
  single-level function does handle depth with a block depth; no Wonder BNTX is 3D, so it is only tested on
  synthetic data.
- Pitch-linear textures (tile mode 1, `stride` set in NVN): no untiling needed; none in Wonder's BNTX files.
- Tiling back (readbacks) and a compute-shader version: not written yet.

## Testing

```sh
clang++ -std=c++20 -O2 -Iruntime/src/nvn tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp -o private/tiling_test
private/tiling_test --selftest           # fast path vs. the byte address formula, 3000 random shapes, + timing
private/venv/bin/python tools/maxwell/bntx.py private/wonder/romfs/Layout/Title.Nin_NX_NVN.blarc.zs   # list
private/venv/bin/python tools/maxwell/bntx.py <file> --extract <texture> private/wonder/tiling_test/raw
private/tiling_test private/wonder/tiling_test/raw/<texture>.tiled.bin private/wonder/tiling_test/raw/<texture>.txt \
    private/wonder/tiling_test/raw     # linear blocks + level table; PPM for RGBA8-like formats
```

To look at compressed output on macOS, wrap a level's blocks in a KTX1 header and convert with `sips` (ImageIO
decodes ASTC, BC1, BC5 and BC7; its BC4 output was wrong on small textures, so BC4 was checked with a small Python
decoder). Checked this way: the 1920x1080 title background, the 662x390 logo, small icons (117x114, 40x40), BC1,
BC5, BC7 and ASTC 6x5 mip chains up to 2048x2048 with 12 levels, and a 33-layer array.
