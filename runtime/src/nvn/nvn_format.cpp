#include <algorithm>
#include <cstdlib>
#include <cstring>
// NVNformat -> VkFormat (docs/nvn/API.md). The NVN values index the SDK driver's own format table (sdk+d34198,
// 0x30 bytes each): its block size, bytes per block, bits per channel, integer/normalized flags, sRGB bit and
// depth/stencil kinds fix what each value is. The families come in the table's order:
//   R8 R16 R32 RG8 RG16 RG32 RGB8 RGB16 RGB32 RGBA8 RGBA16 RGBA32 (normalized, signed, integer variants), stencil and
//   depth formats, sRGB RGBA8, 16-bit packed, RGB10A2, R11G11B10F, RGB9E5, BC1-BC7, vertex-only formats, ASTC
//   4x4..12x12 (linear then sRGB), BGR(A) orders.
// The Apple M1 has no D24 formats: D24 becomes D32 float (same range for the game's 0..1 depths).
#include "nvn.h"

namespace nx::nvn {

namespace {

struct Entry {
    VkFormat vk;
    uint8_t aspect;  // 0 color, 1 depth, 2 stencil, 3 depth + stencil
};

constexpr Entry kFormats[] = {
    /* 0x00 */ {VK_FORMAT_UNDEFINED, 0},
    /* 0x01 */ {VK_FORMAT_R8_UNORM, 0}, {VK_FORMAT_R8_SNORM, 0}, {VK_FORMAT_R8_UINT, 0}, {VK_FORMAT_R8_SINT, 0},
    /* 0x05 */ {VK_FORMAT_R16_SFLOAT, 0}, {VK_FORMAT_R16_UNORM, 0}, {VK_FORMAT_R16_SNORM, 0}, {VK_FORMAT_R16_UINT, 0},
    /* 0x09 */ {VK_FORMAT_R16_SINT, 0},
    /* 0x0a */ {VK_FORMAT_R32_SFLOAT, 0}, {VK_FORMAT_R32_UINT, 0}, {VK_FORMAT_R32_SINT, 0},
    /* 0x0d */ {VK_FORMAT_R8G8_UNORM, 0}, {VK_FORMAT_R8G8_SNORM, 0}, {VK_FORMAT_R8G8_UINT, 0}, {VK_FORMAT_R8G8_SINT, 0},
    /* 0x11 */ {VK_FORMAT_R16G16_SFLOAT, 0}, {VK_FORMAT_R16G16_UNORM, 0}, {VK_FORMAT_R16G16_SNORM, 0},
    /* 0x14 */ {VK_FORMAT_R16G16_UINT, 0}, {VK_FORMAT_R16G16_SINT, 0},
    /* 0x16 */ {VK_FORMAT_R32G32_SFLOAT, 0}, {VK_FORMAT_R32G32_UINT, 0}, {VK_FORMAT_R32G32_SINT, 0},
    /* 0x19 */ {VK_FORMAT_R8G8B8_UNORM, 0}, {VK_FORMAT_R8G8B8_SNORM, 0}, {VK_FORMAT_R8G8B8_UINT, 0},
    /* 0x1c */ {VK_FORMAT_R8G8B8_SINT, 0},
    /* 0x1d */ {VK_FORMAT_R16G16B16_SFLOAT, 0}, {VK_FORMAT_R16G16B16_UNORM, 0}, {VK_FORMAT_R16G16B16_SNORM, 0},
    /* 0x20 */ {VK_FORMAT_R16G16B16_UINT, 0}, {VK_FORMAT_R16G16B16_SINT, 0},
    /* 0x22 */ {VK_FORMAT_R32G32B32_SFLOAT, 0}, {VK_FORMAT_R32G32B32_UINT, 0}, {VK_FORMAT_R32G32B32_SINT, 0},
    /* 0x25 */ {VK_FORMAT_R8G8B8A8_UNORM, 0}, {VK_FORMAT_R8G8B8A8_SNORM, 0}, {VK_FORMAT_R8G8B8A8_UINT, 0},
    /* 0x28 */ {VK_FORMAT_R8G8B8A8_SINT, 0},
    /* 0x29 */ {VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {VK_FORMAT_R16G16B16A16_UNORM, 0},
    /* 0x2b */ {VK_FORMAT_R16G16B16A16_SNORM, 0}, {VK_FORMAT_R16G16B16A16_UINT, 0}, {VK_FORMAT_R16G16B16A16_SINT, 0},
    /* 0x2e */ {VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {VK_FORMAT_R32G32B32A32_UINT, 0}, {VK_FORMAT_R32G32B32A32_SINT, 0},
    /* 0x31 */ {VK_FORMAT_S8_UINT, 2}, {VK_FORMAT_D16_UNORM, 1}, {VK_FORMAT_D32_SFLOAT, 1} /* D24 */,
    /* 0x34 */ {VK_FORMAT_D32_SFLOAT, 1}, {VK_FORMAT_D32_SFLOAT_S8_UINT, 3} /* D24S8 */, {VK_FORMAT_D32_SFLOAT_S8_UINT, 3},
    /* 0x37 */ {VK_FORMAT_R8G8B8A8_SRGB, 0} /* RGBX */, {VK_FORMAT_R8G8B8A8_SRGB, 0},
    /* 0x39 */ {VK_FORMAT_A4B4G4R4_UNORM_PACK16, 0}, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0},
    /* 0x3b */ {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0}, {VK_FORMAT_R5G6B5_UNORM_PACK16, 0},
    /* 0x3d */ {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0}, {VK_FORMAT_A2B10G10R10_UINT_PACK32, 0},
    // 0x3f R11G11B10F becomes RGBA16F: Maxwell stores negative/NaN results of blending as 0 in this unsigned float
    // format, Metal writes Inf/NaN, and the NaN spread through every later pass (bloom, the dialog's frosted-glass
    // blur: green screens). Twice the bandwidth, same values otherwise.
    /* 0x3f */ {VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0},
    /* 0x41 */ {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 0}, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0}, {VK_FORMAT_BC2_UNORM_BLOCK, 0},
    /* 0x44 */ {VK_FORMAT_BC3_UNORM_BLOCK, 0}, {VK_FORMAT_BC1_RGB_SRGB_BLOCK, 0}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 0},
    /* 0x47 */ {VK_FORMAT_BC2_SRGB_BLOCK, 0}, {VK_FORMAT_BC3_SRGB_BLOCK, 0},
    /* 0x49 */ {VK_FORMAT_BC4_UNORM_BLOCK, 0}, {VK_FORMAT_BC4_SNORM_BLOCK, 0}, {VK_FORMAT_BC5_UNORM_BLOCK, 0},
    /* 0x4c */ {VK_FORMAT_BC5_SNORM_BLOCK, 0}, {VK_FORMAT_BC7_UNORM_BLOCK, 0}, {VK_FORMAT_BC7_SRGB_BLOCK, 0},
    /* 0x4f */ {VK_FORMAT_BC6H_SFLOAT_BLOCK, 0}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, 0},
};

// 0x51..0x78: vertex-attribute-only formats and RGBX variants (not textures), then ASTC, then BGR orders.
constexpr VkFormat kAstc[] = {
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK,   VK_FORMAT_ASTC_5x4_UNORM_BLOCK,   VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_6x5_UNORM_BLOCK,   VK_FORMAT_ASTC_6x6_UNORM_BLOCK,   VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_8x6_UNORM_BLOCK,   VK_FORMAT_ASTC_8x8_UNORM_BLOCK,   VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
    VK_FORMAT_ASTC_10x6_UNORM_BLOCK,  VK_FORMAT_ASTC_10x8_UNORM_BLOCK,  VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
    VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
};
constexpr VkFormat kAstcSrgb[] = {
    VK_FORMAT_ASTC_4x4_SRGB_BLOCK,   VK_FORMAT_ASTC_5x4_SRGB_BLOCK,   VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_6x5_SRGB_BLOCK,   VK_FORMAT_ASTC_6x6_SRGB_BLOCK,   VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x6_SRGB_BLOCK,   VK_FORMAT_ASTC_8x8_SRGB_BLOCK,   VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x6_SRGB_BLOCK,  VK_FORMAT_ASTC_10x8_SRGB_BLOCK,  VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
    VK_FORMAT_ASTC_12x10_SRGB_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
};

bool g_astc_native = true;
uint32_t g_astc_decode = 3;  // bit 0 linear ASTC formats, bit 1 sRGB ones: decoded when not native (bisecting)
uint32_t g_astc_list[8] = {}, g_astc_listed = 0;  // NX_ASTC_DECODE=<hex format>,...: only those (bisecting)

}  // namespace

// Without textureCompressionASTC_LDR (NVIDIA, AMD) ASTC textures are RGBA8 images, decoded at upload (nvn_astc.cpp).
// NX_ASTC_DECODE=linear|srgb decodes only that half on a GPU that has ASTC (bisecting).
void set_astc_native(bool native) {
    g_astc_native = native;
    if (const char* e = std::getenv("NX_ASTC_DECODE"); e && native) {
        g_astc_decode = std::strcmp(e, "linear") == 0 ? 1u : std::strcmp(e, "srgb") == 0 ? 2u : 3u;
        g_astc_native = false;
        for (const char* p = e; std::strchr(p, ',') || std::strtoul(p, nullptr, 16) > 1; p = std::strchr(p, ',') + 1) {
            if (g_astc_listed < 8) g_astc_list[g_astc_listed++] = uint32_t(std::strtoul(p, nullptr, 16));
            if (!std::strchr(p, ',')) break;
        }
    }
}
bool astc_native() { return g_astc_native; }
bool is_astc(uint32_t f) { return f >= 0x79 && f <= 0x94; }
bool astc_decoded(uint32_t f) {
    if (g_astc_native || !is_astc(f)) return false;
    if (g_astc_listed) return std::find(g_astc_list, g_astc_list + g_astc_listed, f) != g_astc_list + g_astc_listed;
    return g_astc_decode >> (f >= 0x87 ? 1 : 0) & 1;
}

bool native_r11g11b10();  // nvn_caps.cpp
bool native_d24s8();      // nvn_caps.cpp

VkFormat vk_format(uint32_t f) {
    if (f == 0x3f && native_r11g11b10()) return VK_FORMAT_B10G11R11_UFLOAT_PACK32;  // nvn_caps.cpp
    if (f == 0x35 && native_d24s8()) return VK_FORMAT_D24_UNORM_S8_UINT;
    if (f < sizeof kFormats / sizeof kFormats[0]) return kFormats[f].vk;
    if (f >= 0x79 && f <= 0x86) return astc_decoded(f) ? VK_FORMAT_R8G8B8A8_UNORM : kAstc[f - 0x79];
    if (f >= 0x87 && f <= 0x94) return astc_decoded(f) ? VK_FORMAT_R8G8B8A8_SRGB : kAstcSrgb[f - 0x87];
    switch (f) {
        case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16;
        case 0x96:
        case 0x97: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
        case 0x98: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case 0x99:
        case 0x9a: return VK_FORMAT_B8G8R8A8_UNORM;
        case 0x9b:
        case 0x9c: return VK_FORMAT_B8G8R8A8_SRGB;
        default: return VK_FORMAT_UNDEFINED;
    }
}

uint32_t format_aspect(uint32_t f) {
    return f < sizeof kFormats / sizeof kFormats[0] ? kFormats[f].aspect : 0;
}

uint32_t vk_block_bytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
        case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_A4B4G4R4_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: return 2;
        case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT: return 3;
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
        case VK_FORMAT_R16G16B16_SFLOAT: case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: return 6;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: return 8;
        case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
            return 16;
        default: return 0;  // block-compressed, D32S8 (its depth and stencil copy apart), undefined
    }
}

}  // namespace nx::nvn
