// NVN device (docs/NVN.md, phases N2-N3). Values come from what the game checks and from the SDK's own NVN driver
// (recompiled in sdk; NX_NVN_REAL=1 records where each of its procs lives, see nvn_trace.cpp).
#include <atomic>
#include <map>

#include <nx/runtime.h>

namespace nx {

namespace {

std::atomic<uint32_t> g_depth_mode{0};  // NVNdepthMode: 0 NEAR_IS_MINUS_W (NVN's default), 1 NEAR_IS_ZERO

// nvnDeviceGetInteger(const NVNdevice*, NVNdeviceInfo pname, int* value).
// Read out of the SDK 16.2 driver (sdk+451090: a jump table over pname 0..0x62, constants inline or from a second
// table at sdk+440c80; see docs/nvn/API.md). 0, 1 are the API version: Wonder requires major == 55 and
// minor >= 15 (main+5a7b48). The "provisional" ones are fields the real nvnDeviceInitialize fills in its device
// struct; their values are placeholders until that init is read.
const std::map<uint32_t, int32_t> kDeviceInfo = {
    {0, 0x37},
    {1, 0xf},
    {2, 0xe},
    {3, 0x10000},
    {4, 0x100},
    {5, 0x8},
    {6, 0x10},
    {7, 0x4},
    {8, 0x10},
    {9, 0x20},
    {10, 0x10},
    {11, 0x4},
    {12, 0x4},
    {13, 0x4},
    {14, 0x10},
    {15, 0x20},
    {16, 0x20},
    {17, 0x100},
    {18, 0x100},
    {19, 0x4},
    {20, 0x8},
    {21, 0x200},
    {22, 0x100},
    {23, 0x2},  // provisional: the driver reads [dev+0xb4]
    {24, 0x1000},  // provisional: the driver reads [dev+0xb4]*[dev+0xac]
    {25, 0x1000},
    {26, 0x20000},
    {27, 0x10},
    {28, 0x600},
    {29, 0x400},
    {30, 0x40},
    {31, 0x600},
    {32, 0xffff},
    {33, 0xffff},
    {34, 0xffff},
    {35, 0x8},
    {36, 0x100000},
    {37, 0x1000},
    {38, 0x10},
    {39, 0x1000},
    {40, 0x1},
    {41, 0x1},
    {42, 0x1},
    {43, 0x40000},  // provisional: the driver reads [dev+0x20]<<10
    {44, 0xf},
    {45, 0x0},  // provisional: the driver reads [dev+0x3c]
    {46, 0x1},
    {47, 0x1},
    {48, 0x10},
    {49, 0x5},
    {50, 0x1},
    {51, 0x8},
    {52, 0x8},
    {53, 0x4},
    {54, 0x20},
    {55, 0x800},  // provisional: the driver reads [dev+0xac]
    {56, 0x0},  // provisional: the driver reads [dev+0x44]
    {57, 0x80},
    {58, 0x0},  // provisional: the driver reads [dev+0xc8]
    {59, 0x1},  // provisional: the driver reads !byte[dev+0x1b1d]
    {60, 0x4},
    {61, 0x0},  // provisional: the driver reads [dev+0x28]
    {62, 0x0},  // provisional: the driver reads [dev+0x40]
    {63, 0x0},  // provisional: the driver reads [dev+0x2c]
    {64, 0x0},  // provisional: the driver reads [dev+0x30]
    {65, 0x0},  // provisional: the driver reads [dev+0x34]
    {66, 0x1},
    {67, 0x1},
    {68, 0x1},
    {69, 0x1},
    {70, 0x5},
    {71, 0x1},
    {72, 0x1},
    {73, 0x1},
    {74, 0x1},
    {75, 0x1},
    {76, 0x1},
    {77, 0x10},
    {78, 0x400},
    {79, 0x20},
    {80, 0x1000},
    {81, 0x10000},
    {82, 0x10000},
    {83, 0x100},
    {84, 0x4000},
    {85, 0x40000},
    {86, 0x1000},
    {87, 0x1},
    {88, 0x4},
    {89, 0x2},
    {90, 0x0},
    {91, 0x1000},
    {92, 0x4000},
    {93, 0x1000},
    {94, 0x80},
    {95, 0x20},
    {96, 0xffff},
    {97, 0x1},
    {98, 0x0},  // provisional: the driver reads [dev+0xbc]
};

void nvnDeviceGetInteger(CPU& c) {
    uint32_t pname = uint32_t(c.x[1]);
    auto it = kDeviceInfo.find(pname);
    if (it == kDeviceInfo.end()) log("nvn: nvnDeviceGetInteger(%u) unknown -> 0 (lr %s)", pname, describe(c.x[30]).c_str());
    if (c.x[2]) st<int32_t>(c.x[2], it == kDeviceInfo.end() ? 0 : it->second);
}

// NVNboolean nvnDeviceRegisterFastClear{Color,Colori,Colorui,Depth}(device, value(s), format): the driver keeps a
// small table of clear values its compression can clear for free. Only a hint; Vulkan clears the same either way.
// P5R treats false as a fatal device error (main+1025248).
void nvnDeviceRegisterFastClear(CPU& c) { c.x[0] = 1; }

// nvnDeviceSetDepthMode(device, NVNdepthMode) / Get: P5R sets NEAR_IS_ZERO (1) before making anything, so clip z is
// 0..w as in Vulkan and the pipelines leave it alone (nvn_pipeline.cpp). Wonder never calls it.
void nvnDeviceSetDepthMode(CPU& c) {
    uint32_t mode = uint32_t(c.x[1]);
    if (g_depth_mode.exchange(mode) != mode) log("nvn: depth mode %u (%s)", mode, mode ? "clip z 0..w" : "clip z -w..w");
}
void nvnDeviceGetDepthMode(CPU& c) { c.x[0] = g_depth_mode.load(); }

// nvnDeviceSetWindowOriginMode(device, NVNwindowOriginMode) / Get: the replay always draws with NVN's upper-left
// origin (1; a negative-height viewport, nvn_draw.cpp), what Wonder and P5R use. Lower-left (0) would be upside down.
std::atomic<uint32_t> g_origin_mode{1};
void nvnDeviceSetWindowOriginMode(CPU& c) {
    uint32_t mode = uint32_t(c.x[1]);
    g_origin_mode = mode;
    if (mode != 1) log("nvn: window origin mode %u: only upper-left (1) is drawn right", mode);
}
void nvnDeviceGetWindowOriginMode(CPU& c) { c.x[0] = g_origin_mode.load(); }

}  // namespace

namespace nvn {
bool clip_z_minus_one() { return g_depth_mode.load(std::memory_order_relaxed) == 0; }
}  // namespace nvn

NX_HLE("nvnDeviceGetInteger", nvnDeviceGetInteger);
NX_HLE("nvnDeviceRegisterFastClearColor", nvnDeviceRegisterFastClear);
NX_HLE("nvnDeviceRegisterFastClearColori", nvnDeviceRegisterFastClear);
NX_HLE("nvnDeviceRegisterFastClearColorui", nvnDeviceRegisterFastClear);
NX_HLE("nvnDeviceRegisterFastClearDepth", nvnDeviceRegisterFastClear);
NX_HLE("nvnDeviceSetDepthMode", nvnDeviceSetDepthMode);
NX_HLE("nvnDeviceGetDepthMode", nvnDeviceGetDepthMode);
NX_HLE("nvnDeviceSetWindowOriginMode", nvnDeviceSetWindowOriginMode);
NX_HLE("nvnDeviceGetWindowOriginMode", nvnDeviceGetWindowOriginMode);

}  // namespace nx
