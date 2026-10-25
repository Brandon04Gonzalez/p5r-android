# Wonder rendering glitches: study results and fix plan

Five read-only studies (2026-09-30) on what still renders wrong on the Mac. Each item has the evidence, a one-run
check and the fix. Order = expected visual impact per unit of work. Tracked in milestone **W6** on GitHub.

## G1. Sampler state is lost (critical regression)

- **Cause.** When commit c41b534 split `nvn_sampler.cpp`, it dropped every sampler proc:
  - `nvnSamplerInitialize` / `Finalize`;
  - `nvnSamplerPoolRegisterSampler` / `RegisterSamplerBuilder`;
  - the eight `nvnSamplerGet*`.
  - Nothing writes `DescriptorPool::samplers` any more, and logs since then show `first call of nvnSamplerInitialize`
    (a trace stub).
- **Effect.** Every texture samples with `SamplerInfo{}`:
  - nearest filtering;
  - clamp-to-edge;
  - `maxLod` 0.25, so mip 0 only;
  - shadow compare ALWAYS.
- **Explains** (three studies agree):
  - #28 blocky sky (nearest upscale of a low-res buffer);
  - #27 missing blob shadows (1,450 shadow-compare shaders), dead water shimmer (scrolling texture clamped) and
    sparkles;
  - #29 white streaks on the world map (clamped strip textures);
  - most of #26: the dark materials all read their AO with `textureLod(ao, uv, 1.0)`, AO is rendered only into
    level 1, and with `maxLod` 0.25 they read the never-written level 0 (see G2).
- **Fix.** Restore the procs from `c41b534^:runtime/src/nvn/nvn_sampler.cpp`, keeping `sampler_for`.
- **Check.** One course run: sky smooth, shadows under actors, Goombas lit. `NX_TRACE_DRAWS` shows non-zero
  min/mag/wrap.

## G2. Dark materials (#26)

- **Evidence.** The 11 fragment shaders that render dark all sample the 960x540 R8 SSAO target (2 levels) at LOD 1,
  and no correctly lit shader does:
  - characters, including Peach's dress;
  - Goombas;
  - the tree (41f489…).
- **Why it is black.** AO = 0 drops lighting to the `c6[43]` floor. The mask comes from each material's alpha, so
  only some parts of an actor go dark.
- **Fixes.**
  - G1 (LOD).
  - Keep `reuse_image` for GPU-written images whatever the texture flags. The uncommitted `flags & 0x8` gate
    excludes the AO targets, which have flags 0.
- **If still dark.** Force AO = 1 (`NX_AO_ONE` experiment) to separate AO from the depth-EQUAL invariance issue (G9).

## G3. Garbage band after a respawn; aliased transient targets

- **Cause 1.** NVN 0x3f R11G11B10F is stored as RGBA16F (8 bytes), but uploads detile it at 4 bytes per texel. The
  copy reads twice the data: rows side by side, bits reinterpreted, and other uploads fill the bottom half.
  - That is the band's pattern.
  - It is also the "1920x1080 f3f uploaded over memory rendered as 960x540 f1" case, 36 times per run.
- **Cause 2.** A render target (flags 0x8) whose image is new is filled from guest memory, which never holds GPU
  output here: stale data.
- **Cause 3.** `drain_objects()` frees objects before taking the GPU lock (`gpu_submit`, `gpu_present`): a possible
  use-after-free.
- **Fixes.**
  - Widen R11G11B10 to half4 on upload.
  - Add a guard that skips any upload short of the host format's size.
  - Clear (not upload) a fresh render target never written by the GPU; later, model aliasing with a GPU-write
    interval map.
  - Begin passes on UNDEFINED images with loadOp CLEAR.
  - Move `drain_objects` under the GPU lock.
- **Check.** `NX_ALIAS_MAGENTA` patch: the band turns magenta if cause 2 is it.

## G4. R11G11B10F semantics

- Alpha must read 1 (swizzle A=ONE).
- DST_ALPHA blends must use 1.
- Negatives clamp to 0 (Maxwell unsigned float); otherwise NaNs come back in `pow`/`log`.
- `view_format` must compare host texel sizes.

## G5. Compute results within a submit

- **Cause.**
  - Draws and dispatches later in the same submit read guest memory, not what an earlier dispatch wrote.
  - Two dispatches writing one buffer overwrite each other at write-back.
  - Indirect arguments are stale.
- **Effect.** Particles and GPU-driven effects late or lost.
- **Fix.** A per-submit map from guest range to the written copy: bind that copy, and write back once.

## G6. Clears ignore scissor and channel mask

- **Effect.** Atlas tiles wiped, alpha-only clears destroy RGB. (The hollow "Too bad!" is by design: DEBUG #56.)
- **Fix.**
  - Scissored clears: `vkCmdClearAttachments` with rect = scissor ∩ extent.
  - Masked clears: a small fullscreen draw with that write mask.

## G7. Stubbed state the game uses

- **`nvnCommandBufferBindMultisampleState`.** Alpha-to-coverage is lost (foliage, hair edges): decode it and set
  `alphaToCoverageEnable`.
- **`SetPointSize`.** Only if point lists appear.
- **Dual-source blend factors.** The SPIR-V has no index-1 output: log and bake the variant.
- **Alpha test.** Decoded, never applied (all traced draws use ALWAYS so far).

## G8. 1D textures sample the zero dummy

- **Cause.** Ryujinx samples 1D textures as 2D, but the runtime creates them as Vulkan 1D images, whose 2D view is
  refused, so the draw gets the black dummy.
- **Fix.** Create NVN targets 0/3 as 2D / 2D-array with height 1.
- **Other shape mismatches.** 2D sampled as a cube and similar also get zero: log the fs hash and slot.

## G9. Float and depth precision

- **Fast math.** MoltenVK compiles MSL with fast math: `isnan` in 970 shaders folds away, and the FMZ selects can
  fold.
  - Check: `MVK_CONFIG_FAST_MATH_ENABLED=0` A/B.
  - Fix: float-controls in the harness plus ON_DEMAND.
- **Saturate/min/max.** Maxwell saturate maps NaN to 0 and FMNMX drops NaN. Ryujinx emits clamp/min/max, so NaN
  reaches targets: patch FPSaturate and use NMin/NMax.
- **Depth-EQUAL shading pass.** It follows a prepass drawn with other vertex shaders; any position difference fails
  EQUAL. Check with a LESS_OR_EQUAL switch; confirm `preserveInvariance`.
- **Depth clip control.** Log `VK_EXT_depth_clip_control` at startup.

## G10. Separate bugs

Update (DEBUG #58): the character-select blur chain runs once and the game's own stream clears its capture; needs a
console reference before any change. The items below are the original study.

- **#30, character select blur + halftone.** Missing since before c41b534. The `multi_filter_expand` program is
  never bound.
  - `replay_copy` drops copies when `src == dst`; images are shared since #36, so same-image level copies vanish.
  - Allow copies between different subresources of one image; trace the blur chain.
- **Copies within one image.** Same fix, also for mips and atlases.
- **Button glyphs.** The shared font needs `master_key_16` (the user's own keys).
- **"Welcome to the Flower Kingdom!" dark panel.** It is the course-intro layout, not a video: likely correct.

## Order of work

1. G1 samplers, and revert the `flags & 0x8` reuse gate. One run: sky, shadows, Goombas, map streaks.
2. G3 R11G11B10F upload, fresh render targets, `drain_objects` lock.
3. G4 R11G11B10F semantics; G8 1D textures.
4. G9 fast-math A/B, then the saturate/min/max patch.
5. G5 compute results; G6 clears; G7 multisample state.
6. G10 character-select blur chain, same-image copies.

Every step: an env switch for bisecting, a DEBUG.md entry, before/after screenshots, a character-select
regression check and a course run.

## W7: glitches from the course 1-1 recording (milestone W7, issues #43-#48)

Fixes are in [DEBUG-2.md](DEBUG-2.md), entries 59-64. All pass the headless course; the windowed checks are pending.

| Issue | Glitch | Fix | DEBUG-2 |
|---|---|---|---|
| #43 | Horizon band, exploded meshes | Game CPU waits cover submits being replayed | 59 |
| #44 | Black silhouettes, black/white wedges | `NX_EQUAL_SLACK` depth bias on EQUAL colour passes | 60 |
| #45 | Flat green silhouettes | A slot or object empty at the mark uses the current one | 64 |
| #46 | 60 fps | Pipelined fences on by default | 61 |
| #47 | Windowed stutter | MAILBOX > IMMEDIATE > FIFO | 62 |
| #48 | Mark gaps | Snapshots retired properly; sampler slot history | 63 |
