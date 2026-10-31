// nn::hid at the API level: player 1's Npad state comes from the platform (XInput + keyboard on Windows, game
// controllers + touch on Android). Vibration is accepted and dropped.
#include <array>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

using namespace plat;

constexpr uint32_t kStyleFullKey = 1u << 0;
uint64_t g_sampling = 0;

// Scripted input for automated runs: NX_INPUT="3300:A,3900:A,4200:Down" holds the button for 8 frames from the
// given presented frame.
struct Scripted {
    uint64_t frame;
    uint64_t buttons;
};

std::vector<Scripted> input_script() {
    std::vector<Scripted> v;
    const char* e = std::getenv("NX_INPUT");
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            uint64_t bits = b == "A" ? kA : b == "B" ? kB : b == "X" ? kX : b == "Y" ? kY : b == "Plus" ? kPlus :
                            b == "Minus" ? kMinus : b == "Up" ? kUp : b == "Down" ? kDown : b == "Left" ? kLeft :
                            b == "Right" ? kRight : b == "L" ? kL : b == "R" ? kR : 0;
            v.push_back({std::strtoull(item.c_str(), nullptr, 10), bits});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

PadState read() {
    PadState p;
    static const std::vector<Scripted> script = input_script();
    for (const auto& s : script)
        if (g_frame >= s.frame && g_frame < s.frame + 8) p.buttons |= s.buttons;
    read_pad(p);
    // Stick directions as buttons, like the hid sysmodule reports them.
    constexpr int32_t t = kStickMax / 2;
    if (p.lx < -t) p.buttons |= kStickLLeft;
    if (p.lx > t) p.buttons |= kStickLRight;
    if (p.ly > t) p.buttons |= kStickLUp;
    if (p.ly < -t) p.buttons |= kStickLDown;
    if (p.rx < -t) p.buttons |= kStickRLeft;
    if (p.rx > t) p.buttons |= kStickRRight;
    if (p.ry > t) p.buttons |= kStickRUp;
    if (p.ry < -t) p.buttons |= kStickRDown;
    return p;
}

// NpadFullKeyState / NpadJoyDualState / NpadHandheldState:
// {s64 sampling; u64 buttons; {s32 x, y} left; {s32 x, y} right; u32 attributes; pad}
void GetNpadState(CPU& c) {
    uint64_t out = c.x[0];
    uint32_t id = ld<uint32_t>(c.x[1]);
    std::memset(reinterpret_cast<void*>(out), 0, 0x28);
    st<int64_t>(out, int64_t(++g_sampling));
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s", id, hle_current_name());
    }
    if (id != 0) return;  // only player 1 is connected
    PadState p = read();
    static bool pressed = false;
    if (p.buttons && !pressed) log("hid: first buttons %llx", (unsigned long long)p.buttons), pressed = true;
    st<uint64_t>(out + 8, p.buttons);
    st<int32_t>(out + 16, p.lx);
    st<int32_t>(out + 20, p.ly);
    st<int32_t>(out + 24, p.rx);
    st<int32_t>(out + 28, p.ry);
    st<uint32_t>(out + 32, 1);  // IsConnected
}

// GetNpadStates(State* out, int count, const u32& id) -> number written: the newest first. One sample: the current
// one for player 1's full-key style, a not-connected one otherwise.
// GetNpadStates(State* out, int count, const u32& id) -> number written, newest first. Like the real hid, a history:
// Wonder ignores the return value and reads all 16 entries it asks for, telling new samples by their sampling
// numbers, so every call adds a sample and returns up to 16 of the latest (older entries stay as they were).
void GetNpadStatesFullKey(CPU& c) {
    {
        static std::atomic<uint64_t> calls{0};
        uint64_t n = ++calls;
        if (n % 3000 == 0) log("hid: %llu full-key reads (lr %s)", (unsigned long long)n, describe(c.x[30]).c_str());
    }
    uint64_t out = c.x[0], id_ref = c.x[2];
    int n = int(c.x[1]);
    if (n < 1) {
        c.x[0] = 0;
        return;
    }
    static std::mutex m;
    static std::deque<std::array<uint8_t, 0x28>> history;
    std::lock_guard lk(m);
    c.x[0] = out;
    c.x[1] = id_ref;
    GetNpadState(c);  // the new sample, into out[0]
    if (ld<uint32_t>(id_ref) != 0) {
        c.x[0] = 1;
        return;
    }
    std::array<uint8_t, 0x28> now;
    std::memcpy(now.data(), reinterpret_cast<const void*>(out), now.size());
    history.push_front(now);
    if (history.size() > 16) history.pop_back();
    int count = std::min<int>(n, int(history.size()));
    for (int k = 1; k < count; k++) std::memcpy(reinterpret_cast<void*>(out + 0x28ull * k), history[k].data(), 0x28);
    c.x[0] = uint64_t(count);
}

void GetNpadStatesOther(CPU& c) {
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        uint32_t id = ld<uint32_t>(c.x[2]);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s (not connected)", id, hle_current_name());
    }
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// MouseState {s64 sampling; s32 x, y, dx, dy, wheel dx, wheel dy; u32 buttons; u32 attributes}: no mouse.
void GetMouseStates(CPU& c) {
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// KeyboardState {s64 sampling; u64 modifiers; u8 keys[32]}: nothing pressed (the keyboard drives the pad).
void GetKeyboardState(CPU& c) {
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x30);
    st<int64_t>(c.x[0], int64_t(g_sampling));
}

void zero(CPU& c) { c.x[0] = 0; }
void one(CPU& c) { c.x[0] = 1; }

void GetNpadStyleSet(CPU& c) {
    static int logged = 0;
    if (logged++ < 4) log("hid: GetNpadStyleSet id %#x", ld<uint32_t>(c.x[0]));
    c.x[0] = ld<uint32_t>(c.x[0]) == 0 ? kStyleFullKey : 0;
}

void BindNpadStyleSetUpdateEvent(CPU& c) {  // (const u32& id, SystemEventType*, EventClearMode)
    uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t ev = c.x[1], mode = c.x[2];
    uint32_t id = ld<uint32_t>(c.x[0]);
    c.x[0] = ev;
    c.x[1] = mode;
    c.x[2] = 0;
    call(c, create);
    // Player 1's controller is there from the start: the style set "changed" (connected) once, and games wait for
    // this event before they look at the npad.
    static uint64_t signal = find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (signal) {
        c.x[0] = ev;
        call(c, signal);
    }
    log("hid: style set event bound for id %#x, signalled", id);
}

void GetVibrationDeviceHandles(CPU& c) { c.x[0] = 0; }  // no vibration devices
void ShowControllerSupport(CPU& c) {                     // (ControllerSupportResultInfo*, const Arg&)
    st<int8_t>(c.x[0], 1);        // player count
    st<uint32_t>(c.x[0] + 4, 0);  // selected npad id: No1
    c.x[0] = 0;
}
void noop(CPU& c) { c.x[0] = 0; }

// TouchScreenState<N>: {int64 samplingNumber; int32 count; int32 reserved; TouchState touches[N]}. No touches.
void GetTouchScreenState(CPU& c) {
    st<int64_t>(c.x[0], int64_t(g_sampling));
    st<int32_t>(c.x[0] + 8, 0);
}

void populate_sixaxis(uint64_t s) {
    for (uint64_t k = 0; k < 0x5c; k += 4) st<uint32_t>(s + k, 0);
    st<int64_t>(s, 16666666);           // deltaTime: ~16.6ms
    st<int64_t>(s + 8, int64_t(g_sampling));
    // Acceleration: 1G downwards on Y axis (rest state)
    st<float>(s + 0x10, 0.0f);          // acc.x
    st<float>(s + 0x14, -1.0f);         // acc.y (-1G)
    st<float>(s + 0x18, 0.0f);          // acc.z

    // Read current right-stick / gyro inputs to simulate motion aiming (e.g. Darts minigame)
    PadState p{};
    read_pad(p);
    float rx = float(p.rx) / float(kStickMax);
    float ry = float(p.ry) / float(kStickMax);

    static float angle_x = 0.0f, angle_y = 0.0f;
    static uint64_t last_time = 0;
    uint64_t now = monotonic_ns();
    float dt = (last_time && now > last_time) ? float(now - last_time) * 1e-9f : 0.016f;
    if (dt > 0.1f) dt = 0.016f;
    last_time = now;

    float ang_vel_x = rx * 3.14159f;   // rad/s
    float ang_vel_y = ry * 3.14159f;
    angle_x = std::clamp(angle_x + ang_vel_x * dt, -1.57f, 1.57f);
    angle_y = std::clamp(angle_y + ang_vel_y * dt, -1.57f, 1.57f);
    if (std::abs(rx) < 0.05f) angle_x *= 0.95f;
    if (std::abs(ry) < 0.05f) angle_y *= 0.95f;

    st<float>(s + 0x1c, ang_vel_x);     // angularVelocity.x
    st<float>(s + 0x20, ang_vel_y);     // angularVelocity.y
    st<float>(s + 0x24, 0.0f);          // angularVelocity.z

    st<float>(s + 0x28, angle_x);       // angle.x
    st<float>(s + 0x2c, angle_y);       // angle.y
    st<float>(s + 0x30, 0.0f);          // angle.z

    // Rotation direction matrix (3x3 float matrix at 0x34..0x54)
    st<float>(s + 0x34, 1.0f);          // direction.x = (1, 0, 0)
    st<float>(s + 0x44, 1.0f);          // direction.y = (0, 1, 0)
    st<float>(s + 0x54, 1.0f);          // direction.z = (0, 0, 1)

    // Attributes at 0x58: bit 0 = IsConnected, bit 1 = IsInterpolated
    st<uint32_t>(s + 0x58, 1);
}

void GetSixAxisSensorHandles(CPU& c) {
    uint64_t out = c.x[0];
    int32_t count = int32_t(c.x[1]);
    if (out && count > 0) {
        st<uint32_t>(out, 1);
        c.x[0] = 1;
    } else {
        c.x[0] = 0;
    }
}

void GetSixAxisSensorState(CPU& c) {
    if (c.x[0]) populate_sixaxis(c.x[0]);
}

void GetSixAxisSensorStates(CPU& c) {
    uint64_t out = c.x[0];
    int32_t count = int32_t(c.x[1]);
    if (out && count > 0) {
        populate_sixaxis(out);
        c.x[0] = 1;
    } else {
        c.x[0] = 0;
    }
}

}  // namespace

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", noop);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", noop);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
NX_HLE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", ShowControllerSupport);
// SDK 16 (Super Mario Bros. Wonder)
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", GetNpadStatesFullKey);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid15InitializeMouseEv", noop);
NX_HLE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", GetMouseStates);
NX_HLE("_ZN2nn3hid18InitializeKeyboardEv", noop);
NX_HLE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", GetKeyboardState);
NX_HLE("_ZN2nn3hid14DisconnectNpadERKj", noop);
NX_HLE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", noop);
NX_HLE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", noop);
NX_HLE("_ZN2nn3hid21StartLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20StopLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", zero);  // dual
NX_HLE("_ZN2nn3hid20IsVibrationPermittedEv", one);
NX_HLE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", noop);
NX_HLE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetSixAxisSensorHandles);
NX_HLE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", GetSixAxisSensorStates);
// SDK 13 (Persona 5 Royal)
NX_HLE("_ZN2nn3hid21InitializeTouchScreenEv", noop);
NX_HLE("_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE", GetTouchScreenState);
NX_HLE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", GetSixAxisSensorState);
NX_HLE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid34ResetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", noop);

}  // namespace nx
