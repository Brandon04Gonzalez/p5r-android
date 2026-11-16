// Persona 5 Royal (01005CA01580E000, 1.0.0, BID 708BCACA873EA32B): what the shared runtime needs to know about the
// game, and what only P5R needs. The game draws through NVN (Atlus' GFD engine); its shaders live in the GFD shader
// caches BASE/GFDNXHHPRESET*.GSC of ALL_USEU.CPK (tools/gsc_shaders.py). Notes: games/p5r/PROGRESO.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace nvn {
uint64_t presents_recorded();  // runtime nvn_state.cpp: nvnQueuePresentTexture calls so far
void linear_write_back_pause(bool paused);  // runtime nvn_draw.cpp: no copies of linear targets back to memory
}

const game::Info game::kInfo = {"p5r", "Persona 5 Royal", 0x01005CA01580E000ull, "io.github.p5rrecomp",
                                "private/p5r", 0 /* main.npdm: no system resource (SDK 13) */,
                                "708BCACA873EA32B" /* 1.0.0, the base game without the update */};

namespace {

// glslc, NVN's run-time GLSL compiler, is linked against but the game ships its shaders precompiled (the GSC
// caches). Initialize fails, so nothing is ever compiled.
void glslc_fail(CPU& c) {
    log("p5r: %s called: run-time shader compilation is not supported", hle_current_name());
    c.x[0] = 0;
}

void glslc_nop(CPU& c) { c.x[0] = 0; }

// The engine's assertion handler (main+1028d70, "needle" Rsdx and friends) formats its message and fputs it to
// stderr right before it traps; the SDK's stdio goes nowhere. Copy every fputs line into the log, then do the real
// one.
void fputs_logged(CPU& c) {
    static const uint64_t real = find_symbol("fputs");
    const char* s = reinterpret_cast<const char*>(c.x[0]);
    if (s) {
        std::string line(s, strnlen(s, 1024));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        log("[guest fputs] %s", line.c_str());
    }
    if (real) call(c, real);
    else c.x[0] = 0;
}

// The frame rate. The game's target frame rate is [[main+0x2209f50]+0x84]: 30 on the Switch, read through main+f73730
// all over the game and set by main+f73740(fps), which also resets the engine's frame timer ([[main+0x220a9c0]]).
// Every frame the game's main thread (main+fd4140) sets the engine's frame interval to 60 / it through a virtual setter
// (main+1048ee0: vsyncs per frame, 1..4, stored at obj+0x30), which the present (main+1025c9c) hands to
// nvnWindowSetPresentInterval. The launcher's 60 fps (NX_FPS_CAP=60) raises the cap to 60 from that setter: on the PC
// the game then ran at 45-60 fps instead of 30. (The Atmosphere cheat that ships with the dump writes
// [[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14, a chain that is empty in this port.)
//
// Most of the game steps 1 / target per frame, so under the target it runs slow: at ~10 fps the ATLUS logos took 35 s
// instead of 15 (with a target of 10, 17 s). On the Adreno 610 (~8-13 fps in the 3D title and the casino's cutscenes)
// Joker's fall to the title menu and the police corner played at 30-45 % speed while their music and voices kept real
// time: the sound "ahead". A target that follows the device's frame rate (paced_target) keeps real time, but the game
// is made for steps of 1/30 and 1/60 s: with targets of 19-21 the casino battle where Violet joins never reached its
// command menu (shadows idle, buttons dead; no thread blocked, the battle's logic waited for good), twice. (Running
// the task update more than once per frame drawn instead was worse: PROGRESO 62.) So the target follows the device
// only in scenes (scan_tasks): while the engine runs TITLE_DRAW (the 3D title: Joker's fall to the menu) or event_proc
// (the 3D cutscenes, EVT), and no battle task; the floor there is NX_P5R_SCENE_FPS (10). Everywhere else it stays
// between NX_P5R_MIN_FPS (30 by default: the Switch's own steps) and the cap, which only moves it with the launcher's
// 60 fps (30..60). The casino's scripted field scenes (D00_*_START, with EventSkip and General_* inside, no event_proc) played at
// ~52 % speed there (5.2 s of steps per 10 s at 15 fps: the music ahead, the user's "ambush"). They move Joker to points
// (covers), the kind of wait that hung the battle with targets of 19-21, so there the target only takes the Switch's
// own step and its multiples, the cap, a half and a third of it (30, 15, 10: steps of 1, 2 or 3 thirtieths), mixed
// frame by frame so they add up to real time (paced_target). NX_P5R_FIELD_SCENES=0: they stay at 30 as before.
// The present interval stays the cap's. A file named p5r_fps in the
// data folder holding a number overrides the floor everywhere while it exists (read every ~2 s; to test).
// NX_P5R_AUTO_FPS=0: the target is the cap. NX_P5R_FPS=<5..60>: a fixed target with its own interval, to study.
// NX_P5R_TRACE_INTERVAL=1: the setter's calls are logged.
uint64_t g_interval_setter = 0;  // the original setter (call(c, it))
uint64_t g_main_base = 0;
constexpr double kTicks = 19.2e6;  // the system tick the engine times its frames with

int target_fps() {
    uint64_t p = ld<uint64_t>(g_main_base + 0x2209f50);
    return p ? int(ld<uint32_t>(p + 0x84)) : 0;
}

void set_target_fps(CPU& c, int fps) {
    CPU saved = c;
    c.x[0] = uint32_t(fps);
    call(c, g_main_base + 0xf73740);
    c = saved;
}

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

int forced_fps() {
    static const int fps = [] {
        const int v = env_int("NX_P5R_FPS", 0);
        return v >= 5 && v <= 60 ? v : 0;
    }();
    return fps;
}

int cap_fps() {
    static const int fps = env_int("NX_FPS_CAP", 30) >= 60 ? 60 : 30;
    return fps;
}

bool auto_fps() {
    static const bool on = env_int("NX_P5R_AUTO_FPS", 1) != 0;
    return on;
}

bool g_scene = false;  // scan_tasks: the title or a cutscene is running, and no battle
bool g_field_scene = false;  // scan_tasks: a scripted field scene (EventSkip, no event_proc, no battle): steps only

// The lowest target: in scenes NX_P5R_SCENE_FPS (10), in field scenes a third of the cap, elsewhere NX_P5R_MIN_FPS
// (30); the number in the data folder's
// p5r_fps file overrides both (looked at every 64 calls).
int min_fps() {
    static const int base = std::clamp(env_int("NX_P5R_MIN_FPS", 30), 5, cap_fps());
    static const int scene = std::clamp(env_int("NX_P5R_SCENE_FPS", 10), 5, cap_fps());
    static int live = 0, calls = 0;
    if (calls++ % 64 == 0) {
        int v = 0;
        if (FILE* f = plat::kDiagnostics ? plat::fopen_utf8(plat::data_root() + "/p5r_fps", "rb") : nullptr) {
            if (std::fscanf(f, "%d", &v) != 1) v = 0;
            std::fclose(f);
        }
        v = v ? std::clamp(v, 5, cap_fps()) : 0;
        if (v != live) log("p5r: lowest target frame rate %s (%s)", v ? std::to_string(v).c_str() : "back to the default",
                           v ? "p5r_fps file" : "file removed");
        live = v;
    }
    return live ? live : g_scene ? scene : g_field_scene ? cap_fps() / 3 : base;
}

// The target for the frames the device makes, chosen every frame so the game's steps (1 / target each) add up to the
// real time. A debt (real time minus steps; a frame counts at most the slowest step, 1 / min_fps(), so a load's
// hitch is not made up) and the expected frame period (smoothed) give the next step: period + debt / 4, as a whole
// frame rate between min_fps() and the cap. The first version (a half-second window's mean with a dead band)
// left the target 0.5-1 fps over the device's on the tablet: the game ran at 94-97 % of real time at 15-24 fps and the
// sound still drifted ahead through a scene. `period`: the frame that just ended, in ticks; `used`: its target.
int paced_target(uint64_t period, int used) {
    const int cap = cap_fps(), lo = min_fps();
    static double debt = 0, expect = 0;
    static int slow_streak = 0;
    const double raw_s = double(period) / kTicks;
    const bool is_slow = raw_s > 1.25 / double(cap);
    slow_streak = is_slow ? (slow_streak + 1) : 0;
    // Ignore isolated 1-2 frame asset/menu load hitches during field exploration so opening Quick Travel or entering
    // a room does not falsely step the target frame rate from 30 down to 15 fps for a second.
    if (g_field_scene && !g_scene && used >= cap && slow_streak > 0 && slow_streak < 3) {
        return cap;
    }
    const double r = std::min(raw_s, 1.0 / lo);
    expect = expect > 0 ? expect + (r - expect) * 0.25 : r;
    if (used > 0) debt = std::clamp(debt + r - 1.0 / used, -0.15, 0.15);
    if (lo >= cap) debt = 0;  // pinned (outside scenes): a scene does not start by making up for it
    const double step = std::max(expect + debt * 0.25, 1.0 / cap);
    if (g_field_scene && !g_scene) {  // only the cap, a half or a third of it: the nearest step
        const int a = cap, b = cap / 2, c = cap / 3;
        const double s = step * cap;  // in steps of the cap
        return s < 1.5 ? a : s < 2.5 ? b : c;
    }
    return std::clamp(int(1.0 / step + 0.5), lo, cap);
}

// The game clock. The engine's loop (main+fe75ac) times each frame with the system tick, clamps it to the frame
// timer's [[main+0x220a9c0]+0x34] seconds (0.05, set at boot by main+ce39ac) and stores it in [[main+0x2209f50]+0x50]
// (ticks; +0x48 is the frame's start), which main+f730c0 hands to the game's update as seconds. The parts of the game
// that use it run slow under 20 fps whatever the target. With a floor under 30 (the sync experiment) the clamp goes up
// with it, 1.5 / floor (150 ms at 10); NX_P5R_MAX_FRAME_MS=<ms> sets it. The log's `p5r: game clock` says when the game
// ran under real time: by that clock, and by its steps (frames / target). NX_P5R_SLOW_MS=<ms>: every frame sleeps that
// long (a slow phone on the PC). Returns the real length of the frame that just started, in ticks, once per frame (0
// otherwise).
uint64_t last_start = 0, prev_start = 0;  // the engine's last two frame starts ([[main+0x2209f50]+0x48], ticks)

float max_frame_s() {
    static const int ms = env_int("NX_P5R_MAX_FRAME_MS", 0);
    if (ms > 0) return float(std::clamp(ms, 50, 500)) / 1000.0f;
    return std::max(0.05f, 1.5f / float(min_fps()));
}

uint64_t game_clock() {
    if (uint64_t t = ld<uint64_t>(ld<uint64_t>(g_main_base + 0x220a9c0))) {
        uint32_t bits;
        const float want = max_frame_s();
        std::memcpy(&bits, &want, 4);
        if (const uint32_t old = ld<uint32_t>(t + 0x34); old != bits) {
            float was;
            std::memcpy(&was, &old, 4);
            log("p5r: longest frame the game clock follows %.0f -> %.0f ms", double(was) * 1e3, double(want) * 1e3);
            st<uint32_t>(t + 0x34, bits);
        }
    }
    static const int slow_ms = env_int("NX_P5R_SLOW_MS", 0);
    if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));

    // A frame is a new start in +0x48: its real length is the step between starts, the game's is +0x50.
    const uint64_t g = ld<uint64_t>(g_main_base + 0x2209f50);
    if (!g) return 0;
    static uint64_t real = 0, game = 0, frames = 0, cut = 0, longest = 0;
    static double steps = 0;
    static int lowest = 0, highest = 0;
    const uint64_t start = ld<uint64_t>(g + 0x48);
    if (start == last_start) return 0;
    prev_start = last_start;
    uint64_t r = 0;
    if (last_start && start > last_start) {
        const uint64_t d = ld<uint64_t>(g + 0x50);
        r = start - last_start;
        real += r, game += d, frames++;
        if (const int fps = target_fps(); fps > 0) {
            steps += kTicks / fps;
            lowest = lowest ? std::min(lowest, fps) : fps, highest = std::max(highest, fps);
        }
        if (d + 19200 < r) cut++;
        longest = std::max(longest, r);
    }
    last_start = start;
    if (real < 3 * uint64_t(kTicks)) return r;
    // Logged when the game is off real time or the device under the cap (the target then moves).
    const double fps = double(frames) * kTicks / double(real);
    if (game * 100 < real * 97 || std::abs(steps / double(real) - 1.0) > 0.02 || fps < cap_fps() * 0.95)
        log("p5r: game clock %.1f s, steps %.1f s in %.1f s real, %.1f fps (target %d-%d), %llu of %llu frames cut "
            "(longest %.0f ms)", double(game) / kTicks, steps / kTicks, double(real) / kTicks, fps, lowest, highest,
            (unsigned long long)cut, (unsigned long long)frames, double(longest) * 1e3 / kTicks);
    real = game = frames = cut = longest = 0, steps = 0, lowest = highest = 0;
    return r;
}


// The engine's running tasks by name (update list [[main+0x2209f50]+0x2c40], next at +0x80; name at [task+0x18],
// its length in the low half of +0x20). Seen on the tablet (2026-10-23, a trace from the title to Violet's battle):
// TITLE_DRAW on the title, MOVIE_DRAW in films, event_proc (with EventCtrlFade, EventCtrlPostEffect, EventSkip) in the
// 3D cutscenes, General_<field>_<n> with EventSkip in the scripted field scenes, battle (and BTL_<n>_EVENT) in battles.
// Every 4 frames g_scene says whether the target may follow the device (TITLE_DRAW or event_proc, and no battle).
// While a file named trace_tasks exists in the data folder (looked at every ~2 s), every change of the list is logged
// with the wall clock: `p5r: tasks HH:MM:SS (count) +added -removed`.
bool name_is(uint64_t name, uint32_t len, const char* want) {
    const size_t n = std::strlen(want);
    if (len != n || !name) return false;
    for (size_t k = 0; k < n; k++)
        if (char(ld<uint8_t>(name + k)) != want[k]) return false;
    return true;
}

// A scenario event's task: D<nn>_..._START (D00_01_e100_102_START, D00_01_e100_305_af_START; the whole scene, where
// EventSkip and General_* come and go: the cover runs and the wire swing run without them).
bool scenario_event(uint64_t name, uint32_t len) {
    if (!name || len < 10 || len > 63) return false;
    char b[64];
    for (uint32_t k = 0; k < len; k++) b[k] = char(ld<uint8_t>(name + k));
    return b[0] == 'D' && b[1] >= '0' && b[1] <= '9' && b[2] >= '0' && b[2] <= '9' && b[3] == '_' &&
           std::memcmp(b + len - 6, "_START", 6) == 0;
}

// The scenario's own tasks between and around its events: the map's script (General_0150_003_100), which runs through a
// scene's events and the seconds between them, and the talking portraits' lip animation (bustupLipAnim00209600).
bool name_starts(uint64_t name, uint32_t len, const char* want) {
    const size_t n = std::strlen(want);
    if (len < n || !name) return false;
    for (size_t k = 0; k < n; k++)
        if (char(ld<uint8_t>(name + k)) != want[k]) return false;
    return true;
}

static std::string g_current_field_task;

void scan_tasks() {
    static int calls = 0;
    static bool trace = false;
    if (calls % 64 == 0) {
        bool now = false;
        if (const char* env = std::getenv("NX_P5R_TRACE_TASKS"); env && (env[0] == '1' || std::strcmp(env, "true") == 0))
            now = true;
        else if (FILE* f = plat::kDiagnostics ? plat::fopen_utf8(plat::data_root() + "/trace_tasks", "rb") : nullptr)
            now = true, std::fclose(f);
        if (now != trace) log("p5r: task trace %s", now ? "on" : "off");
        trace = now;
    }
    if (calls++ % 4) return;
    const uint64_t g = ld<uint64_t>(g_main_base + 0x2209f50);
    if (!g) return;
    static std::vector<std::string> last;
    std::vector<std::string> cur;
    bool title = false, event = false, battle = false, skip = false, movie = false;
    int n = 0;
    for (uint64_t t = ld<uint64_t>(g + 0x2c40); t && n < 400; t = ld<uint64_t>(t + 0x80), n++) {
        const uint64_t name = ld<uint64_t>(t + 0x18);
        const uint32_t len = ld<uint32_t>(t + 0x20);
        title = title || name_is(name, len, "TITLE_DRAW");
        event = event || name_is(name, len, "event_proc");
        battle = battle || name_is(name, len, "battle");
        skip = skip || name_is(name, len, "EventSkip") || scenario_event(name, len) || name_starts(name, len, "General_") ||
               name_starts(name, len, "bustupLipAnim");
        movie = movie || name_is(name, len, "MOVIE_DRAW");
        if (name_starts(name, len, "General_") || scenario_event(name, len)) {
            std::string s;
            if (name && len && len < 64)
                for (uint32_t k = 0; k < len; k++) s += char(ld<uint8_t>(name + k));
            if (!s.empty() && s != g_current_field_task) {
                g_current_field_task = s;
                log("p5r: active field map script: %s", g_current_field_task.c_str());
            }
        }
        if (!trace) continue;
        std::string str;
        if (name && len && len < 64)
            for (uint32_t k = 0; k < len; k++) str += char(ld<uint8_t>(name + k));
        for (char& ch : str) if (ch == ' ') ch = '_';
        cur.push_back(str.empty() ? "?" : str);
    }
    const bool scene = (title || event) && !battle;
    static const bool field_on = env_int("NX_P5R_FIELD_SCENES", 1) != 0;
    // Restrict discrete step pacing (cap, cap/2, cap/3 = 30, 15, 10) strictly to scripted field scenes (with EventSkip/General_*)
    // so free-roam palace exploration runs at full native 30 fps without artificial 15/10 fps clamping cliffs.
    const bool field = field_on && skip && !event && !title && !battle && !movie;
    if (scene != g_scene)
        log("p5r: scene %s (%s%s%s): the target %s", scene ? "starts" : "ends", title ? "title " : "", event ? "event " : "",
            battle ? "battle" : "", scene ? "follows the device" : "stays at 30 or more");
    if (field != g_field_scene)
        log("p5r: field scene %s: the target %s", field ? "starts" : "ends",
            field ? "follows the device in steps of the cap, a half or a third of it" : "stays at 30 or more");
    g_scene = scene, g_field_scene = field;
    // Films hide the 3D: the depth pyramid the culling reads need not come back to memory meanwhile (runtime).
    static bool paused = false;
    if (movie != paused) {
        paused = movie;
        nvn::linear_write_back_pause(movie);
        log("p5r: film %s: linear render target copies %s", movie ? "starts" : "ends", movie ? "paused" : "resume");
    }
    if (!trace) return;
    std::string added, removed;
    for (const std::string& x : cur) if (std::find(last.begin(), last.end(), x) == last.end()) added += " +" + x;
    for (const std::string& x : last) if (std::find(cur.begin(), cur.end(), x) == cur.end()) removed += " -" + x;
    if (added.empty() && removed.empty()) return;
    const std::time_t wall = std::time(nullptr);
    char hms[16] = "?";
    if (const std::tm* tm = std::localtime(&wall)) std::strftime(hms, sizeof hms, "%H:%M:%S", tm);
    log("p5r: tasks %s (%d)%s%s", hms, n, added.c_str(), removed.c_str());
    last = std::move(cur);
}

// A/V sync (the runtime's av_sync): the logic time of the frame this thread is about to present. The setter runs on
// the render thread right before the present (main+1025c9c); the main thread hands a frame over and starts the next
// one, so the frame being presented is usually the one before the last start. When the last start is old (over 80 % of
// a frame ago: the main thread waited for this one) it is the last.
void frame_origin() {
    if (!last_start) return;
    const uint64_t now = system_tick();
    uint64_t origin = last_start;
    if (prev_start && prev_start < last_start && now > last_start &&
        (now - last_start) * 5 < (last_start - prev_start) * 4)
        origin = prev_start;
    frame_origin_ns(origin * 10000 / 192);  // ticks (19.2 MHz) -> monotonic ns (system_tick())
}

static std::atomic<uint64_t> g_cull_eval{0}, g_cull_hidden{0};

struct MeshBoundsSlot {
    uint64_t mesh = 0;
    uint64_t node = 0;
    uint32_t stamp = 0;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f, radius = 0.0f;
    bool structural = false;
    uint8_t static_streak = 0;
};

void interval_setter_hook(CPU& c) {
    scan_tasks();
    const uint64_t period = game_clock();
    frame_origin();
    const int now = target_fps();
    int fps = forced_fps();
    const int interval = fps ? std::clamp(60 / fps, 1, 4) : 60 / cap_fps();
    if (!fps) fps = !auto_fps() ? cap_fps() : period ? paced_target(period, now) : now;
    if (fps > 0 && now > 0 && now != fps) {
        static int last_logged_fps = 0;
        static uint64_t last_log_ns = 0;
        uint64_t now_ns = plat::monotonic_ns();
        if (fps != last_logged_fps || (now_ns - last_log_ns > 2'000'000'000ull)) {
            log("p5r: target frame rate %d -> %d%s (field=%d scene=%d frame=%.1f ms task=%s)",
                now, fps, auto_fps() && !forced_fps() ? " (paced)" : "",
                g_field_scene, g_scene, double(period) * 1e3 / kTicks,
                g_current_field_task.c_str());
            last_logged_fps = fps;
            last_log_ns = now_ns;
        }
        set_target_fps(c, fps);
    }
    c.x[1] = uint32_t(interval);
    static int traced = 0;
    if (std::getenv("NX_P5R_TRACE_INTERVAL") && traced++ < 40)
        log("p5r: frame interval set to %u on %llx from %s", uint32_t(c.x[1]), (unsigned long long)c.x[0], describe(c.x[30]).c_str());
    call(c, g_interval_setter);

    static uint64_t last_cull_eval = 0, last_cull_hidden = 0;
    static uint64_t cull_log_frame = 0;
    if (++cull_log_frame % 300 == 0) {
        uint64_t ev = g_cull_eval.load(std::memory_order_relaxed);
        uint64_t hd = g_cull_hidden.load(std::memory_order_relaxed);
        uint64_t d_ev = ev - last_cull_eval;
        uint64_t d_hd = hd - last_cull_hidden;
        last_cull_eval = ev;
        last_cull_hidden = hd;
        if (d_ev > 0) {
            static const float s_log_scale = [] {
                const char* e = std::getenv("NX_P5R_DIST_SCALE");
                return e ? std::clamp(float(std::atof(e)), 0.5f, 3.0f) : 1.0f;
            }();
            log("p5r: distance culling: %llu of %llu visible meshes culled (%.1f%%, scale %.2fx) over 300 frames",
                (unsigned long long)d_hd, (unsigned long long)d_ev, 100.0 * double(d_hd) / double(d_ev), s_log_scale);
        }
    }
}

// NX_P5R_CULL_CHECK=1 (PROGRESO 72): P5R's per-model culling job (main+ab0f50(job), job = {model, states}, run by
// the job threads through the table) re-tests each mesh every 8 frames: its bounds (mesh+0x90) are turned into an
// oriented box by main+a92c80 with the mesh's matrix (main+fa9460(mesh[8])), and main+a94870 tests the box against
// the 6 frustum planes ([main+0x22cd268], 32 bytes each: normal, then distance at +0x10); a box outside is hidden
// (bit 14 of mesh+0x20, reason 1 in the low bits of state+0x10). A box inside, for meshes with bit 6 of their state,
// then goes through occlusion (main+ab11d0, reason 2): its screen rectangle against the depth pyramid the GPU drew into
// pitch-linear R32F targets, read from guest memory (main+fdd6e0) - which the runtime never filled until the linear
// render target write-back. Hidden meshes are re-tested every frame. Here: for each mesh that this call newly hid for
// reason 1, the frustum test redone natively from the same box and planes (agreed with the guest code every time), the
// planes before and after the job (never changed during one), how often each model's job runs (every frame), and how
// long meshes stay hidden.
uint64_t g_cull_job = 0;  // the original job

uint64_t guest_fn(CPU& c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0) {
    CPU saved = c;
    c.x[0] = a0, c.x[1] = a1, c.x[2] = a2;
    c.x[31] = ((saved.x[31] - 0x400) & ~uint64_t(15));
    call(c, g_main_base + fn);
    const uint64_t r = c.x[0];
    c = saved;
    return r;
}

struct Planes {
    uint64_t ptr = 0;
    float p[6][4] = {};  // normal, distance
};

Planes read_planes(bool all = false) {
    Planes pl;
    pl.ptr = ld<uint64_t>(g_main_base + 0x22cd268);
    if (!pl.ptr) return pl;
    const int max_k = all ? 6 : 1;
    for (int k = 0; k < max_k; k++)
        for (int j = 0; j < 4; j++) {
            const uint32_t w = ld<uint32_t>(pl.ptr + 32 * k + (j < 3 ? 4 * j : 16));
            std::memcpy(&pl.p[k][j], &w, 4);
        }
    return pl;
}

// The plane that rejects the box (b: centre, 3 axes at 4/8/12, half sizes at 16..18), or -1.
int reject_plane(const Planes& pl, const float* b, float* dist = nullptr, float* radius = nullptr) {
    for (int k = 0; k < 6; k++) {
        const float* p = pl.p[k];
        const float r = b[16] * std::fabs(p[0] * b[4] + p[1] * b[5] + p[2] * b[6]) +
                        b[17] * std::fabs(p[0] * b[8] + p[1] * b[9] + p[2] * b[10]) +
                        b[18] * std::fabs(p[0] * b[12] + p[1] * b[13] + p[2] * b[14]);
        const float d = p[0] * b[0] + p[1] * b[1] + p[2] * b[2] - p[3];
        if (d <= -r) {
            if (dist) *dist = d, *radius = r;
            return k;
        }
    }
    return -1;
}

void cull_job_hook(CPU& c) {
    const uint64_t job = c.x[0];
    if (!job) return;
    const uint64_t model = ld<uint64_t>(job), states = ld<uint64_t>(job + 8);
    const uint64_t sp = c.x[31];
    static const bool trace_cull = std::getenv("NX_P5R_CULL_CHECK") != nullptr;
    const Planes before = trace_cull ? read_planes(true) : Planes{};
    call(c, g_cull_job);
    const Planes after = read_planes(trace_cull);
    const uint64_t list = model ? ld<uint64_t>(model + 0x38) : 0;
    if (!list || !states) return;

    static const bool dist_cull_on = [] {
        const char* e = std::getenv("NX_P5R_DIST_CULL");
        return e && e[0] == '1';
    }();
    static const float dist_cull_scale = [] {
        const char* e = std::getenv("NX_P5R_DIST_SCALE");
        if (e) return std::clamp(float(std::atof(e)), 0.5f, 3.0f);
        return 1.0f;  // Default 1.0x for lowend-r50 proven distance curve
    }();

    if (dist_cull_on && after.ptr) {
        const int n = int(ld<uint32_t>(list + 8));
        const int64_t stride = int32_t(ld<uint32_t>(list + 4)), sstride = int32_t(ld<uint32_t>(states + 4));
        const uint64_t arr = ld<uint64_t>(list + 0x10), sarr = ld<uint64_t>(states + 0x10);
        if (arr && sarr && stride > 0 && sstride > 0) {
            const float* p = after.p[0];  // Near plane normal (p[0], p[1], p[2]) and distance (p[3])
            const uint64_t box = (sp - 0x200) & ~uint64_t(15);
            thread_local MeshBoundsSlot tl_bounds_cache[4096];
            const uint64_t cur_p = nvn::presents_recorded();
            uint64_t last_node = 0;
            uint64_t last_matrix = 0;

            for (int i = 0; i < n && i < 4096; i++) {
                const uint64_t mesh = ld<uint64_t>(arr + stride * i);
                if (!mesh) continue;
                uint32_t flags = ld<uint32_t>(mesh + 0x20);
                if ((flags >> 14) & 1) continue;  // Already hidden by engine frustum/occlusion culling

                const uint64_t node = ld<uint64_t>(mesh + 8);
                const uint64_t local_bounds = mesh + 0x90;
                if (!node || !local_bounds) continue;

                MeshBoundsSlot& slot = tl_bounds_cache[((mesh >> 5) ^ (mesh >> 17)) & 4095];
                const uint32_t shift = (slot.mesh == mesh && slot.node == node && slot.static_streak >= 2) ? 9u : 5u;
                const uint32_t cur_stamp = uint32_t((cur_p + (mesh >> 6)) >> shift) + 1u;
                float b0, b1, b2, radius;
                bool structural = false;
                if (slot.mesh == mesh && slot.node == node && slot.stamp == cur_stamp) {
                    b0 = slot.cx;
                    b1 = slot.cy;
                    b2 = slot.cz;
                    radius = slot.radius;
                    structural = slot.structural;
                } else {
                    CPU scratch = c;
                    scratch.x[31] = sp;
                    uint64_t matrix = (node == last_node) ? last_matrix : guest_fn(scratch, 0xfa9460, node);
                    if (!matrix) continue;
                    last_node = node;
                    last_matrix = matrix;
                    guest_fn(scratch, 0xa92c80, box, local_bounds, matrix);

                    b0 = ld<float>(box);
                    b1 = ld<float>(box + 4);
                    b2 = ld<float>(box + 8);
                    const float r16 = std::fabs(ld<float>(box + 16 * 4));
                    const float r17 = std::fabs(ld<float>(box + 17 * 4));
                    const float r18 = std::fabs(ld<float>(box + 18 * 4));
                    if (!std::isfinite(r16) || !std::isfinite(r17) || !std::isfinite(r18)) continue;
                    radius = std::sqrt(r16 * r16 + r17 * r17 + r18 * r18);
                    const float max_e = std::max({r16, r17, r18});
                    const float min_e = std::min({r16, r17, r18});
                    structural = (max_e >= 6.5f) || (max_e >= 2.5f && max_e > 4.5f * std::max(min_e, 0.01f));
                    uint8_t streak = 0;
                    if (slot.mesh == mesh && slot.node == node &&
                        std::fabs(b0 - slot.cx) < 1e-3f && std::fabs(b1 - slot.cy) < 1e-3f &&
                        std::fabs(b2 - slot.cz) < 1e-3f && std::fabs(radius - slot.radius) < 1e-3f) {
                        streak = std::min<uint8_t>(slot.static_streak + 1, 10);
                    }
                    const uint32_t next_shift = (streak >= 2) ? 9u : 5u;
                    const uint32_t next_stamp = uint32_t((cur_p + (mesh >> 6)) >> next_shift) + 1u;
                    slot = {mesh, node, next_stamp, b0, b1, b2, radius, structural, streak};
                }

                // Signed distance of box center and closest surface point from near plane along camera view axis
                const float view_dist = p[0] * b0 + p[1] * b1 + p[2] * b2 - p[3];
                const float near_dist = view_dist - radius;
                if (!std::isfinite(view_dist) || view_dist <= 10.0f || near_dist <= 7.5f) continue;

                // Protect planar structural geometry (walls, floors, ceilings, road slabs) and massive shells
                const float struct_limit = (near_dist < 18.0f) ? 8.0f : (near_dist < 30.0f ? 14.0f : 18.0f);
                if (structural || radius >= struct_limit) continue;

                g_cull_eval.fetch_add(1, std::memory_order_relaxed);

                const float ratio = radius / view_dist;
                bool cull = false;
                if (view_dist < 20.0f) {
                    if (radius < 0.45f * dist_cull_scale || ratio < 0.025f * dist_cull_scale) cull = true;
                } else if (view_dist < 40.0f) {
                    if (radius < 0.85f * dist_cull_scale || ratio < 0.025f * dist_cull_scale) cull = true;
                } else {
                    if (radius < 2.0f * dist_cull_scale || ratio < 0.030f * dist_cull_scale) cull = true;
                }

                if (cull) {
                    g_cull_hidden.fetch_add(1, std::memory_order_relaxed);
                    flags |= (1u << 14);
                    st<uint32_t>(mesh + 0x20, flags);
                    st<uint8_t>(sarr + sstride * i + 0x10, 1);
                }
            }
        }
    }

    if (!trace_cull) return;

    static std::mutex lock;
    static std::unordered_map<uint64_t, bool> hidden_before;
    static std::unordered_map<uint64_t, uint64_t> hidden_since;  // mesh -> frame it was hidden
    struct ModelRuns {
        uint64_t last = 0, runs = 0, max_gap = 0;
    };
    static std::unordered_map<uint64_t, ModelRuns> model_runs;
    static uint64_t next_stats = 0;
    static int logged = 0;
    std::lock_guard lk(lock);
    const uint64_t frame = nvn::presents_recorded();
    ModelRuns& mr = model_runs[model];
    const uint64_t gap = mr.last ? frame - mr.last : 0;
    if (mr.last) mr.max_gap = std::max(mr.max_gap, gap);
    mr.last = frame, mr.runs++;
    if (frame >= next_stats) {  // how often each model's job runs: those that skipped frames
        next_stats = frame + 300;
        int slow = 0;
        std::string list;
        for (auto& [m, r] : model_runs)
            if (r.max_gap > 1 && slow++ < 12) {
                char b[80];
                std::snprintf(b, sizeof b, " %llx:%llu runs, max gap %llu", (unsigned long long)m,
                              (unsigned long long)r.runs, (unsigned long long)r.max_gap);
                list += b;
            }
        log("p5r cull jobs: frame %llu, %zu models, %d with gaps:%s", (unsigned long long)frame, model_runs.size(), slow,
            list.c_str());
        for (auto& [m, r] : model_runs) r.runs = r.max_gap = 0;
    }
    const int n = int(ld<uint32_t>(list + 8));
    const int64_t stride = int32_t(ld<uint32_t>(list + 4)), sstride = int32_t(ld<uint32_t>(states + 4));
    const uint64_t arr = ld<uint64_t>(list + 0x10), sarr = ld<uint64_t>(states + 0x10);
    int hides = 0;
    for (int i = 0; i < n && i < 4096; i++) {
        const uint64_t mesh = ld<uint64_t>(arr + stride * i);
        if (!mesh) continue;
        const bool hidden = ld<uint32_t>(mesh + 0x20) >> 14 & 1;
        const uint32_t reason = ld<uint8_t>(sarr + sstride * i + 0x10) & 3;
        bool& was = hidden_before[mesh];
        const bool newly = hidden && !was;
        if (!hidden && was && frame >= 600 && logged <= 6000) {  // shown again: how long it stayed hidden
            const uint64_t since = hidden_since[mesh];
            if (since && frame - since >= 4) {
                logged++;
                log("p5r cull: frame %llu model %llx mesh %llx shown after %llu frames hidden (job gap %llu)",
                    (unsigned long long)frame, (unsigned long long)model, (unsigned long long)mesh,
                    (unsigned long long)(frame - since), (unsigned long long)gap);
            }
        }
        if (newly) hidden_since[mesh] = frame;
        was = hidden;
        if (!newly || reason != 1 || frame < 600 || logged > 6000) continue;
        // The box, through the game's own code, into a scratch area below the job's stack.
        const uint64_t box = (sp - 0x200) & ~uint64_t(15);
        CPU scratch = c;
        scratch.x[31] = sp;
        const uint64_t matrix = guest_fn(scratch, 0xfa9460, ld<uint64_t>(mesh + 8));
        guest_fn(scratch, 0xa92c80, box, mesh + 0x90, matrix);
        float b[19];
        for (int k = 0; k < 19; k++) {
            const uint32_t w = ld<uint32_t>(box + 4 * k);
            std::memcpy(&b[k], &w, 4);
        }
        float d = 0, r = 0;
        const int kb = reject_plane(before, b, &d, &r), ka = reject_plane(after, b);
        logged++, hides++;
        log("p5r cull: frame %llu model %llx mesh %llx: plane %d (after the job: %d) dist %g radius %g centre %g %g %g "
            "half %g %g %g", (unsigned long long)frame, (unsigned long long)model, (unsigned long long)mesh, kb, ka,
            d, r, b[0], b[1], b[2], b[16], b[17], b[18]);
    }
    if (hides) {
        bool same = before.ptr == after.ptr;
        for (int k = 0; k < 6; k++)
            for (int j = 0; j < 4; j++) same = same && before.p[k][j] == after.p[k][j];
        const auto& p = before.p;
        log("p5r cull planes: frame %llu, %d hidden; planes at %llx%s: near %g %g %g %g far %g %g %g %g | %g %g %g %g "
            "| %g %g %g %g | %g %g %g %g | %g %g %g %g", (unsigned long long)frame, hides, (unsigned long long)before.ptr,
            same ? "" : " (CHANGED during the job)", p[0][0], p[0][1], p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3],
            p[2][0], p[2][1], p[2][2], p[2][3], p[3][0], p[3][1], p[3][2], p[3][3], p[4][0], p[4][1], p[4][2], p[4][3],
            p[5][0], p[5][1], p[5][2], p[5][3]);
    }
}

}  // namespace

NX_HLE("fputs", fputs_logged);

NX_HLE("glslcInitialize", glslc_fail);
NX_HLE("glslcCompile", glslc_fail);
NX_HLE("glslcCompilePreSpecialized", glslc_fail);
NX_HLE("glslcCompileSpecialized", glslc_fail);
NX_HLE("glslcFinalize", glslc_nop);
NX_HLE("glslcSetAllocator", glslc_nop);
NX_HLE("glslcGetDefaultOptions", glslc_nop);
NX_HLE("glslcGetVersion", glslc_nop);

void game::after_init() {
    game::renderer_start();
    // P5R never binds a storage image: colour images without STORAGE keep Adreno's framebuffer compression
    // (nvn_image.cpp). config.txt's NX_IMAGE_STORAGE=1 brings it back.
    // In handheld mode (720p) the game draws its 3D at 960x540 and its post pass, HUD and menus into a 1280x720 target
    // it copies to its 1280x720 window textures; the HUD's art is made for 1080p (vertices in 1920x1080 units), so
    // shrunk to 720p its fine lines turned to specks (PROGRESO 56). NX_HUD_1080 makes those 1280x720 targets at
    // 1920x1080 (runtime, nvn_vk.h): the HUD and menus reach the screen at their own size, the 3D keeps 960x540.
    // Off by default since PROGRESO 78: on the Adreno 610 it cost ~4.5 ms of GPU a frame (casino 17.8 against 19.5
    // fps). The launcher's "Interfaz" option (config.txt NX_HUD_1080=1) turns it on; docked (1080p) has no such
    // targets.
#if defined(_WIN32)
    if (!std::getenv("NX_IMAGE_STORAGE")) _putenv_s("NX_IMAGE_STORAGE", "0");
    if (!std::getenv("NX_HUD_1080")) _putenv_s("NX_HUD_1080", "0");
    if (!std::getenv("NX_GAME_LANGUAGES")) _putenv_s("NX_GAME_LANGUAGES", "ja en-us fr de it es ko zh-hans zh-hant");
#else
    setenv("NX_IMAGE_STORAGE", "0", 0);
    setenv("NX_HUD_1080", "0", 0);
    // The text languages P5R has (runtime language.h): a device in another language gets English, not Japanese.
    setenv("NX_GAME_LANGUAGES", "ja en-us fr de it es ko zh-hans zh-hant", 0);
    // The HUD pass that darkens partly transparent HUD pixels takes alpha >= 0.9 as opaque: the BC3 art's alphas of
    // 246..254 inside the shapes made dark specks on the battle portraits (runtime nvn_spirv_patch.cpp, PROGRESO 70).
    setenv("NX_OPAQUE_FS", "21835d19dd86fcec:0.9", 0);
    // Its plain alpha-blended HUD art (textured draws only) leaves the HUD target's alpha alone (runtime nvn_draw.cpp,
    // PROGRESO 70-71): the faces' translucent layers came out grey with the threshold alone.
    setenv("NX_HUD_KEEP_ALPHA", "1", 0);
#endif
    const LoadedModule* m = module_at(find_symbol("nnMain"));
    if (m && (g_interval_setter = hook_function(m->base + 0x1048ee0, interval_setter_hook))) {
        g_main_base = m->base;
        const char* dc = std::getenv("NX_P5R_DIST_CULL");
        if ((std::getenv("NX_P5R_CULL_CHECK") || (dc && dc[0] == '1')) &&
            !(g_cull_job = hook_function(m->base + 0xab0f50, cull_job_hook)))
            log("p5r: the culling job could not be hooked");
    } else {
        log("p5r: nnMain or the frame interval setter not found: the game's own 30 fps");
    }
}
static constexpr const char* kPerfBuildTag = "universal-nvn-opt";

void game::renderer_start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    log("p5r perf build: %s (65K linear-probing dedup_upload, flat g_seen_slots + 64KB page churn filter, idle condition_variable movie pump)",
        kPerfBuildTag);
#if defined(__ANDROID__)
    // Universal Android defaults:
    // - Anisotropic filtering cap defaults in nvn_sampler.cpp (1x on Mali, 2x on Adreno/others unless overridden by NX_MAX_ANISO).
    // - Enable per-image deferred clears (PROGRESO 77) so P5R's depth-only clears and dual
    //   shadow-map clears fold into VK_ATTACHMENT_LOAD_OP_CLEAR instead of 12,000+ standalone
    //   vkCmdClear*Image transfer clears.
    setenv("NX_CLEARS_V2", "1", 0);
    // - Skip redundant CPU write-backs of GPU compute skinning SSBOs (s.write_backs):
    //   Submit 2 vertex shaders read Submit 1's compute-skinned buffers directly on the GPU
    //   via g_regions (nvn_written.cpp), while CPU occlusion culling uses s.late_write_backs.
    //   Saves ~3.0 ms/frame of r.before snapshot + write-combined readback on CPU.
    setenv("NX_EXP_NO_WB", "1", 0);
    // - NOTE: Keep default NX_SUBMIT_CHUNK_DRAWS=150 and frame_plan_split active on Mali!
    //   Queuing 150-draw chunks at render-pass boundaries lets the Mali-G57 GPU execute
    //   90%+ of the frame in parallel with the 25 ms CPU replay, keeping gpu_present() at
    //   ~1.1 ms instead of stalling for ~13.7 ms waiting on an unchunked command buffer.
    // - NOTE: Do NOT enable NX_MALI_FP16 globally in P5R! P5R shaders use packed bitfields,
    //   integer-as-float encodings, and high-range lighting/skinning math in fragment shaders
    //   that overflow IEEE754 fp16 (max 65504), causing severe black/corrupted screen glitches.
    // - Do NOT force-clear color attachments on every new render pass without an explicit
    //   NVN clear command; this avoids wiping UI/compositor targets updated across passes.
    setenv("NX_LOAD_CLEAR", "0", 0);
#endif
}
void game::first_frame() {}

}  // namespace nx
