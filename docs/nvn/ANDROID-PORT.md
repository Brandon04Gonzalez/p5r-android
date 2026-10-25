# Wonder on Android: port study

Read-only study (2026-09-30). The subject is Super Mario Bros. Wonder 1.0.0. Today it runs natively on an Apple M1:
the CPU code is recompiled and NVN is replayed on Vulkan through MoltenVK (PROGRESS.md). The target is arm64
Android phones with a native Vulkan driver (Adreno, Mali/Immortalis, Xclipse).

Inputs: [../ANDROID.md](../ANDROID.md) (the Hades port), [../NVN.md](../NVN.md), [PERF-PLAN.md](PERF-PLAN.md),
[DEBUG-2.md](DEBUG-2.md), [TRANSLATOR.md](TRANSLATOR.md), CMakeLists.txt and `runtime/src`. Numbers from other
devices are estimates and are marked as such. Nothing here was run on a phone.

## 1. What carries over

1. **The recompiled guest code.** `private/wonder/gen/{main,sdk}` is portable C++. Guest pointers are host pointers
   at `NX_BASE + offset` (`runtime/include/nx/cpu.h`). Android uses the same non-Apple layout as Windows:
   `NX_BASE` 0x40_0000_0000, heap 0x50_0000_0000, stacks 0x68_0000_0000, thunks 0x6F_0000_0000, all below Android's
   39-bit top (`runtime/include/nx/runtime.h`). Wonder was recompiled with `--base 0x4000000000`
   (WONDER.md), so one generated tree serves both platforms. Its host-sensitive parts already work on arm64:
   - LSE atomics and exclusive pairs;
   - fused `frsqrts`/`frecps`;
   - AES/SHA through `arm_neon.h`, guarded by `__ARM_FEATURE_AES && __ARM_FEATURE_SHA2` (`cpu.h:459`).

   The NDK's default `-march=armv8-a` defines neither macro, so the slow software crypto path would build silently.
   Build the game libraries with `-march=armv8.2-a+crypto` (every current phone has it).
2. **The Android host layer from Hades** (`runtime/src/platform_android.cpp`, 781 lines): `MAP_FIXED_NOREPLACE`
   reservations, pthreads (64 MiB stacks, 256 MiB for the guest main thread), the NativeActivity window and its
   generation counter, controllers and touch zones (`on_input`, `read_pad`), AAudio (`audio_open`, `audio_submit`,
   `audio_queued`), `VK_KHR_android_surface`, the crash handler, `config.txt` (`setting()`) and the profiler.
3. **The shared runtime** is already free of Apple code, except for:
   - the memory guard (`memguard_mac.cpp`);
   - the font fallback in `hle_pl.cpp:54`;
   - `kHeapBase` and the other `__APPLE__` constants in `runtime.h`.
4. **Audio.** Wonder's AudioRenderer runs in process: the sdk's own DSP command processor. The final mix is captured
   around `CommandListProcessor::Process` (`hle_audio.cpp:219-290`), folded to stereo and handed to
   `plat::audio_submit`. That is the same interface AAudio already implements for Hades, and there is no SDL in the
   path.
5. **The NVN layer** (`runtime/src/nvn/*`) calls Vulkan only through `vk_gen`'s function pointers, and gets its
   surface from `plat::create_window` / `plat::vulkan_create_surface` (`nvn_vk.cpp`, instance and surface setup).
   Buffer data reaches the GPU through a per-submit upload ring (`upload_alloc`, `nvn_vk.cpp:310`), not through
   `VK_EXT_external_memory_host`. So the NVN.md N3 concern ("Android: shadow buffers with page tracking") does not
   apply: the Mac path already copies.
6. **Hades lessons that apply** (ANDROID.md, memory notes): Adreno surfaces hand out 4+ images; the S23 returned
   `VK_SUBOPTIMAL_KHR` on every present; leaving the app destroys the window, which must be re-made under the game
   (`vk_bridge.cpp:270`); the rotated present pass (`vk_present.cpp`) lets the display controller scan out directly;
   `simpleperf` is blocked on Samsung, so the in-process profiler is the tool.
7. **Build scripts.** `android/build_apk.sh --game wonder` already takes the game as a parameter. It needs
   `games/wonder/AndroidManifest.xml` (only `game.cpp` exists) and a Wonder package id.

## 2. Platform-layer gaps

1. **File cap first.** `platform_android.cpp` is 781 lines, over the 700-line cap. Before anything is added, split
   it into `platform_android.cpp` (memory, threads, window, main), `platform_android_input.cpp` and
   `platform_android_audio.cpp`.
2. **Window and surface.** The NVN path has its own swapchain (`make_swapchain`, `nvn_vk.cpp:240-300`). It does not
   use the Hades bridge's, and three things are wrong for Android:
   - It sets `preTransform = caps.currentTransform` but presents with `vkCmdBlitImage` (`nvn_vk.cpp:553`), which
     does not rotate. The picture is wrong in any orientation other than the panel's native one, or the compositor
     rotates it at extra cost.
   - It re-makes the swapchain on `VK_SUBOPTIMAL_KHR` (`nvn_vk.cpp:585`). Hades on the S23 got SUBOPTIMAL on every
     present, which here means one swapchain rebuild per frame.
   - It never watches `plat::window_generation()`. A new `ANativeWindow` after home or screen lock needs a new
     `VkSurfaceKHR`, as `vk_bridge.cpp:270` does for Hades.

   Fix: present through `vk_present.cpp` (it samples the window texture, draws it rotated and scaled, and supports
   the crop), and move the surface-loss logic from `vk_bridge.cpp` into a shared helper.
3. **Input.**
   - Controllers map through `key_to_button` as they do for Hades.
   - The touch layout is Hades's invisible zones. Wonder is a 2D platformer that needs a d-pad or stick, plus
     A/B/Y (run, jump, action) and +. It needs a drawn overlay. Draw it in the present pass, as a second pipeline in
     `vk_present.cpp` with our own shapes, not Nintendo glyphs from the RomFS.
   - Menus need A held for about 1 s (PROGRESS.md 2026-09-29). That is a runtime bug to fix before touch testing.
   - Motion controls are not needed for Wonder.
4. **Audio out.** AAudio is already there at 48 kHz stereo s16, the format `hle_audio.cpp` produces. On the Mac,
   audio updates are paced to renders (PROGRESS.md "Crashes"), which ties sound to frame rate; on a phone that drops
   frames, pace from the AAudio callback instead, and measure latency with `AAudioStream_getTimestamp`.
5. **Game data.** The user supplies their own dump, their own keys, and their own translator output. Nothing is
   shipped or downloaded. A PC-side tool (`tools/pack_phone_wonder`, modelled on `tools/pack_phone.ps1`) produces
   `Wonder-data.zip` with:
   - `analysis/{main,sdk}.bin` (67 MB);
   - `romfs/` (3.5 GB);
   - `spirv/`: the offline Ryujinx translator's output (443 MB in 13,568 files), packed into one indexed file,
     because thousands of small files are slow on external storage;
   - optionally `fonts/shared_0.ttf`, decoded from the user's own firmware.

   On the phone, the fallback font is `/system/fonts/Roboto-Regular.ttf` (`hle_pl.cpp:54`).

   **The APK contains recompiled game code**, just as the Hades APK does. It is a private per-user build and must
   never be published.
6. **Settings.** Every `NX_*` switch is read with `getenv`. Copy `config.txt` into the environment (`setenv`) in
   `android_main` before `boot()`. Defaults for Android: `NX_HANDHELD=1`, and `NX_FIFO` behaviour (see 3.4).
7. **Memory guard.** `memguard_mac.cpp` reads `phys_footprint`, which includes GPU memory on Apple silicon. A new
   `memguard_android.cpp` reads `/proc/self/smaps_rollup` (Pss) plus `VK_EXT_memory_budget` (GPU allocations are
   not in Rss on Android), handles `APP_CMD_LOW_MEMORY` to drop caches before the low-memory killer acts, logs every
   30 s, and takes its limit from `ActivityManager.getMemoryInfo` (JNI, once) instead of a fixed 8 GB.
8. **Threads.** Mac game threads run at `QOS_CLASS_USER_INTERACTIVE` (`platform_mac.cpp:141`). On Android: ADPF
   (`APerformanceHint`, API 33+) for the game main thread and the submit thread with a 16.6 ms target, and those two
   pinned to the big cores (`sched_setaffinity`, from `/sys/devices/system/cpu/*/cpu_capacity`).
   `svcSetThreadCoreMask` stays honoured logically (ignoring it caused the 2026-09-28 frame deadlock); host
   affinity is separate.
9. **Page size.** Some Android 15+ devices run 16 KB pages. The Mac already runs 16 KiB host pages, and the decommit
   of a single 4 KiB guest page was fixed for it (PROGRESS.md 2026-09-28), so the same code path applies. Link with
   `-Wl,-z,max-page-size=16384`.

## 3. Vulkan on Android and MoltenVK

1. **Instance and device** (`nvn_vk.cpp` instance and device setup). `portability_enumeration` and
   `VK_KHR_portability_subset` are enabled only when present, so they are skipped on Android as they are. Requirements:
   - Vulkan 1.1+ with timeline semaphores (core in 1.2), `synchronization2` and dynamic rendering;
   - these exist on Adreno 7xx and Mali-G710+ with current drivers, and are often missing on Adreno 6xx drivers.
     Plan for 1.3-class devices only.
2. **Workarounds, by fate:**

   | Workaround (file) | Why on Mac | On Android |
   |---|---|---|
   | D24 and D24S8 → D32F (`nvn_format.cpp:8,40`) | M1 has no D24 | Adreno and Mali support `D24_UNORM_S8_UINT`: use it when format properties allow. That gives exact depth precision, and the depth-bias rescale note (`nvn_raster.cpp:123`) goes away |
   | R11G11B10F → RGBA16F (`nvn_format.cpp:46`, `nvn_upload.cpp:22`, `nvn_pipeline.cpp:178,347,543`) | Metal writes NaN/Inf where Maxwell writes 0 | Vulkan converts negatives to 0 for unsigned floats. Try native `B10G11R11_UFLOAT` as an attachment (half the memory and bandwidth); keep widening as the fallback per GPU |
   | 8-bit indices widened on the host (`nvn_written.cpp:207`) | MoltenVK converted them on the GPU per draw | keep; use `VK_EXT_index_type_uint8` where present (already probed) |
   | depthBounds recorded only (`nvn_raster.cpp:92`) | Metal has none | the same on Adreno and Mali |
   | depthBiasClamp gated (`nvn_raster.cpp:129`) | feature check | already generic |
   | depth clip control (`nvn_vk.cpp:182`, `nvn_pipeline.cpp:294`) | NVN depth −1..1 | not guaranteed on Qualcomm/ARM drivers. Fallback: a SPIR-V patch in `nvn_spirv_patch.cpp` (z = (z + w) / 2 on the last vertex-stage output), as Ryujinx does without the extension |
   | uniforms/storage padded to 64 KB (`nvn_sets.cpp:318,331`) | Metal has no bounds checks | keep at first (correctness). Later, `robustBufferAccess2` with smaller padding, measured: it costs on Mali |
   | lodMin ≥ 0, view restrictions, cube views (`nvn_sampler.cpp:52`, `nvn_pipeline.cpp:114`, `nvn_image.cpp:87,111`) | Metal aborts | harmless; keep (Vulkan validation wants most of them too) |
   | 32 KB threadgroup memory patch (`nvn_spirv_patch.cpp:255`) | M1 limit | required: Adreno and Mali also have 32 KB `maxComputeSharedMemorySize` |
   | integer vertex input patch, stage linking by name (`nvn_spirv_patch.cpp:4,305`) | Metal | valid Vulkan; keep |
   | EQUAL depth slack (`nvn_raster.cpp:65`, DEBUG-2 #60) | separately compiled stages round differently | probably needed on Adreno too. Better: decorate `Position` `Invariant` in the patch pass |
   | present MAILBOX, else IMMEDIATE (`nvn_vk.cpp:265-279`) | two drifting 60 Hz clocks | Android offers FIFO (MAILBOX on some drivers). Use FIFO with `ANativeWindow_setFrameRate(60)` on 90/120 Hz panels, and drive `hle_vi`'s vsync event from `AChoreographer` so there is one clock |
   | pipelined fences (`nvn_frames.cpp`, DEBUG-2 #61) | encoding happens inside MoltenVK's `vkQueueSubmit` | keep; the logic is generic. On native drivers the cost moves from submit to the `vkCmd*` calls in the replay, so the "GPU wait" share shrinks and replay CPU becomes the bound |
   | clear-only passes, pending clears (`nvn_clears.cpp:138`) | Metal encoder per pass | more important on tilers, see 3.4 |
3. **Mobile GPU risks**, to be measured in milestone A2/A3 on one Adreno and one Mali device:
   - **BCn textures.** Adreno and Mali have no `textureCompressionBC`; Xclipse (RDNA) does. The game ships BC1, BC4,
     BC5 and BC7 next to ASTC (TILING.md). Options:
     - decode to RGBA8/RG8/R8 at upload (CPU, after untiling in `nvn_upload.cpp`). This costs up to 8x memory for
       BC1/BC4;
     - transcode BC7 → ASTC 4x4 (same 8 bits per texel) offline, in the data tool. This is derived from the user's
       dump, so it is allowed.

     First count BC bytes in the RomFS to size this. ASTC stays native (it is native on the M1 too).

     **Decided (2026-09-30, by the project owner): transcode on the Mac, when the phone's data pack is built.** See
     "Decisions" below for how.
   - **Storage images without format.** Ryujinx declares images as `Unknown` format, and the harness adds
     `StorageImageRead/WriteWithoutFormat` (TRANSLATOR.md:134). `shaderStorageImageReadWithoutFormat` is often false
     on Adreno and Mali. Fix: in `nvn_spirv_patch.cpp`, rewrite the image's format from the texture bound at draw
     time, per pipeline key. 26 compute blocks are affected.
   - **Subgroups.** The translation assumes warp 32. Adreno runs waves of 64 or 128 and Mali uses 16. Count the
     blocks that use SHFL/VOTE in `results.tsv`; those need `subgroup_size_control` or an emulation.
   - **Fragment shader interlock** is listed in the translator's host caps (TRANSLATOR.md:77) and is absent on
     Adreno and Mali. Confirm that no Wonder block emits it; if any does, re-translate with the capability off.
   - **Descriptor limits.** Every draw allocates 4 sets (`nvn_sets.cpp`; pool of 65,536 UBO and 16,384 SSBO in
     `nvn_vk.cpp:340`). Per stage, NVN allows 14 uniform blocks plus `c[1]` plus the support buffer, 16 storage
     buffers and 32 textures. The Vulkan minimums are 12 UBO, 4 SSBO and 16 sampled images per stage, and older
     Mali drivers report values close to them. Take the real per-stage maxima from the reflection (TRANSLATOR.md:356)
     and check them against `maxPerStageDescriptor*` at start.
   - **Per-draw descriptor cost.** Allocating 4 sets for each of 594 draws per frame (PERF-PLAN) is costly on mobile
     drivers. Use `VK_KHR_push_descriptor` (Adreno 6xx+, recent Mali) for sets 0-1, and cache set 2 (PERF-PLAN
     step 4).
   - **Image compression.** Rendered images stay `GENERAL` (PROGRESS.md 2026-09-28), and sRGB views need
     `MUTABLE_FORMAT`. Both can disable UBWC (Adreno) or AFBC (Mali), which multiplies bandwidth. Use optimal layouts
     for attachments, and a format list (`VK_KHR_image_format_list`) instead of a blanket mutable format.
   - **Pipeline creation.** There are 12,717 blocks and pipelines are built lazily per state key. Adreno compiles
     take tens of ms (estimate), which shows as hitches the Mac never had. Mitigations:
     - a `VkPipelineCache` saved to the data folder;
     - a key list recorded while playing, then pre-built on worker threads at load screens;
     - EDS1/2 as dynamic state to keep keys few. EDS3 is patchy on mobile, so keys grow there.
4. **Tilers.** A course frame has 62 passes, 10 of them clear-only (PERF-PLAN). On TBDR every pass loads and stores
   its attachments through memory. So PERF-PLAN step 5 moves from "headroom" to required on phones:
   - pending clears keyed per attachment;
   - no depth attachment in passes that do not use it;
   - `storeOp DONT_CARE` from NVN's discard commands;
   - `loadOp DONT_CARE` when the whole target is overwritten.

## 4. Binary size and build

- **Generated code.** `gen/main` is 1.3 GB of C++ in 1,201 files (measured today; the "~630 MB" figure is out of
  date), plus `gen/sdk` at 139 MB. On the Mac `libgame_main.a` is 201 MB, the `wonder` executable 186 MB (153 MB
  of `__text`), and `sdk` builds in 69 s with `-j6`. Extrapolated, a clean NDK build of `main` takes about 10-15 min at `-j6`. NDK r28 is the same LLVM generation as
  Apple clang, so a similar speed is expected. Peak memory is roughly 1-2 GB per compile job (estimate), so use
  `-j4` on the 16 GB Mac, and never while the game is running. Incremental builds only rewrite changed files
  (PROGRESS.md: rebuild in 25 s).
- **No LTO**, and no split of the game into several `.so`. Hidden visibility makes calls between recompiled
  functions direct (`CMakeLists.txt:39-41`); a second `.so` would put PLT stubs on those calls. Lazy loading is not
  needed either: the `.so` is mapped and paged on demand.
- **Package.** Stripped, `libwonder.so` should be about 160-175 MB. By Hades's ratio (45 MB → 15 MB APK) a
  compressed APK is ~55-60 MB, but installing extracts the library again. Better for sideloading: the library stored
  uncompressed with `extractNativeLibs=false` (the script already runs zipalign `-p`): a ~175 MB APK mapped in
  place, no second copy. AAB and Play are irrelevant: the APK is never published (2.5).
- **Data on the phone:** about 4 GB (RomFS 3.5 GB, SPIR-V ~0.45 GB, modules), plus the pipeline cache and saves.

## 5. Performance budget

- **Mac today** (course 1-1, docked 1080p, headless, pipelined): 60.0 fps steady. The game main thread takes under
  12 ms for 92-99% of frames; the game's governor drops to 30 fps when it passes 15.8 ms (PROGRESS.md #43). The
  submit thread took ~22 ms synchronously: replay 8.2-8.8 ms (lower after PERF-PLAN steps 2 and 4), encode plus
  fence 12 ms. Handheld 720p: 52-57 fps synchronous.
- **Phone CPU** (estimates from public single-thread scores against the M1 Firestorm core): Snapdragon 8 Gen 2
  (X3) ~0.8x, 8 Gen 3 (X4) ~0.9-1.0x, 8 Elite and Dimensity 9400 ≥1.1x. The main thread scales with that: 12 ms on
  the M1 becomes ~15 ms on an 8 Gen 2, right at the governor's 15.8 ms threshold. **The CPU main thread is the first bound on phones, not the GPU.** Hades, with 1/10th of the code, is
  already CPU-bound in combat on the S23.
- **Submit thread.** The MoltenVK encode cost goes away, but the driver's `vkCmd*` cost and the 4 descriptor sets
  per draw come in. The replay must stay under ~12 ms on the second big core.
- **GPU.** Adreno 740/750 and Immortalis-G720 are in the M1's class on paper. As tilers they suffer from 62 passes
  per frame and `GENERAL` layouts. Handheld mode (720p, `NX_HANDHELD=1`) should be the Android default. Going below
  720p would need the translator's render-scale support-buffer fields (TRANSLATOR.md:278). Not planned.
- **What matters most on mobile, in order:** (1) both hot threads on big cores with ADPF hints; (2) push
  descriptors and the per-submit handle cache (PERF-PLAN 2/4); (3) the pass and load/store work of 3.4; (4) the
  pipeline pre-build of 3.3; (5) native D24 and R11G11B10.
- **Thermals.** Phones sustain about 60-70% of peak after 10-20 min (estimate). Plan a 30 fps cap as a config option
  (`frame_limit=30`, via the existing frame limiter). The game already has its own 30 fps mode, which the governor
  triggers.

## 6. Memory

- **Mac:** 3.5-4.6 GB physical footprint in runs to Bowser's castle, GPU images included (unified memory). It is
  the guest heap (committed as touched; Switch applications get about 3.2 GB), a host `VkImage` next to every guest
  texture (packaged textures exist twice: tiled in guest memory, untiled on the GPU), widened formats (R11G11B10 →
  8 bytes per texel, D24 → D32F), the 256 MB upload ring (`nvn_vk.cpp:225`, peak use 67 MB) and the 186 MB binary.
- **Android budget.** A foreground game on a 12 GB phone can usually hold 5-6 GB before the low-memory killer acts;
  on an 8 GB phone, 3-4 GB (estimates; varies by OEM). Target 12 GB phones. 8 GB phones are best effort.
- **What to cut, in order of risk:**
  1. upload ring from 256 MB to 96 MB, checked against the logged peak;
  2. native D24 and R11G11B10 (3.2);
  3. `VkShaderModule`s destroyed after the pipeline is built, and the SPIR-V file bytes not kept;
  4. host images of sampled-only textures that are unused for N frames, evicted and re-uploaded from guest memory
     (the guest copy is authoritative for packaged textures; render targets are never evicted);
  5. BC decode output kept as small as possible (R8 and RG8 for BC4 and BC5);
  6. `madvise(MADV_DONTNEED)` on guest pages the game decommits. Already the case through `plat::decommit`: verify.

  Do not drop mips or reduce texture sizes: that is visible and needs its own validation.

## 7. Risks, ranked

1. **CPU main thread over 15.8 ms on 8 Gen 2-class phones.** The governor then locks 30 fps. Mitigations: big-core
   pinning and ADPF; recompiler codegen work (the `NX_RESTRICT` experiment from Hades); accept 30 fps on older
   SoCs.
2. **BCn textures on Adreno and Mali.** Decode cost at load and memory growth. Mitigation: offline BC7 → ASTC in the
   data tool.
3. **Memory and the low-memory killer** on 8 GB phones (section 6).
4. **Translated-shader features that mobile drivers lack:** read-without-format images, subgroup size, interlock,
   depth clip control. Each has a known workaround, but each is a correctness hunt per GPU vendor.
5. **Pipeline-compile hitches.** Mitigation: the disk cache plus pre-build.
6. **Driver bugs.** Adreno drivers differ by OEM build. Hades already found one class (image count). Testing on
   devices with the latest drivers mitigates most.
7. **Tiler bandwidth** from `GENERAL` layouts and many passes.
8. **Legal and hygiene.** The APK carries recompiled game code: private builds only. No NVN headers
   (`runtime/src/nvn/nvn.h` stays our own, from traces). Ryujinx (MIT) stays offline tooling; no code from GPL
   emulators. Only the user's own dump, keys and firmware fonts.

## Decisions

### D1. BC textures: transcoded to ASTC on the Mac, in the data pack (2026-09-30)

Chosen by the project owner, over decoding at load time on the phone. Why:
- the phone spends no CPU on it at load;
- it spends no extra memory: decoding to RGBA8 takes up to 8x the memory of BC1/BC4;
- the phone's GPU reads ASTC natively.

The cost is a slight quality loss, far less visible than any glitch. It is also derived from the owner's own dump,
so it is within the ground rules.

**How.** The BNTX files in the RomFS are not rewritten. BC and ASTC blocks differ in size for BC1/BC4 (8 bytes per
4x4 block, where ASTC always uses 16), and the game computes its texture memory and tiling from the file. Rewritten
files would move every texture after the first changed one. Instead the data pack carries a side table:
1. `tools/pack_phone_wonder.py` walks the RomFS textures (`tools/maxwell/bntx.py` already reads BNTX). For each BC
   image and mip level it:
   - untiles the image as the runtime does (`nvn_texture_layout`);
   - transcodes it with an ASTC encoder run on the Mac, at a mapping that keeps the bits per texel:

     | BC | ASTC |
     |---|---|
     | BC7, BC3 (RGBA, 8 bpp) | 4x4 (8 bpp) |
     | BC5 (RG, 8 bpp) | 4x4 (8 bpp) |
     | BC1 (RGB, 4 bpp) | 6x6 (3.56 bpp) |
     | BC4 (R, 4 bpp) | 6x6 |
     | BC6H (HDR) | ASTC HDR 4x4 where the phone has it, else RGBA16F at load (the only runtime decode, for few textures) |

   - stores the result in `astc.pack`, one indexed file keyed by a hash of the tiled BC bytes and the level, so a
     lookup costs one hash.
2. At upload (`nvn_upload.cpp`), when the device has no `textureCompressionBC`:
   - a BC image is created as the table's ASTC format and size;
   - the table's data is uploaded in place of the BC bytes;
   - views and samplers follow the image format, as the R11G11B10F → RGBA16F widening already does.

   The guest never sees the change: its memory keeps the BC bytes. A miss (a BC texture the pack does not have, for
   example one written by the GPU) falls back to a CPU decode and is logged, so the pack can be completed.
3. Devices with `textureCompressionBC` (Xclipse, and Macs) keep BC as is: the table is ignored.

**First step (A2):**
- count BC images, bytes and formats in the RomFS;
- time the encoder on the whole set on the M1 (it runs once per pack);
- compare a few transcoded textures against the BC originals (PSNR) before committing to the 6x6 mapping.

### D2. Home screen: the player picks the data folder (2026-10-01)

The app opens on a home screen (`games/wonder/java/.../Home.java`, the APK's only Java, built by
`android\build_apk.ps1` with `javac` + `d8` when `games\<game>\java` exists; the interface is built in code).
- It finds the data by itself, or lets the player browse the internal storage and pick a folder. Any of our zips
  extracted as they come works: the phone's (`Super Mario Bros. Wonder/`), the PC's (`private/wonder/`, also below a
  `SwitchGamesRecomp/` folder) and the portable one (`WonderPC/data/`), or the folder itself.
- It shows what the folder has (modules, RomFS, `spirv/results.tsv`, the button font) and asks for all-files access
  when missing. "Jogar" writes the path to `<files>/data_root.txt` and starts `android.app.NativeActivity`.
- `android_main` (`platform_android.cpp`) uses that folder first, then the app's own folder (adb push), then
  `Internal storage/<title>` as before. Test runs that start the NativeActivity directly read the same file: pick
  the default folder on the home screen before them, or their `config.txt` goes to another folder.
- `minSdkVersion` 30 (Android 11): all-files access is checked with `Environment.isExternalStorageManager`.
## 8. Phased plan

Rough effort is for one developer working at the Mac track's pace. The Mac track stays the reference: every Android
milestone compares frame dumps (`NX_DUMP_FRAMES`) against the Mac on the same scripted input (`NX_INPUT_SCRIPT`).

| # | Milestone | Work | Exit criteria | Effort |
|---|---|---|---|---|
| A0 | Groundwork | split `platform_android.cpp` under 700 lines; `games/wonder/AndroidManifest.xml`; `-march=armv8.2-a+crypto` and 16 KB page alignment in CMake; `config.txt` → `setenv`; `memguard_android.cpp`; Roboto font fallback; data pack tool (single-file SPIR-V) | `build_apk.sh --game wonder` produces an installable APK; the Mac build is unchanged | 3-4 days |
| A1 | CPU side on the phone, headless | boot with `NX_HEADLESS=1` on an Adreno 740 phone | 2,000+ frames, no crash, fps and memory in `nx.log`; the main thread's frame time is measured (the key number for risk 1) | 2-3 days |
| A2 | First pixels | present through `vk_present.cpp` (rotation, crop); surface loss; SUBOPTIMAL not treated as a rebuild; device feature and limit report; D24 native; BC → ASTC side table (D1); depth-clip fallback | the title screen matches the Mac dump (the key-art texel within ±2); home and back works 10 times | 1.5-2 weeks |
| A3 | Course correct | read-without-format patch; subgroup and interlock audit; descriptor-limit check; R11G11B10 native trial; `Invariant` position; pipeline cache on disk | course 1-1 frames match the Mac across the W7 glitch set; zero pipeline failures; second launch with no compile hitch over 50 ms | 2-3 weeks |
| A4 | Playable | drawn touch overlay; controller mapping; audio paced from AAudio; A-hold fix | 1-1 finished with touch and with a pad; 10 min with no audio underrun in the log | 1 week |
| A5 | Performance | ADPF and affinity; push descriptors; pass load/store and clear merging; pipeline pre-build; FIFO with Choreographer vsync | 1-1 handheld at 60 fps (99% of frames under 16.7 ms) on 8 Gen 3-class; a stable 30 on 8 Gen 2 if 60 is not reached; a 30-min run holds its target | 3-4 weeks |
| A6 | Memory | the cuts of section 6; memory budget via JNI | peak PSS + GPU budget under 3.5 GB; 30 min on a 12 GB phone with no kill; one 8 GB phone reported | 1-2 weeks |
| A7 | Breadth | a Mali/Immortalis device and an Xclipse device; World 1 end to end; saves across app kills | the W7 glitch set is clean on 2 vendors; world 1 done; the save survives a force stop | 2-3 weeks |

### Progress (branch `feat/android-wonder`)

A0, 2026-09-30:
- `platform_android.cpp` is split into window/threads/main (568 lines), `platform_android_input.cpp` and
  `platform_android_audio.cpp`, with `platform_android.h` between them.
- `games/wonder/AndroidManifest.xml`: package `io.github.belmantegu.wonderrecomp`, Vulkan 1.3 required, landscape.
- CMake adds `-march=armv8.2-a+crypto` (game code and runtime) and `-Wl,-z,max-page-size=16384`.
- `config.txt`: its `NX_*` lines become environment variables before boot. `NX_HANDHELD=1` is the Android default.
- `memguard_android.cpp`: Pss from `/proc/self/smaps_rollup`; the limit is `NX_MEM_LIMIT_GB`, else 60% of RAM.
- The shared font falls back to Roboto.
- `tools/pack_phone_wonder.py` makes `Wonder-data.zip`: 31,766 files, 3.92 GB, stored. The SPIR-V stays loose files
  for now; the single-file pack is moved to A2, where its loading time can be measured.
- All 62 runtime objects, the NVN layer included, compile with NDK 28.2. The Mac build is unchanged.
- **Exit met (build side):** `android/build_apk.sh --game wonder` produces `private/wonder/apk/wonder.apk`: 58 MB
  (libwonder.so 187 MB stripped), package `io.github.belmantegu.wonderrecomp`, min SDK 29, signature verified. A clean
  NDK build at `-j4` took about 2 hours on the M1 (1,332 objects, with `-g`). Installing it waits for a phone (A1).

A1, 2026-09-30, Galaxy S23 (DEBUG-2.md #78):
- First fixes: the alias region moved below the modules (Android's zygote sits in the old one at random), and the
  data went to Internal storage with all-files access by `appops`.
- **Exit met:** the game runs on the phone at 40-60 fps at the title, with no failed draw and no crash, and memory
  under the guard (5 GB set in `config.txt`).
- Tools: `tools/wonder_push_android.sh` (install and push, any serial) and `tools/wonder_run_android.sh` (run, log,
  pull the frame dumps).

A2, in progress (DEBUG-2.md #79, #80, #82):
- The present pass (`nvn_present.cpp`): rotation, fit, alpha 1, surface format and composite alpha from the surface,
  and a new window after home or the lock screen. Checked on the Mac with `NX_PRESENT_TEST`; the on-device check
  waits for the phone.
- BC is native on Adreno: D1 is for Mali.
- The emulator cannot render on the M1 (SwiftShader only).

A4, started (DEBUG-2.md #81):
- Drawn touch controls for Wonder, with dash at full tilt.
- Short taps work in the menus.

**To test when the phone is back:**
1. `bash tools/wonder_push_android.sh <serial> --no-data` installs the new APK and keeps the data.
2. `bash tools/wonder_run_android.sh <serial> 90 s23b`.
3. Look at the screen: the title, upright, with bars and the touch controls drawn.

Total: about 12-16 weeks. A0-A2 can start while the Mac track finishes W7. They share no files with the glitch work
except `nvn_vk.cpp` (present and swapchain) and `nvn_format.cpp`, where the Android changes are per-device branches
chosen from capabilities, not platform `#ifdef`s.
