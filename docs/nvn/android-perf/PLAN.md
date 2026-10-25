# Wonder on Android: the plan to a steady 60 fps in course (Galaxy S23)

Research and planning only (2026-10-01): no code was changed. Inputs: `MEASUREMENTS.md`, `MEMORY.md`, DEBUG-2 #59-#89,
`private/wonder/phone_{pipe1,gpu1,sync1,rec}` and the code under `runtime/src/nvn/`. "Est." means estimated, not
measured.

**Symbols.** `private/wonder/ba/libwonder.so` was built at 09:54, and pipe1 ran 10:00-10:05 and gpu1 10:06-10:12.
The commits after that (b9ede1d 10:13, ed8606a 10:56) changed tools, and the Android upload ring went 256 → 128 MB,
but the library was not rebuilt, so the pipe1/gpu1 symbols match. The ring was still 256 MB in those runs. Rebuilding
`ba/` before the next profile breaks this match for pipe1.

**Goal (priority order, from the user):**

1. Steady 60 fps in course: the submit/replay thread ≤ ~12 ms, the GPU ≤ ~14 ms, even 16.7 ms present intervals.
2. Only then memory (MemAvailable ≥ 2 GB) and the glow flicker. They are covered here only where they affect frame
   time or block pipelined submission.

## 0. What bounds the frame today (measured)

### 0.1 The game locks itself to 30 fps

The game's own governor sets the present interval (`frame_time_probe`, nvn_queue.cpp; DEBUG.md #43). Interval 2 is
chosen when the main thread's frame reaches 15.83 ms. After 3 slow frames it is locked there, and it takes 15 fast
frames to come back to 1.

pipe1 in course:
- `600 of the last 600 presents at interval >= 2`;
- main-thread frame time over 600 frames: <12 ms 0, <15.8 ms 2-4, <20 ms 170-178, <33 ms 404-423.

The main thread itself is not the cost. Its CPU time is ~31% of the submit thread's (517 against 1,648 profile
samples). It is waiting on the submit thread. **So the in-course 32 fps is the governor reacting to a ~30 ms submit
thread.** The fix is to shorten the submit thread. Clamping the interval alone (`NX_MAX_INTERVAL=1`) would give
slow motion.

### 0.2 The submit (replay) thread, per frame in course (pipe1, 1 submit per frame)

| Part | ms | Why |
|---|---|---|
| replay (recording NVN into Vulkan) | 20.4-21.4 | profile below |
| fence wait | 5.9-6.7 | the first part of the split submit holds **72%** of the replay. The GPU starts it only at the split, so the CPU waits for ~12 ms of GPU work after only ~5.5 ms more of replay |
| other (rest of gpu_submit: drain, frees, write-backs) | 1.8-2.1 | write-backs read the upload ring, which is uncached memory (§2, R5) |
| encode (vkQueueSubmit) | 0.7 | |
| present | 0.5 | |
| **total** | **~30** | the sync run (sync1): replay 28-30, fence 17-18, present 7-8 → 17 fps in course |

Note: the replay is ~9 ms slower in sync1 than in pipe1 for the same course: 29 against 20 ms. CPU clocks follow the
load (DVFS), so **clocks and core placement are a first-order variable** (R2).

### 0.3 Where the replay's CPU goes (pipe1 `profile.txt`, thread 21093)

The profile is 1,648 samples. The timer is per-thread CPU time at ~250 Hz, so 1 sample ≈ 4 ms of CPU, and 1% ≈ 0.24
ms per frame. The scratchpad scripts break samples down by the first libwonder frame of each sample (with inlines),
and libc samples by their caller.

| Bucket | % | ms/frame (est.) | Where |
|---|---|---|---|
| Adreno driver (`vkCmd*`, set updates, begin/submit) | 26 | ~6.1 | draw_call 9.0% (bind pipeline/vertex/index, set dynamic, draw), draw_command in replay_list 7.1%, build_sets/bind_sets 3.4% (`vkAllocateDescriptorSets` + `vkUpdateDescriptorSets` per draw), `begin` 1.9% (`vkResetCommandBuffer`+`vkBeginCommandBuffer`), frame_split/frame_submit 1.9% |
| libc memcpy/memset/memcmp (libc 0x9e000 block) | 11 | ~2.6 | draw_call 63 samples (704-byte `PipelineKey` memset/copy/compare and attachment structs), dedup_upload 50 (ring copies and the 4 KB/64 KB zero padding), clears_flush 27 |
| libc malloc/free/mutex (0x108000, 0x119000, 0xcf000, ...) | 17 | ~4.0 | `vkQueueSubmit`/present internals, build_sets, image_of, program_shaders/get (object lookups), sampled_ready, ensure_contents |
| our code: tree/hash lookups | ~15 | ~3.5 | `command_list` std::map + mutex per CallList (4.7%, also walked twice per submit by `count_dispatches`), `lookup_handle` pool history (2.4%), `find_object` + `__dynamic_cast` (3.6%), `dynamic_uniform_bindings` std::map keyed by ShaderInfo* (1.8%), dedup tree (1.5%), `pool_at_gpu` (0.6%) |
| our code: the rest | ~23 | ~5.5 | replay_list loop, gather_textures, sets, check_linear_content (hashing linear textures every draw, 1%) |

Per frame (deltas of the cumulative counters over 300 presents): 629 draws and ~70 passes (53 ended by a clear of
other targets, 15 by a draw, 3 by dispatches, 12 clear-only); ~1,590 state binds/sets recorded; ~2,400 texture
bindings resolved; 10.3 MB copied into the ring (67 MB more reused by dedup), 11.1 MB of ring used; 3 dispatches,
20 written ranges, 1 split submit.

### 0.4 GPU (gpu1, `NX_GPU_PROFILE=1`, course, at 30 fps)

17.0 ms per frame, over 56 labels. Each label is a pass, named by the first draw's fragment shader:

| Pass (label) | ms |
|---|---|
| 960x540 fs 8963933a (uber fs: 10 samplers, ~50k SPIR-V words, all fp32) | 5.12 |
| 960x540 fs de5d38a4 (trivial fs, no texture: depth/pre-pass-like, geometry-bound?) | 2.56 |
| 960x540 fs 505b565c | 1.35 |
| outside passes (72 ops/frame: clears, uploads, copies) | 0.91 |
| 960x540 fs 352e8b76 / 31f60d40 / 77fc39b1 | 0.70 / 0.54 / 0.43 |
| 1280x720 fs 255093ce | 0.58 |
| 0x0 (depth-only) fs efe0f43c | 0.51 |
| 1920x1080 fs 38edabcd | 0.47 |
| 47 more labels | ~4.3 |

Caveats: measured at 30 fps, with the GPU's DCVS free to idle down, and the submit is waited whole when profiling.
The 960x540 chain is ~11 ms of the 17.

How every pass is set up today: `loadOp` LOAD (CLEAR when fresh or pending), `storeOp` always STORE (depth and
stencil too), attachments in `GENERAL`; every colour image is `MUTABLE_FORMAT` + `STORAGE` + `TRANSFER_SRC|DST`
(nvn_image.cpp:153-167); R11G11B10F is stored as RGBA16F (8 B/px) and D24S8 as D32S8; each pass ends with an
ALL_GRAPHICS → ALL_COMMANDS memory barrier (#71), and each command buffer begins with ALL → ALL.

### 0.5 Pacing and compiles

- **Present:** MAILBOX, 5 swapchain images (1080x2340, pre-rotated, present pass). The game's vsync event is a
  free-running host timer (`hle_vi.cpp`), and pacing is a CPU sleep in `pace_present`: nothing is tied to the
  panel's refresh (adaptive up to 120 Hz).
- **ADPF:** one session with 1 thread (the submit/present thread). The game's main thread is not in it, and no
  affinity is set.
- **Pipelines, warm cache:** 0.4-0.6 ms each, but 34-142 per 60-frame window at course entry (19-68 ms windows).
  That alone trips the governor's "3 slow frames".

## 1. Acceptance (S23, handheld, the user's replayed play)

Run:

```
REPLAY=private/wonder/phone_rec/replay.txt bash tools/wonder_phone_course.sh RXCX402B6JX <name> NX_PIPELINED=1 [NX_...]
```

Then read in `nx.log` (course part, lines after the course load):

| Check | Target |
|---|---|
| `per frame over 60` | ≥ 59.5 fps in every course interval |
| replay | ≤ 12 ms |
| fence wait | ≤ 1 ms |
| replay + encode + other + present | ≤ 14 ms |
| `presents at interval >= 2` | 0 of 600 in course |
| main-thread histogram | ≥ 95% in `<15.8` |
| `NX_GPU_PROFILE=1` run | ≤ 14 ms/frame in all, **measured with the game at interval 1** (`NX_MAX_INTERVAL=1` for that run only, so DCVS sees 60 fps load) |

Frame pacing comes from the present timestamps: add a 1 ms-bucket present-interval histogram line (Phase 0).

Mac regression for every item: the headless course 1-1 at 60.0 fps, and frame dumps diffed against the current
reference (`tools/find_glitches.py`, `tools/detect_explode.py`).

## 2. Ranked changes

Sorted by contribution to 60 fps per unit of risk and effort, within the user's priority order: CPU, then GPU, then
pacing.

ms are per frame:
- **sub** = submit-thread time;
- **GPU** = GPU time.

Effort scale: S ≤ 1 day, M 2-4 days, L 1-2 weeks, XL > 2 weeks.

| # | Change | Gain (est.) | Risk | Effort | Mac |
|---|---|---|---|---|---|
| R1 | Chunked submission: flush at pass ends every ~N draws | sub −5 to −6 (fence 6 → ≤1); sync mode 37 → ~24 | low-med | M | gated, default 1 chunk on Mac until measured |
| R2 | Big-core placement + ADPF for all frame threads | sub −0 to −6 (measure first) | low | S | none (Android only) |
| R3 | Descriptor sets: reuse within a submit, dynamic UBO offsets (push descriptors if exposed) | sub −2 to −3 | med | M-L | gains too |
| R4 | Lookups: per-submit CallList table, flat hashes, cached per-shader data | sub −2 to −3 | low | M | gains too |
| R5 | Readback memory and command pools: cached memory for written ranges, `before` copied from guest, per-slot `vkResetCommandPool` | sub −1 to −2 | low | S | neutral/gain |
| R6 | Uniform padding to the reflected max offset (robustness2 for indexed banks) | sub −0.5 to −1; −4-6 MB ring/frame | med | M | gains |
| R7 | Pipeline lookup by an incremental state hash, no 704-byte key per draw | sub −0.5 to −1 | low | S-M | gains |
| R8 | Pipelined fences default on Android, behind the gates of §3 | sub −2 to −4 (the GPU tail left by R1) | med | S once gated | unchanged |
| G1 | UBWC-friendly images: no MUTABLE_FORMAT without a format list, STORAGE only for image-flagged textures | GPU −1 to −3 | low-med | S-M | likely gain (Apple lossless compression) |
| G2 | Precise inter-pass barriers (no vertex-stage wait unless a vertex shader samples the output) | GPU −0.5 to −2 | med | S | test #71 class |
| G3 | Native B10G11R11 (keep the ufloat clamp) and D24S8 on Adreno | GPU −0.5 to −1.5; −30-60 MB | med | S-M | none (gated) |
| G4 | AGI frame capture of the 5.1/2.6 ms passes, then targeted fixes | GPU −1 to −3 (unknown) | low | S + ? | - |
| G5 | Dead depth/stencil stores (`STORE_OP_DONT_CARE` by per-submit lookahead) | GPU −0.3 to −1 | med-high | M | gains |
| P1 | Pipeline pre-creation from a persisted key list (background, at boot/scene load) | removes 20-70 ms windows at scene entry | low-med | M | gains |
| P2 | Display-locked pacing: FIFO + `ANativeWindow_setFrameRate(60)` + 3 images, vsync event from AChoreographer | even 16.7 ms intervals | med | M | none (gated) |
| P3 | `NX_MAX_INTERVAL=1` Android default once Phases 1-3 pass | no 30-fps lock on a hitch | low (then) | S | none |
| L1 | Vulkan recording on a worker thread (Ryujinx ThreadedRenderer idea, MIT) | sub −4 to −6 (the driver's 26% leaves the critical thread) | med-high | L-XL | gains |
| L2 | Internal resolution 0.75x of the scene chain, upscale (bilinear or FSR1) | GPU −3 to −4 | high | XL | option |
| L3 | spirv-opt on the translated SPIR-V (offline, packer) | compiles −20-40%, GPU 0-5% | low-med | S | gains |

**Totals:**
- Phase 1 (R1, R2, R5, R8) brings the submit thread from ~30 to ~17-21 ms.
- Phase 2 (R3, R4, R6, R7) takes off 5-8 ms more, to **~11-15 ms**.
- Phase 3 (G1-G4) takes the GPU from ~17 to **~11-14 ms**. That is a conservative range: the 17 ms was measured at 30
  fps, and some of it may be DCVS.
- If Phase 2 lands above 12 ms, L1 is the next big CPU step.

## 3. The pipelined decision

### 3.1 What the code does

These are already sound by review:
- **Per slot:** command buffers, fences, the descriptor pool, and the ring stretch. The ring tail only advances at
  retire.
- **Every command buffer** begins with an ALL_COMMANDS → ALL_COMMANDS memory barrier, so earlier submits' writes are
  ordered before it on one queue.
- **Image lifetime:** layouts are tracked in submission order. Destroyed images go to a grave keyed by
  `g_submitted+1` and are freed after retire. `drain_objects` runs under the GPU lock.
- **Never destroyed while in flight:** samplers, pipelines and buffers. Views die only with their images.
- **Write-backs** are applied after the first part's fence, behind a HOST_READ barrier, before `gpu_submit` returns.
  The only exception is the `binds_at_split` check, which waits for the whole submit.
- **The game's fences and finish** wait for submits replayed (#59). Everything the GPU reads from guest memory is
  copied into the ring at replay time.

**No GPU-side hazard specific to pipelining was found.**

The one real hole is shared by both modes: `upload buffer full` wraps a submit over its own unsubmitted data
(nvn_frames.cpp:440-451). It was never logged on the phone, and R1 removes it: a chunk flush makes room instead of
wrapping.

### 3.2 Root-cause hypotheses for #76 (exploded meshes, Windows, pipelined), most likely first

1. **A CPU-side race in the replay-versus-recording-thread family (#55-#64), exposed by timing.**
   - Pipelined returns from `nvnQueueSubmitCommands` ~GPU-time earlier, so the game's recording thread overlaps
     the next replay at another phase.
   - #57 showed exactly this class: exploded meshes and the horizon band in 1 run of 3, depending on that phase.
   - Remaining suspects:
     - the "empty at the mark → current entry" fallbacks (#63, #64), which deliberately read newer state;
     - `BindDeferred` arrays read at replay time (`NX_DEFERRED_SNAPSHOT=1` reads them at the submit call);
     - a game write into a range during its replay that no sync covers.
2. **The game's 30-fps mode, not pipelining.**
   - The Windows pipelined session ran at 40 fps, toggling interval 1 ↔ 2. The sync session ran at 58 (interval 1).
   - The Mac's #43 recordings were made at ~45 fps too.
   - Something in the recomp may break when the game alternates intervals.
3. **A GPU hazard that only the Vulkan validation layer can name.** Least likely given 3.1, but cheap to rule out.

Evidence weight: #76 is one session per mode (n = 1), with Parsec load and very different frame rates. #85/#86 saw
exploded geometry in pipelined Mac runs from other causes (the pipeline cache, since fixed).

**Can it bite Adreno?** Hypotheses 1 and 2 are platform-independent, so yes, as much as Windows. Adreno adds no
known pipelining-specific hazard.

### 3.3 How to make pipelined safe (gates), in order

1. **Sync validation (S).**
   - Run `VK_LAYER_KHRONOS_validation` with `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` over 2,000
     course frames, pipelined, on the Mac and on Windows.
   - On the phone, load the layer APK via `settings put global enable_gpu_debug_layers 1`.
   - Pass: no hazard reports from the replay's command buffers.
2. **Replay race detector (S-M, `NX_RACE_CHECK=1`).**
   - At each `dedup_upload`/`read_upload` of a vertex, index or uniform range, record (pointer, size, 64-bit hash of
     the bytes copied).
   - After the replay ends (before `gpu_submit` returns), re-hash a sample (all ranges, or 1 in 8).
   - A mismatch means the game wrote the range while it was being read. Log the range, the draw, the shader, and
     whether a sync was pending.
   - Run it in sync and pipelined. Pass: pipelined's mismatch rate is no higher than sync's, and every mismatch is
     explained.
3. **Interval discriminator (S, `NX_FORCE_INTERVAL=2`).**
   - Clamp the game's interval up to 2 in `frame_time_probe` (the mirror of `NX_MAX_INTERVAL`).
   - Run synchronously on the Mac/PC with the explode detectors. If meshes explode, hypothesis 2 holds, and the bug is
     in the 30-fps path, not in pipelining.
4. **Statistical A/B on the phone (M).**
   - 5 runs pipelined and 5 sync of `phone_rec/replay.txt`, with `dump_frames` bursts in the two places the user saw
     glitches, plus `tools/detect_explode.py` and `find_glitches.py` on all frames.
   - Pass: 0 exploded frames pipelined.
5. **Ring safety (with R1).** A full ring flushes the current chunk and waits for the oldest. It never wraps over
   unsubmitted data.

### 3.4 Verdict

Make pipelined the Android default (`NX_PIPELINED` read as 2 on `__ANDROID__`) after gates 1, 2 and 4 pass, together
with R1.

If gate 4 fails while 1 is clean, it is a CPU race. Gate 2 names the range: fix that instead of dropping pipelining.

**Fallback:** synchronous with fine chunking (R1 with ~8 chunks). It costs only the GPU tail of the last chunk,
~2-3 ms, instead of the whole GPU time. That makes "sync" ~24 ms now and ~14-15 ms after Phase 2: close enough to keep
60 fps reachable while the race is hunted.

## 4. Roadmap

Each phase ends with a phone checkpoint (§1 command, 1 run plus 1 `NX_GPU_PROFILE=1` run) and the Mac regression.
Record in MEASUREMENTS.md: course fps, replay / fence / other, the interval-2 count, the main-thread histogram, GPU
total.

### Phase 0: instrumentation (S, 1-2 days; no fps change)

- The CPU each profile sample ran on (`sched_getcpu()` in `on_sample`, platform_android.cpp). Also the per-frame CPU
  and core of the submit and main threads (`/proc/thread-self/stat` fields 14/15 and 39), once per 60 frames.
- Log at startup the memory types (flags per index) and the device extensions that matter: `VK_KHR_push_descriptor`,
  `VK_EXT_descriptor_buffer`, `VK_EXT_graphics_pipeline_library`, `VK_EXT_robustness2`,
  `VK_EXT_extended_dynamic_state{,2,3}`, `VK_GOOGLE_display_timing`, `VK_KHR_present_wait`.
- A present-interval histogram (1 ms buckets) per 600 presents.
- Time `apply_write_backs`, `free_retired_images` and `drain_objects` separately inside "other".
- A summary tool, `tools/phone_course_summary.py`: course-only medians of the lines above, as in the scratchpad's
  `logstat.py`.
- One AGI (Android GPU Inspector) frame capture in course, with per-render-pass GMEM loads/stores, bin mode
  (GMEM/sysmem) and UBWC state. It decides G1/G2/G4/G5 with data instead of estimates.

### Phase 1: structure (target: submit thread ~30 → ≤ 21 ms, fence ≤ 1 ms)

#### R1. Chunked submission (`nvn_frames.cpp`, `nvn_vk.cpp` gpu_submit, `nvn_draw.cpp` pass_end_only)

- Today a submit is at most 2 command buffers. Make a slot hold up to K (8) command buffers and K fences, or one
  timeline semaphore (core 1.2; Adreno 740 has it) with a value per chunk.
- At a pass end (`pass_end_only`, never inside a dynamic-rendering instance), flush the current chunk:
  - after ≥ D draws since the last flush (start at D = 150, ~4 chunks/frame), or ≥ T µs of replay;
  - always right after the chunk containing the last dispatch. This is today's split, so write-backs need only that
    chunk's fence.
- Each chunk begins with today's `begin_part` barrier.
- **Why:** the GPU starts within ~5 ms of the replay's start instead of at 72% of it. With GPU 17 < CPU 20 ms, the
  GPU trails by about one chunk. The fence wait on the compute chunk becomes ~0.
- Synchronous mode gets the same benefit: it waits only for the last chunk's tail.
- Cost: ~0.1-0.2 ms CPU per extra `vkQueueSubmit` on Adreno. Keep K small.
- Ryujinx does the same for the same reason: `AutoFlushCounter` flushes at attachment changes, and a "fast flush"
  mode for long passes.
- **Verify:** fence wait ≤ 1 ms (pipelined) or ≤ ~4 ms (sync) in course; `NX_SUBMIT_CHUNKS=1` reproduces today's
  numbers; no `upload buffer full`.
- **Mac:** MoltenVK encodes Metal per `vkQueueSubmit`, so measure before enabling there (default 1 off Android).

#### R2. Placement and clocks (`perf_hint.cpp`, `platform_android.cpp`)

- Measure first (Phase 0 core ids).
- **Affinity (`NX_AFFINITY=0|1`):** the submit/replay thread on the prime core (cpu7, X3 3.36 GHz), the game's main
  and recording threads on cpu3-6 (A715/A710, 2.8 GHz), never on cpu0-2 (A510). `sched_setaffinity(tid)`,
  re-applied every ~60 frames: Android resets affinity when the app's cpuset changes (Eden's Android work found the
  same).
- **ADPF, following Google's best practice:**
  - two sessions, one for the game's main thread (and the recording thread) and one for the submit + present
    threads;
  - add threads via `APerformanceHint_setThreads` (API 34) instead of closing and recreating the session;
  - keep reporting the actual work (pacing sleep excluded, as today);
  - log `AThermal_getThermalHeadroom` every 10 s to see throttling.
- **Expected:** if the replay ran on mid cores at a low DVFS point (sync1's 29 ms against pipe1's 20 ms for the same
  work says clocks matter), the prime core gives 1.2-1.5x: −3 to −6 ms. If it already ran there, ~0.
- **Verify:** replay ms against the per-thread core histogram; thermal headroom over a 5-minute run, with no
  throttling cliff.

#### R5. Readback memory and command pools (`nvn_frames.cpp`, `nvn_written.cpp`, `nvn_vk.cpp`)

- **Uncached reads.** The ring is `HOST_VISIBLE|HOST_COHERENT`, the first matching type. On Adreno that is
  write-combined (uncached), and two paths read it on the CPU:
  - `written_bind` builds `before` from the ring copy (`std::vector(r.up.ptr, ...)`): copy from the guest bytes
    instead (identical content, cached memory);
  - `apply_write_back` reads every written range from the ring. Allocate written regions from a second, small ring
    in a `HOST_CACHED` type (`HOST_CACHED|HOST_COHERENT` if exposed, else `HOST_CACHED` plus
    `vkInvalidateMappedMemoryRanges` after the fence).
- **Command pools.** One command pool per slot, without `RESET_COMMAND_BUFFER_BIT`, reset with `vkResetCommandPool`
  at retire. Arm measured pool resets far cheaper than per-buffer resets.
- **Expected:** "other" 1.9 → ~0.6-1.0 ms; `begin` 0.7 → ~0.2 ms.
- **Verify:** Phase 0's write-back timer; the `begin` share in the profile.

#### R8. Pipelined default on Android, after §3.3 gates 1, 2 and 4

It removes the GPU tail that R1 leaves in sync mode: −2 to −4 ms on the submit thread.

**Checkpoint 1.** Expected: submit thread 17-21 ms, fence ≤ 1; the governor still often at interval 2 (> 15.8 ms);
course fps 30 → 40-50.

### Phase 2: replay CPU (target: submit thread ≤ 12-14 ms)

#### R3. Descriptor sets (`nvn_sets.cpp`, `nvn_pipeline.cpp` layouts, `nvn_frames.cpp` pools)

- **Today:** up to 4 `vkAllocateDescriptorSets` per draw, all bindings rewritten (`vkUpdateDescriptorSets`), then
  `vkCmdBindDescriptorSets`. That is ~1,500 set allocations and ~5,000 writes per frame.
- **Step 1:** a per-submit cache of sets keyed by (layout, hash of the writes). Identical sets are frequent: dedup
  already returns the same ring offset for the same bytes, and texture sets repeat across draws of a material.
- **Step 2:** uniform buffers as `UNIFORM_BUFFER_DYNAMIC` over the ring, so set 0 depends only on (layout, ranges).
  The per-draw ring offsets go in `vkCmdBindDescriptorSets`' dynamic offsets, so set 0 is reused across the whole
  submit.
- **Step 3**, if Phase 0 shows `VK_KHR_push_descriptor` (gpuinfo lists it for Adreno 740 on driver 512.676; the S23
  reports `802a4001` = 512.676.1): push descriptors with an update template for sets that do not repeat.
- Qualcomm's guide prefers UBOs over push constants on Adreno; this plan keeps UBOs.
- **Expected:** −2 to −3 ms.
- **Risk:** stale sets if a key misses an input, for example a sampler or view change after #50/#67. Key on the
  resolved view and sampler handles.
- **Verify:** `texture handles`/`empty descriptor sets` log lines plus new cache-hit counters; the course frame diff
  on the Mac.

#### R4. Lookups (`nvn_cmd.cpp`, `nvn_frames.cpp`, `nvn_dedup.cpp`, `nvn_incarnation.cpp`, `nvn_objects.cpp`, `nvn_sets.cpp`, `nvn_draw.cpp`)

| Lookup today | Change |
|---|---|
| `command_list()`: mutex + `std::map` per CallList, and the whole tree walked twice (`count_dispatches` and the replay) | at submit, resolve the CallList closure once into a per-submit flat table (handle → `shared_ptr`), reused by `frame_plan_split` and `replay_list`. Hardware semantics are unchanged: still resolved at submit time |
| `dynamic_uniform_bindings(s)`: std::map keyed by ShaderInfo* | a field in ShaderInfo |
| `find_object` + `dynamic_cast` per `get<T>` | typed registries, or a type tag compared before the cast |
| `pool_entry_at` and `pool_at_gpu` trees | last-hit caches; the pool history only consulted when the slot changed after the mark (a per-slot epoch) |
| dedup `g_copies` std::map | an open-addressing table keyed by address. Ranges for `dedup_invalidate` in a small sorted vector rebuilt per submit (invalidations are rare: inline uniform updates and reports) |
| `check_linear_content` hashes linear textures on every draw | once per texture per submit (`g_submit_serial` stamp) |

- **Expected:** −2 to −3 ms. **Risk:** low.
- **Verify:** profile shares of `__lower_bound`/`find`/`__dynamic_cast` → < 3% in all.

#### R6. Uniform padding (`nvn_sets.cpp` buffer_of, `nvn_spirv_reflect.cpp`, `nvn_dedup.cpp`)

- **Today:** every uniform bank is copied, then zero-padded to 4 KB (64 KB when indexed), so that reads past the
  bound size return 0 as on Maxwell (the UI Inf bug).
- **Change:** pad only to the shader's reflected maximum *constant* offset for that bank, computed once per
  ShaderInfo. Most reads are constant offsets.
- **Indexed banks:** keep 64 KB, or, if `robustBufferAccess2` is exposed, bind the exact range and let robustness2
  return zeros. Enable it only for pipelines whose shaders index uniforms (VK_EXT_pipeline_robustness), not
  globally.
- **Expected:** −0.5 to −1 ms of memcpy/memset; roughly halves the 11 MB/frame of ring traffic (est.), which also
  helps memory and L2.
- **Risk:** medium. A wrong maximum offset gives garbage reads, the #18 class. Keep `NX_UNIFORM_PAD_4K=1` for
  bisecting.

#### R7. Pipeline key (`nvn_draw.cpp`, `nvn_pipeline.cpp`)

- **Today, per draw:** memset the 704-byte key, copy the attributes, memcmp against the last key, memcpy into a
  `ByteKey`, hash 704 bytes, memcmp in `find`.
- **Change:** keep a dirty flag per state group (`nvn_draw_state.cpp` already sees every bind). When nothing changed
  and the targets' formats are the same, reuse the last pipeline without building the key. When something did, build
  the key and hash only the changed groups (an incremental 64-bit hash).
- **Expected:** −0.5 to −1 ms.
- **Verify:** the libc share under `draw_call`.

#### Clears and passes (`nvn_clears.cpp` `clears_flush`, ~3.5%)

- A clear-only pass resolves images and views like a draw pass. Cache the target's resolved images and views for the
  pending clear when it is recorded (`g_pending`), not again at flush.
- Small (−0.2-0.4 ms); bundle with R4.

**Checkpoint 2.** Expected: submit thread 11-15 ms; interval-2 count falling; course fps 50-60 with the GPU now the
limit (17 ms). If the submit thread is still > 13 ms: L1.

### Phase 3: GPU (target: ≤ 14 ms at 60 fps load)

Start with the AGI capture (G4). The items below are ranked by expected value without it.

#### G1. UBWC (`nvn_image.cpp` image creation, `nvn_pipeline.cpp` views)

- Qualcomm's mobile guide says `MUTABLE_FORMAT` "usually degrades or disables UBWC" and should be avoided "on all
  devices prior to Adreno 750". The S23 is a 740. Storage/compute usage also disables UBWC.
- **Today:** every colour image is mutable, and every one whose format allows it gets `STORAGE`.
- **Change:**
  - `MUTABLE_FORMAT` only for formats with a real alternative view: UNORM ↔ SRGB of RGBA8/BGRA8 (#29), and the view
    formats `TargetViews.format` asks for.
  - Add `VkImageFormatListCreateInfo` with exactly those formats (core 1.2).
  - `STORAGE` only when the NVN texture flags carry the image-usage bit (the runtime already reads the 0x8
    compressible bit there; check the bit on the game's textures), or when `BindImage` meets an image made without it.
    In that case re-create the image and log it.
- **Expected:** −1 to −3 ms GPU (est.). Every load and store of every pass compresses (Qualcomm quotes significant
  bandwidth and power savings).
- **Verify:** AGI (UBWC per image), the GPU profile, frame diffs.
- **Mac:** Metal also disables lossless compression with pixel-format views, so this may help the M1 too.

#### G2. Inter-pass barriers (`nvn_draw.cpp` pass_end_only)

- **Today:** ALL_GRAPHICS → ALL_COMMANDS with MEMORY_READ|WRITE after each of ~70 passes. The next pass's vertex
  and binning work then waits for the previous pass's last fragment, so Adreno cannot overlap binning of pass N+1
  with rendering of pass N.
- **Arm's guidance:** ALL_GRAPHICS → FRAGMENT_SHADER (plus EARLY/LATE_FRAGMENT_TESTS and COLOR_ATTACHMENT_OUTPUT for
  attachment loads, and TRANSFER/COMPUTE for those users) between passes. Avoid backward fragment → vertex
  dependencies.
- **Change:** use that dst mask, and add VERTEX_SHADER only when a draw's *vertex* shader samples a rendered image
  (reflection knows which stages read which bindings).
- **Expected:** −0.5 to −2 ms GPU (est.; AGI shows the bubbles).
- **Risk:** the #71 class (NVIDIA tiles), so keep `NX_PASS_BARRIER_FULL=1`.
- **Verify:** the course HUD/sky on the PC (#71's check) and the phone frame diffs.

#### G3. Native formats on Adreno (`nvn_format.cpp`, `nvn_upload.cpp`, `nvn_pipeline.cpp` ufloat)

- **R11G11B10F** (0x3f) → `B10G11R11_UFLOAT_PACK32` when the device reports COLOR_ATTACHMENT + BLEND for it. Keep
  the fragment-output clamp (`ufloat_colors`): the shader's output never carries a negative or NaN. Blending is then
  done by the hardware in a ufloat target, as on Maxwell.
- **D24S8** → `D24_UNORM_S8_UINT` where supported (Adreno: yes; the M1: no). Re-tune `NX_EQUAL_SLACK`: depth-bias
  units differ between UNORM and float depth.
- **Expected:** −0.5 to −1.5 ms GPU (half the bandwidth of the HDR chain; 4 instead of 5 B/px of depth), and 30-60 MB
  of GPU memory.
- **Risk:** bloom/blur NaN regressions; EQUAL-pass silhouettes (#60).
- **Verify:** dialog blur, bloom and the course; an `NX_EQUAL_SLACK` sweep; A/B with a new `NX_R11G11B10_EMULATE=1`.

#### G4. AGI capture → targeted

- **fs 8963933a (5.1 ms):** a 50k-word fp32 uber shader starting the main 960x540 pass. Check for:
  - sysmem (bypass) instead of GMEM mode: GENERAL-layout attachments and feedback-like use can push the driver
    there;
  - overdraw;
  - ALU against texture bound.
- **fs de5d38a4 (2.6 ms):** a trivial shader, so geometry/binning-bound or full-screen overdraw.
- **Candidate fixes:**
  - mediump (RelaxedPrecision) for colour math in the translator, opt-in per shader, if ALU-bound (Adreno runs fp16
    at 2x);
  - `VK_EXT_attachment_feedback_loop_layout` instead of GENERAL where a pass really samples its target;
  - the per-pass mode.

#### G5. Dead stores

- The whole submit is known before replay (the recorded lists).
- **Change:** a pre-scan marks, per depth/stencil image, whether its next use after a pass in this submit is a clear
  (or nothing, and it is cleared at the next frame's first use, learned per image over 10 frames). Those passes get
  `storeOp DONT_CARE` for depth and stencil. Stencil gets `DONT_CARE` in passes where no pipeline enables the
  stencil test.
- **Expected:** −0.3 to −1 ms GPU.
- **Risk:** medium-high (a later read sees garbage). `NX_STORE_ALL=1`.

**Checkpoint 3.** Expected: GPU 11-14 ms at interval 1; course 55-60 fps. What remains is pacing.

### Phase 4: pacing (target: 0 interval-2 presents, even intervals)

#### P1. Pipeline pre-creation (`nvn_pipeline.cpp`, `nvn_pipeline_cache.cpp`)

- **Today:** with the driver's cache warm, `vkCreateGraphicsPipelines` still costs ~0.5 ms each, inline. Course entry
  makes 34-142 in a window, so scene starts trip the governor.
- **Change:**
  - persist the key list (PipelineKey with shader hashes instead of pointers, plus the layout) next to the cache
    blob;
  - at boot and at each scene load, create every known pipeline on a background thread into the same map;
  - draws find them ready.
- yuzu-family emulators keep the same "disk pipeline cache" idea: keys, not just driver blobs. Learned as an idea;
  no code was used.
- `VK_EXT_graphics_pipeline_library` would shorten first-time compiles (15-20 ms on Adreno). Only if Phase 0 shows
  the S23 driver exposes it: Turnip has it, and the proprietary driver is uncertain.
- **Verify:** `created over 60 frames` ≤ 5 pipelines in course after the first run; no interval-2 presents at course
  entry.

#### P2. Display-locked pacing (`nvn_vk.cpp` swapchain, `hle_vi.cpp`, `nvn_queue.cpp` pace_present, `platform_android.cpp`)

- **Today:** the game's vsync event is a host timer, frames are paced by a CPU sleep, and presents are MAILBOX into a
  5-image swapchain on a panel that may run at 120 Hz. Phase is free-running against the display, so 60 fps content
  can show 1-3 refreshes per frame (judder) and drift.
- **Change (Android only):**
  - `ANativeWindow_setFrameRate(window, 60, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)` (API 30+). The
    compositor then runs the panel at 60 or 120 with an even cadence.
  - FIFO with `minImageCount` 3: FIFO's back-pressure gives the even intervals.
  - Signal the game's vsync event from `AChoreographer_postVsyncCallback` (API 33; `postFrameCallback64` before),
    every frame period at 60 Hz instead of the free-running timer. The game's clock and the panel's are then one
    clock.
  - Keep `pace_present` as a cap.
  - The alternative is the Android Frame Pacing library (Swappy, Apache-2.0), which does the same with
    `VK_GOOGLE_display_timing`.
- **Expected:** present intervals 16.7 ± 1 ms in the Phase 0 histogram, at +1 frame of latency.
- **Risk:** FIFO turns a late frame into 33 ms (it was so already, for the eye).
- **Desktop keeps MAILBOX** (#62's two-clock problem stays solved there).

#### P3. `NX_MAX_INTERVAL=1` on Android (`config.txt` default)

- Only after Checkpoint 3 shows ≥ 95% of frames < 15.8 ms. A rare late frame then costs one 33 ms frame instead of
  locking 15+ frames at 30.
- The game's own variable step absorbs it. Verify that its animation does not visibly slow on late frames.

### Phase 5: levers, if a phase misses its target

- **L1. Vulkan recording on a worker thread.**
  - The replay thread produces a compact command stream (resolved handles, ring offsets, sets, pipeline handles,
    begin/end-pass records); a second big-core thread executes it into the command buffer and submits.
  - The driver's 26% (~6 ms) and the submits leave the critical thread. The two run in a 2-stage pipeline within the
    frame.
  - Ryujinx's `ThreadedRenderer` (MIT) is the model: a command ring between GPU emulation and a backend thread.
  - Effort L-XL: object lifetimes and `g_bound`-style state move with the stream.
  - Expected: −4 to −6 ms on the submit thread.
- **L2. Internal resolution:** §5.1.
- **L3. spirv-opt** (`-O`) on the translated SPIR-V in the packer (SPIRV-Tools, Apache-2.0). Validate every module
  with `shader_cache_test`.
  - It shortens driver compiles: the translator emits 243 `OpVariable` and 900 `OpLoad` for 8963933a, a non-SSA style
    the driver must clean up.
  - It may trim ALU.

## 5. Emulator-style levers

### 5.1 Internal resolution scaling (0.75x / 0.5x of the 1280x720 handheld output)

- **Today's sizes.**
  - Images are made from the guest `Texture` (`image_of`, nvn_image.cpp) at the game's sizes.
  - Attachments, viewports and scissors use those sizes (`begin_rendering`, `draw`).
  - Copies, clears and texel fetches assume them.
  - In handheld, the game already renders its 3D scene into 960x540 targets and composites into 1280x720 window
    textures cropped from 1920x1080 (`WindowSetCrop`). Our present pass then scales 1280x720 → the 16:9 area of the
    2340x1080 panel, bilinear.
- **Feasibility.** Possible, but every pixel-space path must scale consistently:
  - image extents of rescaled render targets, and of textures copied from them;
  - viewport/scissor (`draw`, `nvn_mask_clear.cpp`);
  - `CopyTextureToTexture` regions (`nvn_copy.cpp`);
  - clear rectangles;
  - shader `texelFetch`/`imageLoad`/`imageStore` coordinates and `textureSize` results, through a SPIR-V patch and a
    per-draw push-constant mask of which bindings are rescaled (the approach yuzu-family emulators use);
  - `gl_FragCoord`-based math between scaled and unscaled targets.
  - The decision of which targets to scale (the 960x540 scene chain only, not the 1280x720 UI) has to be made at
    image creation, before their use is known. That is the hard part.
  - Effort XL (3+ weeks), risk high.
- **GPU saved (est.):**
  - the 960x540 chain is ~11 of the 17 ms;
  - at 0.75x (56% of the pixels), with ~60-70% of it pixel-bound: **−3 to −4 ms**;
  - at 0.5x: **−5 to −6 ms**;
  - vertex/binning-bound passes (de5d38a4 looks like one) do not scale.
- **Artifacts to expect:** softer scene and shimmering edges; 1-px seams where a `texelFetch` misses the rescale
  mask; bloom/blur/DOF kernels defined in pixels look wider or narrower; depth-based effects (fog, SSAO) misaligned
  at edges; the UI stays sharp only if it is not scaled.
- **Upscale.**
  - A plain bilinear upscale in the game's own composite costs nothing extra.
  - **FSR1** (EASU + RCAS, MIT) fits only where we own the upscale, our present pass, which also scales the UI. Used
    there, it means scaling *all* targets, UI included (the yuzu model), at ~0.3-0.6 ms of GPU at 1920x1080 output
    (est.).
  - FSR1 alone, without lowering the internal resolution, is a sharpness option, not a speed one.
- **First, a cheap probe:**
  - log the scene viewport against its target size over a run to see whether Wonder has its own dynamic resolution;
  - it measures GPU time through `ReportCounter` timestamps, which we fill with CPU time at replay (see
    `NX_FAKE_GPU_TIME`).
  - If it has, feeding it real GPU timestamps would let the game lower its own resolution at no engineering risk.
- **Rank:** last-resort lever (L2), after G1-G4.

### 5.2 Present interval and frame limit

- **How the game asks.** `nvnWindowSetPresentInterval` every frame. The value is chosen by its governor in
  `sead::GameFramework::procFrame_` from the main thread's last frame time: 2 at ≥ 15.83 ms, locked after 3 slow
  frames, back to 1 after 15 fast ones (DEBUG.md #43). `nvnWindowGetPresentInterval` returns it.
- **What we do:**
  - `WindowSetPresentInterval` stores it and runs `frame_time_probe` (histogram; `NX_MAX_INTERVAL` clamp).
  - `pace_present` sleeps until `interval × 16.67 ms` after the previous present. Late frames are not snapped to
    vsync.
  - The swapchain is MAILBOX; the vsync event is a free host timer.
- **What it produces on the S23:**
  - in course, 600/600 presents at interval 2 (30 fps, intervals ~33 ms, steady but half rate);
  - outside the course, 7-90 of 600 at interval 2: 30/60 alternation;
  - any 3 slow frames (pipeline compiles at scene entry) lock 30 fps for ≥ 15 frames.
  - MAILBOX + CPU sleep is not phase-locked to the panel, so even "60" can judder on a 120 Hz panel.
- **Changes:** P1 (no compile bursts), P2 (display-locked clock), P3 (clamp once frames fit). Ranked in §2.

## 6. Secondary: memory, where it touches frame time

- MemAvailable ~1 GB and ~1.9 GB of zram-swapped guest memory (MEMORY.md §2).
- Each touch of a swapped page is a major fault that decompresses on the faulting core, which is often the replay
  thread, reading guest vertex and uniform data. kswapd competes for the big cores.
- **To check in Phase 0:** majflt of the submit and main threads per 60 frames (`/proc/thread-self/stat` field 12).
  If > ~50 per 60 frames in course, MEMORY.md items 12/16/17/18 move up.
- They are low-risk: the ring (already 128 MB on Android; 64 MB would be enough), descriptor pools, SPIR-V words.
  G3 also saves 30-60 MB.

## 7. Secondary: the background glow flicker

The flicker does not block pipelined unless E1 below says it is pipelining.

**Hypotheses, by prior on this code:**

1. **Depth EQUAL/LEQUAL precision on Adreno.**
   - Glow/background layers drawn against a pre-pass by another vertex shader rely on #60's depth-bias slack.
   - The bias units are implementation-specific for D32F, and Adreno's may make the slack too small, so pixels pass
     or fail frame to frame as the camera moves.
2. **Content carried across frames through re-created targets.**
   - Wonder re-creates ~180 texture objects per frame (#34); `reuse_image` keeps the image 8 ticks.
   - A temporal glow (ping-pong or accumulation) whose key changes, or a miss, starts "fresh" (CLEAR to 0): a black
     frame.
3. **R11G11B10F emulation.**
   - Additive glow in an RGBA16F target can reach Inf (> 65504). Blending can produce negatives that Maxwell's ufloat
     would have stored as 0.
   - The shader clamp does not cover the blend result. Values then flicker as the animation crosses the threshold.
4. **An intra-pass feedback hazard on a tiler.**
   - A pass that samples its own target (GENERAL layout) before `need_break` ends it.
   - The M1 is a tiler too, but Metal tracks hazards itself.
5. **Pipelined ring/slot reuse:** ruled unlikely by §3.1.
6. **Uninitialised DONT_CARE loads:** none (loads are LOAD or CLEAR).
7. **Precision (mediump):** none (no RelaxedPrecision in our SPIR-V).

**Cheap discriminating experiments:**
- Same replay, one switch per run. Capture with no timing impact: `adb shell screenrecord --bit-rate 40000000`
  during the glow scene, then `ffmpeg -i rec.mp4 -vf fps=60 f%04d.png`.
- A scratch script scores flicker on the glow's screen region:
  - mean |f(t) − f(t−1)| against mean |f(t) − f(t−2)|;
  - flicker makes the first large and the second small; motion makes both large.
- Frame dumps (`dump_frames` "120 1") are second choice: a dumped frame is waited for, which serialises pipelined.

| Run | Switch | If the flicker... | Points to |
|---|---|---|---|
| E1 | `NX_PIPELINED=0` | stays | not pipelining (5 out) |
| E2 | `NX_EQUAL_SLACK=0` and `=16` | changes with slack | 1 |
| E3 | `NX_IMAGE_REUSE=0`; `NX_UNDEFINED_LOAD=1` | worsens / disappears | 2 |
| E4 | `NX_R11G11B10_RAW=1`; after G3, native B10G11R11 | changes | 3 |
| E5 | new `NX_DRAW_BARRIER=1`: end the pass and a full barrier after every draw (debug only) | disappears | 4 |
| E6 | `NX_NO_DEDUP=1` | disappears | dedup/inline-uniform bug |

To localise the effect once E1-E6 have narrowed it:
- `NX_DUMP_ON_FS=<glow fs hash>` with `NX_DUMP_FROM`: dumps the glow target and its inputs over consecutive frames;
- `NX_TRACE_DRAWS` names the fs.

## 8. Adreno-specific gotchas for this code

- **UBWC** is lost with MUTABLE_FORMAT (before Adreno 750), storage usage, linear tiling, aliasing (G1).
- **Load and store ops:** never LOAD what will be fully overwritten. Clear or invalidate instead: on Adreno a LOAD is a
  GMEM load per bin. Clear via `loadOp`, not `vkCmdClearAttachments` mid-pass (nvn_clears already defers clears into
  loadOps).
- **Render passes:** fewer, and no needless splits. Every split is a store plus a load.
  - Today a pass ends at: texture-upload breaks (3/frame), new targets, dispatches, clear-only passes (12/frame).
  - Keep uploads and layout changes out of the pass: batch the textures a pass will sample before it begins (look
    ahead over the pass's draws).
- **Sysmem fallback:** the driver may render a pass in sysmem (bypass) mode, slow for heavy passes, for example when
  attachments are also sampled. AGI shows the mode per pass.
- **Memory types:** host-visible coherent memory is uncached. Never read it on the CPU (R5). Use HOST_CACHED for
  readbacks.
- **Command buffers:** one pool per frame slot, `vkResetCommandPool`. Avoid `RESET_COMMAND_BUFFER_BIT`.
- **Submit early and often:** for the GPU to start, and to keep both the binning and rendering slots busy (Arm and
  Ryujinx alike).
- **Driver quirks** seen by emulators on the proprietary driver:
  - broken or slow extended dynamic state on some versions: test before relying on it; we use only core dynamic
    state today;
  - BCn is exposed on Adreno 740 (#80).
  - Use a per-feature runtime check plus a log line, never a blanket `is_adreno` switch.
- **Pipeline compiles** are ~15-20 ms each cold. The on-disk `VkPipelineCache` is essential. Validate its header
  (done), and prefer pre-creation (P1) over inline creation.

## 9. References

- Qualcomm, Snapdragon Game Developer Guide. Adreno GPU on Mobile: Best Practices (UBWC, MUTABLE_FORMAT, load/store, barriers, UBO vs push constants): https://docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html
- Qualcomm, Understanding and resolving GMEM loads: https://docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html
- Qualcomm, Snapdragon Profiler "Avoid GMEM loads": https://docs.qualcomm.com/nav/home/avoid_gmem_loads.html?product=1601111740010426
- Qualcomm, Tile Memory Heap extension blog: https://www.qualcomm.com/developer/blog/2026/05/high-performance-memory-extension-optimize-memory
- Arm, Vulkan synchronization on Mali GPUs (barrier stages, bubbles, submit frequency): https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware
- Khronos Vulkan Samples, pipeline barriers: https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html
- Khronos Vulkan Samples, command buffer usage (pool reset vs buffer reset): https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/command_buffer_usage/README.adoc
- Khronos Vulkan Samples, render pass attachments (load/store, transient): https://docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html
- Khronos guide, tile-based rendering best practices: https://github.khronos.org/Vulkan-Site/guide/latest/tile_based_rendering_best_practices.html
- Samsung, Vulkan usage recommendations: https://developer.samsung.com/galaxy-gamedev/resources/articles/usage.html
- Arm, Vulkan CPU memory mapping (cached vs uncached host memory): https://support.arm.com/documentation/101897/0304/CPU-overheads/Vulkan-CPU-memory-mapping
- VK_EXT_attachment_feedback_loop_layout proposal: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_attachment_feedback_loop_layout.html
- Vulkan Robustness guide (robustBufferAccess2): https://docs.vulkan.org/guide/latest/robustness.html
- Vulkan Hardware Database, Adreno 740 (driver 512.676) report: https://vulkan.gpuinfo.org/displayreport.php?id=49267
- Mesa Freedreno/Turnip docs (GMEM vs sysmem, autotuner): https://docs.mesa3d.org/drivers/freedreno.html
- Turnip FOSDEM 2024 update (A740 support, GPL in Turnip): https://archive.fosdem.org/2024/events/attachments/fosdem-2024-2033-turnip-update-on-open-source-vulkan-driver-for-adreno-gpus/slides/22737/Update_on_Open_Source_Vulkan_Driver_for_Adreno_gqrqgmC.pdf
- Android, ADPF overview and best practices: https://developer.android.com/games/optimize/adpf , https://developer.android.google.cn/games/optimize/adpf/best-practices-adpf
- Android NDK, Performance Hint Manager (setThreads): https://developer.android.com/ndk/reference/group/a-performance-hint
- Android, frame pacing library (Swappy) for Vulkan: https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions
- Android, frame rate API (setFrameRate): https://developer.android.com/media/optimize/performance/frame-rate
- Android, optimize refresh rates: https://developer.android.com/games/optimize/display-refresh-rate-change
- Ryujinx (MIT), Vulkan "Fast Flush" / AutoFlushCounter PR: https://github.com/Ryujinx/Ryujinx/pull/4667
- Ryujinx (MIT), ThreadedRenderer (GAL multithreading): https://git.nadeko.net/Ryujinx_Mirror/Ryujinx/src/tag/1.1.1253/src/Ryujinx.Graphics.GAL/Multithreading/ThreadedRenderer.cs
- yuzu progress reports (buffer cache, stream buffer, vertex explosions from a stream-buffer off-by-one; ideas only, GPL, no code): https://yuzu-emu.org/entry/yuzu-progress-report-mar-2022/ , https://yuzu-emu.org/entry/yuzu-progress-report-apr-2023/
- Eden (GPL; idea only: re-apply affinity on Android): https://git.eden-emu.dev/eden-emu/eden/commit/71b9d8d3df930f783a854b517fa3e982f2b4c30c
- AMD FidelityFX Super Resolution 1 (MIT): https://github.com/GPUOpen-Effects/FidelityFX-FSR , https://gpuopen.com/fidelityfx-superresolution/
- Android GPU Inspector (frame profiling on Adreno): https://gpuinspector.dev/
