#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "platform_android.h"
#include "touch_layout.h"

// Android input: game controllers and touch (split from platform_android.cpp).
namespace nx::plat {

// ---- input: game controllers + touch --------------------------------------------------------------------------
// Touch: left half = virtual stick (relative to where the finger went down); right half = four buttons in a
// diamond around the thumb (Y attack up-left, X special up-right, B dash down-left, A interact down-right), top
// corners L / R, top centre +. Games with a drawn layout (touch_layout.cpp) use its buttons instead, and may make the
// rest of the right half a second stick (P5R: the camera).

namespace {
std::mutex g_in;
PadState g_pad_buttons;  // controller state
enum Stick : uint8_t { kNoStick, kLeftStick, kRightStick };
struct Touch {
    int32_t id;
    float x0, y0, x, y;
    Stick stick;
    uint64_t button;
    uint64_t down_ns;
};
std::vector<Touch> g_touches;
// A quick tap still reaches the game: a button let go sooner than kMinHoldNs after it went down stays held until
// then (some P5R menus missed taps of 100-200 ms that the game had read on several frames).
struct Latch {
    uint64_t button, until_ns;
};
std::vector<Latch> g_latched;
constexpr uint64_t kMinHoldNs = 150'000'000;

void release_touch(const Touch& t, uint64_t now) {
    if (t.button && now < t.down_ns + kMinHoldNs) g_latched.push_back({t.button, t.down_ns + kMinHoldNs});
}
float g_w = 1, g_h = 1;
bool g_gamepad = false;  // the last input came from a controller: the drawn touch controls hide
uint64_t g_last_touch_ns = 0;  // the drawn controls dim after kTouchIdleNs without a touch
constexpr uint64_t kTouchIdleNs = 8'000'000'000;

bool gamepad_swap_abxy() {
    static const bool swap = [] {
        const char* e = std::getenv("NX_GAMEPAD_LAYOUT");
        return e && std::strcmp(e, "1") == 0;
    }();
    return swap;
}

float stick_sens() {
    static const float s = [] {
        const char* e = std::getenv("NX_STICK_SENSITIVITY");
        int v = e ? std::atoi(e) : 100;
        if (v < 25) v = 25;
        if (v > 300) v = 300;
        return float(v) / 100.0f;
    }();
    return s;
}

float camera_sens() {
    static const float s = [] {
        const char* e = std::getenv("NX_CAMERA_SENSITIVITY");
        int v = e ? std::atoi(e) : 100;
        if (v < 25) v = 25;
        if (v > 300) v = 300;
        return float(v) / 100.0f;
    }();
    return s;
}

uint64_t key_to_button(int32_t k) {
    if (gamepad_swap_abxy()) {
        switch (k) {
            case AKEYCODE_BUTTON_A: return kA;
            case AKEYCODE_BUTTON_B: return kB;
            case AKEYCODE_BUTTON_X: return kX;
            case AKEYCODE_BUTTON_Y: return kY;
            default: break;
        }
    } else {
        switch (k) {
            case AKEYCODE_BUTTON_A: return kB;  // bottom face button = Nintendo B
            case AKEYCODE_BUTTON_B: return kA;  // right face button = Nintendo A
            case AKEYCODE_BUTTON_X: return kY;
            case AKEYCODE_BUTTON_Y: return kX;
            default: break;
        }
    }
    switch (k) {
        case AKEYCODE_BUTTON_L1: return kL;
        case AKEYCODE_BUTTON_R1: return kR;
        case AKEYCODE_BUTTON_L2: return kZL;
        case AKEYCODE_BUTTON_R2: return kZR;
        case AKEYCODE_BUTTON_START: return kPlus;
        case AKEYCODE_BUTTON_SELECT: return kMinus;
        case AKEYCODE_BUTTON_THUMBL: return kStickL;
        case AKEYCODE_BUTTON_THUMBR: return kStickR;
        case AKEYCODE_DPAD_LEFT: return kLeft;
        case AKEYCODE_DPAD_UP: return kUp;
        case AKEYCODE_DPAD_RIGHT: return kRight;
        case AKEYCODE_DPAD_DOWN: return kDown;
        case AKEYCODE_BACK: return kMinus;
        // A keyboard: Enter / Space confirm (A), Escape goes back (B). (Some tablets keep BUTTON_B for their stylus.)
        case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_SPACE: case AKEYCODE_DPAD_CENTER: return kA;
        case AKEYCODE_ESCAPE: return kB;
        default: return 0;
    }
}

uint64_t touch_button(float x, float y) {
    float nx_ = x / g_w, ny = y / g_h;
    if (ny < 0.2f) return nx_ < 0.6f ? (nx_ < 0.45f ? kL : kPlus) : (nx_ > 0.85f ? kR : kZR);
    bool up = ny < 0.6f, right = nx_ > 0.8f;
    if (up) return right ? kX : kY;
    return right ? kA : kB;
}

}  // namespace

int32_t on_input(android_app*, AInputEvent* e) {
    std::lock_guard lk(g_in);
    int32_t type = AInputEvent_getType(e);
    int32_t src = AInputEvent_getSource(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_to_button(AKeyEvent_getKeyCode(e));
        if (!b) return 0;
        g_gamepad = true;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN)
            g_pad_buttons.buttons |= b;
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP)
            g_pad_buttons.buttons &= ~b;
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        g_gamepad = true;
        auto axis = [&](int32_t a) {
            float v = AMotionEvent_getAxisValue(e, a, 0);
            constexpr float kDeadzone = 0.12f;
            if (std::fabs(v) < kDeadzone) return 0;
            float sign = v > 0 ? 1.f : -1.f;
            float scaled = (v - sign * kDeadzone) / (1.f - kDeadzone);
            return int32_t(std::clamp(scaled, -1.f, 1.f) * kStickMax);
        };
        g_pad_buttons.lx = axis(AMOTION_EVENT_AXIS_X);
        g_pad_buttons.ly = -axis(AMOTION_EVENT_AXIS_Y);
        int32_t rx = axis(AMOTION_EVENT_AXIS_Z);
        if (!rx) rx = axis(AMOTION_EVENT_AXIS_RX);
        int32_t ry = -axis(AMOTION_EVENT_AXIS_RZ);
        if (!ry) ry = -axis(AMOTION_EVENT_AXIS_RY);
        g_pad_buttons.rx = rx;
        g_pad_buttons.ry = ry;
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        g_pad_buttons.buttons &= ~(kLeft | kRight | kUp | kDown | kZL | kZR);
        if (hx < -0.5f) g_pad_buttons.buttons |= kLeft;
        if (hx > 0.5f) g_pad_buttons.buttons |= kRight;
        if (hy < -0.5f) g_pad_buttons.buttons |= kUp;
        if (hy > 0.5f) g_pad_buttons.buttons |= kDown;
        if (AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0) > 0.4f ||
            AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0) > 0.4f)
            g_pad_buttons.buttons |= kZL;
        if (AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0) > 0.4f ||
            AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0) > 0.4f)
            g_pad_buttons.buttons |= kZR;
        return 1;
    }
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return 0;
    if (!touch_enabled()) return 0;  // NX_TOUCH_SHOW=0
    int32_t action = AMotionEvent_getAction(e);
    int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    size_t index = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const uint64_t now = monotonic_ns();
    g_last_touch_ns = now;
    if (ANativeWindow* w = android_window()) {
        g_w = float(ANativeWindow_getWidth(w));
        g_h = float(ANativeWindow_getHeight(w));
    }
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        float x = AMotionEvent_getX(e, index), y = AMotionEvent_getY(e, index);
        g_gamepad = false;
        size_t drawn = 0;
        touch_buttons(drawn);
        if (drawn) {  // a drawn layout (touch_layout.cpp): its buttons first, the rest of the left half is the stick
            uint64_t b = touch_hit(x / g_w, y / g_h, g_w / g_h);
            bool left = !b && touch_stick_side(x / g_w);
            Stick stick = b ? kNoStick : left ? kLeftStick : touch_right_stick() ? kRightStick : kNoStick;
            if (b || stick) g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, stick, b, now});
        } else {
            bool left = x < g_w * 0.5f;
            g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, left ? kLeftStick : kNoStick,
                                 left ? 0 : touch_button(x, y), now});
        }
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP ||
               kind == AMOTION_EVENT_ACTION_CANCEL) {
        int32_t id = AMotionEvent_getPointerId(e, index);
        const bool all = kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL;
        g_touches.erase(std::remove_if(g_touches.begin(), g_touches.end(),
                                       [&](const Touch& t) {
                                           if (!all && t.id != id) return false;
                                           release_touch(t, now);
                                           return true;
                                       }),
                        g_touches.end());
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int32_t id = AMotionEvent_getPointerId(e, k);
            for (auto& t : g_touches)
                if (t.id == id) {
                    t.x = AMotionEvent_getX(e, k);
                    t.y = AMotionEvent_getY(e, k);
                }
        }
    }
    return 1;
}

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void read_pad(PadState& p) {
    std::lock_guard lk(g_in);
    p.buttons |= g_pad_buttons.buttons;
    if (g_pad_buttons.lx || g_pad_buttons.ly) {
        p.lx = g_pad_buttons.lx;
        p.ly = g_pad_buttons.ly;
    }
    p.rx = g_pad_buttons.rx;
    p.ry = g_pad_buttons.ry;
    float radius = std::max(1.f, g_h * 0.12f);
    size_t drawn = 0;
    touch_buttons(drawn);
    const uint64_t now = monotonic_ns();
    g_latched.erase(std::remove_if(g_latched.begin(), g_latched.end(), [&](const Latch& l) { return now >= l.until_ns; }),
                    g_latched.end());
    for (const auto& l : g_latched) p.buttons |= l.button;
    for (const auto& t : g_touches) {
        float dx = (t.x - t.x0) / radius, dy = (t.y - t.y0) / radius;
        if (t.stick == kLeftStick) {
            float sx = dx * stick_sens();
            float sy = -dy * stick_sens();
            p.lx = int32_t(std::clamp(sx, -1.f, 1.f) * kStickMax);
            p.ly = int32_t(std::clamp(sy, -1.f, 1.f) * kStickMax);
            // A drawn layout's stick pushed nearly all the way may hold a button too (Wonder: Y, dash), so running
            // needs no second thumb on the right side.
            if (drawn && std::abs(dx) > 0.85f) p.buttons |= touch_stick_full_button();
        } else if (t.stick == kRightStick) {
            float sx = dx * camera_sens();
            float sy = -dy * camera_sens();
            p.rx = int32_t(std::clamp(sx, -1.f, 1.f) * kStickMax);
            p.ry = int32_t(std::clamp(sy, -1.f, 1.f) * kStickMax);
        } else {
            p.buttons |= t.button;
        }
    }
    input_script(p);  // NX_INPUT_SCRIPT (config.txt): scripted runs on the phone, as on the Mac
}

// The drawn layout's buttons (brighter while pressed) and the stick under the thumb; nothing while a controller is
// in use or for games without a drawn layout.
size_t touch_overlay(OverlayShape* out, size_t max) {
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    if (!n) return 0;
    std::lock_guard lk(g_in);
    if (g_gamepad || !touch_enabled()) return 0;
    // Dimmed to a third while nobody touches the screen (cutscenes, videos), back at once on the next touch.
    const bool idle = g_touches.empty() && monotonic_ns() - g_last_touch_ns > kTouchIdleNs;
    const float shade = touch_opacity_scale() * (idle ? 0.35f : 1.0f);
    const Touch* stick = nullptr;
    const Touch* camera = nullptr;
    uint64_t held = 0;
    for (const auto& t : g_touches) {
        if (t.stick == kLeftStick) stick = &t;
        if (t.stick == kRightStick) camera = &t;
        held |= t.button;
    }
    for (const auto& l : g_latched) held |= l.button;
    for (size_t i = 0; i < n && k < max; i++)
        out[k++] = {b[i].x, b[i].y, b[i].r, std::min(1.0f, (held & b[i].button ? 0.5f : 0.2f) * shade), b[i].label};
    // A stick under the thumb: its ring where the finger went down and the knob where it is now.
    auto draw_stick = [&](const Touch& t) {
        if (k + 2 > max) return;
        float r = 0.12f, dx = (t.x - t.x0) / g_h, dy = (t.y - t.y0) / g_h;
        float len = std::hypot(dx, dy), s = len > r ? r / len : 1.f;
        out[k++] = {t.x0 / g_w, t.y0 / g_h, r, std::min(1.0f, 0.14f * shade)};
        out[k++] = {(t.x0 / g_h + dx * s) * g_h / g_w, t.y0 / g_h + dy * s, 0.055f, std::min(1.0f, 0.45f * shade)};
    };
    if (stick) {
        draw_stick(*stick);
    } else if (k < max) {  // where the stick goes
        float sx, sy, sr;
        touch_stick_rest(sx, sy, sr);
        out[k++] = {sx, sy, sr, std::min(1.0f, 0.08f * shade)};
    }
    if (camera) draw_stick(*camera);
    return k;
}

}  // namespace nx::plat
