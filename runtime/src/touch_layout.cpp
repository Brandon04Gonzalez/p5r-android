// Drawn touch controls (docs/nvn/ANDROID-PORT.md A4): a fixed layout of round buttons per game, hit-tested by
// platform_android_input.cpp and drawn by the NVN present pass (nvn_present.cpp, tools/shaders/present/overlay.frag)
// over the picture, each with its label.
//
// Super Mario Bros. Wonder, landscape: A (jump, the big one) at the bottom right with B (jump), Y (dash, action) and
// X (action) around it; ZR (roll), R, L, + and - along the top. The left half is the stick (platform_android_input.cpp):
// it appears where the thumb lands, and pushed nearly all the way it also holds Y, so running needs no second thumb.
//
// Persona 5 Royal, landscape: the four face buttons in the Switch diamond at the bottom right (A confirm/talk, B back,
// X menu, Y), L / ZL and R / ZR stacked in the top corners, - and + at the top centre, the d-pad on the left above
// the stick, with a gap (the d-pad's down arrow touched the stick's ring). The left half is the stick; the rest of
// the right half drags the right stick (the camera). The launcher's editor (games/p5r/java: ControlsEditor.java, which
// has a copy of this table) moves any of them, the stick's ring included (NX_TOUCH_LAYOUT).
//
// Games without a layout here keep their own touch handling (Hades: zones around the thumb).
//
// Elsewhere touch_overlay() draws nothing, except with NX_OVERLAY_DEMO=1 (the buttons at rest), to check the look
// on the PC or the Mac with NX_PRESENT_TEST.
#include "touch_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nx/game.h>

namespace nx::plat {

namespace {

struct Named {
    const char* name;  // for NX_TOUCH_LAYOUT
    TouchButton b;
};

// x, y: 0..1 across the landscape screen; r: a fraction of the screen height.
const Named kWonder[] = {
    {"A", {0.905f, 0.74f, 0.105f, kA, touch_label(kGlyphA)}},
    {"B", {0.775f, 0.86f, 0.085f, kB, touch_label(kGlyphB)}},
    {"Y", {0.775f, 0.60f, 0.085f, kY, touch_label(kGlyphY)}},
    {"X", {0.905f, 0.47f, 0.075f, kX, touch_label(kGlyphX)}},
    {"ZR", {0.955f, 0.25f, 0.055f, kZR, touch_label(kGlyphZ, kGlyphR)}},
    {"R", {0.955f, 0.07f, 0.045f, kR, touch_label(kGlyphR)}},
    {"L", {0.045f, 0.07f, 0.045f, kL, touch_label(kGlyphL)}},
    {"PLUS", {0.545f, 0.07f, 0.040f, kPlus, touch_label(kGlyphPlus)}},
    {"MINUS", {0.455f, 0.07f, 0.040f, kMinus, touch_label(kGlyphMinus)}},
};

const Named kP5R[] = {
    {"A", {0.940f, 0.66f, 0.068f, kA, touch_label(kGlyphA)}},
    {"B", {0.875f, 0.80f, 0.068f, kB, touch_label(kGlyphB)}},
    {"X", {0.875f, 0.52f, 0.068f, kX, touch_label(kGlyphX)}},
    {"Y", {0.810f, 0.66f, 0.068f, kY, touch_label(kGlyphY)}},
    {"R", {0.945f, 0.08f, 0.052f, kR, touch_label(kGlyphR)}},
    {"ZR", {0.945f, 0.24f, 0.052f, kZR, touch_label(kGlyphZ, kGlyphR)}},
    {"L", {0.055f, 0.08f, 0.052f, kL, touch_label(kGlyphL)}},
    {"ZL", {0.055f, 0.24f, 0.052f, kZL, touch_label(kGlyphZ, kGlyphL)}},
    {"MINUS", {0.440f, 0.07f, 0.045f, kMinus, touch_label(kGlyphMinus)}},
    {"PLUS", {0.560f, 0.07f, 0.045f, kPlus, touch_label(kGlyphPlus)}},
    {"UP", {0.135f, 0.31f, 0.048f, kUp, touch_label(kGlyphUp)}},
    {"DOWN", {0.135f, 0.49f, 0.048f, kDown, touch_label(kGlyphDown)}},
    {"LEFT", {0.085f, 0.40f, 0.048f, kLeft, touch_label(kGlyphLeft)}},
    {"RIGHT", {0.185f, 0.40f, 0.048f, kRight, touch_label(kGlyphRight)}},
    {"L3", {0.055f, 0.58f, 0.046f, kStickL, touch_label(kGlyphL, kGlyph3)}},
    {"R3", {0.740f, 0.82f, 0.046f, kStickR, touch_label(kGlyphR, kGlyph3)}},
};

struct Layout {
    const char* game;
    const Named* buttons;
    size_t count;
    bool right_stick;
    uint64_t stick_full;
    float stick_x, stick_y;  // where the stick's ring is drawn at rest (radius kStickRing)
};

const Layout kLayouts[] = {
    {"wonder", kWonder, std::size(kWonder), false, kY, 0.15f, 0.72f},
    {"p5r", kP5R, std::size(kP5R), true, 0, 0.15f, 0.76f},
};

const Layout* layout() {
    static const Layout* l = [] () -> const Layout* {
        for (const auto& k : kLayouts)
            if (std::strcmp(game::kInfo.id, k.game) == 0) return &k;
        return nullptr;
    }();
    return l;
}

// NX_TOUCH_LAYOUT=A:x,y,r;B:...: the launcher's place for `name` (x, y across the screen, r a fraction of its height).
bool layout_entry(const char* name, float& x, float& y, float& r) {
    const char* e = std::getenv("NX_TOUCH_LAYOUT");
    if (!e) return false;
    const std::string all = e;
    const size_t len = std::strlen(name);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(';', pos);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        float ix, iy, ir;
        if (item.size() <= len || item.compare(0, len, name) != 0 || item[len] != ':' ||
            std::sscanf(item.c_str() + len + 1, "%f,%f,%f", &ix, &iy, &ir) != 3)
            continue;
        if (ix < 0 || ix > 1 || iy < 0 || iy > 1 || ir <= 0.01f || ir >= 0.3f) return false;
        x = ix, y = iy, r = ir;
        return true;
    }
    return false;
}

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

// The layout as the launcher set it (config.txt, which becomes the environment): NX_TOUCH_LAYOUT replaces the positions
// and radii of the named buttons, NX_TOUCH_SIZE (50..160, percent) scales every radius. Read once.
const std::vector<TouchButton>& buttons() {
    static const std::vector<TouchButton> v = [] {
        std::vector<TouchButton> out;
        const Layout* l = layout();
        if (!l) return out;
        for (size_t k = 0; k < l->count; k++) {
            out.push_back(l->buttons[k].b);
            layout_entry(l->buttons[k].name, out[k].x, out[k].y, out[k].r);
        }
        float scale = float(std::clamp(env_int("NX_TOUCH_SIZE", 100), 50, 160)) / 100.0f;
        for (auto& b : out) b.r *= scale;
        return out;
    }();
    return v;
}

}  // namespace

// NX_TOUCH_SHOW=0 (launcher: On-screen buttons off): nothing is drawn and touches are ignored.
bool touch_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_SHOW");
        return !(e && e[0] == '0');
    }();
    return on;
}

// NX_TOUCH_MIRROR=1 (left-handed): the stick takes the right half of the screen instead of the left.
bool touch_mirrored() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_MIRROR");
        return e && e[0] == '1';
    }();
    return on;
}

// NX_TOUCH_OPACITY (10..100, default 85): the drawn buttons' brightness relative to the original look.
float touch_opacity_scale() {
    static const float s = float(std::clamp(env_int("NX_TOUCH_OPACITY", 85), 10, 100)) / 85.0f;
    return s;
}

const TouchButton* touch_buttons(size_t& count) {
    count = buttons().size();
    return count ? buttons().data() : nullptr;
}

bool touch_right_stick() { return layout() && layout()->right_stick; }

// The stick's ring at rest: the layout's place (NX_TOUCH_MIRROR=1: mirrored), or the launcher's (STICK:x,y,r).
void touch_stick_rest(float& x, float& y, float& r) {
    struct Rest {
        float x, y, r;
    };
    static const Rest rest = [] {
        const Layout* l = layout();
        Rest v{l ? l->stick_x : 0.15f, l ? l->stick_y : 0.72f, kStickRing};
        if (touch_mirrored()) v.x = 1 - v.x;
        layout_entry("STICK", v.x, v.y, v.r);
        return v;
    }();
    x = rest.x, y = rest.y, r = rest.r;
}

bool touch_stick_side(float x) {
    float sx, sy, sr;
    touch_stick_rest(sx, sy, sr);
    return sx < 0.5f ? x < 0.5f : x >= 0.5f;
}

uint64_t touch_stick_full_button() { return layout() ? layout()->stick_full : 0; }

uint64_t touch_hit(float x, float y, float aspect) {
    size_t n = 0;
    const TouchButton* b = touch_buttons(n);
    uint64_t best = 0;
    float best_d = 1e9f;
    for (size_t k = 0; k < n; k++) {
        float d = std::hypot((x - b[k].x) * aspect, y - b[k].y) / b[k].r;
        if (d < 1.35f && d < best_d) best = b[k].button, best_d = d;  // a little past the drawn edge still counts
    }
    return best;
}

#if !defined(__ANDROID__)
size_t touch_overlay(OverlayShape* out, size_t max) {
    static const bool demo = std::getenv("NX_OVERLAY_DEMO") != nullptr;
    if (!demo) return 0;
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    for (; k < n && k < max; k++) out[k] = {b[k].x, b[k].y, b[k].r, 0.22f, b[k].label};
    float sx, sy, sr;
    touch_stick_rest(sx, sy, sr);
    if (k < max) out[k++] = {sx, sy, sr, 0.10f};  // where the stick usually goes
    return k;
}
#endif

}  // namespace nx::plat
