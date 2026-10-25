# Wonder on Android: measurements (Galaxy S23)

Device: Galaxy S23 SM-S911B. Snapdragon 8 Gen 2, Adreno 740, Vulkan 1.3, 8 GB of RAM (6.9 GB visible), Android 16.
Handheld mode (`NX_HANDHELD=1`: the game renders for 1280x720). Build: branch `feat/android-wonder` after the
Windows merge (DEBUG-2.md #89). The studies that build on these numbers are in this folder.

## How they are taken

| Tool | What |
|---|---|
| `tools/wonder_phone_course.sh <serial> <name> [NX_X=Y ...]` | scripted course 1-1 run on the phone. It writes `config.txt` with the timed inputs, dumps a frame every 600 and samples memory every 20 s, takes a 10 s CPU profile at 220 s, and pulls everything into `private/wonder/phone_<name>/` |
| `REPLAY=<file>` (with the script above) | replays a recorded play instead of the blind script |
| `NX_INPUT_RECORD=1` (`config.txt`) | records what the player does into `input_record.txt`, in `NX_INPUT_SCRIPT` format (`runtime/src/input_script.cpp`) |
| `tools/wonder_replay_script.py <input_record.txt>` | the scripted menus, then the recorded course inputs shifted to when the run enters the course |
| `tools/wonder_phone_memwatch.sh <serial> <out>` | memory from `dumpsys meminfo` every 20 s while the app runs: total PSS, the Graphics part (GPU), native heap, the phone's MemAvailable |
| `tools/android_profile_report.py <profile.txt>` | samples per thread, libraries, hottest functions of our code, symbolized against `private/wonder/ba/libwonder.so` (the build that ran) |
| `NX_GPU_PROFILE=1` | GPU time per render pass (timestamps), top labels every 60 frames |

## Results (2026-10-01)

| Run | Mode | fps (whole run) | Course only | Replay (CPU) | Fence wait | Notes |
|---|---|---|---|---|---|---|
| user play, 2026-09-30 (old APK) | pipelined | 37.1 mean | - | 18-26 ms in course | 1-8 ms | first run: every pipeline compiled (43-47 ms spikes) |
| `sync1` | synchronous (default off macOS since #76) | 30.4 mean, 53% of intervals < 30 | - | - | - | blind script; cold pipeline cache |
| `phone_rec` (user playing, recorded) | pipelined | 53.0 mean, 11% < 30 | - | - | - | the user's own run |
| `pipe1` (the user's run replayed) | pipelined | 43.8 mean | **32.0** | **19.6 ms** | 5.3 ms | pipeline cache warm (0.5 ms per pipeline) |
| `gpu1` (same, GPU profile on) | pipelined | 32.4 mean | - | - | - | **GPU 17.0 ms/frame**; one 960x540 pass 5.1 ms |
| `chunk1` (PLAN.md R1: parts every 150 draws) | pipelined | 45.0 mean | 30 (interval 2) | 21 ms | **1.0 ms** | submit thread ~30 → ~25 ms; 2 early parts + the split per frame |
| `sets1` (+ R3 set cache, R4 masks/scan) | pipelined | 45.9 mean | 30 (interval 2) | 18-19 ms | 1.5-2.7 ms | sets reused 36% |
| `ring1` (+ `NX_RING_CACHED=1`) | pipelined | 48.8 mean | 30 (interval 2) | 19 ms | 1-2 ms | no gain: the ring stays write-combined |
| `dyn1` (+ dynamic uniform offsets, Android default) | pipelined | 46.8 mean | 30 (interval 2) | 17.5-19.8 ms | 2-3 ms | sets reused 82%; images correct |
| `rt1` (+ `NX_REPLAY_THREAD=1`) | pipelined | 47.3 mean, 12 < 30 | 30 (interval 2) | 18.7-20.9 ms | 0.5-1.6 ms | no clear gain |
| `max1` (dyn1 + `NX_MAX_INTERVAL=1`) | pipelined | 53.1 mean, 1 < 30 | **48-50** | **16-17 ms** | 0.2-1.1 ms | the 30-fps lock off: CPU clocks stay up; 77% of present intervals 14-19 ms; the game runs ~17% slow when a frame is late |
| `watch1` (+ write watches; lock off) | pipelined | 54.4 mean | 45-49 | 16-17.6 ms | 0.5 ms | no hash spike every 4th frame |
| `heap1` (+ heap zero-fill off, delayed page release; lock off) | pipelined | 53.2 mean | 42-47 | 15.8-18.8 ms | 0.2-1.1 ms | **518/600 present intervals 14-19 ms** (best so far); 41 over 50 ms |
| `gpu2` (heap1 + `NX_GPU_PROFILE=1`) | pipelined | - | - | - | - | **GPU 18.7 ms/frame at the higher load**: two 960x540 passes 2.8 + 2.4 ms |
| `c2` (+ PHASE3 C2: a command pool per slot; lock off) | pipelined | 54.0 mean | 47.6-50.4 | **12.5-14.5 ms** | 1.3-2.4 ms | other 1.2 → 2.3 ms (the pool reset): net ~-2 ms. Default on |
| `c1` (c2 + C1 `NX_RENDER_PASS_OBJECTS=1`) | pipelined | 53.8 mean | 42-50 | 13.6-15.3 ms | 2-4.5 ms | no gain on Adreno, the GPU waits rose; thermal headroom falling over runs. Off by default |
| `spin1` (c2 + `NX_YIELD=spin`) | pipelined | 52.6 mean | 39-45 | 14.6-16.2 ms | 2-3.4 ms | worse: the game's yields keep sched_yield |
| `aff1` (R1 + `NX_AFFINITY=1`) | pipelined | 47.1 mean | 30 (interval 2) | 20 ms | 2.5 ms | replay thread 600/600 on the prime core: only ~1 ms less, so the replay is work, not clocks |

Conclusions so far:
- Pipelined submission is worth ~20 fps on the phone over synchronous. Whether it can be the Android default
  depends on the exploded-mesh cause (`PLAN.md`, sections 3 and 7).
- In the course both sides are over the 16.7 ms frame: the CPU replay at ~19.6 ms (the M1 does it in ~6) and the
  GPU at ~17 ms.
- On the replay thread (`phone_pipe1/profile_report.txt`): our code takes 38% of the samples, libc 34%
  (copies, allocations), the Adreno driver 26%. The hottest places are `draw` (`vkCmd*` into the driver),
  `replay_list`, `clears_flush` and map lookups.
- Adreno compiles a new pipeline in ~15-20 ms (149 pipelines took 3.1 s in one window of the first run), which
  makes the on-disk pipeline cache (DEBUG-2 #73) essential: warm, 0.5 ms each.

Memory (`dumpsys meminfo`, MB):

| Run | App total (peak) | Graphics (GPU) | Phone available |
|---|---|---|---|
| `sync1` | 3,280 | 1,066 | ~950-1,230 |
| `phone_rec` | 3,640 | 1,168 | ~1,000 |
| `pipe1` | 3,695 | 1,207 | ~930-1,400 |

The runtime's own guard (Pss from `smaps_rollup`) reported only 0.5-1.2 GB: it does not see most of it
(`MEMORY.md`).

Graphics bugs reported by the user on the phone:
- the background's glow flickers from frame to frame;
- other graphics bugs, not yet localised.

The sampled frames of `pipe1` look clean: the flicker is temporal and needs frame bursts
(`PLAN.md`, sections 3 and 7).

## Phase 0 findings (2026-10-01, `chunk1`/`aff1`)

- **Device:**
  - CPUs: 3 x 2016 MHz (A510), 4 x 2803 MHz (A715/A710), 1 x 3360 MHz (X3).
  - Extensions present: `VK_KHR_push_descriptor`, `VK_KHR_descriptor_update_template`, `VK_EXT_robustness2`,
    `VK_EXT_extended_dynamic_state{,2}`, `VK_GOOGLE_display_timing`, `VK_QCOM_render_pass_store_ops`,
    `VK_EXT_load_store_op_none`.
  - Missing: `VK_EXT_graphics_pipeline_library`, `VK_EXT_descriptor_buffer`, `VK_EXT_extended_dynamic_state3`,
    `VK_KHR_present_wait`.
  - Memory types include host-cached ones (`DV$`, `DVC$`). These are R5's readback memory.
- **Threads, without affinity:**
  - The replay + present thread ran on the 2.8 GHz cores 552 of 600 ticks and on the prime core 47.
  - The game's main thread ran on the 2.8 GHz cores, busy only 21-27% of the wall clock: it waits on the replay.
- **Pacing in course:** 600 of 600 presents at interval 2. Present intervals cluster at 25-50 ms.
- **ADPF** now has 2 threads (the game's main thread joined).
- **Conclusion:** the replay is ~20 ms of CPU work per frame whatever the core. Phase 2 (R3/R4, less work per draw) is
  what brings 60 fps; R1 is kept on by default on Android.

## Phase 2 notes (2026-10-01)

- **Dynamic uniform offsets** (`ProgramLayout::dynamic`, nvn_pipeline.cpp) work on Adreno. On the Mac (MoltenVK) every
  frame came out black and the GPU hung near the world map, even with the set cache off and without compute. The
  offsets logged were right: `UNIFORM_BUFFER_DYNAMIC`, 256-aligned, binding 0 written by both stages. The suspect is
  MoltenVK with a dynamic binding shared by two stages. Default on Android only; `NX_DYNAMIC_UBO=1` forces it
  elsewhere.
- **At 30 fps the replay looks ~2-3 ms slower than it is.** The replay thread is idle half the time, and the clocks
  drop. `max1`, with the lock off, gives the cost at full clocks: ~16-17 ms.

## State after phase 2 work (2026-10-01, `heap1`/`gpu2`)

- **CPU** (submit thread, full clocks): replay ~16-17 ms + other ~1.2 + encode 0.6 + present 0.4 → ~18.5 ms, about
  54 fps.
- **GPU:** 18.7 ms/frame. It is a blocker as much as the CPU; Phase 3 (G1-G4) is needed.
- **Profiler caveat:** when the sampled PC is a leaf in libc or the Adreno driver, the frame-pointer walk skips its
  caller. "First libwonder frame" then names the caller's caller. ~26% of the replay thread's samples sat under
  `begin_rendering` this way: the driver's per-pass allocation and memset in `vkCmdBeginRendering`.
- **Memory** (heap1, course): app total PSS 3.29 GB, Graphics 0.97 GB, swap PSS 1.9 GB, MemAvailable ~1.0-1.1 GB.
- **Temperature** after the 5-minute run: AP (SoC) 52.6 °C, skin and battery 39.9 °C, thermal status LIGHT,
  headroom 0.87 (throttling from 1.0).

## Hitches (PHASE3 I1, `c2`)

- **The study's hypothesis is out.** No `hitch:` line (main-thread frames over 33 ms) waited on the replay: SyncWait,
  QueueFinish, pool invalidate and finalize all show 0.0 ms blocked.
- **The hitches are periodic:** every 14-16 frames in course, 45-60 ms on the game's main thread.
- **Per 600 frames:**
  - main thread: ~31,000 voluntary context switches (~52 per frame), 35-46 major page faults;
  - replay thread: ~41,000 voluntary context switches and ~48,000 minor faults (~80 per frame).
- **Still open:** what the main thread waits on every ~15 frames. The next step is to instrument the kernel's waits
  (svc WaitSynchronization / ArbitrateLock) per frame.
- **Measurement hygiene:** thermal headroom drops from 0.87 to 0.83 over back-to-back runs. A/B runs need a pause
  between them, and the headroom must be logged with each result.

## Phase 4 (2026-10-01; PHASE4.md)

From here every run uses the new protocol:
- `tools/phone_thermal_wait.py` gates the start: battery ≤ 33 °C, skin ≤ 34 °C, thermal status 0.
- `MEMWATCH=0`: no dumpsys stalls.
- `NX_GPU_PROFILE=3`: GPU busy time from per-part timestamps, no waits.
- `NX_SHOW_WHEN_LOCKED=1`: the app shows over the lock screen, so unattended runs survive a locked phone.
- `tools/wonder_run_summary.py`: medians and IQRs over course windows (replay ≥ 8 ms).

### Course results, game lock off (`NX_MAX_INTERVAL=1`)

| Run | Change | fps median (IQR) | replay ms | GPU busy ms | hitches / 600 | > 50 ms / 600 |
|---|---|---|---|---|---|---|
| `c2` | before phase 4 (smaps guard) | 52.4 (48.6-57.7) | 12.2 | - | 11.9 (capped) | 11.4 |
| `h0` | `NX_MEMGUARD=ab` | 48.1 | 12.1 | - | **smaps 16.5, statm 1.5** | 5.8 |
| `h1b` | H1: statm guard (default) | **57.1** (49.4-59.4) | 11.4 | - | 1.4 | 1.7 |
| `n1` | + C2b janitor, U1 ring uploads, C4 resolve cache, C5 CallList table, G2 native R11G11B10F, G5A transfer clears | **59.7** (49.9-60.0) | **10.2** | 13.9 | 0.7 | 1.4 |

**H0, the hitch cause, confirmed.** In the smaps windows the guard's read took 38-63 ms (avg 47). Each slow
main-thread frame overlapped one read almost entirely, e.g. 43.4 ms of a 44.8 ms frame. statm reads take 0.12 ms.

**n1, by section:**
- World map and first course section: 60 fps, replay ~10 ms, GPU ~13 ms.
- A later section: 50 fps, replay 13 ms, GPU 14-15 ms. There the replay thread was on the 2.8 GHz cores:
  `sched_setaffinity` to the prime core failed, presumably because Android pauses the prime core when the phone is
  hot. A fallback to all big cores is now in `frame_stats.cpp`.

### Default config (the game's lock on): the governor

- **`n1def`, without a governor:** 30 fps in course. At 30 fps the clocks drop, so replay takes 15.9 ms and the
  game's main-thread frame stays past 15.83 ms: the game never leaves 30.
- **New `NX_GOVERNOR`** (nvn_queue.cpp; default on Android):
  - it clamps the game to 60;
  - it hands back to the game's choice only after two 120-frame windows in a row with > 20% of frames over 20 ms;
  - each try skips its first 90 frames while the clocks ramp up;
  - the wait between tries doubles from 10 s up to 80 s.
- **`n3def`, first version** (one window decided, no warm-up): 60 fps in the first course section, then a scene load
  sent it to 30 and the next tries failed at the low clocks.
- **`n4def`, tolerant version:** course median **58.5 fps** (IQR 50.2-60).
  - The first section holds 60 throughout.
  - The heavier later section runs 50-54 at 60-fps settings (replay 12, GPU busy 14-15 ms). The governor sends it
    back to 30 at the right speed instead of a visibly slow 50, and its tries every 10-20 s fail there.
  - **To hold 60 there, ~2 ms more must come off both the GPU and the replay.**

### Not taken

- **`NX_GPU_TURBO`** (KGSL power control off, the libadrenotools idea): the session's permission policy refused it as a
  security weakening. Left for the user to decide.
- **"Nyushu"** (the emulator the user suggested): an APK repackage of a yuzu-family emulator, never published with
  source, and taken down by DMCA on 2026-03-31. Nothing to reuse.

### Seen, not yet explained

- **Black silhouettes in `h1b`:** in that run the player and the nearby enemies of the first course section rendered
  black. The pipe section was normal, and h0, n1 and the earlier runs were clean. Intermittent, the dark-Mario class
  (DEBUG.md #39: ambient/occlusion targets of a re-created texture). To watch in every run's frames.

### Phase 4 status (end of 2026-10-01)

| Step | State | Evidence |
|---|---|---|
| T0 tooling | done | thermal gate, summary, build id per run, `MEMWATCH`, `PROFILE_AT`, `GPUWATCH`, show-when-locked |
| H0 / H1 guard | **done, default** | h0 in-run A/B; h1b |
| G0 GPU busy (`NX_GPU_PROFILE=3`) | done | n1 and later: 13-15 ms busy in course |
| C2b janitor | default | in n1 (bundle) |
| U1 ring uploads | default | n1: staging buffers in course 29 → ~15 per 60 frames |
| C4 resolve cache, C5 CallList table | default | n1 |
| G2 native R11G11B10F (Android) | default | n1 frames clean |
| G5A transfer clears (Android) | default | n1: all 181,613 clear-only passes became transfer clears |
| Governor (`NX_GOVERNOR`, Android) | default | n4def: 58.5 fps median in the default config |
| C1r render pass objects | not measured | the phone was unplugged during the cooled run |
| G1a UBWC-friendly flags (Android) | default, **S23 not yet run** | Mac with `NX_UBWC_FLAGS=1`: no "not mutable" view, frames match |
| G4 narrow pass-end barrier (Android) | default, **S23 not yet run** | Mac with `NX_PASS_BARRIER_NARROW=1`: 0 frames flagged; vertex-read barriers only for images rendered since the last full barrier |
| D1 simpleperf | not run | needs the phone |
| `NX_GPU_TURBO` | refused by the session's permission policy | user decision |

**Bundle attribution.** The n1 gain over h1b (57.1 → 59.7 fps median, replay 11.4 → 10.2 ms) is the bundle's. Each
item has its switch for a leave-one-out run: `NX_JANITOR=0`, `NX_STAGING_MIN=0`, `NX_RESOLVE_CACHE=0`,
`NX_CALL_TABLE=0`, `NX_R11G11B10_EMULATE=1`, `NX_CLEARS_PASS=1`, `NX_UBWC_FLAGS=0`, `NX_PASS_BARRIER_FULL=1`.

**Next on the phone:**
1. One default-config run with G1a + G4: the heavier course section needs GPU ≤ ~13 ms to hold 60.
2. C1r, cooled.
3. Leave-one-out runs for G1a and G4.

### GPU turbo (`NX_GPU_TURBO=1`, user-approved test, 2026-10-01)

- **What it does:** turns off the Adreno kernel driver's power control (KGSL_PROP_PWRCTRL), so the GPU sits at its top
  clock, 719 MHz on the S23. Without it the GPU was sampled at 220-295 MHz in course.
- **Persistence:** the setting outlived the process (719 MHz with the game closed). The app now sets it at every
  start: off only with `NX_GPU_TURBO=1`, back to normal otherwise.

| Run (default config: governor on) | fps median (IQR) | replay | fence wait | governor fallbacks | AP at the end |
|---|---|---|---|---|---|
| `n6def` (G1a + G4, no turbo) | 58.45 (51.5-60) | 9.6 | 1.50 | 2 | 45.7 °C, status 0 |
| `n7turbo` (G1a, turbo) | **59.9 (58.5-60)** | 9.2 | **0.06** | 1 | 55.1 °C, status 1 (light) |
| `n5def` (G1a + G4, turbo left on by accident) | 59.9 (56.8-60) | 9.4 | 0.06 | - | 55.2 °C, status 1 |

**Result:** the GPU stops being the limit; the heavier course section holds 60. It costs ~10 °C more on the SoC and
more battery. It stays opt-in: `NX_GPU_TURBO=1` in config.txt.

**G4 (narrow pass-end barrier):** off by default again. `n6def` (G4 on, turbo off) showed a black band over the
course background in 2 of 28 frames (15000 and 16200); none of the other 8 runs did. `NX_PASS_BARRIER_NARROW=1` turns
it back on for study.

## Hand-off (2026-10-01): rendering bugs, still open

The performance side is in place: S23 course median 58.5-59.9 fps in the default config. The user then asked for
the rendering bugs. A study (RENDER-BUGS.md) was started, then stopped before it wrote anything, because the work
moves to Windows. What is known:

| Bug | Evidence | Leading suspects |
|---|---|---|
| Exploded black vertices | user report while playing with `NX_PIPELINED=1`; 1 in 600 dumped frames cannot catch it | the pipelined CPU race of PLAN.md §3.2 (game writes vertex/uniform memory during the replay); stale data from today's per-submit caches (`NX_RESOLVE_CACHE`, `NX_CALL_TABLE`, `NX_SET_CACHE`, dynamic uniform offsets); ring wrap |
| Black silhouettes | `phone_h1b` frames 12600-16200 (player and nearby enemies) | the dark-Mario class (DEBUG.md #39: AO targets of re-created textures, `NX_IMAGE_REUSE`); intermittent |
| Black band over the background | `phone_n6def` frames 15000 and 16200, with G4 on | G4 (narrow pass barrier, now off by default) or another intermittent hazard |
| Background glow flicker | user report | PLAN.md §7 |

**Tools ready:**
- `BURST="200 240 280"` in `tools/wonder_phone_course.sh`: 60 consecutive frames at each time.
- `tools/burst_anomalies.py <frames dir> --out <dir>`: frames that differ from both neighbours (one-frame glitches),
  with side-by-side images.
- Every new optimization has its off switch for bisecting: `NX_RESOLVE_CACHE=0`, `NX_CALL_TABLE=0`, `NX_SET_CACHE=0`,
  `NX_DYNAMIC_UBO=0`, `NX_JANITOR=0`, `NX_STAGING_MIN=0`, `NX_SUBMIT_CHUNK_DRAWS=0`, `NX_CLEARS_PASS=1`,
  `NX_R11G11B10_EMULATE=1`, `NX_UBWC_FLAGS=0`, `NX_PIPELINED=0`.

**Suggested first steps:**
1. A burst run with the user's play, pipelined, then `burst_anomalies.py` on its frames.
2. If the vertex explosions show, the same run with `NX_PIPELINED=0`, then with the caches off one at a time.

**Before merging into the Windows branch**, run course 1-1 on the NVIDIA PC once. These changes are on for every
platform and were tested on the Mac only: the set cache, the janitor, the resolve cache, the CallList table and the
ring uploads.
