// NVN copies between textures (docs/NVN.md, N6). nvnCommandBufferCopyTextureToTexture(cmd, const NVNtexture* src,
// const NVNtextureView* srcView, const NVNcopyRegion* srcRegion, const NVNtexture* dst, const NVNtextureView* dstView,
// const NVNcopyRegion* dstRegion, int copyFlags) (Wonder's call sites). NVNcopyRegion {int x, y, z, width, height,
// depth}; for array textures z/depth are layers. Flags: 1 linear filter, 4/8/16 mirror x/y/z (2 selects the 2D
// engine: no difference here). Replayed as a vkCmdCopyImage when formats and sizes match, else a vkCmdBlitImage.
// Wonder copies its frame into a texture for screen transitions (the opening's cloud wipe).
// Copies within one image (another level or layer, or another rectangle of the same one: blur chains, mips, atlases;
// textures over one storage share an image since #36) are replayed with the image in GENERAL, as Vulkan allows when
// the regions do not overlap; overlapping ones are dropped (G10). NX_COPY_NO_SAME_IMAGE=1: all dropped, the old way.
// P5R also fills textures from buffers and clears them outside passes (CopyBufferToTexture, ClearTexture, below).
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "nvn_draw_state.h"
#include "nvn_pipeline.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace vk;

namespace {

struct CopyInfo {
    ViewInfo src_view, dst_view;
    int32_t src[6] = {}, dst[6] = {};
    uint32_t flags = 0;
};

void CopyTextureToTexture(CPU& c) {
    CopyInfo info;
    info.src_view = read_view(c, c.x[2]);
    info.dst_view = read_view(c, c.x[5]);
    for (int k = 0; k < 6; k++) {
        info.src[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
        info.dst[k] = c.x[6] ? ld<int32_t>(c.x[6] + 4ull * k) : 0;
    }
    info.flags = uint32_t(c.x[7]);
    Cmd cmd{Cmd::CopyTextureToTexture};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[4];
    record(c.x[0], cmd, &info, sizeof info);
}

// nvnCommandBufferCopyBufferToTexture(cmd, NVNbufferAddress src, const NVNtexture* dst, const NVNtextureView* dstView,
// const NVNcopyRegion* dstRegion, int flags): how P5R loads every texture (a staging buffer it reuses after waiting
// for the copy, on a command buffer of its own). The source is tightly packed (P5R never sets a copy stride). Its
// bytes are read when replayed, as the GPU would, and go to the image like nvnTextureWriteTexels data: the texture's
// guest memory never gets them, so the image is not uploaded from there again.
struct BufferCopyInfo {
    ViewInfo dst_view;
    int32_t dst[6] = {};
    uint32_t flags = 0;
};

void CopyBufferToTexture(CPU& c) {
    BufferCopyInfo info;
    info.dst_view = read_view(c, c.x[3]);
    for (int k = 0; k < 6; k++) info.dst[k] = c.x[4] ? ld<int32_t>(c.x[4] + 4ull * k) : 0;
    info.flags = uint32_t(c.x[5]);
    Cmd cmd{Cmd::CopyBufferToTexture};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[2];
    record(c.x[0], cmd, &info, sizeof info);
}

// nvnCommandBufferClearTexture(cmd, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const float* color,
// int mask): a colour texture (or a rectangle of it) cleared outside any pass. Mask bits 0..3: r, g, b, a.
struct ClearTextureInfo {
    ViewInfo view;
    int32_t region[6] = {};
};

void ClearTexture(CPU& c) {
    ClearTextureInfo info;
    info.view = read_view(c, c.x[2]);
    for (int k = 0; k < 6; k++) info.region[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
    Cmd cmd{Cmd::ClearTexture};
    cmd.a[0] = c.x[1];
    for (int k = 0; k < 4; k++) cmd.f[k] = c.x[4] ? ld<float>(c.x[4] + 4ull * k) : 0.0f;
    cmd.mask = uint32_t(c.x[5]);
    record(c.x[0], cmd, &info, sizeof info);
}

// The image of a texture a copy or clear is about to fill. One never uploaded is cleared, not untiled from its guest
// memory first: the GPU writes such textures, so that memory holds nothing of them here.
GpuImage* fill_target(VkCommandBuffer cb, uint64_t texture) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    if (!img) return nullptr;
    if (!img->uploaded && !img->rendered && !t->dirty && !t->has_writes.load(std::memory_order_acquire))
        clear_image(cb, *img), img->uploaded = true;
    return ensure_contents(cb, texture);
}

void sampled_layout(VkCommandBuffer cb, GpuImage& img) {
    transition(cb, img, img.rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool compressed(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

// A copy within one image keeps it GENERAL, where transition() records nothing: the writes before it (a pass,
// another copy) and the reads after it need a barrier of their own.
void general_barrier(VkCommandBuffer cb, const GpuImage& img) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
}

// Half-open ranges [a0, a0 + an) and [b0, b0 + bn) intersect.
bool ranges_meet(int64_t a0, int64_t an, int64_t b0, int64_t bn) { return a0 < b0 + bn && b0 < a0 + an; }

uint64_t g_same_image = 0, g_same_overlap = 0;

}  // namespace

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    static const bool off = std::getenv("NX_NO_COPY") != nullptr;  // debug: skip texture copies
    if (off) return;
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(CopyInfo)) return;
    CopyInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    GpuImage* src = ensure_contents(cb, c.a[0]);
    GpuImage* dst = ensure_contents(cb, c.a[1]);
    if (!src || !dst) return;
    // NX_HUD_1080: a rectangle on a 1920x1080 image of a 1280x720 texture is taken 3/2 (the frame's copy into the
    // window texture: both scaled, a plain copy; one side only: a blit that scales).
    auto hud_rect = [](int32_t* r) {
        const int32_t x1 = hud_up_end(r[0] + r[3]), y1 = hud_up_end(r[1] + r[4]);
        r[0] = hud_up(r[0]), r[1] = hud_up(r[1]), r[3] = x1 - r[0], r[4] = y1 - r[1];
    };
    if (src->hud) hud_rect(info.src);
    if (dst->hud) hud_rect(info.dst);
    bool same = src == dst;
    static const bool no_same = std::getenv("NX_COPY_NO_SAME_IMAGE") != nullptr;  // bisecting: the old way
    if (same && no_same) return;
    uint32_t sl = std::min(info.src_view.base_level, src->levels - 1), dl = std::min(info.dst_view.base_level, dst->levels - 1);
    bool s3d = src->view_type == VK_IMAGE_VIEW_TYPE_3D, d3d = dst->view_type == VK_IMAGE_VIEW_TYPE_3D;
    auto sub = [](const GpuImage& img, uint32_t level, int32_t z, int32_t depth, uint32_t view_layer, bool is3d) {
        VkImageAspectFlags aspect = img.aspect;
        uint32_t layer = is3d ? 0 : std::min<uint32_t>(uint32_t(std::max(z, 0)) + view_layer, img.layers - 1);
        uint32_t count = is3d ? 1 : std::clamp<uint32_t>(uint32_t(std::max(depth, 1)), 1, img.layers - layer);
        return VkImageSubresourceLayers{aspect, level, layer, count};
    };
    VkImageSubresourceLayers ss = sub(*src, sl, info.src[2], info.src[5], info.src_view.min_layer, s3d);
    VkImageSubresourceLayers ds = sub(*dst, dl, info.dst[2], info.dst[5], info.dst_view.min_layer, d3d);
    int32_t sw = std::max(info.src[3], 1), sh = std::max(info.src[4], 1);
    int32_t dw = std::max(info.dst[3], 1), dh = std::max(info.dst[4], 1);
    if (same) {
        // One subresource and layer range, and the rectangles (and 3D slices) meet: Vulkan forbids it, dropped.
        bool overlap = ss.mipLevel == ds.mipLevel &&
                       ranges_meet(ss.baseArrayLayer, ss.layerCount, ds.baseArrayLayer, ds.layerCount) &&
                       ranges_meet(info.src[0], sw, info.dst[0], dw) && ranges_meet(info.src[1], sh, info.dst[1], dh) &&
                       (!s3d || ranges_meet(info.src[2], std::max(info.src[5], 1), info.dst[2],
                                            std::max(info.dst[5], 1)));
        g_same_image++, g_same_overlap += overlap;
        if (g_same_image == 1 || g_same_image % 1000 == 0 || (overlap && g_same_overlap <= 5))
            log("nvn: %llu copies within one image (%llu overlapping, dropped); this one %llx level %u -> %u, "
                "layer %u -> %u%s",
                (unsigned long long)g_same_image, (unsigned long long)g_same_overlap, (unsigned long long)c.a[0],
                ss.mipLevel, ds.mipLevel, ss.baseArrayLayer, ds.baseArrayLayer, overlap ? " (overlapping)" : "");
        if (overlap) return;
    }
    static const bool trace_cubes = std::getenv("NX_TRACE_CUBES") != nullptr;
    static int cube_logs = 0;
    if (trace_cubes && (dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE || dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
        cube_logs++ < 400)
        log("cubes: copy %llx -> %llx level %u layer %u+%u (z %d depth %d view layer %u) %dx%d", (unsigned long long)c.a[0],
            (unsigned long long)c.a[1], ds.mipLevel, ds.baseArrayLayer, ds.layerCount, info.dst[2], info.dst[5],
            info.dst_view.min_layer, info.dst[3], info.dst[4]);
    VkImageLayout sly = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageLayout dly = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transition(cb, *src, sly);
    transition(cb, *dst, dly);
    if (same) general_barrier(cb, *src);
    bool mirror = info.flags & (4 | 8 | 16);
    if (src->format == dst->format && sw == dw && sh == dh && !mirror && ss.layerCount == ds.layerCount) {
        VkImageCopy r{};
        r.srcSubresource = ss, r.dstSubresource = ds;
        r.srcOffset = {info.src[0], info.src[1], s3d ? info.src[2] : 0};
        r.dstOffset = {info.dst[0], info.dst[1], d3d ? info.dst[2] : 0};
        r.extent = {uint32_t(sw), uint32_t(sh), s3d ? uint32_t(std::max(info.src[5], 1)) : 1u};
        p_vkCmdCopyImage(cb, src->image, sly, dst->image, dly, 1, &r);
    } else if (!compressed(src->format) && !compressed(dst->format) && src->aspect == dst->aspect) {
        VkImageBlit b{};
        b.srcSubresource = ss, b.dstSubresource = ds;
        ds.layerCount = ss.layerCount = std::min(ss.layerCount, ds.layerCount);
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = ss.layerCount;
        b.srcOffsets[0] = {info.src[0], info.src[1], 0};
        b.srcOffsets[1] = {info.src[0] + sw, info.src[1] + sh, 1};
        b.dstOffsets[0] = {info.dst[0], info.dst[1], 0};
        b.dstOffsets[1] = {info.dst[0] + dw, info.dst[1] + dh, 1};
        if (info.flags & 4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);
        if (info.flags & 8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);
        bool linear = (info.flags & 1) && src->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        p_vkCmdBlitImage(cb, src->image, sly, dst->image, dly, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        static int logged = 0;
        if (logged++ < 5)
            log("nvn: copy %llx -> %llx not replayed (formats %d -> %d, %dx%d -> %dx%d)", (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], int(src->format), int(dst->format), sw, sh, dw, dh);
        return;
    }
    if (same) general_barrier(cb, *dst);
    dst->rendered = true;  // GPU-written from now on
}

void replay_buffer_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(BufferCopyInfo)) return;
    BufferCopyInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    static uint64_t copies = 0, failed = 0;
    Texture* t = get<Texture>(c.a[1]);
    const uint8_t* src = replay::guest_bytes(c.a[0]);
    GpuImage* img = t && src ? fill_target(cb, c.a[1]) : nullptr;
    if (img && img->hud) img = nullptr;  // NX_HUD_1080: texels at 1280x720 for a 1920x1080 image (never seen)
    if (!img || !upload_region(cb, *t, *img, info.dst_view.base_level, info.dst_view.min_layer, info.dst, src)) {
        if (++failed <= 10)
            log("nvn: buffer-to-texture copy %llx -> %llx not replayed (%s)", (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], !t ? "unknown texture" : !src ? "unknown buffer address" : "no image or format");
        return;
    }
    img->texels = true;
    sampled_layout(cb, *img);
    if (++copies == 1 || copies % 1000 == 0)
        log("nvn: %llu buffer-to-texture copies (%llu not replayed); this one %ux%u f%#x level %u, %dx%dx%d at %d,%d,%d",
            (unsigned long long)copies, (unsigned long long)failed, t->width, t->height, t->format,
            info.dst_view.base_level, info.dst[3], info.dst[4], info.dst[5], info.dst[0], info.dst[1], info.dst[2]);
}

void replay_clear_texture(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(ClearTextureInfo)) return;
    ClearTextureInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    Texture* t = get<Texture>(c.a[0]);
    GpuImage* img = t ? fill_target(cb, c.a[0]) : nullptr;
    if (!img) return;
    uint32_t level = std::min(info.view.base_level, img->levels - 1);
    uint32_t lw = std::max(1u, img->width >> level), lh = std::max(1u, img->height >> level);
    if (img->hud) {  // NX_HUD_1080: the guest rectangle, 3/2
        const int32_t x1 = hud_up_end(info.region[0] + info.region[3]), y1 = hud_up_end(info.region[1] + info.region[4]);
        info.region[0] = hud_up(info.region[0]), info.region[1] = hud_up(info.region[1]);
        info.region[3] = x1 - info.region[0], info.region[4] = y1 - info.region[1];
    }
    const int32_t* r = info.region;
    bool whole = r[0] <= 0 && r[1] <= 0 && r[0] + r[3] >= int32_t(lw) && r[1] + r[4] >= int32_t(lh);
    static int logged = 0;
    if (img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || compressed(img->format) || !whole || (c.mask & 15) != 15) {
        if (logged++ < 10)
            log("nvn: texture clear not replayed: %llx %ux%u f%#x level %u, rect %d,%d %dx%d, mask %x",
                (unsigned long long)c.a[0], img->width, img->height, t->format, level, r[0], r[1], r[3], r[4], c.mask);
        return;
    }
    bool is3d = img->view_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t layer = is3d ? 0 : std::min(uint32_t(std::max(r[2], 0)) + info.view.min_layer, img->layers - 1);
    VkImageSubresourceRange range{img->aspect, level, 1, layer, is3d ? 1u : std::clamp<uint32_t>(uint32_t(r[5]), 1, img->layers - layer)};
    VkClearColorValue v{{c.f[0], c.f[1], c.f[2], c.f[3]}};
    transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    img->texels = true;
    sampled_layout(cb, *img);
}

NX_HLE("nvnCommandBufferCopyTextureToTexture", CopyTextureToTexture);
NX_HLE("nvnCommandBufferCopyBufferToTexture", CopyBufferToTexture);
NX_HLE("nvnCommandBufferClearTexture", ClearTexture);

}  // namespace nx::nvn
