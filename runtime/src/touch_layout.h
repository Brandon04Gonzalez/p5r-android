// Drawn touch controls: the per-game button layout (touch_layout.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

namespace nx::plat {

// Labels drawn on the buttons: glyphs of kFont in nvn/nvn_present.cpp (read by tools/shaders/present/overlay.frag).
enum Glyph : uint32_t {
    kGlyphNone, kGlyphA, kGlyphB, kGlyphX, kGlyphY, kGlyphL, kGlyphR, kGlyphZ, kGlyphPlus, kGlyphMinus,
    kGlyphUp, kGlyphDown, kGlyphLeft, kGlyphRight, kGlyph3, kGlyphDigit0,  // ..kGlyphDigit0 + 9
};
constexpr uint32_t touch_label(Glyph first, Glyph second = kGlyphNone) { return first | uint32_t(second) << 5; }

struct TouchButton {
    float x, y, r;  // centre across the landscape screen (0..1), radius as a fraction of its height
    uint64_t button;
    uint32_t label = 0;  // touch_label()
};
// The game's layout (count 0: the game handles touch its own way).
const TouchButton* touch_buttons(size_t& count);
// Launcher settings (config.txt NX_TOUCH_*): drawn and accepted at all, stick on the right half, brightness scale.
bool touch_enabled();
bool touch_mirrored();
float touch_opacity_scale();
// The button under (x, y) (0..1 across the screen; aspect = width / height), or 0.
uint64_t touch_hit(float x, float y, float aspect);
// The other half of the screen (not the stick's), where no button is, drags the right stick (P5R: the camera).
bool touch_right_stick();
constexpr float kStickRing = 0.12f;  // the stick ring's radius at rest, a fraction of the screen height
// Where the stick's ring is drawn at rest (x, y across the screen, r a fraction of its height). The left stick takes
// the touches of that half of the screen that miss the buttons.
void touch_stick_rest(float& x, float& y, float& r);
bool touch_stick_side(float x);  // x (0..1 across the screen) is on the stick's half
// What the left stick pushed nearly all the way also holds (Wonder: Y, dash), or 0.
uint64_t touch_stick_full_button();

}  // namespace nx::plat
