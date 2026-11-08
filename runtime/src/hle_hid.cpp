// nn::hid at the API level: the Npad states of players 1 and 2 (npad ids 0 and 1) from the platform and the script
// lanes, or from remote input (hid_input.h). Vibration is accepted and dropped.
#include <array>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <nx/runtime.h>

#include "hid_input.h"
#include "platform.h"

namespace nx {

namespace {

using namespace plat;
using hid::kPlayers;
using hid::kRemoteFrames;

constexpr uint32_t kStyleFullKey = 1u << 0;
uint64_t g_sampling = 0;

// Frame lanes for automated runs, keyed by presented frame (nx::g_frame), so they do not drift with frame dumps or
// slow frames: NX_INPUT (player 1) and NX_INPUT_P2 (player 2) = "<frame>:<button>[:<frames>],...", e.g.
// NX_INPUT_P2="3300:Left:60,3400:Y" holds the button for <frames> frames (default 8) from that frame. Buttons: A B X Y
// L R ZL ZR Plus Minus, Up Down Left Right (the d-pad), SLeft SRight SUp SDown (the left stick at full tilt).
struct Scripted {
    uint64_t frame, hold;
    uint64_t buttons;
    int32_t lx, ly;
};

std::vector<Scripted> frame_lane(const char* var) {
    struct Name {
        const char* name;
        uint64_t bits;
        int32_t lx, ly;
    };
    static const Name names[] = {
        {"A", kA, 0, 0},         {"B", kB, 0, 0},           {"X", kX, 0, 0},          {"Y", kY, 0, 0},
        {"L", kL, 0, 0},         {"R", kR, 0, 0},           {"ZL", kZL, 0, 0},        {"ZR", kZR, 0, 0},
        {"Plus", kPlus, 0, 0},   {"Minus", kMinus, 0, 0},   {"Up", kUp, 0, 0},        {"Down", kDown, 0, 0},
        {"Left", kLeft, 0, 0},   {"Right", kRight, 0, 0},   {"SLeft", 0, -kStickMax, 0},
        {"SRight", 0, kStickMax, 0}, {"SUp", 0, 0, kStickMax}, {"SDown", 0, 0, -kStickMax},
    };
    std::vector<Scripted> v;
    const char* e = std::getenv(var);
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t a = item.find(':'), b = a == std::string::npos ? a : item.find(':', a + 1);
        if (a != std::string::npos) {
            std::string button = item.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            Scripted sc{std::strtoull(item.c_str(), nullptr, 10), 8, 0, 0, 0};
            if (b != std::string::npos) sc.hold = std::strtoull(item.c_str() + b + 1, nullptr, 10);
            bool known = false;
            for (const Name& n : names)
                if (button == n.name) sc.buttons = n.bits, sc.lx = n.lx, sc.ly = n.ly, known = true;
            if (known) v.push_back(sc);
            else log("hid: %s: unknown button '%s'", var, button.c_str());
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

// Remote input (hid_input.h): per player a ring of the frames fed, indexed by frame number.
struct Remote {
    bool on = false;
    std::array<uint64_t, kRemoteFrames> frame;
    std::array<PadState, kRemoteFrames> pad;
    uint64_t missing = 0;
    Remote() { frame.fill(~0ull); }
};
std::mutex g_remote_m;
Remote g_remote[kPlayers];

// The remote input of the player for frame f: false when the player has none (its local sources apply).
bool remote_input(int player, uint64_t f, PadState& p) {
    std::lock_guard lk(g_remote_m);
    Remote& r = g_remote[player];
    if (!r.on) return false;
    size_t k = size_t(f % kRemoteFrames);
    if (r.frame[k] == f) {
        p = r.pad[k];
    } else {
        p = PadState{};
        if (r.missing++ % 600 == 0)
            log("hid: player %d has no remote input for frame %llu (%llu such reads)", player + 1, (unsigned long long)f,
                (unsigned long long)r.missing);
    }
    return true;
}

// XInput pad 1 (Windows): connected now? Read at most once per presented frame.
bool pad2_present(PadState* out = nullptr) {
    static std::mutex m;
    static uint64_t at = ~0ull;
    static bool present = false;
    static PadState last;
    std::lock_guard lk(m);
    if (at != g_frame) {
        at = g_frame;
        last = PadState{};
        present = read_pad2(last);
    }
    if (out && present) {
        out->buttons |= last.buttons;
        if (last.lx) out->lx = last.lx;
        if (last.ly) out->ly = last.ly;
        if (last.rx) out->rx = last.rx;
        if (last.ry) out->ry = last.ry;
    }
    return present;
}

// The player's frame lane at frame f, added to p: buttons OR'ed, a stick it pushes overrides.
void lane_input(int player, uint64_t f, PadState& p) {
    static const std::vector<Scripted> lanes[kPlayers] = {frame_lane("NX_INPUT"), frame_lane("NX_INPUT_P2")};
    for (const Scripted& s : lanes[player])
        if (f >= s.frame && f < s.frame + s.hold) {
            p.buttons |= s.buttons;
            if (s.lx) p.lx = s.lx;
            if (s.ly) p.ly = s.ly;
        }
}

// NX_INPUT_REMOTE=1: each player that has a frame lane is fed from it through hid::set_remote_input, one frame ahead,
// the way the netplay core will feed exchanged input. Its host pads and wall-clock lane are then ignored: a check of
// the remote path end to end, and runs whose only input is the frame lanes (determinism checks).
void feed_lane_as_remote(int player, uint64_t f) {
    static const bool on = [] {
        const char* e = std::getenv("NX_INPUT_REMOTE");
        return e && *e == '1';
    }();
    static const bool has_lane[kPlayers] = {std::getenv("NX_INPUT") != nullptr, std::getenv("NX_INPUT_P2") != nullptr};
    static std::atomic<uint64_t> fed[kPlayers] = {~0ull, ~0ull};
    if (!on || !has_lane[player] || fed[player].exchange(f + 1) == f + 1) return;
    PadState p;
    lane_input(player, f + 1, p);
    hid::set_remote_input(player, f + 1, p);
}

PadState read(int player) {
    PadState p;
    feed_lane_as_remote(player, g_frame);
    if (!remote_input(player, g_frame, p)) p = hid::read_local(player);
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
        static std::set<std::tuple<uint32_t, std::string, uint64_t>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        if (seen.size() < 64 && seen.insert({id, hle_current_name(), c.x[30]}).second)
            log("hid: reads id %#x via %s (lr %s)", id, hle_current_name(), describe(c.x[30]).c_str());
    }
    if (id >= uint32_t(kPlayers) || !hid::connected(int(id))) return;  // not connected: all zero
    PadState p = read(int(id));
    static std::atomic<bool> pressed[kPlayers];
    if (p.buttons && !pressed[id].exchange(true))
        log("hid: player %u first buttons %llx (frame %llu)", id + 1, (unsigned long long)p.buttons,
            (unsigned long long)g_frame.load());
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
    uint32_t id = ld<uint32_t>(c.x[0]);
    {
        static std::set<std::pair<uint32_t, uint64_t>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        if (seen.size() < 64 && seen.insert({id, c.x[30]}).second)
            log("hid: GetNpadStyleSet id %#x (lr %s)", id, describe(c.x[30]).c_str());
    }
    // Players 1 and 2 as full-key controllers (P4AU polls ids 0-7 every frame, main+31ab0, and picks the state
    // reader by the style it gets).
    bool on = id < uint32_t(kPlayers) && hid::connected(int(id));
    if (id == 1) {
        static std::atomic<int> was{-1};  // -1: not asked yet (no "disconnected" line at the start)
        int before = was.exchange(on);
        if (before != int(on) && (before != -1 || on)) log("hid: player 2 %s", on ? "connected" : "disconnected");
    }
    c.x[0] = on ? kStyleFullKey : 0;
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

// ---- per-player input (hid_input.h) ----

void hid::set_remote_input(int player, uint64_t frame, const plat::PadState& p) {
    if (player < 0 || player >= kPlayers) return;
    std::lock_guard lk(g_remote_m);
    Remote& r = g_remote[player];
    if (!r.on) log("hid: player %d driven by remote input from frame %llu", player + 1, (unsigned long long)frame);
    r.on = true;
    size_t k = size_t(frame % kRemoteFrames);
    r.frame[k] = frame;
    r.pad[k] = p;
}

void hid::clear_remote_input(int player) {
    if (player < 0 || player >= kPlayers) return;
    std::lock_guard lk(g_remote_m);
    g_remote[player] = Remote{};
}

plat::PadState hid::read_local(int player) {
    plat::PadState p;
    if (player < 0 || player >= kPlayers) return p;
    if (player == 0) plat::read_pad(p);  // host pads (touch on Android) and NX_INPUT_SCRIPT
    else {
        pad2_present(&p);
        plat::input_script(p, 1);  // NX_INPUT_SCRIPT_P2
    }
    lane_input(player, g_frame, p);
    return p;
}

bool hid::connected(int player) {
    if (player == 0) return true;
    if (player != 1) return false;
    {
        std::lock_guard lk(g_remote_m);
        if (g_remote[1].on) return true;
    }
    static const std::string forced = plat::setting("p2");  // NX_P2 on the PC
    if (forced == "0" || forced == "1") return forced == "1";
    static const bool scripted = std::getenv("NX_INPUT_P2") || std::getenv("NX_INPUT_SCRIPT_P2");
    return scripted || pad2_present();
}

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", noop);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", noop);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadState);
// P4AU imports these too (the same 0x28-byte state); never used while every player is full-key.
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", GetNpadState);
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
