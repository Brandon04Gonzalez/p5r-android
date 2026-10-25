# Study: contrast, speed, glitches (2026-09-29)

After the characters rendered (DEBUG.md #26), the user reported three problems: far too much contrast, far too slow
(target 60 fps), and graphical glitches. This is the study done before changing anything: four read-only agents
(colour, performance, glitch catalogue, an audio crash found on the way) plus measured runs (NX_GPU_PROFILE, a
20 s `sample` of the process in the course).

## 1. Contrast: one sRGB conversion is lost

- **Measured.** Frame 06750 (Florian's ceremony) against the same shot from the real game (mariowiki
  `Flower_Kingdom_SMBW.jpg`, 4K):

  | | mean luma | std | p5 | p95 | mean saturation |
  |---|---|---|---|---|---|
  | real game | 0.748 | 0.176 | 0.375 | 0.952 | 0.315 |
  | ours | 0.592 | 0.250 | 0.119 | 0.928 | 0.521 |
  | ours + one sRGB encode | 0.755 | 0.192 | 0.363 | 0.968 | 0.337 |

- **Patches agree too.** Character Select panels measure (9,14,9) where the reference is (64,77,64): that is exactly
  the sRGB decode of the reference. Sky, pedestal and grass match the same way.
- **The engine expects hardware sRGB.** Its tonemaps (ACES fit + 3D LUT) come in pairs: one ends with
  `pow(x, 1/2.2)` for UNORM targets, one does not for sRGB targets.
- **Pure texture copies are right, and computed colours are wrong.** Examples of each:
  - Right: character art, the title key art.
  - Wrong: lighting, UI panels, blended layers.
  - So the conversion is lost per draw, not once at the end.
- **Cause.** The game calls `nvnTextureViewSetFormat`, but the runtime ignores view formats:
  - `read_view` (nvn_objects.cpp) never reads the format.
  - `ViewInfo` has no format field.
  - Every view and attachment is made in `img.format`.
  - Images lack `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`.

  So the sRGB views (0x38 over RGBA8 0x25) lose their decode on sampling and their encode on writing, and blending
  happens in the wrong space.
- **Fix.**
  - Carry the view format through the runtime:
    - add a format field to `ViewInfo`, read by `read_view` via `nvnTextureViewGetFormat`;
    - pass it to `view_of`, `attachment_view` and `storage_view`, including their cache keys;
    - add it to the `TargetViews` blob and to `color_formats` in the pipeline key.
  - Create colour images with the mutable-format flag.
  - Keep storage views UNORM (Metal cannot write sRGB storage).
  - Check that `gpu_present` does not decode an sRGB source into the UNORM swapchain.
  - Success check: frame 06750 mean luma ≈ 0.75 and p5 ≈ 0.37.
- **Colour differences left after the offline encode** (pink clouds, yellow/purple hills, dark red disc behind the
  Wonder emblem) may be the same bug inside the LUT, which is rendered slice by slice through views. Re-check them after
  the fix.

## 2. Speed: from ~15 to 60 fps

### Measurements (course)

| Measure | Value |
|---|---|
| Frame time | ~66 ms, i.e. ~15 fps |
| Submits per frame | 1 |
| Replay of the command lists (CPU) | 12–24 ms |
| "GPU wait" | 37–73 ms |
| Upload per frame | 50–70 MB |
| Draws replayed per frame | ~450 |

- **The game's render thread** (guest thread 0x52) is busy 46% of the time. It waits in MoltenVK's fence
  (`MVKSemaphoreImpl::wait`) for 44% of all samples. Every other game thread is mostly idle.
  - The game is not CPU-bound in its own code; the recompiled code is efficient.
  - The frame is the sum of: game logic, recording, replay, Metal encoding and GPU time. Nothing overlaps.
- **GPU profile.** "Outside passes (clears, uploads, copies)" is the largest line, 24.5 ms/frame, because every clear
  and texture upload is its own encoder with full barriers. The biggest render passes are 1408x792 scene passes of
  1–5 ms each.
- **CPU hot spots in replay:**
  - memcpy/memset of uploads;
  - `texture_for`, `pool_at_gpu`, `sampled_ready`, `forget_image_views`;
  - `get<T>`, which takes a global mutex, a map lookup and a dynamic_cast;
  - `dynamic_uniform_bindings`, a mutex plus a map;
  - 4 descriptor sets allocated and updated per draw;
  - a heap `std::string` built for the key of every pipeline lookup.
- **Why ~130 KB is uploaded per draw:**
  - all 15 vertex streams, even unused ones;
  - uniform banks padded to 64 KB when indexed and 4 KB otherwise;
  - storage buffers of at least 64 KB;
  - the 4 KB c0 driver constants;
  - no deduplication across draws.

### Plan, by expected gain

| # | Step | Expected gain | Effort |
|---|---|---|---|
| 1 | **Async GPU**: the existing `private/wonder/patches/async_submit.patch` (frames in flight) with `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`, so Metal encoding leaves the render thread; write back only storage bindings not decorated `NonWritable` | −20 to −30 ms | small |
| 2 | **Fewer encoders outside passes**: clears as `loadOp = CLEAR` inside the pass, texture uploads batched before the pass instead of splitting it, barriers scoped instead of ALL→ALL | most of the 24.5 ms | medium |
| 3 | **Upload trims**: only the streams the pipeline reads; per-submit dedup by (address, size) invalidated by `UpdateUniformBuffer`; indexed banks clamped to their real size in the shader instead of 64 KB padding | 60 MB → ~10 MB, −5 to −8 ms | 1–2 days |
| 4 | **Replay CPU**: push descriptors (`VK_KHR_push_descriptor`) or a descriptor-set cache, a lock-free object table instead of `get<T>`, pipeline key hashing without allocation | −3 to −6 ms | 2–3 days |
| 5 | **MoltenVK settings** (`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`, prefill) | a few ms, to measure | minutes |
| 6 | **One pacing clock**: drive the game's vsync event from real presents (the vsync thread and the FIFO swapchain drift today) | smoothness near 60 | small |
| 7 | **Long term**: import guest memory pools as Vulkan buffers (M1 unified memory, `VK_EXT_external_memory_host`), with no uploads and no write-backs | the rest | 1–2 weeks |

- **Expected result.** With 1–4 the frame should become max(CPU ≈ 15–20 ms, GPU ≈ 15–20 ms) instead of their sum,
  which is 50–60 fps.
- **The last step to a stable 60** depends on how much GPU time is left after step 2. Measure with the Metal System
  Trace (Instruments) at that point.

## 3. Glitches (catalogue)

| # | Defect | Probable cause | Confidence |
|---|---|---|---|
| G1 | Wrong colours/contrast everywhere | §1 (sRGB view formats) | high |
| G2 | Character Select background not blurred | the blur samples level 0 or reads before its pass writes; trace its passes | medium |
| G3 | Black dots on the Wonder-emblem ring (frame 07350) | sparkle particles: GPU particles reading compute output of the same submit, or a dummy texture | low-medium |
| G4 | Portal inside the emblem is flat dark; the real one is an iridescent swirl | part G1; re-check after it | medium |
| G5 | Button icons show as "?" ("Press ?+? to Start") | the Nintendo extension shared font (font 5) is replaced by Arial Unicode; needs the font from the user's own firmware dump | high |
| G6 | **Crash**: Metal asserts "5 mipmap levels, but the dimensions (1,1,1) can only support 1" (ended two runs right after Bowser arrives) | `img->levels` (nvn_vk.cpp:303) is not clamped to floor(log2(max(w, h, d))) + 1 | high |
| G7 | Stutter: 100–440 ms stalls | §2 | high |

- **Latent** (stubs the game calls, with no visible defect yet):
  - stencil reference and masks: the reference is fixed at 0 and both masks at 0xFF;
  - polygon offset (depthBias never enabled);
  - depth bounds, depth clamp, line width, point size;
  - multisample state: always 1 sample, no alpha-to-coverage;
  - alpha test and logic op, both decoded but unused;
  - `ClearDepthStencil` ignores its masks;
  - D24S8 is always sampled as depth.
- **Expect** z-fighting on decals, and stencil-masked effects in later courses.

## 4. Audio crash at startup (found during the study)

- **Symptom.** Intermittent, 2 of 4 headless runs: `signal 11, data address 0` in
  `nn::audio::ApplyLightLimiterEffect` (sdk+228220), called from `CommandListProcessor::Process`. The
  `LightLimiterState` is all zero, so it was never initialized.
- **Cause** (confidence ~70%). The "initialize" status of a new effect is carried by exactly one `RequestUpdate`. Our
  render thread signals the game's audio event every 5 ms whether or not a render happened in between. When the game's
  sound loop falls behind, it does two updates before one render, and the second overwrites the status. Then `Process`
  applies the effect with an uninitialized state.
- **Fix.**
  - `AudioRenderSystemUpdate` waits, outside the lock, until at least one render ran since the previous update, which
    is the console's rhythm.
  - A defensive guard in the Process HLE resets the status to "initialize" for enabled limiter commands whose state is
    still null, logged once.

## Order of work

1. Crash fixes: the mip clamp and the audio pacing.
2. sRGB view formats: contrast and colours.
3. Async GPU.
4. Encoders outside passes.
5. Upload trims.
6. Replay CPU.
7. Pacing.
8. Then the remaining glitches (G2–G5) and the latent stubs.
