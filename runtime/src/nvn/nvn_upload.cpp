// Texture uploads (nvn_pipeline.h): guest pool memory into the host image, untiled, widened where the host stores a
// format wider than NVN does, and checked against what the copies read (docs/nvn/DEBUG.md #50).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "nvn_astc.h"
#include "nvn_pipeline.h"
#include "nvn_tiling.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// R11G11B10F (NVN 0x3f) is stored as RGBA16F (nvn_format.cpp). Its channels are unsigned floats with half's 5-bit
// exponent and bias 15, and 6 (R, G) or 5 (B) mantissa bits: each is a half with the low mantissa bits zero and no
// sign, so shifting it into place is exact (Inf and NaN included). Alpha is 1. Copying the 4-byte texels as they are
// read twice the data: rows side by side, bits reinterpreted, the bottom half from other uploads (the garbage band).
void widen_r11g11b10(const uint8_t* src, uint8_t* dst, uint64_t texels) {
    for (uint64_t k = 0; k < texels; k++) {
        uint32_t v;
        std::memcpy(&v, src + k * 4, 4);
        uint16_t h[4] = {uint16_t((v & 0x7FF) << 4), uint16_t(((v >> 11) & 0x7FF) << 4),
                         uint16_t(((v >> 22) & 0x3FF) << 5), 0x3C00};
        std::memcpy(dst + k * 8, h, 8);
    }
}

// NX_FULL_UPLOAD=1: a pitch-linear texture the CPU wrote part of is uploaded whole again (bisecting).
bool full_upload() {
    static const bool on = std::getenv("NX_FULL_UPLOAD") != nullptr;
    return on;
}

// Bytes from `base` to the end of what vkCmdCopyBufferToImage reads for `r`, at `bytes` per bw x bh block.
uint64_t copy_end(const VkBufferImageCopy& r, uint64_t base, uint32_t bw, uint32_t bh, uint32_t bytes) {
    uint64_t row_blocks = ((r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width) + bw - 1) / bw;
    uint64_t image_rows = ((r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height) + bh - 1) / bh;
    uint64_t w = (r.imageExtent.width + bw - 1) / bw, h = (r.imageExtent.height + bh - 1) / bh;
    uint64_t slices = uint64_t(r.imageExtent.depth) * r.imageSubresource.layerCount;
    if (!w || !h || !slices) return r.bufferOffset - base;
    return r.bufferOffset - base + ((slices - 1) * image_rows + (h - 1)) * row_blocks * bytes + w * bytes;
}

// The copies read at most `staged` bytes from `base`, counted at the host format's size. A copy past it reads other
// uploads of the submit (or past the buffer): garbage, so the image is cleared instead. NX_NO_UPLOAD_GUARD=1: the old
// way (bisecting).
bool copies_fit(const Texture& t, const VkBufferImageCopy* r, uint32_t n, uint64_t base, uint64_t staged, uint32_t bw,
                uint32_t bh, uint32_t bytes) {
    static const bool off = std::getenv("NX_NO_UPLOAD_GUARD") != nullptr;
    uint64_t need = 0;
    for (uint32_t k = 0; k < n; k++) need = std::max(need, copy_end(r[k], base, bw, bh, bytes));
    if (off || need <= staged) return true;
    static std::set<uint32_t> logged;  // once per format
    if (logged.insert(t.format).second)
        log("nvn/vk: upload short: texture %ux%u format %#x reads %llu bytes, %llu staged: cleared instead", t.width,
            t.height, t.format, (unsigned long long)need, (unsigned long long)staged);
    return false;
}

// ASTC on a GPU without it (nvn_format.cpp): the untiled blocks of every level become RGBA8 texels, and the levels
// are re-laid out for them (each level's layers back to back, 16-byte aligned). Logged every 200 textures: count,
// bytes and CPU time, the cost of the decoder at load.
void decode_astc(uint32_t format, std::vector<uint8_t>& linear, std::vector<LinearLevel>& levels, uint32_t layers,
                 uint32_t bw, uint32_t bh) {
    static uint64_t textures = 0, bytes = 0, ns = 0, invalid = 0;
    const uint64_t t0 = plat::monotonic_ns();
    const bool srgb = format >= 0x87;
    std::vector<LinearLevel> out = levels;
    uint64_t at = 0;
    for (LinearLevel& l : out) {
        l.offset = at;
        l.layer_size = uint64_t(l.width) * l.height * 4;
        l.width_blocks = l.width, l.height_blocks = l.height;
        at += (l.layer_size * layers + 15) & ~15ull;
    }
    std::vector<uint8_t> rgba(at);
    for (size_t k = 0; k < levels.size(); k++)
        if (levels[k].offset + levels[k].layer_size * layers <= linear.size())
            invalid += astc_decode_image(linear.data() + levels[k].offset, levels[k].width, levels[k].height, layers,
                                         bw, bh, srgb, rgba.data() + out[k].offset);
    linear.swap(rgba);
    levels.swap(out);
    const uint64_t spent = plat::monotonic_ns() - t0;
    note_astc_decoded(spent);  // per timing window (nvn_pipeline_cache.cpp)
    bytes += at, ns += spent;
    if (++textures % 200 == 1)
        log("nvn/vk: ASTC decoded on the CPU: %llu textures, %.1f MB, %.0f ms, %llu invalid blocks",
            (unsigned long long)textures, double(bytes) / 1e6, double(ns) / 1e6, (unsigned long long)invalid);
}

// Whether a 1 MB+ upload gets a staging buffer of its own (PHASE4.md U1). In steady state (a course frame stages ~30
// such textures, ~40 MB per second) the ring takes them: a fresh buffer each was an allocation (a kgsl ioctl that
// zero-fills), a page fault per 4 KB of the copy and a free at retire, on the replay thread. A scene load, which
// stages hundreds of MB in one submit, still gets its own buffers: past a quarter of the ring in this submit, or one
// upload over an eighth of it. NX_STAGING_MIN=0: a staging buffer for every 1 MB+ upload, as before.
bool staging_wanted(uint64_t size) {
    static const bool always = [] {
        const char* e = std::getenv("NX_STAGING_MIN");
        return e && e[0] == '0';
    }();
    uint64_t ring = frame_ring_size();
    return always || !ring || frame_upload_used() + size > ring / 4 || size > ring / 8;
}

// Which uploads may get a staging buffer at all: 1 MB and up, and, once this submit has put a quarter of the ring
// in the ring already (a scene load), 64 KB and up. NX_STAGING_LOAD=0: the 1 MB floor only, as before.
bool staging_candidate(uint64_t size) {
    static const bool load = [] {
        const char* e = std::getenv("NX_STAGING_LOAD");
        return !(e && e[0] == '0');
    }();
    if (size >= (1u << 20)) return true;
    uint64_t ring = frame_ring_size();
    return load && ring && size >= (64u << 10) && frame_upload_used() > ring / 4;
}

// Room for one texture's upload: a staging buffer of its own when it is big (or the submit already is), else the ring.
// NX_STAGING_RING=1: always the ring, as before (bisecting).
Upload texture_stage(uint64_t size) {
    static const bool ring_only = std::getenv("NX_STAGING_RING") != nullptr;
    if (!ring_only && staging_candidate(size) && staging_wanted(size)) return staging_alloc(size);
    return upload_alloc(size, 16);
}

}  // namespace

// NX_1D_IMAGES=1: NVN 1D textures as Vulkan 1D images again (they are 2D of height 1: Ryujinx samples 1D as 2D, G8).
bool vk_1d_images() {
    static const bool on = std::getenv("NX_1D_IMAGES") != nullptr;
    return on;
}

void clear_image(VkCommandBuffer cb, GpuImage& img, bool magenta) {
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{img.aspect, 0, img.levels, 0, img.layers};
    // A block-compressed image (BC, ETC2, ASTC) cannot be cleared (VUID-vkCmdClearColorImage-image-00007): NVIDIA
    // let it pass, the Adreno 610 driver faulted the GPU at the first submit. It is left as it is, ready for copies.
    if (img.format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && img.format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return;
    if (img.aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v{};
        if (magenta) v = {{1.0f, 0.0f, 1.0f, 1.0f}};
        p_vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    } else {
        VkClearDepthStencilValue v{1.0f, 0};  // far: the course passes test LESS
        p_vkCmdClearDepthStencilImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
}

// Uploads of large pitch-linear textures: the CPU-written pictures of a film (nvn_queue.cpp counts them per present).
std::atomic<uint64_t> g_linear_uploads{0};

void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img, uint64_t lo, uint64_t hi) {
    if (t.stride && uint64_t(t.width) * t.height >= 640 * 360) {
        g_linear_uploads++;
        static long trace = std::getenv("NX_TRACE_VIDEO") ? 3 * std::atol(std::getenv("NX_TRACE_VIDEO")) : 0;
        if (trace > 0 && trace--)  // the film's cadence (hle_movie.cpp, nvn_queue.cpp)
            log("movie trace: upload %llx %ux%u rows %llu..%llu at %.1f ms", (unsigned long long)t.memory, t.width,
                t.height, (unsigned long long)(hi == UINT64_MAX ? 0 : lo / std::max(1u, t.stride)),
                (unsigned long long)(hi == UINT64_MAX ? t.height : (hi + t.stride - 1) / std::max(1u, t.stride)),
                double(plat::monotonic_ns() % 1000000000000ull) / 1e6);
    }
    const bool was_uploaded = img.uploaded;
    img.uploaded = true;
    if (gpu_profile_on()) gpu_profile_mark(cb, "texture uploads");
    if (!t.memory || !t.storage_size) return;
    frame_guest_read(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size);  // deferred write-backs first
    if (std::getenv("NX_TRACE_UPLOADS") && t.storage_size >= (1u << 20)) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(t.memory);
        size_t nz = 0;
        for (uint64_t k = 0; k < t.storage_size; k += 64) nz += m[k] != 0;
        log("nvn: upload %ux%u fmt %#x stride %u: %zu of %llu sampled bytes non-zero", t.width, t.height, t.format,
            t.stride, nz, (unsigned long long)(t.storage_size / 64));
    }
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    // What the copies read per block: the host format's size (RGBA16F for R11G11B10F), else the NVN one.
    uint32_t host_bytes = vk_block_bytes(img.format) ? vk_block_bytes(img.format) : bytes;
    // NX_R11G11B10_NARROW=1: R11G11B10F copied at 4 bytes per texel, as before (the guard then clears it unless
    // NX_NO_UPLOAD_GUARD=1 too; bisecting).
    static const bool narrow = std::getenv("NX_R11G11B10_NARROW") != nullptr;
    bool widen = !narrow && t.format == 0x3f && img.format == VK_FORMAT_R16G16B16A16_SFLOAT && !img.alpha_from_r;
    std::vector<uint8_t> linear;
    std::vector<VkBufferImageCopy> copies;
    uint32_t layers = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t.depth) : img.layers;
    if (img.alpha_from_r && t.stride) {  // R8 pitch-linear -> RGBA8 (255, 255, 255, R), level 0
        const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
        uint64_t rows = uint64_t(img.height) * layers;
        if (uint64_t(t.stride) * rows > t.storage_size) rows = t.storage_size / t.stride;
        // Only the rows the CPU wrote (a single layer), when the texture is already there (NX_FULL_UPLOAD=1: all).
        uint64_t y0 = 0, y1 = rows;
        if (was_uploaded && layers == 1 && !full_upload() && hi != UINT64_MAX)
            y0 = std::min(rows, lo / t.stride), y1 = std::min(rows, (hi + t.stride - 1) / t.stride);
        if (y1 <= y0) return;
        Upload u = texture_stage((y1 - y0) * img.width * 4);  // the 2048x8192 glyph atlas: 64 MB at its first upload
        for (uint64_t y = y0; y < y1; y++) {
            const uint8_t* s = src + y * t.stride;
            uint32_t* d = reinterpret_cast<uint32_t*>(u.ptr + (y - y0) * img.width * 4);
            for (uint32_t x = 0; x < img.width; x++) d[x] = 0x00FFFFFFu | (uint32_t(s[x]) << 24);
        }
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageOffset = {0, int32_t(y0), 0};
        r.imageExtent = {img.width, uint32_t(std::min<uint64_t>(y1, img.height) - y0), 1};
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    bool decode = bc_host_decode(t.format);  // nvn_bc.cpp: no BC on this GPU
    if (decode && t.stride) {
        static bool logged = false;
        if (!logged) log("nvn/vk: pitch-linear BC texture (format %#x): not decoded, cleared", t.format), logged = true;
        return clear_image(cb, img);
    }
    if (t.stride && astc_decoded(t.format)) {  // never seen: pitch-linear ASTC would need the decoder too
        log("nvn/vk: pitch-linear ASTC texture %ux%u format %#x: cleared", img.width, img.height, t.format);
        return clear_image(cb, img);
    }
    if (t.stride) {  // pitch-linear: rows as they are
        uint64_t rows = (img.height + bh - 1) / bh;
        VkBufferImageCopy r{};
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, img.height, 1};
        Upload u;
        uint64_t staged = 0;
        if (widen) {  // tightly packed RGBA16F rows; rows past the storage stay zero
            const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
            uint64_t n = rows * layers, have = std::min<uint64_t>(n, t.storage_size / t.stride);
            uint64_t texels = std::min<uint64_t>(img.width, t.stride / 4);
            staged = n * img.width * 8;
            u = texture_stage(staged);
            std::memset(u.ptr, 0, staged);
            for (uint64_t y = 0; y < have; y++) widen_r11g11b10(src + y * t.stride, u.ptr + y * img.width * 8, texels);
        } else {
            uint64_t y0 = 0, y1 = rows;  // only the rows the CPU wrote, for a texture already there
            if (was_uploaded && bw == 1 && bh == 1 && layers == 1 && !full_upload() && hi != UINT64_MAX) {
                y0 = std::min(rows, lo / t.stride), y1 = std::min(rows, (hi + t.stride - 1) / t.stride);
                if (y1 <= y0) return;
                r.imageOffset = {0, int32_t(y0), 0};
                r.imageExtent = {img.width, uint32_t(y1 - y0), 1};
            }
            staged = uint64_t(t.stride) * (y1 - y0) * layers;
            u = texture_stage(staged);
            std::memcpy(u.ptr, reinterpret_cast<const uint8_t*>(t.memory) + y0 * t.stride, staged);
            r.bufferRowLength = t.stride / bytes * bw;
        }
        r.bufferOffset = u.offset;
        if (!copies_fit(t, &r, 1, u.offset, staged, bw, bh, host_bytes)) return clear_image(cb, img);
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    auto levels = deswizzle_texture(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size, img.width,
                                    img.height, layers, img.levels, bw, bh, bytes, linear, t.block_height_gobs);
    if (levels.empty()) {
        log("nvn/vk: texture %ux%u format %#x: could not untile (%llu bytes)", img.width, img.height, t.format,
            (unsigned long long)t.storage_size);
        return;
    }
    if (decode) {  // every level's blocks -> texels, tightly packed, then copied as an uncompressed image
        uint32_t tb = bc_decoded_bytes(t.format), slices = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : img.layers;
        uint64_t total = 0;
        for (const auto& l : levels) total += uint64_t(l.width) * l.height * slices * tb;
        Upload u = upload_alloc(total, 16);
        uint64_t at = 0;
        for (uint32_t k = 0; k < levels.size(); k++) {
            bc_decode(t.format, linear.data() + levels[k].offset, levels[k].width, levels[k].height, slices, u.ptr + at);
            VkBufferImageCopy r{};
            r.bufferOffset = u.offset + at;
            r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
            r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
            copies.push_back(r);
            at += uint64_t(levels[k].width) * levels[k].height * slices * tb;
        }
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                                 copies.data());
        return;
    }
    if (astc_decoded(t.format)) {  // RGBA8 texels from here on
        decode_astc(t.format, linear, levels, layers, bw, bh);
        bw = bh = 1, bytes = host_bytes = 4;
    }
    // Widened, every offset in the linear buffer doubles (level offsets are multiples of the 4-byte texel).
    uint64_t scale = widen ? 2 : 1;
    Upload u = texture_stage(linear.size() * scale);
    if (widen)
        widen_r11g11b10(linear.data(), u.ptr, linear.size() / 4);
    else
        std::memcpy(u.ptr, linear.data(), linear.size());
    for (uint32_t k = 0; k < levels.size(); k++) {
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset + levels[k].offset * scale;
        r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
        r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
        copies.push_back(r);
    }
    if (!copies_fit(t, copies.data(), uint32_t(copies.size()), u.offset, linear.size() * scale, bw, bh, host_bytes))
        return clear_image(cb, img);
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                             copies.data());
}

bool upload_region(VkCommandBuffer cb, const Texture& t, GpuImage& img, uint32_t level, uint32_t layer0,
                   const int32_t region[6], const uint8_t* src) {
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    const bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    level = std::min(level, img.levels - 1);
    const uint32_t lw = std::max(1u, img.width >> level), lh = std::max(1u, img.height >> level);
    const uint32_t x = uint32_t(std::max(region[0], 0)), y = uint32_t(std::max(region[1], 0));
    const uint32_t z = uint32_t(std::max(region[2], 0));
    // The source is laid out for the region as given; what lies past the level's edge is not copied.
    const uint32_t w = uint32_t(std::max(region[3], 1)), h = uint32_t(std::max(region[4], 1));
    const uint32_t d = uint32_t(std::max(region[5], 1));
    if (x >= lw || y >= lh) return true;
    const uint64_t blocks = uint64_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * d;
    const uint32_t host_bytes = vk_block_bytes(img.format) ? vk_block_bytes(img.format) : bytes;
    const bool widen = t.format == 0x3f && img.format == VK_FORMAT_R16G16B16A16_SFLOAT && !img.alpha_from_r;
    Upload u;
    uint32_t texel_w = bw, texel_h = bh;  // the staged data's block, in texels
    if (bc_host_decode(t.format)) {
        uint32_t tb = bc_decoded_bytes(t.format);
        u = texture_stage(uint64_t(w) * h * d * tb);
        bc_decode(t.format, src, w, h, d, u.ptr);
        texel_w = texel_h = 1;
    } else if (astc_decoded(t.format)) {
        u = texture_stage(uint64_t(w) * h * d * 4);
        astc_decode_image(src, w, h, d, bw, bh, t.format >= 0x87, u.ptr);
        texel_w = texel_h = 1;
    } else if (widen) {
        u = texture_stage(blocks * 8);
        widen_r11g11b10(src, u.ptr, blocks);
    } else if (img.alpha_from_r) {  // R8 -> RGBA8 (255, 255, 255, R)
        u = texture_stage(blocks * 4);
        uint32_t* out = reinterpret_cast<uint32_t*>(u.ptr);
        for (uint64_t k = 0; k < blocks; k++) out[k] = 0x00FFFFFFu | (uint32_t(src[k]) << 24);
    } else if (host_bytes == bytes) {
        u = texture_stage(blocks * bytes);
        std::memcpy(u.ptr, src, blocks * bytes);
    } else {
        static std::set<uint32_t> logged;
        if (logged.insert(t.format).second)
            log("nvn/vk: buffer-to-texture copy into format %#x (%u bytes per block, host %u): not replayed", t.format,
                bytes, host_bytes);
        return false;
    }
    VkBufferImageCopy r{};
    r.bufferOffset = u.offset;
    r.bufferRowLength = (w + texel_w - 1) / texel_w * texel_w;
    r.bufferImageHeight = (h + texel_h - 1) / texel_h * texel_h;
    VkImageAspectFlags aspect = img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                    ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                    : img.aspect;
    uint32_t layer = is3d ? 0 : std::min(z + layer0, img.layers - 1);
    r.imageSubresource = {aspect, level, layer, is3d ? 1u : std::min(d, img.layers - layer)};
    r.imageOffset = {int32_t(x), int32_t(y), is3d ? int32_t(z) : 0};
    r.imageExtent = {std::min(w, lw - x), std::min(h, lh - y), is3d ? d : 1u};
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    return true;
}

}  // namespace nx::nvn
