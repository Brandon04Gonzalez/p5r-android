# Debugging the NVN → Vulkan path

The tools used to take Wonder from "frames presented but black" to the title screen, and what each bug looked like.
Everything below is switched on with environment variables on the normal build: no rebuild needed.

## Environment variables

They are listed in [SWITCHES.md](SWITCHES.md).

## Bugs found this way (2026-09-28), in order

1. **Only the composite was ever submitted** (24k draws recorded, ~1 per frame replayed). `NX_NVN_LISTS` showed the
   scene lists (625 commands, 29 draws) ended but never submitted. The engine reads
   `nvnCommandBufferGetCommandMemoryUsed` right after `EndRecording` (main+6fef0) and drops a list that "used
   nothing"; ours restarted at 0 with each recording. Fix: usage accumulates across recordings, reset only when memory
   is added (nvn_cmd.cpp).
2. **Solid magenta window** (screenshot `docs/media/wonder/01-magenta-culled-composite.png`). The only draw into the
   window is a plain copy shader; forcing a black texture still gave magenta, so the draw wrote nothing: it was
   **culled**. NVN's front face is in GL terms; with the upper-left window origin Wonder sets, the winding is reversed
   in Vulkan's framebuffer. Fix: invert `NVNfrontFace` (nvn_pipeline.cpp). The magenta was the window texture's
   guest memory.
3. **Title art scrambled in 32-row bands** (`02-title-wrong-block-height.png`). `nvnTextureBuilderGetPackagedTextureLayout`
   returns 1 for every packaged texture, so it is not the BNTX block height. Fix: packaged textures use the fit from
   the BNTX survey (docs/nvn/TILING.md): smallest power of two >= rows * 3 / 32, at most 16 GOBs
   (`03-title-tiling-fixed.png`).
4. **Channel masks read with overlapping outputs**: `nvnChannelMaskStateGetChannelMask` writes four `NVNboolean`
   words; the output slots were 1 byte apart. Fix: 4-byte slots.
5. **Text quads covering the screen / black boxes**: textures are read through swizzles (a font atlas in R8 is
   registered with alpha = R; BC4 masks as 1,1,1,R). Vulkan returns alpha 1 without them, so the SDF text shader never
   discarded. Fix: the builder's swizzle (`nvnTextureBuilderGetSwizzle`) and the registering view's
   (`nvnTextureViewGetSwizzle` in `nvnTexturePoolRegisterTexture`) go into the sampled `VkImageView`.
6. **Everything green** (`04-title-swizzle-nan-green.png`): not a mask, not the target format (RGBA16F instead of
   R11G11B10F changed nothing), not Metal fast math. `NX_DUMP_RAW` showed R and B are **NaN/Inf in the scene**, G
   valid; RGBA8 turns NaN into 0. The NaN comes from the bloom chain, whose passes render into one mip level of a
   texture while sampling another: NVN selects the level with the `NVNtextureView` given to `SetRenderTargets` and
   to the texture pool, which we ignored (everything went to level 0, the sampled levels were uninitialised). Fix:
   `nvnTextureViewGetLevels/GetLayers/GetSwizzle` read for render targets (recorded as a `TargetViews` blob) and pool
   entries; attachments are single-level/layer views with that mip's extent; images the GPU renders stay in
   `VK_IMAGE_LAYOUT_GENERAL` (sampled and rendered at once). 3D images are created 2D-array compatible, since a
   colour-grading LUT is rendered slice by slice and Metal refuses a 2D view of a plain 3D texture (it aborted with
   `_mtlValidateArgumentsForTextureViewOnDevice`). Result: `05-title-screen.png`, correct colours.
7. **2-3 fps, the GPU busy ~550 ms per frame** on a title screen. Sampling the process: every thread waited on the
   submit thread, which waited on MoltenVK's fence. `NX_GPU_PROFILE` showed ~10 ms per render pass whatever its size
   (a 32x18 pass: 44 ms); dropping the draw calls only (everything else kept) brought the GPU to 9 ms, while
   `rasterizerDiscardEnable` left it at 525 ms, so the cost was before the fragment stage. Cause: **8-bit index buffers**. Metal has none and MoltenVK converts them on the GPU at every draw.
   Widening them to 16 bits on the host: GPU ~10 ms per frame, the title at the game's 30 fps.
8. **After the title, a crash: "call into the middle of a function" main+22e810.** Not a function pointer: a
   `switch` jump table (`adr x10, anchor; ldrh w11, [x9, x8, lsl #1]; add x10, x10, x11, lsl #2; br x10`) whose
   index is a byte with no bounds check. The recompiler's `_bound` walked back to an older `cmp w8, #2` / `b.ne`
   guarding another value and capped the table at 3 entries; case 3 was missing. Fix (recomp/analysis.py): the
   search stops when the index register is redefined, and only a `cmp` followed by `b.hi`/`b.hs` is a bound. The
   recompiler now writes only changed files, so the rebuild after regenerating took 25 s instead of ~15 min.
9. **Metal abort on a cube view with 1 layer**: a shader declaring `samplerCube` given a 2D texture. Views Metal
   cannot make (cube of a non-cube image, 3D of 2D, 1D of 2D) now read the dummy texture.
10. **Input**: the pad reached the game (`hid: first buttons 1`) and A works: after it the game loads the file
    select (`NX_TRACE_FS`). `GetNpadStates` now keeps a 16-sample history like hid (Wonder ignores the count and reads
    all 16 entries), and `BindNpadStyleSetUpdateEvent` signals its event once (the controller is connected).
    Screenshot: `06-file-select.png` (text still missing).
11. **The opening was garbage after compute** (chevron patterns). Compute shaders search the buffer descriptor table
    in `c[0]` to find which buffer an address falls in; it was bound as zeros. Also, storage buffers are copies in the
    upload buffer and Metal does not bound-check, so a shader writing past a small or missing one overwrote the next
    data. Fix: `c[0]` filled with the bound descriptors, storage padded to 64 KB, compute writes copied back to guest
    memory after the submit.
12. **"upload buffer full"**: a busy submit needed 67 MB of the 64 MB ring and wrapped over its own data. 256 MB now.
13. **The opening never advanced: streamed music was never consumed** (`NX_TRACE_FS` showed each `.bwav` read 2-3
    times, then never). `ExecuteAudioRendererRendering` returned 0x40299 in 0.5 us: it only renders when the render
    system's execution mode and rendering device are manual/CPU (`GetExecutionMode`, `GetRenderingDevice`, render
    system +0x2d0/+0x2d4), and the game opens the renderer in the automatic mode. Switched after opening: ~250 us a
    frame, the streams play.
14. **The Mac froze: the process reached 85 GB.** GPU images of destroyed textures were never freed (no
    `vkDestroyImage` anywhere) and the opening creates textures continuously; with unified memory the machine ran out
    of RAM and swapped until it hung. Fix: a texture's image goes to a graveyard freed at the next submit/present,
    with its cached views. The same run stays at ~4.5 GB. The memory guard (`NX_MEM_LIMIT_GB`) now stops any leak
    before it matters.
15. **Dialog text invisible** (open). With `NX_SWAP_FS` the glyph quads land correctly; without the view swizzle
    they are solid boxes, so the pipeline works and the sampled coverage is 0. The glyph atlas (GPU copy and guest
    memory) holds the glyphs at the recorded UVs (`NX_TRACE_GLYPHS`). The translated vertex shader picks each corner's
    texture coordinate through a table in `c[1]` (values 0x378, 0x390, 0x3a0, 0x450, 0x430, 0x440) and compares the
    looked-up value with a single constant (888, 1072) the offline translator resolved; the other corners fall back to
    a position value, so they sample empty atlas. Needs the translator to express that indexed load.
16. **Another "call into the middle of a function"** (main+28b3c0, after the opening dialogs): a jump table whose base
    register (x24) is set by an `adrp/add` on another control-flow path, 0x5000 bytes before; the linear backward
    search found an unrelated `movk`. The recompiler now tries every constant the function puts in the base register
    when the bound is known, and keeps the one whose entries all land on instructions of the function.
17. **The first course showed a mirrored sky; the whole scene was upside down** (`NX_SNAP_DRAWS` mid-frame: the course
    was drawn flipped, sky gradient included; the tonemap then produced half a mirrored sky). Root cause, older than
    all of this: NVN with the upper-left window origin maps NDC +Y to the top of the render target, like D3D; Vulkan
    maps it to the bottom. Bug #2 had been "fixed" by inverting the front face, which made culling right but left
    every render target upside down; some scenes (the title, the opening) came out right because their post chain
    flipped twice, the course did not. A global experiment (`NX_TMP_FLIPY`: negative-height viewports, front face as
    is) made the title, the opening and the course all correct. Fix: every draw uses a negative-height viewport and
    NVN's front face unchanged.
18. **Dialog text invisible (#15) — and 792 other shaders silently wrong.** Found by a parallel investigation of the
    offline translator: the glyph vertex shader's per-corner texture coordinates come from a Maxwell `BRX` jump table
    (0x378/0x390/0x3a0 are jump targets, not data); Ryujinx decodes it into `if / else if / else`, and its SPIR-V back
    end drops `else if` blocks (they become unreachable), so only the first case survived. Fixed in the harness with a
    local patch to `SpirvGenerator.Generate` (docs/nvn/TRANSLATOR.md, "Silent miscompile: `else if` chains"); 793
    blocks regenerated, all validated and compiled to Metal.
19. **EQUAL depth after a pre-pass** — relaxed to LESS_OR_EQUAL when the pass does not write depth (Metal compiles the
    pre-pass and colour-pass vertex shaders separately; bit-identical depth is not guaranteed). Kept as a safety net;
    it was not the cause of #17.
21. **The picture froze at the first talking-flower balloon of the prologue course; the sound played on.** (#20 is
    the integer vertex inputs.) The game was not stuck: guest threads kept running, frames kept being presented and
    scripted input kept driving the logic (walking right, it even finished the prologue course and reached the world
    map). The GPU was gone: `vkQueueSubmit failed (-4)` on every submit from then on, and with
    `MVK_CONFIG_LOG_LEVEL=1` MoltenVK said why: `Caused GPU Hang Error (kIOGPUCommandBufferCallbackErrorHang)`, a
    command buffer that ran too long. Found in three steps. (1) Not the 793 regenerated shaders: the same hang with
    the pre-fix `.spv.orig` files (a spirv folder of symlinks under `NX_DATA_ROOT`). (2) A bisection over the 2,330
    shaders that contain a loop, leaving half of them out of that folder per run (a missing shader skips its draws),
    12 runs: fragment shader `b2f3dc2ff54d1ba4` (uber). (3) Its loop walks a tiled light list: the tile is
    `floor(gl_FragCoord * c[6][62].zw * (16, 9))`, the count is `c[8]` at byte 8196 + 256 * tile, and the loop runs
    that many times. A local trace of the bound banks: `c[8]` bound with 0xb000 bytes (144 tiles) and `c[6][62].zw`
    = 1/384, 1/216, but the same materials also draw into 1920x1080 targets, where the tile reaches ~800 and the count
    is read up to 213 KB into the bank. On Maxwell a constant read past the bound size returns 0 (no lights). Here the
    bank was the bound bytes zero-padded to 4 KB in the upload buffer, the translator declares every bank as
    vec4[4096] and Metal checks nothing: the count came from whatever followed, up to 4 billion iterations. 2,071 of
    the loop shaders read the same light list; which draw hung first depended on what lay in the upload buffer.
    Fix (`clamp_uniform_indices`, nvn_spirv_patch.cpp): when a shader module is created, every uniform access chain
    whose array index is computed gets `UMin(index, 4095)` (a negative index clamps too), and the set 0 bindings read
    that way are bound zero-padded to the full 64 KB (`dynamic_uniform_bindings`, `build_sets`), so every read past
    the data returns 0 as on Maxwell. 5,159 of the 12,751 modules change; all pass `spirv-val`. With it, no GPU hang:
    390 s in the prologue course, then a run from that save through the world map into the first course (Course001)
    until the 5 GB memory guard stopped it. Another process that hangs the GPU makes ours fail as well
    (`kIOGPUCommandBufferCallbackErrorInnocentVictim`): one game run at a time.
22. **The Mac restarted (WindowServer watchdog) during a test run.** The run used a build without #21: a shader hung
    the GPU, WindowServer (which draws the whole desktop) waited in Metal/IOGPU, its watchdog fired twice and macOS
    restarted. Safeguards: a lost device (`VK_ERROR_DEVICE_LOST`) or a submit not finishing within 2 s ends the
    process at once (nvn_vk.cpp), so a hang cannot keep the GPU busy. Test runs must use a build with the current
    fixes, one game process at a time.
23. **Compute pipelines refused: "Threadgroup memory size (49152) exceeds the maximum threadgroup memory allowed
    (32768)"** (compute `0cd5c52ea70a3bb0`, world map and first course; its effect was missing). The translator sizes
    shared memory with `IGpuAccessor.QueryComputeSharedMemorySize()`. The harness did not answer it, so Ryujinx's
    default of 0xc000 applied, and all 21 of Wonder's 71 compute shaders that use shared memory declared `uint[12288]`
    (48 KB). The M1 allows 32 KB, so Metal refused every one of them. The real size is in the control section at
    **+0x754**, right after the work-group size. It is 384 to 2,312 bytes in Wonder, and it is 0 for exactly the 50
    compute shaders without shared memory. The SPIR-V agrees with it. In the 18 shaders with a 32x1x1 work group,
    every access is `shared[(tid * S + k) >> 2]` with a constant `k < S` and `32 * S` equal to the control size (for
    `0cd5c52ea70a3bb0`, S = 60 and the size is 1,920 bytes). The three 16x16 ones are tiles with a border: 800 bytes
    = 20 rows of 40 bytes, and 2,312 = 17 x 17 x 8. This is also the amount Maxwell allocates per work group. Fix
    (`resize_shared_memory`, nvn_spirv_patch.cpp): `ShaderControl::shared_size` reads +0x754, and when a module is
    created, the Workgroup array is redeclared with `ceil(size / 4)` elements. It only ever shrinks the array. The
    harness answers the query from +0x754 too (`StaticGpuAccessor.QueryComputeSharedMemorySize`), so a future
    re-translation declares the right size and the runtime pass does nothing. Offline check
    (`private/wonder/patchwork/test/run.sh`): all 21 modules pass `spirv-val`, and `spirv-cross --msl` gives
    `threadgroup spvUnsafeArray<uint, 480>` for `0cd5c52ea70a3bb0`, not 12288. Metal
    `makeComputePipelineState` refuses all 21 before the fix and accepts all 21 after it (1,920 B for
    `0cd5c52ea70a3bb0`).
24. **Graphics pipeline refused: "Fragment input(s) `user(locn6)` mismatching vertex shader output type(s) or not
    written by vertex shader"** (vs `c5f64e8c84fa824d` + fs `4a9740c442f9ba61`, effect_static). The vertex shader
    writes locations 0-5 and 7, and the fragment shader reads 0-7. Vulkan allows an input that nothing writes (its
    value is undefined), but Metal links the stages by name and refuses the pipeline. On Maxwell such an attribute
    reads as (0, 0, 0, 1): Ryujinx initializes every output the next stage reads to that (Translator.cs,
    `InitializeOutputComponent`). This shader multiplies its alpha by `in6.w`, so 0 would make the effect invisible.
    Fix (`unwritten_inputs` + `default_unwritten_inputs`, nvn_spirv_patch.cpp): at pipeline creation, the fragment
    input locations missing from the vertex shader's reflected outputs become Private variables that start as
    (0, 0, 0, 1). The patch takes out their Location decoration, retypes the access chains to Private pointers, and
    removes them from the entry point interface (SPIR-V 1.3). The fragment module cache is keyed by that mask as
    well. When a pipeline needs it, the log says `inputs 0x40 not written by the vertex stage read (0, 0, 0, 1)`.
    Offline check: `spirv-val` and `spirv-cross --msl` pass, and the MSL fragment keeps inputs `locn0-5,7` with
    `float4 _45 = float4(0.0, 0.0, 0.0, 1.0)`. Metal `makeRenderPipelineState` on the pair fails as above before the
    fix and links after it. As a worst case, all 7,662 fragment shaders with inputs were patched with every input
    marked unwritten: 0 refused, 0 `spirv-val` failures, and a sample of 510 passed through `spirv-cross --msl`.
25. **Huge flat pale-green wedges across the course** (and still no characters). They came with the work-in-progress
    SPIR-V patch that retypes vertex inputs to integers for integer attribute formats (`patch_int_inputs`): an A/B run
    with it off (`NX_NO_INT_ATTRIBS`, now inverted to `NX_INT_ATTRIBS=1` to turn it on) showed clean scenes (the Wonder
    Flower, the airship, the golden Wonder sky). Off by default until it is right; characters are still invisible
    either way, so integer inputs are not (or not the only) cause. **Update (#26):** the patch is right and is now on by default. The
    characters were missing for another reason (#26); with both fixes no wedge showed up in the course runs.
26. **No character in the course: Mario, Luigi, the Talking Flowers, Bowser... never drawn.** Two layers.
    - *Metal refused every skinned pipeline.* The bone indices (vertex location 6) have an integer format, and the
      translated shader declares every input as `vec4`: MoltenVK logged `SPIR-V to MSL conversion error: Vertex
      attribute type mismatch between host and shader` (on stderr only, in the `.out` files, not in `NX_LOG`) and the
      draws were dropped as `no pipeline` (83,680 in one play session). `patch_int_inputs` (#25) fixes it; it was off.
    - *The draw lists were gone.* With the patch on, most runs still drew no skinned model at all: no pipeline was
      even requested. `NX_TRACE_SKINLISTS` showed that the lists binding the skinned programs are recorded, called
      and replayed every frame, but right after each skinned bind they call per-mesh lists recorded when the course
      loaded (handles like 0x5833, ~60,000 lists earlier), and those were `GONE`: nvn_cmd.cpp kept the last 65,536
      handles and dropped older ones that had not been used yet. The characters' lists are first called only after
      the intro, so whether they survived depended on timing (runs differed: some logged the pipeline errors above,
      most never drew a character). `NX_ZERO_DRAWS` could not see any of this, since those draws never reached the GPU.
    - Fix: a handle now lives as long as its command memory, as on the GPU. Each list remembers the bytes it used
      (memory added by `AddCommandMemory` + our usage estimate), and a newer list recorded over those bytes drops it.
      The count limit is a safety net only (2^18), and it never drops a list that was not called yet. Memory stayed
      at 3.2 GB (peak 3.5) through the course. The integer input patch is on by default (`NX_INT_ATTRIBS=0` turns it
      off). Result: Mario, Luigi, Peach, Daisy, the Toads, Yoshi, Nabbit, Prince Florian, Bowser and Kamek all render
      (docs/media/wonder/19-21).
27. **Metal abort: "MTLTextureDescriptor requests 5 mipmap levels, but the dimensions (1, 1, 1) can only support a
    maximum of 1 levels".** The game creates a 1x1 texture with 5 levels, which NVN accepts. Two runs ended right
    after Bowser arrives (a Metal assertion, not a Vulkan error). Fix (nvn_vk.cpp, image creation): levels are clamped
    to floor(log2(largest dimension)) + 1. Issue #15.
28. **Intermittent crash at startup in `nn::audio::ApplyLightLimiterEffect`** (signal 11, data address 0; 2 of 4
    headless runs). The limiter's DSP state was all zero, so it was never initialized.
    - A new effect's "initialize" status travels in exactly one `RequestUpdate`: the server copies the client's status
      at every update, and the client sets it to "updated" right after sending it.
    - Our audio render thread signalled the game's audio event every 5 ms whether or not it had rendered since the
      last update. A sound thread that fell behind updated twice in a row, the second update overwrote the status,
      and the next render applied a limiter whose state was null.
    - Fix (hle_audio.cpp): an update waits (outside the render lock, at most 50 ms) until a render ran since the
      previous one, the console's rhythm.
    - Result: `audio: update waits for a render` fires in every run (the race is real). 4 of 4 startup runs had no
      crash, against 2 of 4 before. Issue #16.
29. **Far too much contrast and saturation everywhere.**
    - Measured on the Florian ceremony shot against the same shot from the real game (mariowiki,
      `Flower_Kingdom_SMBW.jpg`):
      - real game: mean luma 0.748, 5th percentile 0.380, saturation 0.314;
      - ours: mean luma 0.592, 5th percentile 0.122, saturation 0.517;
      - ours with one sRGB encode applied offline: 0.755 / 0.363 / 0.337.
    - The engine renders into RGBA8 (0x25) textures through **sRGB views** (0x38): the GPU encodes on write, blends
      in linear, and decodes on sampling. The runtime ignored the view format: `read_view` never called
      `nvnTextureViewGetFormat`, and every view and attachment was made in the image's format. So each draw lost a
      conversion.
    - Fix:
      - `ViewInfo` and the `SetRenderTargets` blob carry the view's NVN format.
      - `view_format()` (nvn_pipeline.cpp) turns it into the Vulkan format when it is a same-size colour format.
      - Sampled views, attachment views and the pipelines' colour formats use it.
      - Colour images are created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`.
      - The log names each pair once (`texture format 0x25 viewed as 0x38`).
    - Result: ceremony mean luma 0.756, 5th percentile 0.366, saturation 0.332 (the real game: 0.748 / 0.380 /
      0.314). See docs/media/wonder/26-colours-before-after.png and 22-25.
    - Left: the disc behind the Wonder emblem is still dark red (study G4). Issue #17.
30. **50-70 MB of uploads per frame (~130 KB per draw).** Every draw copied its uniform banks, storage buffers and
    all 15 vertex streams from guest memory into the upload ring, even when the previous draw had just copied the
    same bytes.
    - Fix (nvn_dedup.cpp): within a submit, one copy per guest address and size serves every later draw. Guest memory
      only changes during a replay through `UpdateUniformBuffer` and `ReportCounter` (both invalidate what they
      overlap) and compute results (written back after the submit, so every draw reads the same pre-dispatch bytes
      either way). Only the vertex streams the shader reads are uploaded.
    - A first version also shared the support buffer and c[0]. Those are host scratch data on the stack, at the same
      address for every draw, so every draw got the first draw's viewport and buffer table; the review caught it.
      Now only guest memory is shared.
    - Result: ~12 MB per frame in the course, 19 GB reused against 2.5 GB copied in one run.
31. **Clears were transfer operations between render passes**, each a Metal encoder of its own that made the next
    pass reload its targets. `draw_clear()` (nvn_draw.cpp) clears inside the pass with `vkCmdClearAttachments`:
    - it honours the depth and stencil masks, and a colour mask of 0;
    - it falls back to a transfer clear when the target is larger than the pass;
    - the cleared target counts as written for #35.
32. **Compute write-backs for read-only storage.** Every storage binding of a compute shader was copied back to guest
    memory after the submit. That forced the async path to wait before every replay, and a read-only binding's stale
    copy could overwrite real results. `written_storage_bindings()` (nvn_spirv_patch.cpp) scans the SPIR-V for stores
    and atomics through each binding; a pointer it cannot trace marks every binding. `NX_WRITE_BACK_ALL=1` restores
    the old behaviour.
33. **Two clocks.** The game paces on its vsync event (60 Hz host timer), and the FIFO swapchain also waited for the
    display. The swapchain now uses MAILBOX when the driver has it (`NX_FIFO=1` for FIFO).
34. **~300 render passes per frame, each reloading and storing its targets.**
    - Counters (`nvn passes:` log lines) showed 1.2 M passes ended by a draw whose texture was "not ready", against
      3.2 M draws. The texture was nearly always a pool entry naming a destroyed NVNtexture (see #38): it sampled the
      dummy anyway, but ended the pass first.
    - GPU "outside passes" went from 24.5 to ~12 ms per frame, and GPU wait from ~32 to ~13 ms per frame.
    - Also:
      - `SetRenderTargets` no longer ends the pass by itself (see #35);
      - `forget_image_views` is a range erase instead of a scan of every cached view;
      - the object registry is a hash map under a reader/writer lock.
    - Reusing the images of destroyed render targets for new textures over the same memory is implemented but off by
      default (`NX_IMAGE_REUSE=1`): the review found that a CPU-written texture placed there would sample the old
      render.
35. **Characters turned into dark silhouettes** after `SetRenderTargets` stopped ending the pass.
    - The character lighting samples a target of the open pass (the depth buffer) after drawing to it. That needs
      what was rendered to be stored first, and the old pass break did it by chance.
    - Found with bisect switches: `NX_TARGETS_BREAK=1` restored the colours, `NX_NO_IMAGE_REUSE` and
      `NX_WRITE_BACK_ALL` did not.
    - Fix: a draw that samples an image that is one of the open pass's targets and was written in this pass (by a draw
      or a clear) starts a new pass.
36. **Two NVNtextures over the same memory had separate images.** The game renders through one texture object and
    samples the same memory through another (UI capture and blur chains, agl filters), so the sampled image was a
    stale upload. Live textures with the same memory and shape now share one image (image_of, nvn_vk.cpp). The
    graveyard frees an image only when no other texture holds it. Pool views that set only a base level, a layer or a
    format are kept too (they were dropped, so level 0 was sampled).
37. **Near 26 fps everywhere, menus included.**
    - The kernel's spin throttles (a 0.5 ms nap for a thread yielding more than 32 times in a row or spinning on
      GetSystemTick) were made for Hades on phones.
    - Wonder's job system joins its workers by yielding in a loop, so every join took over 1 ms. The main thread spent
      ~18% of its time in those naps, and its own work was only ~8%.
    - The throttles are now Hades-only (`NX_SPIN_NAPS=0/1` overrides).
    - Game threads run at `QOS_CLASS_USER_INTERACTIVE`, so macOS keeps them off the efficiency cores.
    - A frame limiter paces presents to the window's present interval (a late frame is not snapped to the next vsync;
      `NX_NO_FRAME_LIMIT=1` for unpaced).
    - Title and menus now run at 58-60 fps.
38. **Goombas, trees and bushes of the course drew black.**
    - NVN texture pools keep a copy of the descriptor, so a game may finalize an NVNtexture while it stays registered
      and sampleable. Wonder does it for most course objects.
    - The runtime kept only the pointer: those draws sampled the dummy (black) and ended the render pass (#34).
    - Fix (nvn_objects.cpp): a finalized texture that a pool slot still names moves to a host key, and the slots point
      at it. It is released when its slot is re-registered, 2 s after its last sample, or 250 ms after being kept if
      never sampled. Without the expiry they piled up at ~450 per second (6 GB in two minutes).
    - Stencil reference, compare and write masks, polygon offset (depth bias) and depth clamp are now recorded and
      applied (nvn_raster.cpp). Depth bounds are recorded only, since MoltenVK has no depthBounds.

    **Update:** keeping finalized pool textures is now off by default (`NX_KEEP_TEXTURES=1` turns it on).
    - Bisect runs showed that it fed stale data to the course's sky: white and blocky in the second part of
      1-1. With it off, the sky is blue with clouds, as in the real game.
    - The actors stayed dark either way.
    - Its other gain, fewer pass breaks, comes from #34 alone: a missing pool texture samples the dummy without a break.
39. **Course actors (Goombas, the player, trees, bushes) render dark. Not solved yet.** What is known:
    - Their fragment shaders multiply by an AO term, `textureLod(R8 half-res target, uv, 1.0)`, and an ambient term
      from a 192x108 RGBA16F target. Neither is the dummy: a run with the dummy cleared to magenta showed none.
    - The texture handles those draws bind name textures of an older frame, two frames earlier. The dynamic
      resolution changes every frame, so the sizes differ. The draw's own frame renders AO and ambient into other
      texture objects.
    - Tried without effect:
      - keeping re-initialized textures for their pool slots (`keep_for_slots` in `TextureInitialize`, off with
        `NX_KEEP_TEXTURES`);
      - reading the arrays of the `*Deferred` bind calls when the work is submitted or replayed instead of when it is
        recorded (#41).
    - Next: find where the handle arrays get their values (the texture pool registration order against the
      recording), with `NX_TRACE_TEXTURES=<frame>` (logs from that presented frame on) and a draw trace.
40. **Replay thread** (`NX_REPLAY_THREAD=1`, off by default).
    - What it does: nvnQueueSubmitCommands and nvnQueuePresentTexture queue their work for one host thread, the way a
      GPU runs behind the CPU. The game waits only at nvnSyncWait, nvnQueueFinish, memory pool invalidate/finalize,
      and at most two queued presents.
    - Objects the game destroys are freed between jobs (`defer_object` / `drain_objects`), so a replay never holds a
      freed pointer.
    - Why it stays off: with the replay a frame behind, the course's render passes ended by target changes went from
      125 k to 1.3 M per run, and the course ran slower (15-20 fps against 30). The course still sits near 30 fps
      without it: the game switches its window present interval between 1 and 2 (now logged).
41. **`*Deferred` bind calls** (`NX_DEFERRED_BINDS=1`, off by default).
    - The SDK's BindTexturesDeferred stores the array's address instead of the values: a record of opcode 23, stage,
      first, count and pointer (sdk+469680, against BindTextures at sdk+469430, which copies each handle).
    - The runtime can record the pointer and read the array at submit (`NX_DEFERRED_SNAPSHOT=1`) or at replay.
    - Neither fixed #39, and both multiplied the pass breaks, so the arrays are read at recording, as before.
42. **#39 continued: the player is fixed, the Goombas and trees are not yet.**
    - **Player.** The course's draws sample AO and ambient-light targets through texture objects the game destroyed
      two frames earlier, on memory the engine still holds. Reusing the GPU-written image of a destroyed texture for a
      new one with the same memory and shape (`reuse_image`, nvn_frames.cpp) keeps that content, and Mario renders
      lit. It is on by default now (`NX_IMAGE_REUSE=0` turns it off).
    - **Pool snapshots** (nvn_objects.cpp).
      - Each registration keeps a copy of the texture as registered. When the texture object dies or is re-initialized,
        the copies of cube textures inherit its image.
      - texture_for uses the copy when the slot's texture is gone or has another target or format: the light-probe
        cubemaps get their structs reused as 2D textures at a course load.
      - Only cubes inherit images: inheriting every image kept ~450 a second alive (6 GB).
      - A copy nothing sampled for 2 s drops its image.
    - **Goombas, the big tree, some bushes: still dark.**
      - Study 3/4 (scratchpad) mapped their draws to the same shader template as the player, with the same render
        targets. The difference is which light-probe cubemaps they bind.
      - The probes resolve correctly: the slot's 2-level or 5-level cube, never finalized, with the `rt` flag.
      - `NX_TRACE_CUBES=1` shows each probe rendered face by face and level by level at load, with valid face views,
        so the probe content itself must be dark: the captures render a scene that is still being lit.
      - Next: dump a probe's faces after load (`NX_DUMP_TEXTURES`).
43. **Why the course runs at ~30 fps: the game's own frame-rate governor.**
    - **Where it decides.** Wonder's `sead::GameFramework::procFrame_` override (main+e15ec) calls
      nvnWindowSetPresentInterval every frame (its NVN proc slot is 0x35ad060, not 0x35ad078, which is
      SetNumActiveTextures).
    - **What it measures.** `fw[0x118]` is the main thread's time from the end of the previous present wait to the end
      of this frame's calc and draw recording, in GetSystemTick ticks. GPU timestamps, vsync and performance mode are
      not used: faking the GPU timings (`NX_FAKE_GPU_TIME=1`) changed nothing.
    - **The rule.**
      - It asks for interval 2 when that time reaches 15.83 ms: `int(t*60 + 0.05) + 1`, clamped to 1..2.
      - Three slow frames in a row lock interval 2, and 15 fast ones release it.
    - **Measured.** `nvn: main-thread frame time` log lines (every 600 frames, read from that call site): in the menus
      ~95% of frames are under 12 ms, in the course 30-40% are over 15.8 ms.
    - **Where the main thread's time goes** (a `sample` of the process in the course):
      - It spends ~60% of its time yielding in the job system's join loop (main+105930 → svc YieldThread) and ~25% in
        waits; its own code is a few percent. Workers wait the same way.
      - The job it joins depends on the render thread (g52), which replays our Vulkan work and waits for the GPU
        (replay ~9-10 ms, GPU ~7-13 ms per frame). So the replay is on the game's critical path.
      - The way out is the replay thread (#40), once its pass-break regression is fixed, plus a cheaper replay.
    - `NX_MAX_INTERVAL=1` clamps the game's maximum interval (writes its fields). It is not an improvement: the course
      then runs at 20-30 fps with longer frames.
    - Kept as diagnostics: `NX_FAKE_GPU_TIME`, `NX_MAX_INTERVAL`, `NX_TRACE_CUBES` (writes into cube images), and the
      present-interval logs.
    **#40 data** (`nvn passes ended` log, by reason, same route):
    - Without the replay thread: 183 k passes ended by a draw, 341 k by a clear of other targets, 2.8 M draws.
    - With it: 657 k by a draw, 261 k by clears, 1.7 M draws.
    - The extra breaks are draws whose targets differ from the open pass: the command stream itself alternates targets
      more when the submit does not wait for the replay. Probably the game changes behaviour (its dynamic resolution or
      its pacing) when submits return at once. To study next.
44. **Two fixes from the night's studies.**
    - **The course's 30 fps came from macOS `sched_yield`, not from our replay.** This corrects #43.
      - Wonder's job system yields after every job item and in its barriers (main+105930, main+111cd0). The main
        thread and two workers run those loops together.
      - On macOS `sched_yield` drops the caller to priority 0: measured at 20-180 us per call on the M1. The main
        thread spent ~60% of its frame yielding.
      - `plat::yield` now calls `thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0)` (2-4 us). `NX_YIELD=sched`
        restores the old call.
      - Result: in the course the main thread's frame is under 15.8 ms for ~98% of frames (it was 60-70%), the
        governor stays at interval 1, and the course holds ~40 fps. The limit is now the render thread: replay plus
        GPU, ~25 ms.
    - **UI shapes drawn as rectangles** (the character select's cog badge and star highlight, a black box behind
      "Ver." and the text shadows).
      - Bisected with runtime-only builds of older commits (`private/wonder/bis_commit.sh`): ebe4e5f is correct,
        c41b534 is not.
      - Switches then isolated it: `NX_NO_IMAGE_SHARE=1 NX_IMAGE_REUSE=0` restores the cog badge. Stencil
        (`NX_OLD_STENCIL`), clears (`NX_TRANSFER_CLEARS`), pass breaks (`NX_TARGETS_BREAK`) and upload reuse
        (`NX_NO_DEDUP`) are not the cause.
      - Cause: a GpuImage kept the swizzle of the texture that created it. A shared or inherited image sampled
        through another texture (a UI mask) used the wrong channel mapping, so alpha was 1 and the mask covered its
        whole quad.
      - Fix:
        - `sampled_view` uses the sampling texture's own swizzle (pool view, else builder);
        - image sharing and reuse keys include the swizzle.
      - Sharing only GPU-written images was tried and dropped: the window texture creates its image before the render
        target draws into it, so the two never connected and every presented frame was the grey "never rendered"
        fill.
45. **Cheaper replay; the course at 44-46 fps.** From the night's performance study:
    - `get<T>` keeps a per-thread cache of its last 8 lookups per type. Any exclusive lock of the object registry
      bumps a generation and invalidates it, so the shared lock is taken only on a miss.
    - The pitch-linear texture content hash runs 4 lanes at once, at most once per 4 presented frames.
    - The open pass's target images are resolved once per pass, not per sampled texture.
    - Result: replay ~10 → ~7.9 ms per frame, the course 33 → 44-46 fps.
    - The rest of the frame is GPU wait (~13 ms, synchronous): the next target (issue #31).
    - Probe study (scratchpad study_black): a `cube draw` trace (`NX_TRACE_CUBE_DRAWS=<frame>`) and
      `NX_TRACE_CUBES=<frame>` (from that frame on) for the dark Goombas.
      - Also a rule that uses the pool copy when a slot's texture changed memory or size (`NX_SNAPSHOT_SHAPE=1`, off
        by default).
      - It broke the course's foreground flowers (dynamic-resolution targets are meant to be followed) and did not
        light the Goombas.
46. **Images the GPU rendered, sampled as stale uploads through another texture object.**
    - A diagnostic listed every texture upload whose memory the GPU had already rendered through a live image of the
      same shape (removed after use; `rendered_alias` in the commit history).
    - Cases found:
      - the UI's text strips (608x33, 200x33 RGBA8);
      - the 2048x2048 D16 shadow map;
      - some dynamic-resolution targets.
    - Two rules kept those textures from sharing the rendered image:
      - `image_of` skipped sharing for any texture with a pool view swizzle;
      - the storage key held the builder swizzle, and a render target and the texture sampling it often differ in it.
    - Both date from before #44. Since #44, each texture samples with its own swizzle (`sampled_view`), so neither
      rule is needed.
    - Now:
      - the swizzle is not part of the share and reuse keys;
      - a pool swizzle does not stop sharing;
      - only an R8 texture widened for alpha-from-red keeps its own image.
    - Bisecting: `NX_KEY_SWIZZLE=1` and `NX_SHARE_NO_POOL_SWIZZLE=1` restore the old rules.
    - Character select checked frame by frame against the previous run: no change except timing.
    - Light probes (#39, #42), what the night established. Runs must last about 290 s to reach the course (the input
      script enters it at ~175 s); earlier "course" probe traces at frame ~7100 were the world map's.
      - The course probe chain is:
        1. 256x256 scene faces (GPU-rendered);
        2. a 128x128x12 cube array (layers 0-5), then prefiltered layers and mips sampled from itself;
        3. 64x64 2-level and 5-level cubes, from a 64x64 RGBA16F direction map plus colours in the fragment uniforms
           (c5[16], c5[17]).
      - `NX_DUMP_ON_FS=<fs hash,...>` (only draws into cube targets, from frame `NX_DUMP_FROM`) reads back each
        stage. `NX_DUMP_TARGET_EVERY` sets the period of `NX_DUMP_ON_TARGET`.
      - The cube array looks right (sky, horizon). Most 64x64 probes are lit, a few are black.
      - The black ones are drawn with all-zero colour uniforms (even w). One probe is lit at the course start and
        re-captured black ~5 s later with zeros at the same uniform address.
      - The 64x64 direction map is all zeros in guest memory whenever it is uploaded.
      - Ruled out:
        - CPU writes after the upload (`NX_HASH_SMALL=<bytes>` hashes small block-linear textures);
        - compute write-backs in the same submit;
        - GPU renders through another object;
        - CPU readback of render targets.
47. **Deferred clears (nvn_clears.cpp).**
    - Before: a clear of targets other than the open pass's began a pass with every attachment loaded, then cleared
      with a `vkCmdClearAttachments` quad. That was ~36 passes per course frame, each a full-resolution load on a
      tile GPU.
    - Now the clear stays pending. The next pass on the same targets begins with loadOp CLEAR (same render area, so
      the same pixels as the quad).
    - Anything else first gets the clear done in a pass of its own, with no draw:
      - a pass on other targets;
      - a draw sampling a cleared image;
      - a dispatch, a copy, a snapshot, or the submit's end.
    - `NX_EAGER_CLEARS=1` restores the old way.
    - `Targets` are now compared field by field (`targets_same`); a memcmp also compared the padding after
      `color_count`. `NX_TARGETS_MEMCMP=1` restores the memcmp in draws.
    - Matching a pending clear into a pass on more targets with the same area (`NX_CLEARS_SUBSET=1`) measured no
      faster, so it is off.
    - Course average (headless, synchronous): replay 7.9 → 7.4 ms, GPU wait 13.1 → 12.5-13.0 ms, 44.7 → 46-47 fps.
      Runs vary by ±1 ms with the character the input script happens to pick.
    - GPU profile of the course (`NX_GPU_PROFILE=1`): ~3 ms/frame between passes (~66 gaps), and 1080p shading.
      The biggest passes are fs 4601469d (1.5 ms) and f8a4447a (2x 0.85 ms).
    - Open: an intermittent band of horizontal garbage across the course's middle layer (seen in 2 of 9 course runs,
      both after a respawn, in docked and handheld mode). Not caused by the clear matching; being bisected.
48. **Handheld mode (`NX_HANDHELD=1`) and `nvnWindowSetCrop`.**
    - `GetOperationMode` / `GetPerformanceMode` report handheld and normal (both am and apm), and the default display
      resolution is 1280x720.
    - The game then renders its 3D at 720p into its 1920x1080 window textures and crops to 1280x720 with
      `nvnWindowSetCrop`. That call was a stub; it now sets the part of the window texture blitted to the window.
    - Course: GPU wait 9.7 ms against 12.5-13, and 52-57 fps against 46-47 docked.
    - Docked stays the default (the Switch's docked picture).
    - Async GPU (`NX_ASYNC_GPU=2` + `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`) was retested: menus hold 60, but the
      loads became slow enough that the input script never reached the course. It stays off.
49. **WriteTexels images keep their texels; FMZ in the translator.**
    - An image given texels by `nvnTextureWriteTexels` is not uploaded again from guest memory when a pool flush
      marks it dirty. Those texels never reach guest memory here, so the re-upload wiped them.
      `NX_TEXELS_REUPLOAD=1` restores the old behaviour.
    - `tools/maxwell/spirv_harness/translator/spirv-fmz.patch`:
      - Ryujinx decodes Maxwell's FMZ mode on FMUL/FFMA and ignores it. The patch makes a zero factor give 0 (FFMA:
        c) even against Inf/NaN.
      - A full batch changes 21 blocks, all compute (particle effects).
      - No visible change in the course; kept for correctness.
    - Debug tools:
      - `NX_DUMP_ON_FS=<fs,...>` (`NX_DUMP_FROM`, `NX_DUMP_ANY`, `NX_DUMP_TARGET_EVERY`): reads back a draw's
        target and inputs at the submit's end.
      - `NX_SNAP_FOLLOW=<fs>@<draw>|@f<frame>` (`NX_SNAP_FOLLOW_COUNT`, `NX_SNAP_FOLLOW_NOSNAP`): logs and
        snapshots the draws of a pass one by one.
50. **Sampler state was lost (the biggest remaining glitch source; docs/nvn/GLITCH-PLAN.md G1).**
    - Commit c41b534 split `nvn_sampler.cpp` and overwrote the file that held the sampler objects:
      - `nvnSamplerInitialize` / `Finalize`;
      - `nvnSamplerPoolRegisterSampler` / `RegisterSamplerBuilder`;
      - the `nvnSamplerGet*` getters.
    - They fell back to trace stubs, so every texture sampled with `SamplerInfo{}`:
      - nearest filtering;
      - clamp to edge;
      - mip 0 only;
      - shadow compare ALWAYS.
    - Restored as `nvn_sampler_objects.cpp`. Result:
      - **The dark Goombas, tree and characters (#26, #39, #42) are lit.** Their shaders read the SSAO target with
        `textureLod(ao, uv, 1.0)`. The game renders AO only into level 1, so mip 0 (never written) made AO 0 and
        the lighting fell to its floor.
      - Smooth sky (#28).
      - The world map's repeating textures are back (grass, flower beds, water).
    - The #49 rule that let only render targets (flags 0x8) reuse destroyed images is reverted: the AO targets have
      flags 0 and depend on reuse (#42).
    - A duplicated `nvn_sampler.cpp` line in CMakeLists.txt is removed.

![Course](../media/wonder/33-course-samplers-restored-goombas-lit.png)
![World map](../media/wonder/34-world-map-samplers-restored.png)
51. **R11G11B10F uploads, fresh render targets, unwritten attachments (GLITCH-PLAN G3/G4).**
    - **Uploads.** NVN 0x3f is stored as RGBA16F, but uploads copied its 4-byte texels as 8-byte ones. The copy read
      twice the data (rows side by side, other uploads in the bottom half), which fits the garbage band after a
      respawn.
      - Uploads now widen each texel to four halves (same exponent bias, wider mantissas, alpha 1).
      - Any copy that would read past the staged data is logged ("upload short") and cleared instead.
      - Upload code moved to `nvn_upload.cpp`.
    - **Fresh render targets** (flags 0x8, never written by the GPU, no texel writes) are cleared instead of uploaded
      from guest memory, which never holds GPU output here. `NX_RT_UPLOAD=1` restores the upload;
      `NX_ALIAS_MAGENTA=1` clears them magenta to find them.
    - **Unwritten attachments** (layout UNDEFINED) begin their pass with loadOp CLEAR (colour 0, depth 1), not a LOAD
      of recycled memory (`NX_UNDEFINED_LOAD=1`).
    - **`drain_objects`** runs under the GPU lock in `gpu_submit` / `gpu_present`, so no object is freed while
      another holder of the lock replays.
    - **R11G11B10F semantics.**
      - Sampled alpha reads 1.
      - On such attachments, DST_ALPHA blends become ONE and ONE_MINUS_DST_ALPHA becomes ZERO.
      - `view_format` compares host texel sizes.
      - `NX_R11G11B10_RAW=1` restores the old behaviour.
    - Course and character select unchanged except where they should be (Peach lit, no upload short); course
      ~44 fps.
52. **1D textures, masked clears, alpha to coverage, copies within one image (GLITCH-PLAN G6/G7/G8/G10).**
    - **1D textures.** NVN 1D and 1D-array textures (targets 0/3) are 2D / 2D-array images of height 1. Ryujinx samples
      1D as 2D, so a Vulkan 1D image's 2D view was refused and those draws read the black dummy. `NX_1D_IMAGES=1`
      restores the old images.
    - **Masked clears.** A colour clear with some channels masked off runs as a fullscreen draw with that write mask
      (`nvn_mask_clear.cpp`, two tiny glslc shaders). `NX_CLEARS_NO_MASK=1` restores the old clear. No course clear
      used a partial mask in the test runs.
    - **Scissored clears are off.** Clipping clears to the bound scissor left whole regions of the course uncleared
      (huge black and white shapes); the character select was unaffected. `NX_CLEARS_SCISSOR=1` turns it back on for
      study.
    - **`nvnCommandBufferBindMultisampleState`** is no longer a stub. Its alpha-to-coverage enable goes to the
      pipeline key and `alphaToCoverageEnable` (`NX_NO_ALPHA_TO_COVERAGE=1`).
    - **Copies within one image.** A copy between two subresources of one image (two texture objects sharing it,
      #36) is no longer dropped; it runs in GENERAL layout with explicit barriers (`NX_COPY_NO_SAME_IMAGE=1`).
    - "Too bad!" is drawn hollow by design (see #56): not a masked clear (none happened).
53. **Compute results within a submit (GLITCH-PLAN G5, `nvn_written.cpp`).**
    - The problem:
      - a storage buffer a dispatch writes was a private copy of guest memory, written back only after the submit;
      - later draws and dispatches in the same submit read the old guest bytes;
      - a second writer's write-back overwrote the first's.
    - Now each submit keeps one copy per written guest range:
      - writers reuse or merge copies;
      - reads that overlap one (uniform, storage, vertex, index) bind that copy, composing on the GPU when a read
        only partly overlaps;
      - an indirect dispatch whose arguments were written in the submit uses `vkCmdDispatchIndirect`;
      - each range is written back once.
    - `NX_COMPUTE_SEPARATE=1` restores the old copies.
    - Course: ~6,200 bindings of written copies per log period, no regression, 45.7 fps (fewer copies).
54. **NaN semantics, fast math, and the intermittent band (GLITCH-PLAN G9, G3).**
    - **NaN translator patch** (`translator/spirv-nan.patch`, not in the build):
      - What it does: float Clamp/Min/Max emitted as GLSL.std.450 NClamp/NMin/NMax, Maxwell's NaN-dropping `.SAT`
        and FMNMX.
      - Batch: changes 10,304 of 12,717 blocks, all valid.
      - Course run: horizontal streaks along the horizon, a black and white patch near the ground, replay 7.8 →
        11 ms (new Metal compiles).
      - The same streaks then appeared with the old shaders too, so the patch is not proven to cause them. It is
        kept out anyway: no visible gain.
    - **`MVK_CONFIG_FAST_MATH_ENABLED=0`:** no visual change, course 39.8 fps (against 44-46). It stays on.
    - **The horizon band.**
      - Rows of garbage across the course's background layer appear in about every other 290 s run.
      - `NX_ALIAS_MAGENTA=1` showed no fresh render target being sampled.
      - Serializing texture re-initialization with the replay (`NX_REINIT_WAITS_REPLAY=1`, experiment): no band in
        the run that played, but another run hung at boot, and the course ran at 32 fps. It stays opt-in.
      - The race it points at: other game threads re-initialize dynamic-resolution texture structs while a submit
        is replayed, so `get<Texture>` returns the new incarnation mid-replay. It needs a proper design (per-submit
        incarnations): in progress.
55. **Texture incarnations: a submit sees each texture as it was when the submit started (`nvn_incarnation.cpp`).**
    - Two races between the replay and the game's other threads:
      - `nvnTextureInitialize` put an empty Texture in the registry and filled it over ~15 guest calls with no lock.
        A replay could look it up half-built (new size, memory 0) and give it an image no target shares.
      - A struct re-initialized or finalized mid-replay made `get<Texture>` switch to the new incarnation between
        two draws of one submit.
    - Now:
      - a texture is built locally and published in one locked step; Finalize is one step too;
      - each replaced object is recorded with an epoch;
      - `gpu_submit` marks the epoch (`ReplayScope`), and replay-side `get<>` resolves to the object that was
        current at that mark;
      - retired objects are freed by `drain_objects`, under the GPU lock, with their records.
    - Measured: 640-750 thousand replay lookups per course run hit a texture changed mid-replay.
    - `NX_TEXTURE_RACE_OLD=1` restores the old behaviour.
    - The horizon band is not this: it still appeared in 1 of 3 runs. Up close it is stretched geometry with
      sparkles around it, and it lasts the whole run once it appears: a GPU-simulated effect whose state went bad.
      `NX_TRACE_NAN=1` logs non-finite or huge floats in compute write-backs, with the shader.
56. **The game records frame N+1 while frame N is replayed: pool slots at the replay's mark, selective write-back.**
    - Measured: 98.5% of `nvnTexturePoolRegisterTexture` calls (236 k of 240 k in a run) happen while another thread
      replays a submit. Wonder has a recording thread and a submit thread, so our synchronous replay of frame N runs
      while the next frame's textures are re-initialized and its pool slots registered.
    - **Pool slots.**
      - `texture_for` read the pools' maps with no lock, and as they were at that instant.
      - Each registration now records the entry it replaces, with an epoch (`note_pool_change`).
      - The replay reads the slot as it was at its mark (`pool_entry_at`), under the objects lock, like #55 does for
        texture objects.
      - In the test runs no lookup needed an older entry: the game registers the next frame in other slots. The lock
        and the history stay for safety.
    - **Write-backs.**
      - A compute shader's storage range was copied back whole after the submit. The range is often larger than
        what the shader writes, and the recording thread writes the rest of those buffers meanwhile, so stale bytes
        went over them.
      - Now each written range keeps the guest bytes it was made from, and only the words the GPU changed go back
        (`apply_write_back`).
      - `NX_WRITE_BACK_WHOLE=1` restores the old whole-range copy.
    - **Still open: the horizon band.** It still appeared in 1 of 4 runs after both changes. One run showed the
      fence stumps exploded into huge beams before the band, so it is mesh data, not a texture. Next:
      `private/wonder/band_hunt.sh` snapshots every draw of the main pass, cropped to the band (`NX_SNAP_CROP`), to
      name the draw.
    - **"Too bad!" is hollow by design.**
      - Its font (`fcpx/NewMarioFontOutlineLL`, face `NinNewSuperMarioBrosT53ou`) is an outline typeface: rasterised
        with FreeType it gives the same ring letters.
      - The Miss layout has nothing that fills the letters: two capture panes, one of them a blurred black shadow.
      - Only the soft shadow may be weaker than on hardware (the blur chain, GLITCH-PLAN G10).
57. **The replay's mark is the end of the lists' recording, not the start of the submit.**
    - #55 took the mark when the submit started. But the recording thread runs ahead of the submit thread, by more
      than a frame when our replay is slower than 60 fps (the course: 45 fps). By the time a submit starts, the next
      frames' textures and pool slots are already there, and the mark included them.
    - How often that phase was bad depended on the run, which is why the glitches were intermittent:
      - a band of stretched geometry at the horizon;
      - exploded meshes;
      - a white or striped foreground layer.
    - Now:
      - `nvnCommandBufferEndRecording` stamps each list with the epoch;
      - `gpu_submit` marks the replay with the newest stamp of its lists (`ReplayScope(lists_epoch)`);
      - the mark never goes back;
      - deferred objects carry the epoch at which they were retired, and `drain_objects` frees only those at or
        before the last mark, with their records.
    - `NX_MARK_AT_SUBMIT=1` restores #55's mark.
    - Result: 4 course runs of 290 s, 28 course frames, no band, no exploded mesh, no white foreground (before:
      about 1 run in 3). 44.6-46.7 fps.
    - The 60 fps plan from the performance study is in docs/nvn/PERF-PLAN.md.
58. **The character-select blur (GLITCH-PLAN G10, issue #42): the chain runs, and the game's own stream clears it.**
    - Corrections to G10:
      - The blur is a one-shot capture in the frame after the first button press (layout `TitleBlurBG`), not a
        per-frame pass.
      - `multi_filter_expand` is never bound because `Blur00.baglmf` disables every expand unit.
      - The halftone dots and the grey mask are drawn. Only the blurred picture is missing.
    - The chain, as replayed (`NX_TRACE_PASSES`, `NX_TRACE_CMDS`, snapshots):
      1. `ClearColor` of the work target A (1920x1080 RGBA8);
      2. a quad copies the framebuffer into A. A snapshot right after shows the whole title picture;
      3. `SetRenderTargets(A)` again, then `ClearColor(0, 0, 0, 0)` with mask 0xF, in the same command list;
      4. `multi_filter_reduce` samples A into 480x270, then the gaussian passes and the copy into the pane texture.
         All of them come out black.
    - Step 3 is in the game's recorded stream, and we replay it as recorded. The bound channel mask is 0xF too, and
      the scissor is the whole target.
    - Ruled out: uniforms (`NX_TRACE_UUB`), sampler and view (`NX_TRACE_SAMPLERS`), deferred clears, fresh-target
      clears, image sharing, dedup.
    - Open question: whether the hardware skips or restricts that clear (an NVN clear rule we do not know), or
      whether the console's picture is sharp under the mask and dots too. It needs a capture from a real console.
    - New switches:
      - `NX_TRACE_PASSES=<draw>`: pass begins (load ops), ends (reason) and clears.
      - `NX_TRACE_CMDS=<draw>`: every replayed op.
      - `NX_TRACE_UUB=<draw>`: uniform updates.
      - `NX_TRACE_SAMPLERS=1`: sampler and view of each sampled texture in `NX_TRACE_DRAWS`.
    - In the `NX_TRACE_DRAWS` log, the detail lines printed after a `draw N:` line describe draw N+1.
    - `NX_TRACE_GLYPHS` crashes on draws that are not text (it reads the glyph atlas at the draw's UVs): use it only
      with `NX_TRACE_FS_HASH` of a text shader.

Continued in [DEBUG-2.md](DEBUG-2.md) (entries 59 onward).
