// NVN shader stages -> the SPIR-V translated offline (docs/nvn/TRANSLATOR.md, "Runtime side").
//
// Every Maxwell code block of the game was translated ahead of time by Ryujinx's translator (tools/maxwell/
// spirv_harness) into <data root>/spirv/<hash>.spv, <hash> = the first 16 hex digits of the SHA-1 of the whole code
// block. At nvnProgramSetShaders time the runtime hashes the block the game hands in, loads that file and needs to
// know what each descriptor of it stands for in NVN terms: which uniform block, which texture binding, which storage
// buffer. That is what this file provides.
//
// Everything above "the runtime cache" is plain C++ with no dependency on the rest of the runtime, so that
// tests/shader_cache_test.cpp can run the same code over all 12,717 shaders (build with -DNX_SHADERS_STANDALONE).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nx::nvn {

// ---- hashing ----------------------------------------------------------------------------------------------------

// SHA-1 (FIPS 180-4). Only a cache key here, not a security measure.
class Sha1 {
public:
    Sha1();
    void update(const void* data, size_t size);
    void finish(uint8_t digest[20]);  // the object is spent afterwards

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint64_t bytes_ = 0;
    uint8_t buf_[64];
    size_t used_ = 0;
};

std::string sha1_hex(const void* data, size_t size);                 // 40 lowercase hex digits
std::string shader_hash(const uint8_t* code, uint32_t size);          // the first 16 of them: the .spv file name

// ---- the NVN control section (docs/nvn/SHADERS.md) ----------------------------------------------------------------

constexpr uint32_t kShaderControlMagic = 0x98761234;

struct ShaderControl {
    bool valid = false;           // magic present
    uint32_t program_size = 0;    // +0x6f8: SPH + instructions, from program_start
    uint32_t const_size = 0;      // +0x6fc: constant data (c[1]) bytes, 0 = none
    uint32_t const_offset = 0;    // +0x700: its offset in the code block (0x100-aligned)
    uint32_t code_size = 0;       // +0x704: the whole code block, the bytes the hash covers
    uint32_t program_start = 0;   // +0x708: 0x30 graphics (SPH there), 0x100 compute
    uint32_t local_size[3] = {};  // +0x748: compute work-group size
    uint32_t shared_size = 0;     // +0x754: compute shared memory bytes (0 = none)
};

ShaderControl read_control(const uint8_t* control);

// ---- SPIR-V reflection ------------------------------------------------------------------------------------------

enum class ShaderStage : uint8_t { Unknown, Vertex, Fragment, Compute };

// Values are VkDescriptorType's (checked against the Vulkan headers in the runtime build).
enum class DescriptorType : uint32_t {
    Sampler = 0,
    CombinedImageSampler = 1,
    SampledImage = 2,
    StorageImage = 3,
    UniformTexelBuffer = 4,
    StorageTexelBuffer = 5,
    UniformBuffer = 6,
    StorageBuffer = 7,
};

enum class ScalarType : uint8_t { Other, Float, Sint, Uint, Bool };

// OpTypeImage of a texture or image binding. `dim` is SPIR-V's Dim: 0 1D, 1 2D, 2 3D, 3 Cube, 5 Buffer.
struct ImageShape {
    uint8_t dim = 0;
    bool depth = false, arrayed = false, ms = false;
    uint8_t sampled = 0;   // 1 sampled, 2 storage
    uint32_t format = 0;   // SPIR-V ImageFormat (0 Unknown)
    ScalarType component = ScalarType::Other;
};

// What a binding is in NVN terms. The translator's scheme (TRANSLATOR.md, "Where NVN puts resources"):
//   c[0] driver constants, c[1] the block's constant data, c[2] texture/image handles, c[3+n] uniform block n.
enum class NvnSource : uint8_t {
    Unknown,
    SupportBuffer,    // set 0 binding 0: the translator's own buffer (struct SupportBuffer below), filled by us
    DriverConstants,  // c[0]: NVN driver constants (compute reads its buffer descriptor table there)
    ConstantData,     // c[1]: this block's constant data (ShaderInfo::constants / control const_offset)
    UniformBlock,     // c[3+n]: what the game bound with nvnCommandBufferBindUniformBuffer(stage, n)
    StorageBuffer,    // the address in c[0] descriptor `c0_offset`: nvnCommandBufferBindStorageBuffer(stage, n)
    UniformAsStorage, // compute: the c[0] descriptor of uniform block n, read through a storage buffer
    SamplerBinding,   // texture whose handle is c[2] byte 0x20 + 8n: nvnCommandBufferBindTexture(stage, n)
    ImageBinding,     // image whose handle is c[2] byte 0x120 + 8n: nvnCommandBufferBindImage(stage, n) (inferred)
    HandleInUniform,  // texture whose handle the game wrote into uniform block `index` (c[bank]), at byte word * 4
};

struct NvnMeaning {
    NvnSource source = NvnSource::Unknown;
    int index = -1;      // the NVN binding n (uniform block, storage buffer, sampler, image)
    int bank = -1;       // constant bank c[bank]: the buffer itself (uniforms) or the bank holding the handle
    int word = -1;       // textures/images: 32-bit word of c[bank] holding the handle
    int sb_slot = -1;    // storage buffers: the translator's slot (order found), from the name "<stage>_s<slot>"
    int c0_offset = -1;  // storage buffers: byte offset of the descriptor in c[0]; -1 = not known (see apply_harness)
    bool default_bank = false;  // texture handle bank given by the translator as "the texture bank" (-1), i.e. c[2]
};

struct DescriptorBinding {
    uint32_t set = 0, binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    uint32_t count = 1;  // array length; 0 = runtime array (none in Wonder)
    std::string name;    // OpName, e.g. "fp_c3", "fp_t_tcb_8", "fp_t_cb7_20", "cp_s0", "cp_i_cb2_4A_r32ui"
    ImageShape image;    // texture and image bindings
    NvnMeaning nvn;
};

struct ShaderIo {
    uint32_t location = 0, component = 0;
    ScalarType type = ScalarType::Other;
    uint32_t components = 0;  // 1..4
    uint32_t array = 1;       // array length (1 = not an array)
};

struct ShaderReflection {
    ShaderStage stage = ShaderStage::Unknown;
    std::string entry;                             // entry point name ("main")
    uint32_t local_size[3] = {0, 0, 0};            // OpExecutionMode LocalSize (compute)
    std::vector<DescriptorBinding> bindings;       // sorted by (set, binding)
    std::vector<ShaderIo> inputs, outputs;         // Location-decorated Input/Output variables (not built-ins)
    std::vector<uint32_t> builtins_in, builtins_out;  // SPIR-V BuiltIn values (on variables or block members)
    bool uses_support_buffer = false;

    const DescriptorBinding* find(uint32_t set, uint32_t binding) const;
    // Vertex: the attribute locations the shader reads. Fragment: the render targets it writes.
    const std::vector<ShaderIo>& vertex_inputs() const { return inputs; }
    const std::vector<ShaderIo>& fragment_outputs() const { return outputs; }
};

// Parses a SPIR-V module (`count` 32-bit words). False with a reason in `error` if the module is malformed.
bool reflect_spirv(const uint32_t* words, size_t count, ShaderReflection& out, std::string* error = nullptr);

// ---- the harness's own record (private/wonder/spirv/results.tsv, column "resources") --------------------------------
// The SPIR-V carries everything except where a storage buffer's address came from: the translator names storage
// buffers by slot ("vp_s0"), not by their c[0] offset. The harness printed that offset in results.tsv, so the
// runtime reads it from there. Format (tools/maxwell/spirv_harness/Program.cs, DescribeResources):
//   cb<slot>=s<set>b<binding>                      constant buffer c[slot]
//   sb[c<slot>+0x<off>]=s<set>b<binding>           storage buffer, address at c[slot] 32-bit word <off>
//   tex<type>[c<slot>:<word>(x<n>)]=s<set>b<binding>  texture, handle at word <word> of c[slot] (-1 = c[2])
//   img<type>[c<slot>:<word>(x<n>)]=s<set>b<binding>  image, likewise

struct HarnessResource {
    enum Kind : uint8_t { Uniform, Storage, Texture, Image } kind = Uniform;
    int set = -1, binding = -1;
    int bank = -1;           // c[bank] as printed (-1 = the default texture bank)
    int word = -1;           // storage: descriptor word in c[bank]; texture/image: handle word
    int array_length = 1;
    std::string type;        // Ryujinx SamplerType text, e.g. "Texture2D|Array|Shadow"
};

bool parse_harness_resources(std::string_view text, std::vector<HarnessResource>& out);
// results.tsv text -> hash -> resources column.
size_t parse_results_tsv(std::string_view text, std::unordered_map<std::string, std::string>& out);
// Fills the c[0] offset of each storage buffer (and anything the names did not give). False if a harness entry has
// no matching SPIR-V binding.
bool apply_harness(ShaderReflection& refl, const std::vector<HarnessResource>& resources);

// The "resources" text the harness would print for this reflection (after apply_harness); the test compares it.
std::string describe_like_harness(const ShaderReflection& refl);
std::string sampler_type_text(const ImageShape& s);  // Ryujinx SamplerType text of an image shape

// ---- Ryujinx's support buffer ------------------------------------------------------------------------------------
// Set 0, binding 0 of every stage (std140), declared by every translated shader. Layout copied from Ryujinx
// (Ryujinx.Graphics.Shader/SupportBuffer.cs; filling rules from Ryujinx.Graphics.Gpu/Memory/SupportBufferUpdater.cs),
// MIT License, Copyright (c) Ryujinx Team and Contributors. Every field is a 16-byte vec4 slot.
//
// With the answers the harness baked (TRANSLATOR.md, "What only the runtime knows") Wonder's SPIR-V reads only
// render_scale (texture size queries, texel fetches, gl_FragCoord.xy; 3,390 shaders) and frag_scale_count (183 vertex
// shaders index their scales past the fragment ones): with every scale 1 those reads are identities.
// viewport_* are read only by variants with viewport transform disabled or Y negate, is_bgra only when the host has
// no BGRA formats (Vulkan has), alpha_test never (the alpha test is baked into the shader from the graphics state).
struct SupportBuffer {
    int32_t alpha_test[4];        // 0x000 FragmentAlphaTest (declared, not read)
    int32_t is_bgra[8][4];        // 0x010 FragmentIsBgra: x = 1 if render target i is BGRA (R/B swapped on output)
    float viewport_inverse[4];    // 0x090 2*scale/width, 2*scale/height, 1, 1 if viewport transform disabled else 0
    float viewport_size[4];       // 0x0a0 width, |height|, 1, 0 (gl_FragCoord.y flip under Y negate)
    int32_t frag_scale_count[4];  // 0x0b0 x = number of fragment texture scales (vertex scales follow them)
    float render_scale[73][4];    // 0x0c0 x only: [0] render target, [1..64] textures, [65..72] images
    int32_t tfe_offset[4];        // 0x550 transform feedback emulation (unused: no transform feedback)
    int32_t tfe_vertex_count[4];  // 0x560
};
static_assert(sizeof(SupportBuffer) == 0x570, "Ryujinx SupportBuffer is 1392 bytes");
static_assert(offsetof(SupportBuffer, is_bgra) == 0x10 && offsetof(SupportBuffer, viewport_inverse) == 0x90 &&
                  offsetof(SupportBuffer, viewport_size) == 0xa0 && offsetof(SupportBuffer, frag_scale_count) == 0xb0 &&
                  offsetof(SupportBuffer, render_scale) == 0xc0 && offsetof(SupportBuffer, tfe_offset) == 0x550 &&
                  offsetof(SupportBuffer, tfe_vertex_count) == 0x560,
              "offsets as in the SPIR-V's OpMemberDecorate");

SupportBuffer support_buffer_defaults();  // render scale 1 everywhere, no BGRA, no alpha test, zero viewport
// What Ryujinx writes for viewport 0 (SetViewportTransformDisable / SetViewportSize), with render scale 1.
void support_buffer_set_viewport(SupportBuffer& sb, float width, float height, bool transform_disabled = false,
                                 float scale = 1.0f);

// ---- one stage ---------------------------------------------------------------------------------------------------

struct ShaderInfo {
    std::string hash;
    ShaderStage stage = ShaderStage::Unknown;  // from the SPIR-V execution model
    std::vector<uint32_t> spirv;
    ShaderReflection refl;
    ShaderControl control;
    // c[1]: the constant data at code block + control.const_offset. Copied because the game may reuse the pool; it
    // can also be bound in place (code GPU address + const_offset, 0x100-aligned). The SPIR-V declares every c[k] as
    // vec4[4096]; bind at least const_size rounded up to 16.
    std::vector<uint8_t> constants;
    uint32_t local_size[3] = {1, 1, 1};  // compute: from control +0x748 (equals the SPIR-V's LocalSize)
    // dynamic_uniform_bindings and written_storage_bindings (nvn_spirv_patch.cpp), computed when the runtime cache
    // loads the shader so the replay has no lookup per draw (docs/nvn/android-perf/PLAN.md R4). Unknown: computed
    // on use, as before.
    bool masks_known = false;
    uint64_t dynamic_uniforms = 0, written_storage = 0;
};

// Builds a ShaderInfo from a code block, its control section and the SPIR-V words (no I/O). `resources` = the
// block's results.tsv resources column, or null (storage buffer c[0] offsets then stay -1).
bool make_shader_info(const uint8_t* code, const uint8_t* control, std::vector<uint32_t> spirv,
                      const std::string* resources, ShaderInfo& out, std::string* error = nullptr);

const char* stage_name(ShaderStage s);
const char* source_name(NvnSource s);

// ---- the runtime cache -------------------------------------------------------------------------------------------
#ifndef NX_SHADERS_STANDALONE
// The stage whose code block is at `code_host` (guest = host memory) with control section `control`. Loads
// <data root>/spirv/<hash>.spv once; the pointer stays valid until exit. Null if there is no such file (logged once
// per hash). Hashing costs a pass over the block: call it when the program is set, not per draw.
const ShaderInfo* shader_for(uint64_t code_host, uint64_t control);
const ShaderInfo* shader_by_hash(const std::string& hash);  // one already loaded, or null
#endif

}  // namespace nx::nvn
