// Clears (split from nvn_draw.cpp). A clear of targets other than the open pass's used to end it and begin a pass on
// the new targets with every attachment loaded, then clear with a vkCmdClearAttachments quad: ~36 passes per course
// frame, each a full-resolution load on a tile GPU for data the clear then overwrote. Such a clear is now kept
// pending and becomes the loadOp CLEAR of the pass the next draw begins on the same targets (same extent, so the
// same area as the quad). Anything else first (a draw on other targets or sampling a cleared image, a dispatch, a
// copy, the submit's end) clears it in a pass of its own with no draw (docs/nvn/DEBUG.md #47).
// NX_EAGER_CLEARS=1: the old way (bisecting).
// Clears are scissored and channel-masked, as on Maxwell (G6): a clear inside a scissor smaller than its target, or
// with some channels masked off, is never a loadOp; it runs in the pass, with vkCmdClearAttachments on the scissor
// rectangle, or, masked, a fullscreen draw with that write mask (nvn_mask_clear.cpp). NX_CLEARS_SCISSOR=1 (off: it broke the course) and
// NX_CLEARS_NO_MASK=1: whole-target and all-channel clears, the old way (bisecting).
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nvn_draw_state.h"
#include "nvn_ultrawide.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

struct Pending {
    bool on = false;
    Targets t;
    uint32_t colors = 0;  // bit c: colour target c cleared
    VkClearValue color[8]{};
    VkImageAspectFlags ds = 0;
    VkClearValue depth{};
};
// The pending clears, oldest first. Without NX_CLEARS_V2 there is at most one. With it, several, never two on the same
// image (a clear of an image already pending with other targets runs the older one first), so their order does not
// matter and each becomes a loadOp, or runs alone, on its own.
std::vector<Pending> g_list;
uint64_t g_deferred = 0, g_as_load = 0, g_alone = 0, g_transfer = 0;
uint64_t g_scissored = 0, g_masked = 0, g_mask_fallback = 0, g_scissor_lost = 0;  // G6

// NX_CLEARS_V2 (P5R PROGRESO 77): pending clears kept while passes on other images run, and taken as loadOps by a
// pass on more targets than they were made with (the same images, views and area). P5R clears its depth buffers with
// only the depth bound and then draws with colours too; and clears both shadow maps before drawing either. Without
// it every one of those clears ran alone (a transfer clear) and the pass then loaded what it had just cleared.
bool clears_v2() {
    static const bool on = [] {
        const char* e = std::getenv("NX_CLEARS_V2");
        return e && e[0] == '1';
    }();
    return on;
}

// begin_rendering's render area for these targets: the smallest attachment at its level.
VkExtent2D targets_extent(const Targets& t) {
    VkExtent2D e{UINT32_MAX, UINT32_MAX};
    auto fold = [&](uint64_t guest, uint32_t level) {
        GpuImage* img = guest ? image_of(guest) : nullptr;
        if (!img) return;
        level = std::min<uint32_t>(level, img->levels - 1);
        e = {std::min(e.width, std::max(1u, img->width >> level)), std::min(e.height, std::max(1u, img->height >> level))};
    };
    for (uint32_t k = 0; k < t.color_count; k++) fold(t.colors[k], t.views.level[k]);
    if (GpuImage* d = t.depth ? image_of(t.depth) : nullptr; d && d->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        fold(t.depth, t.views.level[8]);
    return e;
}

// The images an entry clears (slots 0..7 colours, 8 depth/stencil; null where not cleared).
void cleared_images(const Pending& e, GpuImage* out[9]) {
    for (uint32_t k = 0; k < 9; k++) out[k] = nullptr;
    for (uint32_t k = 0; k < e.t.color_count && k < 8; k++)
        if ((e.colors >> k & 1) && e.t.colors[k]) out[k] = image_of(e.t.colors[k]);
    if (e.ds && e.t.depth) out[8] = image_of(e.t.depth);
}

bool entry_has_image(const Pending& e, const GpuImage* img) {
    GpuImage* imgs[9];
    cleared_images(e, imgs);
    for (GpuImage* i : imgs)
        if (i && i == img) return true;
    return false;
}

// Some image the entry clears is attached in `t` (whatever the view).
bool entry_touches(const Pending& e, const Targets& t) {
    for (uint32_t k = 0; k < t.color_count && k < 8; k++)
        if (t.colors[k] && entry_has_image(e, image_of(t.colors[k]))) return true;
    return t.depth && entry_has_image(e, image_of(t.depth));
}

}  // namespace

bool clears_pending_image(const GpuImage* img) {
    if (!img) return false;
    for (const Pending& e : g_list)
        if (entry_has_image(e, img)) return true;
    return false;
}

namespace {

// Where each pending cleared attachment of `e` is in `t` (slot, or -1): the same texture and view at the same render
// area.
bool pending_slots(const Pending& e, const Targets& t, int slot[9]) {
    const Targets& p = e.t;
    for (uint32_t k = 0; k < 9; k++) slot[k] = -1;
    for (uint32_t k = 0; k < p.color_count; k++) {
        if (!(e.colors >> k & 1)) continue;
        for (uint32_t j = 0; j < t.color_count && slot[k] < 0; j++)
            if (t.colors[j] == p.colors[k] && t.views.level[j] == p.views.level[k] &&
                t.views.layer[j] == p.views.layer[k] && t.views.format[j] == p.views.format[k])
                slot[k] = int(j);
        if (slot[k] < 0) return false;
    }
    if (e.ds) {
        if (t.depth != p.depth || t.views.level[8] != p.views.level[8] || t.views.layer[8] != p.views.layer[8])
            return false;
        slot[8] = 8;
    }
    if (targets_same(t, p)) return true;
    // A pass on more targets: its area must cover every cleared image whole, as the clear did (a loadOp clears the
    // render area only). NX_CLEARS_SUBSET=1 alone (the first try, for Wonder): the same area as the clear's targets.
    // It measured no faster there (GPU 13.7 against 13.0 ms) and one run showed a black band across the course.
    static const bool subset = std::getenv("NX_CLEARS_SUBSET") != nullptr;
    if (!subset && !clears_v2()) return false;
    VkExtent2D a = targets_extent(t), b = targets_extent(p);
    if (a.width != b.width || a.height != b.height) return false;
    if (clears_v2()) {
        GpuImage* imgs[9];
        cleared_images(e, imgs);
        for (uint32_t k = 0; k < 9; k++) {
            if (!imgs[k]) continue;
            const uint32_t level = std::min<uint32_t>(p.views.level[k], imgs[k]->levels - 1);
            if (a.width < std::max(1u, imgs[k]->width >> level) || a.height < std::max(1u, imgs[k]->height >> level))
                return false;
        }
    }
    return true;
}

}  // namespace

// live_flags clears_why: the pending clear's targets against those of what is about to flush it (first 80).
void clears_log_mismatch(const char* what, const Targets& t) {
    static int n = 0;
    if (g_list.empty() || n++ >= 80) return;
    auto desc = [](const Targets& x) {
        std::string s = std::to_string(x.color_count) + " [";
        char b[96];
        for (uint32_t k = 0; k < 9; k++) {
            const uint64_t h = k < 8 ? x.colors[k] : x.depth;
            if (!h && !x.views.level[k] && !x.views.layer[k] && !x.views.format[k]) continue;
            GpuImage* img = h ? image_of(h) : nullptr;
            std::snprintf(b, sizeof b, " %u:%llx %ux%u l%u L%u f%x", k, (unsigned long long)h, img ? img->width : 0,
                          img ? img->height : 0, x.views.level[k], x.views.layer[k], unsigned(x.views.format[k]));
            s += b;
        }
        return s + " ]";
    };
    const Pending& e = g_list.front();
    log("clears why %s: %zu pending, the oldest colours %x ds %x on %s | %s", what, g_list.size(), e.colors,
        unsigned(e.ds), desc(e.t).c_str(), desc(t).c_str());
}

// A pass on `t` begins: pending clears that it cannot take as loadOps must run first. Without NX_CLEARS_V2, any
// pending clear the pass does not take; with it, only those on images the pass attaches.
bool clears_pending_other(const Targets& t) {
    int slot[9];
    for (const Pending& e : g_list)
        if (!pending_slots(e, t, slot) && (!clears_v2() || entry_touches(e, t))) return true;
    return false;
}

uint32_t clears_apply(const Targets& t, VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo& depth,
                      VkRenderingAttachmentInfo& stencil) {
    uint32_t cleared = 0;
    for (auto it = g_list.begin(); it != g_list.end();) {
        int slot[9];
        if (!pending_slots(*it, t, slot)) {
            ++it;
            continue;
        }
        const Pending& e = *it;
        for (uint32_t k = 0; k < 8; k++)
            if (slot[k] >= 0 && colors[slot[k]].imageView) {
                colors[slot[k]].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colors[slot[k]].clearValue = e.color[k];
                cleared |= 1u << slot[k];
            }
        if (e.ds && depth.imageView) {
            depth.clearValue = stencil.clearValue = e.depth;
            if (e.ds & VK_IMAGE_ASPECT_DEPTH_BIT) depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (e.ds & VK_IMAGE_ASPECT_STENCIL_BIT) stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            cleared |= 1u << 8;
        }
        it = g_list.erase(it);
        g_as_load++;
    }
    return cleared;
}

// A pending clear that no pass takes, as transfer clears instead of an empty pass (docs/nvn/android-perf/PHASE4.md
// G5A): on Adreno each pass makes the driver's per-pass objects (~40-57 us), ~11 such passes per course frame. A pending
// clear covers its whole target (draw_clear defers only those), so clearing the view's level and layer is the same.
// Not for a view in another format than its image (a loadOp clears through the view, an sRGB view encodes), nor for
// 3D images (a transfer clear takes every slice): those keep the pass. Android by default (MoltenVK encodes a transfer
// clear as a Metal encoder of its own, DEBUG.md #31); NX_CLEARS_PASS=1: the pass; NX_CLEARS_TRANSFER=1: elsewhere too.
bool transfer_clears() {
    static const bool on = [] {
#if defined(__ANDROID__)
        return std::getenv("NX_CLEARS_PASS") == nullptr;
#else
        return std::getenv("NX_CLEARS_TRANSFER") != nullptr;
#endif
    }();
    return on;
}

bool clears_transfer(VkCommandBuffer cb, const Pending& e) {
    const Targets& t = e.t;
    GpuImage* imgs[9] = {};
    for (uint32_t k = 0; k < t.color_count; k++) {
        if (!(e.colors >> k & 1)) continue;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        if (!img) continue;
        if (img->view_type == VK_IMAGE_VIEW_TYPE_3D || view_format(*img, t.views.format[k]) != img->format) return false;
        imgs[k] = img;
    }
    if (e.ds) {
        GpuImage* d = t.depth ? image_of(t.depth) : nullptr;
        if (d && (d->aspect & e.ds) != e.ds) return false;
        imgs[8] = d;
    }
    for (uint32_t k = 0; k < 9; k++) {
        GpuImage* img = imgs[k];
        if (!img) continue;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // as begin_rendering: rendered images stay GENERAL
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        uint32_t layer = std::min<uint32_t>(t.views.layer[k], img->layers - 1);
        if (k < 8) {
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &e.color[k].color, 1, &r);
        } else {
            VkImageSubresourceRange r{e.ds, level, 1, layer, 1};
            p_vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &e.depth.depthStencil, 1, &r);
        }
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    g_transfer++;
    return true;
}

// Why pending clears ran alone instead of as the next pass's loadOp (logged with the clear counts): 0-9 the
// draw_end_rendering reason, 10 a pass on other targets, 11 a draw sampling a cleared image, 12 a clear of other
// targets, 13 the submit's end, 14 anything else.
uint64_t g_flush_why[16] = {};
int g_flush_reason = 14;
void clears_flush_reason(int why) { g_flush_reason = why; }

// Runs one pending clear alone: transfer clears, or an empty pass that takes it as its loadOps.
static void flush_entry(VkCommandBuffer cb, Pending e) {
    g_flush_why[std::clamp(g_flush_reason, 0, 15)]++;
    g_alone++;
    if (transfer_clears() && clears_transfer(cb, e)) return;
    std::vector<Pending> rest;
    rest.swap(g_list);
    g_list.push_back(e);   // the pass below takes this one alone
    pass_begin(cb, e.t);   // takes the clears as its loadOps (counted by clears_apply)
    g_as_load--;
    pass_end_only(cb, 3);
    g_list.swap(rest);
}

void clears_flush(VkCommandBuffer cb) {
    std::vector<Pending> all;
    all.swap(g_list);
    for (const Pending& e : all) flush_entry(cb, e);
    g_flush_reason = 14;
}

// NX_CLEARS_V2: only the pending clears on images `t` attaches that its pass cannot take (begin_rendering).
void clears_flush_other(VkCommandBuffer cb, const Targets& t) {
    if (!clears_v2()) return clears_flush(cb);
    std::vector<Pending> run;
    for (auto it = g_list.begin(); it != g_list.end();) {
        int slot[9];
        if (!pending_slots(*it, t, slot) && entry_touches(*it, t)) {
            run.push_back(*it);
            it = g_list.erase(it);
        } else {
            ++it;
        }
    }
    for (const Pending& e : run) flush_entry(cb, e);
    g_flush_reason = 14;
}

void clears_begin_submit() { g_list.clear(); }
bool clears_keep_pending() { return clears_v2(); }

void clears_log() {
    log("nvn clears: %llu deferred to the next pass on their targets, %llu of those passes with draws, %llu alone "
        "(%llu of those as transfer clears)", (unsigned long long)g_deferred, (unsigned long long)g_as_load,
        (unsigned long long)g_alone, (unsigned long long)g_transfer);
    std::string why;
    for (int k = 0; k < 16; k++)
        if (g_flush_why[k]) why += " " + std::to_string(k) + ":" + std::to_string(g_flush_why[k]);
    log("nvn clears: alone because of (0-9 pass end reason, 10 other targets, 11 sampled, 12 other clear, 13 submit "
        "end, 14 other):%s", why.c_str());
    log("nvn clears: %llu scissored (%llu of those outside the pass: whole target), %llu channel-masked (%llu of those "
        "all channels: integer target or no pipeline)", (unsigned long long)g_scissored,
        (unsigned long long)g_scissor_lost, (unsigned long long)g_masked, (unsigned long long)g_mask_fallback);
}

// Clears run inside the render pass of the current targets (vkCmdClearAttachments), or become the loadOps of the
// next one (above). A transfer clear between passes was a Metal encoder of its own and made the next pass reload its
// targets: most of the ~25 ms per frame of GPU time "outside passes" (docs/nvn/DEBUG.md #31). Depth and stencil
// clears honour the call's masks.
void draw_clear(VkCommandBuffer cb, const Targets& t, const Cmd& c) {
    bool color = c.op == Cmd::ClearColor;
    uint64_t guest = color ? (c.n < 8 ? t.colors[c.n] : 0) : t.depth;
    GpuImage* img = guest ? image_of(guest) : nullptr;
    if (!img || color != (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT)) return;
    if (color && !(c.mask & 0xF)) return;  // no channel enabled
    VkClearAttachment a{};
    if (color) {
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, a.colorAttachment = c.n;
        std::memcpy(a.clearValue.color.float32, c.f, 16);
    } else {
        if ((c.mask & 1) && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((c.mask >> 8 & 0xFF) && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!a.aspectMask) return;
        a.clearValue.depthStencil = {c.f[0], uint32_t(c.i[0])};
    }
    uint32_t level = color ? t.views.level[c.n] : t.views.level[8];
    VkExtent2D full{std::max(1u, img->width >> level), std::max(1u, img->height >> level)};
    static const bool transfer = std::getenv("NX_TRANSFER_CLEARS") != nullptr;  // bisecting: whole-image clears
    static const bool eager = std::getenv("NX_EAGER_CLEARS") != nullptr;
    // Off unless NX_CLEARS_SCISSOR=1: clipping clears to the bound scissor broke the course (whole regions left
    // uncleared: huge black/white shapes, DEBUG.md #52), so NVN's clears are not scissored the way this reads it.
    static const bool no_scissor = std::getenv("NX_CLEARS_SCISSOR") == nullptr;
    static const bool no_mask = std::getenv("NX_CLEARS_NO_MASK") != nullptr;
    // The part of the target the clear covers: the scissor (pass coordinates, as the draws') within the target.
    VkRect2D rect{{0, 0}, full};
    if (!no_scissor && replay::g_state.has_scissor) {
        const int32_t* s = replay::g_state.scissor;
        int64_t x0 = std::max(0, s[0]), y0 = std::max(0, s[1]);
        int64_t x1 = std::min<int64_t>(int64_t(s[0]) + std::max(0, s[2]), full.width);
        int64_t y1 = std::min<int64_t>(int64_t(s[1]) + std::max(0, s[3]), full.height);
        if (x1 <= x0 || y1 <= y0) return;  // nothing of the target inside the scissor
        rect = {{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        if (img->uw) {  // NX_ULTRAWIDE
            int32_t rx = rect.offset.x, rw = int32_t(rect.extent.width);
            uw_span(rx, rw, full.width);
            rect.offset.x = rx, rect.extent.width = uint32_t(rw);
        }
    }
    bool scissored =
        rect.offset.x || rect.offset.y || rect.extent.width < full.width || rect.extent.height < full.height;
    bool masked = color && !no_mask && (c.mask & 0xF) != 0xF;
    g_scissored += scissored, g_masked += masked;
    if (!transfer && !eager && !scissored && !masked && !pass_open_on(t)) {
        VkExtent2D e = targets_extent(t);
        if (full.width <= e.width && full.height <= e.height) {  // the pass covers it: a loadOp can clear it
            pass_end_only(cb, 3);  // other targets' pass, and their pending clears
            auto same = [&] {
                for (Pending& p : g_list)
                    if (targets_same(p.t, t)) return &p;
                return static_cast<Pending*>(nullptr);
            };
            if (!clears_v2()) {
                if (!g_list.empty() && !same()) {  // its slots index its own targets
                    if (live_clears_why()) clears_log_mismatch("clear", t);
                    clears_flush_reason(12), clears_flush(cb);
                }
            } else {  // older clears of this image with other targets run first: pending clears share no image
                const Pending* keep = same();
                std::vector<Pending> run;
                for (auto it = g_list.begin(); it != g_list.end();)
                    if (&*it != keep && entry_has_image(*it, img)) {
                        run.push_back(*it);
                        it = g_list.erase(it);
                        keep = same();  // the erase moved the entries after it
                    } else {
                        ++it;
                    }
                for (const Pending& r : run) clears_flush_reason(12), flush_entry(cb, r);
            }
            Pending* e = same();
            if (!e) g_list.push_back({true, t}), e = &g_list.back();
            if (color) {
                e->colors |= 1u << c.n;
                e->color[c.n] = a.clearValue;
            } else {
                if (a.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) e->depth.depthStencil.depth = c.f[0];
                if (a.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) e->depth.depthStencil.stencil = uint32_t(c.i[0]);
                e->ds |= a.aspectMask;
            }
            g_deferred++;
            replay::trace_pass("clear deferred: slot, mask", t, color ? c.n : 8, c.mask);
            return;
        }
    }
    if (!pass_open_on(t)) {
        draw_end_rendering(cb, 3);
        pass_begin(cb, t);
    }
    VkExtent2D pe = pass_extent();
    if (transfer && !color) a.aspectMask = img->aspect;  // the old clears ignored the masks
    bool in_pass = rect.offset.x + rect.extent.width <= pe.width && rect.offset.y + rect.extent.height <= pe.height;
    if (transfer || !in_pass) {
        // The pass is smaller than this target (a larger shared depth buffer): clear all of it, outside the pass.
        g_scissor_lost += scissored, g_mask_fallback += masked;
        draw_end_rendering(cb, 4);
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        VkImageSubresourceRange r{a.aspectMask, std::min<uint32_t>(level, img->levels - 1), 1,
                                  std::min<uint32_t>(color ? t.views.layer[c.n] : t.views.layer[8], img->layers - 1), 1};
        if (color)
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &a.clearValue.color, 1, &r);
        else
            p_vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &a.clearValue.depthStencil, 1, &r);
        return;
    }
    if (masked && mask_clear(cb, t, c.n, c.mask & 0xF, c.f, rect)) {
        pass_mark_written(1u << c.n);
        return;
    }
    g_mask_fallback += masked;
    VkClearRect r{rect, 0, 1};  // unscissored: all of the target, which is the pass's area
    replay::trace_pass("clear in pass: slot, mask | bound channel mask << 8", t, color ? c.n : 8,
                       c.mask | (color ? uint32_t(replay::g_state.mask.mask[c.n]) << 8 : 0));
    p_vkCmdClearAttachments(cb, 1, &a, 1, &r);
    pass_mark_written(color ? 1u << c.n : 1u << 8);  // a later draw sampling it needs a fresh pass
}

}  // namespace nx::nvn
