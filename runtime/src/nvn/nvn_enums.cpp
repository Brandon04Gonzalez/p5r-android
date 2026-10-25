// NVN enums -> Vulkan (docs/nvn/API.md). NVN numbers its enums like OpenGL, starting at 1; checked against the values
// Wonder's decoded state objects carry (NX_NVN_STATES=1): blend ONE,ZERO = 2,1 and SRC_ALPHA,ONE_MINUS_SRC_ALPHA =
// 5,6, equation ADD = 1, constant color 0x61; depth LESS 2, EQUAL 3, LEQUAL 4, GREATER 5, ALWAYS 8; polygon FILL 2;
// cull 0 none .. 3 both; primitives as GL (4 triangles); index types 0 u8, 1 u16, 2 u32.
#include "nvn.h"

namespace nx::nvn {

VkBlendFactor vk_blend_factor(uint32_t f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp vk_blend_op(uint32_t e) {
    switch (e) {
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp vk_compare(uint32_t f) {
    switch (f) {
        case 1: return VK_COMPARE_OP_NEVER;
        case 2: return VK_COMPARE_OP_LESS;
        case 3: return VK_COMPARE_OP_EQUAL;
        case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 5: return VK_COMPARE_OP_GREATER;
        case 6: return VK_COMPARE_OP_NOT_EQUAL;
        case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp vk_stencil_op(uint32_t op) {  // GL order: KEEP 1, ZERO 2, REPLACE 3, INCR 4, DECR 5, INVERT 6, INCR_WRAP 7, DECR_WRAP 8
    switch (op) {
        case 2: return VK_STENCIL_OP_ZERO;
        case 3: return VK_STENCIL_OP_REPLACE;
        case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 6: return VK_STENCIL_OP_INVERT;
        case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkCullModeFlags vk_cull(uint32_t face) {
    switch (face) {
        case 1: return VK_CULL_MODE_FRONT_BIT;
        case 2: return VK_CULL_MODE_BACK_BIT;
        case 3: return VK_CULL_MODE_FRONT_AND_BACK;
        default: return VK_CULL_MODE_NONE;
    }
}

VkPrimitiveTopology vk_topology(uint32_t mode) {
    switch (mode) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // 4; 7 quads are split by the caller
    }
}

VkIndexType vk_index_type(uint32_t type) {
    return type == 0 ? VK_INDEX_TYPE_UINT8_EXT : type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

uint32_t index_size(uint32_t type) { return type == 0 ? 1 : type == 1 ? 2 : 4; }

// Vertex attribute formats: the texture formats of the same numbers, plus the vertex-only ones (0x51..0x6c: scaled
// integers and 10-10-10-2 variants).
VkFormat vk_vertex_format(uint32_t f) {
    switch (f) {
        case 0x51: return VK_FORMAT_R8_USCALED;
        case 0x52: return VK_FORMAT_R8_SSCALED;
        case 0x53: return VK_FORMAT_R16_USCALED;
        case 0x54: return VK_FORMAT_R16_SSCALED;
        case 0x55: return VK_FORMAT_R32_UINT;  // 32-bit scaled has no Vulkan format: read as integer
        case 0x56: return VK_FORMAT_R32_SINT;
        case 0x57: return VK_FORMAT_R8G8_USCALED;
        case 0x58: return VK_FORMAT_R8G8_SSCALED;
        case 0x59: return VK_FORMAT_R16G16_USCALED;
        case 0x5a: return VK_FORMAT_R16G16_SSCALED;
        case 0x5b: return VK_FORMAT_R32G32_UINT;
        case 0x5c: return VK_FORMAT_R32G32_SINT;
        case 0x5d: return VK_FORMAT_R8G8B8_USCALED;
        case 0x5e: return VK_FORMAT_R8G8B8_SSCALED;
        case 0x5f: return VK_FORMAT_R16G16B16_USCALED;
        case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
        case 0x61: return VK_FORMAT_R32G32B32_UINT;
        case 0x62: return VK_FORMAT_R32G32B32_SINT;
        case 0x63: return VK_FORMAT_R8G8B8A8_USCALED;
        case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED;
        case 0x65: return VK_FORMAT_R16G16B16A16_USCALED;
        case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
        case 0x67: return VK_FORMAT_R32G32B32A32_UINT;
        case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
        case 0x6a: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
        case 0x6b: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case 0x6c: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        default: return vk_format(f);
    }
}

}  // namespace nx::nvn
