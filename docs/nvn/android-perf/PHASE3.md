# Wonder on Android, phase 3: GPU ≤ 14 ms, replay ≤ 12 ms, pacing (Galaxy S23)

Research and planning only (2026-10-01): no runtime code was changed. Inputs: `PLAN.md` (G1-G5, P1-P3, L1),
`MEASUREMENTS.md`, `private/wonder/phone_{heap1,watch1,gpu2,max1,pipe1}` (nx.log, profile.txt), `ba/libwonder.so`
(matches heap1), `phone_libs/{libc.so,vulkan.adreno.so}`, `spirv/*.spv` (decompiled with spirv-cross), and the code
under `runtime/src/`. "Est." marks an estimate, not a measurement. Line numbers are as of commit 780b31f.

## 0. New findings

### 0.1 The driver frames are now resolved: per-pass objects inside `vkCmdBeginRendering`

The profile's "?" frames are PAC-signed return addresses. Masking them to 39 bits and finding, for each library, the
load base that puts *every* frame right after a `BL`/`BLR` recovers them all. In heap1: libc at `0x7c66e88000`,
`vulkan.adreno.so` at `0x796a800000`, libwonder at `0x7b19401000` (the bases change per run). The scratchpad scripts
`bases.py` (bases), `full1.py` (per-sample entry point), `agg2.py` (totals), `own2.py` and `callers.py` (our own
functions) should be folded into `tools/android_profile_report.py`.

Each sample is charged to the outermost driver entry point (`qglinternal::vk*`). Units: % of the replay thread's
samples. heap1's thread was busy 72% at ~47.5 fps, ~15.2 ms of CPU per frame, so **1% ≈ 0.15 ms/frame**.

| Driver entry point (caller) | heap1 | watch1 | gpu2 | ms/frame (heap1) |
|---|---|---|---|---|
| `vkCmdBeginRendering` (begin_rendering) | **15.8%** | 16.7% | 14.8% | **~2.4** |
| `vkResetCommandBuffer` (begin_part) | **5.7%** | 9.0% | 8.2% | ~0.9 |
| `vkCmdDrawIndexed` (draw) | 3.9% | 3.6% | 2.7% | ~0.6 |
| `vkEndCommandBuffer` (submit_part) | 2.6% | 2.0% | 2.6% | ~0.4 |
| `vkAllocateMemory` (image_of, staging_alloc) | 2.1% | - | 2.5% | ~0.3 |
| `vkUpdateDescriptorSets` + `vkCmdBindDescriptorSets` + `vkAllocateDescriptorSets` | 4.3% | - | - | ~0.65 |
| `vkQueueSubmit`, `vkCmdEndRendering`, `vkCmdBindPipeline`, barriers, fences | ~3.5% | - | - | ~0.5 |
| **all driver entry points** | **40%** | | | **~6** |

What the resolved chains show (heap1):
- `vkCmdBeginRendering` creates a framebuffer and render pass of its own on every call
  (`qgl::vkCmdBeginRendering → qgl::vkCreateFramebuffer → (internal) → calloc`).
- The calloc site (`vulkan.adreno.so` 0x2b48f0) is `calloc(1, n × 384)`, with `n` read from the new object (+0x13c),
  probably its bin count. Its memset is 4.6% of the samples, and scudo's madvise under it 1.8%.
- `vkResetCommandBuffer` frees those objects (`munmap`, `vkCreateFramebuffer+0x82dc` under it), and
  `vkEndCommandBuffer` post-processes the render passes.

So the proprietary driver implements dynamic rendering through transient per-pass objects: **~37 µs in
BeginRendering plus ~20 µs at reset and end, ~57 µs per pass**, against ~1 µs per `vkCmdDrawIndexed`. One pass costs
as much as ~50 draws; at ~64 passes per frame that is **~3.6 ms of the replay thread**, the largest single item.
(Earlier this showed up as "26% under begin_rendering" through a libc leaf.)

The other 60% is our own code (heap1, by the first non-std function):

| Function | % | Main callers / why |
|---|---|---|
| `get<Texture>` | 8.5 | lookup_handle 2.3, ensure_contents 1.9, sampled_ready 1.9, image_of 1.3. The thread-local cache dies at every `objects_lock` generation bump (game threads) |
| `replay_list` (own) | 6.1 | the command loop; command_list map lookups |
| `dedup_upload` | 5.4 | memcpy into the write-combined ring, plus the dedup tree |
| `count_dispatches` | 3.8 | `frame_plan_split`: CallList recursion via `command_list()` (mutex + map) |
| `lookup_handle` | 2.9 | pool-history `lower_bound` |
| `guest_bytes` / `pool_at_gpu` | 2.5 | `upper_bound` per range |
| draw, graphics_pipeline, resolve_sets, build_sets, get<Program>, buffer_of | ~6.5 | |

**Wall against CPU.** The submit thread spends ~18.6 ms of wall time per frame (replay 16.5 + other 1.1 + encode 0.6
+ present 0.4) but only ~15.2 ms on the CPU: **~3.4 ms per frame off-CPU** inside the work (page faults, mmap/munmap
under the driver's allocations, lock waits or preemption). Nothing measures it yet (I1, §5).

### 0.2 Pass census per frame (heap1, deltas over 300 frames)

| | per frame |
|---|---|
| passes | **63** (+1 present pass) |
| ended by "clear of other targets" | 50 (reason 3 also counts the clear-only passes) |
| clear-only passes ("alone") | **11** |
| clears taken as a loadOp of a draw pass | 32 |
| ended by a draw: new targets / texture breaks | 10 / 1 |
| ended by a dispatch / by the submit end | 1 / 1 |
| draws | 553 |
| descriptor sets | 199 made, 841 reused (82%) |
| state binds | 1,316 recorded, 4,344 already in place |

Most pass ends are real target switches: the game sets B, clears B, draws to B, and the end is named after the
clear. Merging consecutive passes on the same targets would save ~1 pass per frame. **The 11 clear-only passes are
the only pass count that can come down without changing the image**; the per-pass *cost* (C1) is the bigger lever.

### 0.3 The GPU profile measures a GPU at low clocks

`NX_GPU_PROFILE=1` makes `gpu_submit` wait for every submit whole (`split_after = SIZE_MAX`, `frame_submit(...,
true)`, nvn_vk.cpp:531-578). gpu2 ran at **21 fps with a 21-24 ms fence wait**: the GPU was busy ~18.7 of every
~46 ms, about 40%, so Adreno's DCVS lowers its clock. It also writes a BOTTOM_OF_PIPE timestamp at each of ~130 marks
per frame, which serialises passes. **18.7 ms is an upper bound**; the GPU time at 60-fps load is unknown and could
be ~12-15 ms. In heap1 (no profiling, ~47.5 fps) the fence wait stayed at 0.1-1.1 ms with 2 submits in flight: the
GPU kept up with an 18.6 ms CPU frame.

The labels at steady state (gpu2's full blocks; the 2.78/2.44 block quoted before was a scene change):

| Label (first draw's fs of the pass) | ms/frame | Passes / 60 frames | What it is |
|---|---|---|---|
| 960x540 fs 322e146b | **4.4-5.3** | 48-58 | main forward pass. Uber fs: 10 samplers (3 cubes for IBL, a 3D LUT), ~280 GLSL.std.450 ops, all fp32, 1 loop, no discard, 2 outputs. 8963933a and 77fc39b1 are the same family |
| 960x540 fs de5d38a4 | **3.6-3.9** | 54-59 | opened by a trivial fs (MRT0 = black, MRT1 = a depth-like value): a geometry/pre-pass, probably bound by vertices and binning, not fragments |
| 960x540 fs 505b565c | 1.2-1.4 | 54-59 | 3 fetches |
| 960x540 fs 77fc39b1 | 0.7-1.2 | 54-59 | uber family |
| outside passes | 0.86-0.90 | ~68 per frame | clear-only passes, uploads, copies, the dispatch |
| 960x540 352e8b76 / 31f60d40; 1280x720 255093ce; 480x270 f8a4447a; 1920x1080 38edabcd | 0.5-0.7 each | | post chain and composite |

The 960x540 chain is still ~12 of the 18.7 ms.

### 0.4 The 41 present intervals over 50 ms per 600 frames

Every run with the lock off shows the same count at any fps: ~40 of every 600 main-thread frames (the game's own
`fw[0x118]` time, `frame_time_probe`) take more than 33 ms.

| Run | fps | Main-thread frames > 33 ms per 600 |
|---|---|---|
| max1 / watch1 / heap1 | ~48 | 39-43 / 41-48 / 39-41 |
| gpu2 | 21 | 38-42 |
| pipe1 (lock on) | 30 | ~9 |

Not the cause: pipeline creation (2-9 per 60 frames in course, 2-6 ms; the 62- and 38-pipeline windows are scene
changes), ASTC (0 decoded: Adreno samples it natively), uploads (steady ~11 MB per frame). Two **harness
artifacts** make the >100 ms "stall:" reports, 1-2 per 600 frames and absent in real play:
- the frame dump every 600 frames is waited for: stalls after frames 8999, 9599, 12599, 14999, 15599 (600k − 1);
- `dumpsys meminfo` every 20 s makes the app's binder thread read its smaps: 110-300 ms stalls at 7184, 11666, 12006,
  12555, 14138.

The remaining ~40 are real, about **1 in 15 frames**, and cost **~6-8 fps** of the 47-50 fps mean (518 of 600
intervals are already 14-19 ms). Hypotheses and instrumentation: §5.

## 1. Verification protocol (every item)

```
REPLAY=private/wonder/phone_rec/replay.txt bash tools/wonder_phone_course.sh RXCX402B6JX <name> \
    NX_PIPELINED=1 NX_AFFINITY=1 NX_MAX_INTERVAL=1 [NX_GPU_PROFILE=1|2|3] [item switch]
```

Called "RUN <name> [switches]" below. Every item has an off switch; an A/B pair is two RUNs of the same build with
only the switch changed.
- **CPU:** course medians of `replay`, `other`, `encode`, `present` (per 60 frames); `present intervals over 600`;
  `main-thread frame time`; the item's functions' share in `profile.txt` (`agg2.py`, `own2.py`).
- **GPU:** the per-label delta between steady `gpu profile:` blocks, and the `in all` total. Use
  `NX_GPU_PROFILE=2` (G0) as soon as it exists.
- **Image:** the frames dumped every 600 (`phone_<name>/frames`) as a contact sheet (`tools/frames.py sheet`),
  `tools/find_glitches.py` and `tools/detect_explode.py`; for items not gated to Android, the Mac headless course 1-1
  frame-dump diff (PLAN.md §1).
- **Noise:** ~±1 ms on replay, ~±0.3 ms per GPU label (heap1 against watch1). Below that, repeat both runs.

## 2. Ranked changes

Ranked by value per unit of risk and effort; CPU and GPU together, since both bound the frame. **sub** =
submit/replay-thread ms per frame, **GPU** = GPU ms per frame. Effort: S ≤ 1 day, M 2-4 days, L 1-2 weeks.

| # | Change | Gain (est.) | Risk | Effort | Files |
|---|---|---|---|---|---|
| G0 | Non-serialising GPU profile + GPU clock log | measurement (decides G1-G6) | none | S | nvn_debug.cpp, nvn_frames.cpp, nvn_vk.cpp |
| I1 | Hitch and off-CPU instrumentation (wait sites, rusage per thread) | measurement (decides H1) | none | S | nvn_queue.cpp, nvn_objects.cpp, frame_stats.cpp |
| C1 | Cached VkRenderPass + VkFramebuffer instead of dynamic rendering (after a 1-day benchmark) | sub −1.5 to −2.5 | med | M-L | nvn_draw.cpp, nvn_pipeline.cpp, nvn_clears.cpp, nvn_mask_clear.cpp |
| C2 | Command pool per slot, reset at retire, off the critical path | sub −0.6 to −1.2 | low | S | nvn_vk.cpp:223, nvn_frames.cpp:160/190 |
| H1 | Narrow the game's waits on the replay (pool invalidate/finalize, syncs), if I1 confirms | up to the ~40 hitches: +4 to +7 fps | med | M | nvn_queue.cpp, nvn_objects.cpp, nvn_written.cpp |
| C4 | Per-submit object resolve cache (get<T> in a replay) | sub −0.8 to −1.2 | low-med | S-M | nvn.h:91, nvn_incarnation.cpp |
| G1 | UBWC-friendly images: MUTABLE only RGBA8/BGRA8 UNORM↔SRGB with a format list; STORAGE on demand | GPU −1 to −3 | low-med | S-M | nvn_image.cpp:151-168, nvn_pipeline.cpp:22 |
| G2 | Native B10G11R11_UFLOAT for NVN 0x3f on Adreno | GPU −0.8 to −2; −30-60 MB | med | S-M | nvn_format.cpp:51, nvn_upload.cpp, nvn_pipeline.cpp |
| C5 | CallList closure resolved once per submit | sub −0.5 to −0.8 | low | S | nvn_cmd.cpp:336, nvn_frames.cpp count_dispatches, nvn_replay.cpp |
| G5 | Clear-only passes → `vkCmdClearColorImage` (Android), then per-image pending clears | sub −0.3 to −0.6; GPU −0.2 to −0.6 | low / med | S / M | nvn_clears.cpp:119-196 |
| G4 | Pass-end barrier without vertex stages (A7xx concurrent binning) | GPU −0.5 to −2 | med | S-M | nvn_draw.cpp:468-487, nvn_sets.cpp gather |
| G3 | STORE_OP_NONE / LOAD_OP_NONE for attachments a pass does not touch (predicted, split on a miss) | GPU −0.3 to −1 | low-med | M | nvn_draw.cpp begin_rendering/draw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bytes | sub −0.4 to −0.6 | low | S | nvn_incarnation.cpp, nvn_draw.cpp:27, nvn_objects.cpp |
| C7 | Uniform padding to the reflected maximum (R6) + open-addressing dedup | sub −0.4 to −0.8 | med | M | nvn_sets.cpp buffer_of, nvn_dedup.cpp |
| G6 | Render area of 1920x1080 window-texture passes = the 1280x720 crop | GPU −0.2 to −0.4 | low-med | S | nvn_draw.cpp:103-173, nvn_queue.cpp:288 |
| C8 | scudo secondary cache (`M_CACHE_COUNT_MAX`/`M_CACHE_SIZE_MAX`) for the driver's per-pass blocks | sub −0.2 to −0.4 (0 after C1) | low | S | platform_android.cpp:562 |
| P2 | Display-locked pacing: setFrameRate(60) + FIFO 3 + Choreographer vsync | even 16.7 ms intervals | med | M | nvn_vk.cpp:287, hle_vi.cpp:46, nvn_queue.cpp:248 |
| P3 | `NX_MAX_INTERVAL=1` Android default (and an adaptive clamp before it) | no 30-fps lock on hitches | low (then) | S | nvn_queue.cpp:209 |
| L1 | Vulkan recording on a worker thread | sub −1.3 to −2 after C1/C2 (−3.5 to −4.5 without) | med-high | L | p_vkCmd* thunks, nvn_frames.cpp, nvn_vk.cpp |
| C9 | Push descriptors + update templates for sets that miss the cache | sub −0.1 to −0.3 | low-med | M | nvn_sets.cpp, nvn_pipeline.cpp:192 |
| G7 | D24_UNORM_S8_UINT native | GPU −0.1 to −0.4 | med (#60 slack) | S | nvn_format.cpp:43 |
| G8 | AGI-gated: fp16 in the uber shaders, optimal layouts, LRZ | unknown | high | ? | translator, nvn_image.cpp |

**Totals (est.):**
- **CPU:** C1 + C2 + C4 + C5 + C6 + G5 take the submit thread from ~18.6 to **~12.5-14 ms**, replay alone from ~16.5
  to **~11.5-12.5 ms**. I1 may find more in the 3.4 ms off-CPU; L1 is next if the replay stays above 12 ms.
- **GPU:** from ≤ 18.7 ms (low clocks), G1 + G2 + G4 + G5 + G3 + G6 give ~3-8 ms: **~11-15 ms**. G0 gives the real
  starting point.
- **Pacing:** H1 removes most of the >50 ms intervals; the course mean then follows the CPU/GPU frame.

## 3. GPU items

### G0. GPU time without serialising the frame (do first)

- `NX_GPU_PROFILE=2`: a timestamp query pool and its labels per `Slot` (nvn_frames.cpp:58). `gpu_profile_collect`
  runs in `retire_oldest` (nvn_frames.cpp:160), after the slot's fences, with `vkGetQueryPoolResults` (no WAIT).
  `gpu_submit` keeps its split and chunks: no `split_after = SIZE_MAX`, no whole wait.
- `NX_GPU_PROFILE=3`: only two timestamps per submit part, for GPU busy per frame with the least perturbation.
- Once per 60 frames, log the GPU clock if readable (`/sys/class/kgsl/kgsl-3d0/gpuclk`, else `devfreq/cur_freq`);
  log once if SELinux denies it, never require it.
- Mode 3 against mode 2 shows what the per-pass timestamps cost; mode 2 against mode 1 shows the DCVS effect.
- **Verify:** RUN g0a NX_GPU_PROFILE=2 and RUN g0b NX_GPU_PROFILE=3: fps at heap1's (45-50), fence wait ≤ 1.1 ms.

### G1. UBWC: MUTABLE_FORMAT and STORAGE only where used

**Today** (nvn_image.cpp:151-168): every colour image is `MUTABLE_FORMAT` without `VkImageFormatListCreateInfo`, and
every format whose optimal tiling supports storage gets `STORAGE` (RGBA16F, RGBA8, R32F, ...).

**Evidence:**
- Qualcomm: avoid `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` "on all devices prior to Adreno 750"; it "usually degrades or
  disables UBWC". The S23 is an Adreno 740.
- Turnip (Mesa, MIT; ideas only) drops UBWC for mutable images without a UBWC-compatible format list, and dropped it
  for storage images until a7xx-era fixes.
- In course the logs show a single view reinterpretation: `texture format 0x25 viewed as 0x38` (RGBA8 UNORM as SRGB).

**Change:**
1. `MUTABLE_FORMAT` only for RGBA8/BGRA8 UNORM and SRGB (#29), with a format list {UNORM, SRGB}.
2. `view_format` (nvn_pipeline.cpp:22-39) returns `img.format` when the image is not mutable and logs the pair once.
   The pair goes into a small persisted set (next to the pipeline cache) so the next image of that format is mutable.
3. `STORAGE` on demand. When a set-3 `storage_view` meets an image without it: re-create the image with `STORAGE`
   outside a pass, `vkCmdCopyImage` old → new, swap `t->image` and its `g_live_images` entry, the old image to the
   grave, and remember the `StorageKey` shape for later images. Log a count; with ~1 dispatch per frame it is rare.
4. `NX_UBWC_FLAGS=0` restores today's flags (`NX_MUTABLE_ALL=1` the mutable part only).

**Expected:** GPU −1 to −3 ms (est.): every attachment load/store and every sample of a render target compresses, and
UBWC typically halves the bytes of colour targets. **Risk:** low-med: a missed mutable view gives the wrong
sRGB-ness (a UI colour shift, named by the log line); the re-create path is new code.

**Verify:** RUN g1 NX_GPU_PROFILE=2 against NX_UBWC_FLAGS=0 (mostly the 960x540 labels and "outside"); the contact
sheet (UI colours); UBWC on the 960x540 targets in the SDP/AGI resource view. **Mac:** Metal also loses lossless
compression on pixel-format-view textures, which MoltenVK requests for mutable images: measure the gain there too.

### G2. NVN R11G11B10F as B10G11R11_UFLOAT on Adreno

**Today:** 0x3f is RGBA16F, 8 B/px (nvn_format.cpp:48-51), because Metal wrote NaN/Inf where Maxwell writes 0.
Vulkan converts negative values to 0 when writing unsigned float formats, and NaN stays NaN, so the fragment-output
clamp (`ufloat_colors`) must stay. Alpha reads as 1, as the emulation does today.

**Change:** `vk_format(0x3f)` → `B10G11R11_UFLOAT_PACK32` when its format features have COLOR_ATTACHMENT_BLEND,
SAMPLED and TRANSFER (runtime check, logged once). Bypass the half4 widening on upload (nvn_upload.cpp) and the
8-byte rule in `view_format` (4-byte views become valid again). `NX_R11G11B10_EMULATE=1` keeps RGBA16F; off by
default on MoltenVK.

**Expected:** GPU −0.8 to −2 ms (est.) and −30-60 MB (MEMORY.md #16). The 960x540 HDR targets halve their bytes per
load, store and sample. GMEM is sized in bytes per pixel, so the MRT geometry passes need fewer bins: less binning and
per-bin vertex work for the de5d38a4 pass, which looks geometry-bound. **Risk:** medium: blend results that are
NaN/Inf (Inf × 0 in additive glow) stay so, the #50 class. Also watch the glow flicker (PLAN.md §7 E4).

**Verify:** RUN g2 NX_GPU_PROFILE=2 against NX_R11G11B10_EMULATE=1; frames (bloom, the dialog blur, the sky); no new
"not compatible: ignored" view lines.

### G3. Do not store (or load) what a pass does not touch

**Today:** every attachment STOREs, depth and stencil included, and LOADs unless fresh or cleared
(nvn_draw.cpp:133-149). `VK_ATTACHMENT_STORE_OP_NONE` (core 1.3, from `VK_QCOM_render_pass_store_ops`) keeps the
contents and writes nothing back: correct whenever the pass did not write that attachment. `LOAD_OP_NONE`
(`VK_EXT_load_store_op_none`, present) also skips the load of an attachment the pass neither reads nor writes, such as
the stencil aspect of D32S8 in passes with no stencil test.

**Change:**
- The ops are chosen at `begin_rendering`, before the pass's draws are known, so they are predicted. Key: the targets
  and their ordinal in the submit. Value: the pass's written bits (`g_pass_written` at `pass_end_only`) and whether
  any of its pipelines used stencil. An attachment not written in the last 2 frames gets NONE.
- **A miss is safe.** In `draw()`, before a draw that would write an attachment begun with NONE (colour write mask
  non-zero for that slot, depth write, a stencil write mask with a non-KEEP op), end the pass (nothing to store,
  nothing was written) and begin a new one with STORE/LOAD. A miss costs one pass. `NX_STORE_ALL=1` turns it off.

**Expected:** GPU −0.3 to −1 ms (est.): one D32S8 960x540 store is 2.6 MB, and depth-tested non-writing passes are
the common case in post and transparency. **Risk:** low-med (correct by construction; the split path is new).
**Verify:** RUN g3 NX_GPU_PROFILE=2 against NX_STORE_ALL=1; a miss count per 600 frames (expect < 5); frames.
Android only until MoltenVK's mapping of NONE is checked.

### G4. The pass-end barrier lets the next pass's binning overlap

**Today** (nvn_draw.cpp:478-485): after each of ~63 passes per frame, `ALL_GRAPHICS → ALL_COMMANDS` with
`MEMORY_READ|WRITE`; the dst scope includes `VERTEX_INPUT` and `VERTEX_SHADER`. A7xx bins the next pass (BV thread)
while it renders the current one (BR): Turnip's "concurrent binning" (`TU_DEBUG=nocb/forcecb`). A dependency into
vertex stages forces a drain; Arm's guidance is the same (avoid fragment → vertex dependencies between passes).

**Change:**
- dst stages = `FRAGMENT_SHADER | EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS | COLOR_ATTACHMENT_OUTPUT | TRANSFER |
  COMPUTE_SHADER`, with the matching access masks.
- Vertex readers: a serial per GpuImage (`rendered_at`) and a global `vertex_visible_at`. When `gather_textures`
  finds a *vertex-stage* binding (reflection knows the stage) whose image was rendered after the last vertex-visible
  barrier, it returns `kNeedBreak`; the pass ends with a barrier that includes `VERTEX_SHADER`.
- Vertex and index data come from the ring (host writes, ordered by `begin_part`'s barrier) or from compute write
  ranges (the dispatch keeps its own `COMPUTE → ALL_COMMANDS` barrier).
- `NX_PASS_BARRIER_FULL=1` keeps today's barrier.

**Expected:** GPU −0.5 to −2 ms (est.; AGI's timeline shows the gaps). **Risk:** medium, the #71 class (NVIDIA
overlapped passes; HUD in blocks): check the course HUD on Windows as #71 did. **Verify:** RUN g4 NX_GPU_PROFILE=3
(busy time: per-pass marks serialise) against NX_PASS_BARRIER_FULL=1; frames, the HUD.

### G5. The 11 clear-only passes

**Today:** `clears_flush` (nvn_clears.cpp:119-125) begins and ends a pass with no draw. The game clears target set A,
then B, before drawing to A. There is only one pending target set (`g_pending`): the second clear flushes the first
(line 183), as do sampling, copies, dispatches and the submit end.

- **Change A (Android, S):** `clears_flush` records `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage` on the
  target's level and layer (GENERAL is allowed) instead of an empty pass. On Adreno these are blits, with no pass
  setup. On MoltenVK they were an encoder of their own (#31): gated off there. Expected sub −0.3 to −0.6 ms (11 ×
  ~40-57 µs), GPU −0.1 to −0.3 ms.
- **Change B (M, after A):** pending clears per (image, level, layer, view format), in a small table instead of one
  target set. A pass beginning on an attachment with a pending clear takes it as loadOp CLEAR; only the image that is
  sampled, copied, aliased or dispatched over is flushed (with A's command). Aliasing: `image_of` shares a GpuImage
  only for equal `StorageKey`s, so an image over overlapping memory with another key must flush first (compare
  memory ranges). Expected GPU −0.2 to −0.6 ms (no store and reload of cleared data). Risk: medium (order).
- `NX_CLEARS_PASS=1` restores today's behaviour.

**Verify:** `nvn clears: ... alone` → ~0; RUN g5 NX_GPU_PROFILE=2 (the "outside passes" label); frames.

### G6. Render area of the window-texture passes

In handheld the game renders 1280x720 into 1920x1080 window textures and crops (`WindowSetCrop`), but
`begin_rendering` uses the whole 1920x1080 as the render area: 2.25 times the pixels in every bin, load and store.
**Change:** when colour attachment 0 is one of the `Window::textures` and a crop is set, the render area is the crop
rectangle; `g_pass_extent` stays for the default viewport. Rendering outside the render area is undefined, so before
each draw in such a pass check that viewport and scissor lie inside it, else end the pass and begin it full size (log
it). `NX_FULL_RENDER_AREA=1` turns it off. **Expected:** GPU −0.2 to −0.4 ms (the 1920x1080 label is 0.5 ms).
**Verify:** RUN g6 NX_GPU_PROFILE=2 (the 1920x1080 label); frames (the HUD edges).

### G7, G8, present size

- **G7, D24S8 native** (`D24_UNORM_S8_UINT`, nvn_format.cpp:43): 5 → 4 B/px. Small, and the #60 `NX_EQUAL_SLACK`
  must be re-tuned: last.
- **G8, only on AGI evidence:** RelaxedPrecision on colour math in the uber family if it is ALU-bound (A7xx runs fp16
  at 2x); `*_ATTACHMENT_OPTIMAL`/`SHADER_READ_ONLY_OPTIMAL` instead of GENERAL if AGI ties direct mode or UBWC-off to
  GENERAL; the LRZ state of the de5d38a4 pass.
- **Present size** (MEMORY.md #20): `ANativeWindow_setBuffersGeometry` at a 1280x720-class size lets the hardware
  composer scale: ~−0.2 ms GPU (est.), −20-35 MB.

## 4. CPU items

### C1. Render pass and framebuffer objects instead of dynamic rendering (Adreno)

**Step 0, a benchmark (S, decides C1):** `NX_RP_BENCH=1` at startup, logged once. Record 1,000 begin/end pairs on a
960x540 RGBA16F + D32S8 target with (a) `vkCmdBeginRendering`, (b) `vkCmdBeginRenderPass` with a cached VkRenderPass
and VkFramebuffer, (c) the same with an imageless framebuffer (core 1.2). Time recording, `vkEndCommandBuffer` and
`vkResetCommandBuffer` separately. Go ahead if (b) is under ~10 µs per pass against (a)'s ~50.

**Change** (`NX_RENDER_PASS_OBJECTS=1`, default on Android only after the A/B):
- **Render passes:** `begin_rendering` (nvn_draw.cpp:103-173) builds a `RenderPassKey` (per attachment: format,
  load/store ops, stencil ops separately; GENERAL → GENERAL; one subpass; no external dependencies, since we emit our
  own barriers) → cached VkRenderPass.
- **Framebuffers:** a `FramebufferKey` (render pass, the 9 views, extent) → VkFramebuffer. Entries are dropped in
  `forget_image_views` (nvn_pipeline.cpp:653), which already runs when an image is freed after retire.
- **Begin/end:** `vkCmdBeginRenderPass` with the clear values from `clears_apply`; `vkCmdEndRenderPass` in
  `pass_end_only`. `vkCmdClearAttachments` and the masked clear (nvn_mask_clear.cpp) work unchanged inside.
- **Pipelines** (`graphics_pipeline`, nvn_pipeline.cpp:246, 405-410): `renderPass` = a *compatible* render pass per
  (colour formats, depth format), made with LOAD/STORE. Compatibility ignores load/store ops and layouts, so one
  pipeline serves every variant; `PipelineKey` already holds the formats, so the pipeline count does not change.
- Null colour slots become `VK_ATTACHMENT_UNUSED`, in the render pass and the pipeline alike. The present pass
  (nvn_present.cpp:244, 336, 395) is the in-tree model.

**Expected:** sub −1.5 to −2.5 ms (est., if the benchmark confirms): most of BeginRendering's ~2.4 ms, part of reset
(0.9) and end (0.4). **Risk:** medium: render pass compatibility and UNUSED slots; framebuffers die only after retire,
as views; the pipeline cache's driver blobs change (the first run recompiles). **Verify:** RUN c1
NX_RENDER_PASS_OBJECTS=1 against =0 (replay median; BeginRendering and Reset shares with `agg2.py`); frames; the Mac
headless course with the switch on (MoltenVK handles both).

### C2. A command pool per slot, reset at retire

**Today:** one pool with `RESET_COMMAND_BUFFER_BIT` (nvn_vk.cpp:223-226), and `begin_part` resets each part on the
replay thread (nvn_frames.cpp:190-191). `vkResetCommandBuffer` is 5.7-9.0% in three profiles, much of it freeing the
driver's per-pass objects (munmap).

**Change:** `Slot::pool` without the reset bit, `cbs[]` allocated from it. `retire_oldest` (nvn_frames.cpp:160)
calls `vkResetCommandPool(slot.pool, 0)` after the fences, or hands it to a janitor thread with a per-slot "ready"
flag that `begin_part` waits for (normally already set). `begin_part` then only begins. Khronos' command-buffer sample
measured pool resets as the cheapest option.

**Expected:** sub −0.6 to −1.2 ms (−0.3 to −0.5 after C1). **Risk:** low. **Verify:** RUN c2 against `NX_CB_RESET=1`
(today's path): `vkResetCommandBuffer` → ~0 in the profile; `other` and `replay` medians.

### C4. Object lookups during a replay

**Today** (`get<T>`, nvn.h:91-110): the thread-local 8-entry cache is valid only while `objects_lock().generation`
is unchanged. The game's threads take the lock alone for every texture re-initialisation (~180 per frame), so during
a replay it mostly misses: a shared lock, `unordered_map::find`, the incarnation map and `dynamic_cast` (8.5%).

**Change:** while `t_replay_mark != 0`, a per-submit open-addressing table from guest address to `Object*`, cleared at
`draw_begin_submit`. It is stable for the whole replay: `find_object(guest, mark)` answers "as of the mark" (a later
re-initialisation records the incarnation that was current), and retired objects are drained only between replays
(`drain_objects`). Null results are not cached (an object created after the mark resolves to the current one, #64).
A type tag in `Object` replaces `dynamic_cast`. `NX_RESOLVE_CACHE=0` turns it off.

**Expected:** sub −0.8 to −1.2 ms. **Risk:** low-med (#55/#64 class): it must give exactly `find_object`'s answer;
check under `NX_TRACE`. **Verify:** RUN c4: `get<Texture>` share → < 2%; the `replay lookups resolved a texture
re-initialized` counter unchanged per 600 frames; frames.

### C5, C6, C7, C8, C9

- **C5, the CallList closure once per submit.** In `gpu_submit`, resolve the CallList graph once into a per-submit
  table from handle to `shared_ptr<CmdList>` with each list's `own_dispatches`. `frame_plan_split` sums over it and
  `replay_list` uses it instead of `command_list()` (nvn_cmd.cpp:336: a mutex and a map per call). It removes
  `count_dispatches` (3.8%) and most of the replay loop's map lookups. Resolution stays at submit time.
- **C6, last-hit caches:** a per-thread [begin, end) → pool for `pool_at_gpu`/`guest_bytes` (nvn_draw.cpp:27); for
  `lookup_handle`'s pool history, consult it only when the slot's epoch changed after the mark.
- **C7:** R6 from PLAN.md (padding to the reflected maximum offset), plus open addressing for the `dedup_upload` tree
  (5.4%).
- **C8, scudo cache** (platform_android.cpp:562): `mallopt(M_CACHE_COUNT_MAX, 64)` and `mallopt(M_CACHE_SIZE_MAX,
  4 MB)` keep the driver's large per-pass blocks in scudo's secondary cache instead of munmap/madvise (~2-3% under the
  driver's calloc/free). Measure; superseded if C1 removes those allocations.
- **C9, push descriptors** (`VK_KHR_push_descriptor` and `VK_KHR_descriptor_update_template` are present). The ~199
  sets made per frame cost allocate 1.1% + update 1.8%. Push those the submit's set cache misses, with a template per
  layout (data: the VkDescriptorBufferInfo/ImageInfo array in binding order); keep the cache for hits. −0.1 to −0.3 ms:
  low priority on Android, but also the candidate fix on the Mac (§8).
- **`VK_EXT_extended_dynamic_state{,2}`: not for CPU.** In course 2-9 pipelines are created per 60 frames and the
  lookup is 2.1% (with the `last` fast path); dynamic cull, front face, depth/stencil and topology class would add
  vkCmdSet* calls per draw. Use it only to cut the *number* of pipelines P1 must pre-create, after testing the driver
  (emulators report EDS quirks on some proprietary versions).

### L1. Vulkan recording on a worker thread (minimal design)

The replay runs inline on the game's render thread (`QueueSubmitCommands` → `worker_run` runs at once without
`NX_REPLAY_THREAD`). L1 adds one "vk" thread on a big core that executes a stream of recorded Vulkan commands.
- **What moves:** the 33 `p_vkCmd*` the runtime uses, `vkBeginCommandBuffer`/`vkEndCommandBuffer`/
  `vkResetCommandPool`, `vkQueueSubmit` (only the vk thread touches the queue), and the whole present (acquire,
  `present_pass_record`, submit, `vkQueuePresentKHR`): the swapchain has a single owner, so acquire and present never
  race.
- **Thunks:** when L1 is on, the `p_vkCmd*` global pointers (vk_gen.h) are swapped at init for thunks that append
  `{op, args}` to the slot's arena, deep-copying pointer arguments (rendering/render-pass begin info, attachments and
  clear values, barrier arrays, copy/blit/clear regions, descriptor sets with dynamic offsets, vertex buffer arrays,
  push-constant bytes, viewport, scissor). Unlike Ryujinx's ThreadedRenderer (MIT; ideas only) no proxies are needed:
  pipelines, sets, images, views and framebuffers are still created synchronously on the replay thread.
- **Synchronisation:** each queued part gets an atomic "submitted" flag set by the vk thread. `retire_oldest` and the
  write-back wait first wait for it, then for the fence, so the write-backs after the first part keep their meaning.
  Descriptor pools, the ring and images keep their retire rules: an object dies only after retire, which needs the
  part executed. Debug readers (gpu profile, snapshots, dumps, zero-draw queries) drain the stream first.
- **Expected:** after C1/C2 the vkCmd*, end and submit shares (~12-15%, ~2 ms) leave the critical thread, minus
  ~0.3-0.5 ms of encoding (~1,900 records per frame at ~0.2 µs): **sub −1.3 to −2 ms**. Without C1,
  BeginRendering's 2.4 ms moves too (−3.5 to −4.5 ms), at more total CPU and power.
- **Effort/risk:** L, med-high (ordering against device-level calls, swapchain re-creation, window changes).
  `NX_VK_THREAD=0` turns it off. **Verify:** RUN l1 NX_VK_THREAD=1 (replay median, the vk thread's busy share, fence
  waits); frames; 5 runs with `detect_explode.py` (#76 class).

## 5. Hitches (B5)

**Data (§0.4):** ~40 main-thread frames over 33 ms per 600 in every lock-off run, at 21 or 48 fps, against ~9 with
the 30-fps lock; 31-42 present intervals over 50 ms. Not pipelines, ASTC or uploads; the harness adds 1-2 per 600.

**Hypotheses, most likely first:**
1. **The game waits on the replay.** `MemoryPoolInvalidateMappedRange` and `MemoryPoolFinalize`
   (nvn_objects.cpp:114-120, 191-195) call `wait_submits_so_far()`: every submit called so far, including the one
   just submitted (up to ~18 ms of replay away). `SyncWait` waits for its fence's submits. On a Switch an invalidate is
   cache maintenance, not a wait. With the lock off the replay (18.6 ms) is slower than the game's 16.7 ms frame, so a
   periodic invalidate (GPU timer or report readbacks, for example) blocks for one to two replays; then the replay
   thread starves while the main thread catches up: the >50 ms present gap. With the lock on, the replay keeps up and
   the waits rarely block (9 per 600). The only log line, `1 CPU waits blocked`, prints at 1 and then every 1000, so
   ~700 blocks per course would not show.
2. **Major faults** on the ~1.9 GB of swapped guest memory (MEMORY.md §2) in the game's periodic work.
3. **Game-side periodic work**, about 1 in 15 frames.

**I1, instrumentation (S):**
- Per wait site (SyncWait, QueueFinish, PoolInvalidate, PoolFinalize), per 600 frames: calls, blocked calls, blocked
  ms (in `wait_submits`, nvn_queue.cpp:48, with the site passed in).
- Per 60 frames, for the main and replay threads: `getrusage(RUSAGE_THREAD)` deltas (`ru_majflt`, `ru_minflt`,
  `ru_nvcsw`, `ru_nivcsw`) in `frame_thread_tick` (frame_stats.cpp:130). They also explain the replay thread's 3.4 ms
  off-CPU.
- A `hitch:` line per main-thread frame over 33 ms: frame index, blocked ms per site, majflt, pipelines and images
  created that frame.
- `MEMWATCH=0` in `wonder_phone_course.sh` to skip dumpsys.

**H1, the fix if hypothesis 1 holds (M):** track, per pool range, the last submit sequence that can write guest memory
there (compute write-backs, report and counter targets, texture-to-buffer copies). An invalidate waits only for that
sequence (usually done already: no wait). Finalize keeps the full wait if it is rare (I1 tells), else it waits for the
last submit that *referenced* the pool (a per-pool mark set during replay). `NX_WAIT_ALL=1` keeps today's waits.
**Expected:** most of the ~40 hitches: +4 to +7 fps course mean at the same replay cost (est.). **Risk:** medium: a
missed writer means a stale CPU read (the game-logic class, #59); run the `NX_RACE_CHECK`-style gate of PLAN.md §3.3.
If hypothesis 2 holds instead, MEMORY.md items 12-18 move ahead of the GPU items.

**Verify:** RUN h1 (with `MEMWATCH=0`): "more" in `main-thread frame time` → < 5 per 600; present intervals over
50 ms → < 3 per 600; frames, and 5 runs with the explode detectors.

## 6. Pacing and the 30-fps lock (C)

### P3. When to make `NX_MAX_INTERVAL=1` the Android default

- **Today's trade:** lock on gives 30 fps at the correct game speed; lock off gives 45-50 fps, but the game advances
  16.7 ms per frame, so every late frame runs it slower (~17% at 20 ms frames).
- **Default it when** two course runs show ≥ 95% of main-thread frames < 15.8 ms, ≥ 95% of present intervals at
  14-19 ms and fewer than 5 intervals over 50 ms per 600: realistically after C1, C2, C4 and H1, with G0 showing the
  GPU at ≤ 14 ms.
- **Before that, an adaptive clamp** (`NX_GOVERNOR=1`, `frame_time_probe`, nvn_queue.cpp:209-231): count slow frames
  (`fw[0x118]` ≥ 15.83 ms) in a sliding window of 60. While under 10% are slow, clamp the game's maximum interval to 1
  (fields 0x24c/0x264/0x268, as `NX_MAX_INTERVAL` does); otherwise leave the game's governor alone. Isolated hitches
  and compile bursts then no longer lock 30 fps for 15+ frames, while sustained overload still drops to 30 at the
  correct speed. With today's 59% slow frames it changes nothing; it pays off as the CPU items land.

### P2. Display-locked pacing, concretely

Do this once the frames fit. Until then, MAILBOX on the 120 Hz panel shows 17-20 ms frames better than FIFO at 60,
which turns each of them into 33 ms.
1. **Frame rate** (`make_swapchain`, nvn_vk.cpp:287, Android branch): after creating the swapchain and after each new
   window (`window_generation`), call `ANativeWindow_setFrameRateWithChangeStrategy(android_window(), 60.f,
   ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS)` (API 31),
   else `ANativeWindow_setFrameRate` (API 30), through `dlsym` (the build targets android-29). The panel then runs at
   60 Hz or a multiple of it.
2. **FIFO** (`NX_ANDROID_FIFO=1`, later the default): `presentMode = FIFO`, `minImageCount = max(caps.minImageCount,
   3)` (MAILBOX takes 5 today). FIFO's back-pressure paces at the display. Desktop keeps MAILBOX (#62).
3. **The game's vsync from the display** (`vsync_thread`, hle_vi.cpp:46-56, Android): a thread with `ALooper_prepare`,
   `AChoreographer_getInstance()` and `AChoreographer_postVsyncCallback` (API 33, `dlsym`), else
   `postFrameCallback64` (API 29). In the callback, signal `g_vsync` when `frameTimeNanos` advanced ≥ 16.67 − 2 ms
   since the last signal (every other callback at 120 Hz, every one at 60), then re-post. A watchdog falls back to the
   timer if no callback arrives within 50 ms. The game's clock and the panel's are then one.
4. **`pace_present`** (nvn_queue.cpp:248-259) stays as a cap, with `next` snapped to the Choreographer's last vsync
   plus k × 16.67 ms, so presents land just before a vblank instead of at a free-running phase.
5. **Measure on the display side** with `VK_GOOGLE_display_timing` (present): `vkGetPastPresentationTimingGOOGLE`'s
   `actualPresentTime`. Log a histogram of real display intervals and a "missed vblank" count per 600, next to the
   CPU-side one (frame_stats.cpp:156). Swappy (Apache-2.0) does the same; no library is needed for the idea.

**Expected:** display intervals 16.7 ± 0.5 ms when frames fit, +1 frame of latency. **Risk:** medium: a late frame
shows 33 ms (as it already did, for the eye); Android only. **Verify:** RUN p2 NX_ANDROID_FIFO=1
NX_SET_FRAME_RATE=60 NX_VSYNC_CHOREOGRAPHER=1: ≥ 95% of display intervals at 16.7 ± 1 ms in course.

## 7. Is an AGI or Snapdragon Profiler capture needed?

**Not for the CPU items:** the resolved profiles are enough (§0.1). **Yes for ranking the GPU items**, in half a day:
Snapdragon Profiler (S23 supported; realtime metrics and trace) or AGI's system profile, on a debuggable or
profileable build, in course. It decides:

| Question | Decides |
|---|---|
| GPU frequency and % busy at ~48 fps (cross-check of G0) | how much GPU work is really needed |
| Binned or direct (FlexRender) and bin count, per 960x540 pass | G2's value (bins), and the LOAD/STORE cost |
| GMEM load and store bytes per pass | G3, and LOAD → CLEAR/DONT_CARE candidates |
| UBWC per image | G1 before and after |
| Idle gaps between passes | G4 |
| fs 322e146b: ALU utilisation against texture stalls; the de5d38a4 pass: vertices, binning, LRZ | G8 (fp16), geometry/LRZ work |

Qualcomm does not expose its binned/direct heuristics; the documented direct-mode trigger is "a high ratio of texture
samples in vertex shaders to vertices". Only a capture shows the mode per pass.

## 8. MoltenVK: dynamic uniform offsets render black (D)

**Facts:** `program_layout` (nvn_pipeline.cpp:209-231) turns set 0's single uniform buffers into
`UNIFORM_BUFFER_DYNAMIC`; shared bindings carry `stageFlags` VS|FS (binding 0, the support buffer, is declared and
written by both stages). Offsets are 256-aligned and in binding order (nvn_sets.cpp:508-514), the descriptor offset
is 0, the range up to 64 KB. On the Mac (MoltenVK 1.4.2 from Homebrew, macOS 14.3) every frame was black and the GPU
hung near the world map, even with the set cache off and without compute. Adreno renders correctly.

**How MoltenVK does it:** with Metal argument buffers, dynamic offsets go through SPIRV-Cross: each stage's MSL adds
an entry of a per-stage "dynamic offsets" buffer to the pointer it reads from the argument buffer (since 1.0.37).
1.4.1 fixed this area ("Ensure dynamic buffer offsets get a unique name", "missing use of add_local_variable_name for
dynamic offset resources"). Issue #1870: resources bound in both stages with argument buffers got wrong usage and
residency flags.

**Hypotheses, most likely first:**
1. **The argument-buffer dynamic-offset path mis-indexes the offsets of a binding visible to both stages.** One stage
   then reads its uniforms at the ring's start (zeros or garbage: black), and garbage loop counts or indices in the
   uber shaders give the GPU timeout.
2. **Residency:** the ring is made resident (`useResource`) for one stage only; the other reads invalid memory, a
   GPU fault or hang (the #1870 class).
3. **A range or offset rule Adreno tolerates** (offset + range beyond the bound buffer): the validation layer rules
   it in or out.

**Experiments** (Mac headless course 1-1 with `NX_DYNAMIC_UBO=1`, one run each):
- **E1:** `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`. Correct → hypothesis 1 or 2.
- **E2:** dynamic only for bindings with one stage bit (`__builtin_popcount(b.stageFlags) == 1` in `program_layout`).
  Correct → the shared binding is the trigger.
- **E3:** `VK_LAYER_KHRONOS_validation` (the dynamic-offset VUIDs) plus `MTL_DEBUG_LAYER=1` and
  `MTL_SHADER_VALIDATION=1` for the hang: names an out-of-bounds read.
- **E4:** if E1 fixes it, a current MoltenVK main build, then an upstream report with a minimal two-stage repro.

**Fix to test:** set 0 through `VK_KHR_push_descriptor` on MoltenVK (C9). MoltenVK encodes push descriptors as direct
`setBuffer:offset:` binds, with no argument buffer and no dynamic-offset translation, and it gives the same CPU win (no
set allocation or update per draw). Fallback: keep E2's single-stage rule on the Mac. The Mac already runs course 1-1
at 60 fps, so this is not blocking.

## 9. Order of work

1. **Measure (S each):** G0 and I1; `NX_RP_BENCH` (C1 step 0); `bases.py`/`full1.py` into
   `tools/android_profile_report.py`; one SDP/AGI capture (§7).
2. **Cheap CPU (S):** C2, C5, C6, then C4. Checkpoint: replay ~14 ms.
3. **C1** if the benchmark says so, else L1 moves up. Checkpoint: replay ≤ 12.5 ms.
4. **Hitches:** H1 or the memory items, per I1. Checkpoint: < 5 intervals over 50 ms per 600.
5. **GPU, by the capture:** G1, G2, G5A, G4, G3, G6. Checkpoint: G0 total ≤ 14 ms at 60-fps load.
6. **Pacing:** `NX_GOVERNOR=1` as soon as step 3 lands; P3 and P2 at PLAN.md §1's acceptance.
7. **Only if needed:** L1, then C7, C9, G7, G8.

Record each checkpoint in MEASUREMENTS.md: course fps; replay/other/encode/present; the >50 ms interval count; the
main-thread histogram; the G0 total; the top 6 driver entry shares.

## 10. References

- Qualcomm, Adreno GPU on Mobile, Best Practices (MUTABLE_FORMAT before Adreno 750, UBWC, UBOs over push constants):
  https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html
- Qualcomm, Game Developer Guide (FlexRender binned/direct, direct-mode triggers):
  https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/overview.html ,
  https://docs.qualcomm.com/nav/home/pc_best_practices.html?product=1601111740035277 ; GMEM loads:
  https://docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html
- Khronos: VK_QCOM_render_pass_store_ops (STORE_OP_NONE):
  https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VK_QCOM_render_pass_store_ops.html ;
  VK_EXT_load_store_op_none: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_load_store_op_none.html ;
  formats (unsigned float conversion): https://docs.vulkan.org/spec/latest/chapters/formats.html ; tile-based
  rendering best practices: https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html ; descriptor
  dynamic offsets: https://docs.vulkan.org/guide/latest/descriptor_dynamic_offset.html
- Khronos Vulkan Samples, command buffer usage (pool reset):
  https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/command_buffer_usage/README.adoc
- Arm, Vulkan synchronization on Mali (bubbles, fragment → vertex dependencies):
  https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware
- Mesa Freedreno/Turnip (GMEM vs sysmem autotune, TU_DEBUG incl. nocb/forcecb for A7xx concurrent binning):
  https://docs.mesa3d.org/drivers/freedreno.html , https://docs.mesa3d.org/envvars.html ; Turnip UBWC rules in the
  release notes: https://docs.mesa3d.org/relnotes/20.0.0.html , https://docs.mesa3d.org/relnotes/22.2.0.html ,
  https://docs.mesa3d.org/relnotes/24.3.0.html , https://docs.mesa3d.org/relnotes/25.0.0.html
- Igalia (D. Piliaiev), Low-resolution-Z on Adreno GPUs: https://blogs.igalia.com/dpiliaiev/adreno-lrz/
- MoltenVK, What's New: https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md ; issues #1870
  (resources bound across stages with argument buffers): https://github.com/KhronosGroup/MoltenVK/issues/1870 , #1326
  (descriptor offset with dynamic buffers): https://github.com/KhronosGroup/MoltenVK/issues/1326
- Android NDK: Choreographer https://developer.android.com/ndk/reference/group/choreographer ; Native Window
  (setFrameRate, setFrameRateWithChangeStrategy) https://developer.android.com/ndk/reference/group/a-native-window ;
  Frame Pacing (Swappy) https://developer.android.com/games/sdk/frame-pacing ,
  https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions
- Ryujinx (MIT), ThreadedRenderer (ideas only):
  https://git.nadeko.net/Ryujinx_Mirror/Ryujinx/src/tag/1.1.1253/src/Ryujinx.Graphics.GAL/Multithreading/ThreadedRenderer.cs
- Android GPU Inspector: https://gpuinspector.dev/ ; Snapdragon Profiler:
  https://www.qualcomm.com/developer/software/snapdragon-profiler
