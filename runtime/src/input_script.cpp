// Scripted input for unattended runs: NX_INPUT_SCRIPT="<seconds>:<button>:<hold seconds>;..." presses buttons at
// fixed times after the first pad read, e.g. "70:A:0.2;74:A:0.2;80:RIGHT:5" (buttons: A B X Y L R ZL ZR PLUS MINUS
// LEFT RIGHT UP DOWN; the directions also push the left stick). Added on top of the real pad and keyboard.
//
// NX_INPUT_RECORD=1 (or a file path): what the player does is written in that same format, to
// <data root>/input_record.txt: every press of a button, and the stick past half way as a direction, from the first
// pad read. Played back through NX_INPUT_SCRIPT, a test run follows a real player's run instead of a blind script.
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "platform.h"

namespace nx::plat {

namespace {

struct Press {
    double at = 0, hold = 0;
    uint64_t bits = 0;
    int lx = 0, ly = 0;
};

std::vector<Press> parse(const char* s) {
    struct Name {
        const char* name;
        uint64_t bits;
        int lx, ly;
    };
    static const Name names[] = {
        {"A", kA, 0, 0},          {"B", kB, 0, 0},          {"X", kX, 0, 0},          {"Y", kY, 0, 0},
        {"L", kL, 0, 0},          {"R", kR, 0, 0},          {"ZL", kZL, 0, 0},        {"ZR", kZR, 0, 0},
        {"PLUS", kPlus, 0, 0},    {"MINUS", kMinus, 0, 0},  {"LEFT", kLeft, -kStickMax, 0},
        {"RIGHT", kRight, kStickMax, 0}, {"UP", kUp, 0, kStickMax}, {"DOWN", kDown, 0, -kStickMax},
    };
    std::vector<Press> v;
    std::string all = s;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(';', pos);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        size_t a = item.find(':'), b = item.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        Press p;
        p.at = std::atof(item.substr(0, a).c_str());
        p.hold = std::atof(item.substr(b + 1).c_str());
        std::string button = item.substr(a + 1, b - a - 1);
        for (const Name& n : names)
            if (button == n.name) p.bits = n.bits, p.lx = n.lx, p.ly = n.ly;
        if (p.bits) v.push_back(p);
    }
    return v;
}

// The recorder: per button, when it went down; a release writes "<down>:<name>:<held>;" (flushed at once, so a
// killed app keeps what was played).
void record(const PadState& p, double t) {
    if (!kDiagnostics) return;  // NX_RELEASE
    static const char* names[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "RIGHT", "UP", "DOWN"};
    static const uint64_t bits[] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kLeft, kRight, kUp, kDown};
    static double down[14];
    static bool init = false;
    static FILE* f = nullptr;
    if (!init) {
        init = true;
        for (double& d : down) d = -1;
        const char* e = std::getenv("NX_INPUT_RECORD");
        std::string path = e && std::strcmp(e, "1") != 0 ? std::string(e) : data_root() + "/input_record.txt";
        f = fopen_utf8(path, "w");
    }
    if (!f) return;
    constexpr int32_t half = kStickMax / 2;
    uint64_t now = p.buttons;
    if (p.lx < -half) now |= kLeft;
    if (p.lx > half) now |= kRight;
    if (p.ly > half) now |= kUp;
    if (p.ly < -half) now |= kDown;
    for (int k = 0; k < 14; k++) {
        bool held = now & bits[k];
        if (held && down[k] < 0) down[k] = t;
        if (!held && down[k] >= 0) {
            std::fprintf(f, "%.2f:%s:%.2f;", down[k], names[k], std::max(0.05, t - down[k]));
            std::fflush(f);
            down[k] = -1;
        }
    }
}

}  // namespace

void input_script(PadState& p) {
    static const char* env = std::getenv("NX_INPUT_SCRIPT");
    static const bool recording = std::getenv("NX_INPUT_RECORD") != nullptr;
    static const uint64_t start = monotonic_ns();
    double t = double(monotonic_ns() - start) / 1e9;
    if (recording) record(p, t);  // what the player did, before the script adds to it
    static std::vector<Press> script = env ? parse(env) : std::vector<Press>{};
    // Live presses: a file "press" in the data folder, in the script's format with times from now ("0:A:0.3", or
    // "0:DOWN:0.2;1:A:0.2"), is played and deleted. A test driver can then wait for a screen before each press,
    // where fixed times drift (the network notice comes up at a different time on every boot).
    static double next_check = 0;
    if (kDiagnostics && t >= next_check) {  // not in NX_RELEASE builds
        next_check = t + 0.25;
        std::string path = data_root() + "/press";
        if (FILE* f = fopen_utf8(path, "rb")) {
            char buf[512] = {};
            size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            std::remove(path.c_str());
            buf[n] = 0;
            for (Press s : parse(buf)) s.at += t, script.push_back(s);
        }
    }
    for (const Press& s : script)
        if (t >= s.at && t < s.at + s.hold) {
            p.buttons |= s.bits;
            if (s.lx) p.lx = s.lx;
            if (s.ly) p.ly = s.ly;
        }
}

}  // namespace nx::plat
