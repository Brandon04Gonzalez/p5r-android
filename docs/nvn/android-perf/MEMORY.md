# Wonder on Android: where the memory goes (Galaxy S23, course 1-1)

Code and data study only (no build, no run). Inputs: `private/wonder/phone_{rec,pipe1,sync1,gpu1}/memory.txt`
(`tools/wonder_phone_memwatch.sh`, every 20 s) and the matching `nx.log`. "Est." means inferred, not measured.

## 1. What was measured

| run | title/map (t=60-120 s) | course (end) | Graphics end | Native end | MemAvailable end | guard Pss (nx.log) |
|---|---|---|---|---|---|---|
| rec | 2.09 GB | 3.63 GB | 1.17 GB | 0.09 GB | 0.94 GB | 1.34 peak, then 0.54 → 0.48 |
| pipe1 | 2.43 GB | 3.62 GB | 1.14 GB | 0.11 GB | 0.93 GB | similar |
| sync1 | 2.07 GB | 3.23 GB | 1.02 GB | 0.07 GB | 1.12 GB | similar |

Growth comes in steps at scene loads (title → map → course: +0.4, +0.5, +0.7 GB) and then stays flat, so it is
not a steady per-frame leak. Graphics grows at each step too (0.69 → 0.80 → 1.02 → 1.14 GB).

## 2. Why the guard sees 0.5-1.3 GB and dumpsys 3.6 GB

`memguard_android.cpp` reads only the `Pss:` line of `/proc/self/smaps_rollup`. dumpsys `TOTAL PSS` is
`Debug.MemoryInfo.getTotalPss()` = smaps Pss **+ SwapPss** + memtrack (GL/EGL mtrack). Two parts are invisible to it:

1. **GPU memory (~1.1 GB, "Graphics").** Adreno Vulkan allocations are kgsl buffers. Device-local ones are not
   CPU-mapped at all, and mapped ones (`/kgsl-3d0` VMAs, PFN-mapped) contribute no Pss. dumpsys gets them from the
   memtrack HAL (kgsl per-process accounting).
2. **Swapped-out anonymous memory (est. ~1.9 GB at the end).** The S23 swaps to zram. Under pressure, cold guest
   pages (heap, module data, the font, cold malloc) leave RAM. `Pss` drops, `SwapPss` rises, dumpsys TOTAL does not
   change. The evidence is the shape of the curve: guard Pss *fell* from 1.34 to 0.48 GB at the course load while dumpsys
   rose to 3.6 GB. 3.63 - 1.17 (Graphics) - 0.48 (Pss) ≈ 1.9 GB of SwapPss. Verify this with the `TOTAL SWAP PSS`
   field (item 4).

Consequences: the guard (5 GB limit in `config.txt`, 60 % of RAM by default) can never fire. dumpsys TOTAL also
overstates physical use, because zram pages cost ~1/2-1/3 of their size (ASTC/texture bytes compress poorly, ~1:1).
GPU memory is never swapped. **MemAvailable is the signal the LMK actually acts on.**

### 2.1 How the guard should measure (Android only; Mac/Windows untouched)

3. Track `vk_bytes` in the runtime: an atomic that each `vkAllocateMemory`/`vkFreeMemory` site updates (nvn_image.cpp:177,
   nvn_vk.cpp:440, nvn_frames.cpp:559, nvn_present.cpp:430, nvn_pipeline.cpp:615, nvn_dummy.cpp:44, nvn_debug.cpp).
   This is exact, costs nothing, and works on every platform (also log it on the Mac). Optional cross-checks:
   `VK_EXT_memory_budget` `heapUsage` when the driver exposes it, and `/sys/class/kgsl/kgsl/proc/<pid>/gpumem_*` when
   SELinux allows it (often denied to untrusted_app, so it must never be required).
4. Measure total = `Pss + SwapPss` (both lines of smaps_rollup) + `vk_bytes` + ~60 MB of driver/swapchain slack. This
   is the dumpsys-equivalent. Also log `MemAvailable` (`/proc/meminfo` is readable) and **act on it**: below 700 MB,
   trim (drop `g_reusable`, idle descriptor pools, cold images per item 12); below 350 MB, log and exit cleanly
   before the LMK does. Every 30 s, log a per-region split from `/proc/self/smaps` (Rss+Swap of the heap VMA at
   0x50_0000_0000, alias 0x20_..., modules, kernel region, `libwonder.so`, `[anon:libc_malloc]`): that turns the
   estimates below into measurements. Add `TOTAL SWAP PSS` (last field of the `TOTAL PSS:` line) to
   `wonder_phone_memwatch.sh`.

## 3. Budget (course 1-1 end state, ~3.6 GB dumpsys)

| category | est. MB | how estimated |
|---|---|---|
| **Graphics (measured 1,140-1,170)** | | |
| upload ring | 256 | `make_upload_buffer(256 << 20)`, nvn_vk.cpp:237, host-visible, allocated up front |
| texture + render-target images | ~750-800 | remainder of Graphics; ~1-2k images created per run (log counter); ASTC is native on Adreno 740 (0 decoded, log), BC not used |
| of which render targets | ~150-250 | Wonder's chain at handheld res; R11G11B10F stored as RGBA16F (8 B/px), D24S8 → D32S8 (5 B/px) |
| swapchain | ~50 | 5 images × 1080×2340×4 (log: "5 images"; asks 3, driver gives 5) |
| descriptor pools | ~20-55 | up to 4 slot pools (log: "4 slots"), each 65,536 UBO + 65,536 CIS + 16,384 SSBO…, maxSets 32,768; Adreno ~64-128 B/descriptor |
| pipelines / driver | ~30-60 | cache blob is 6.7 MB of binaries; per-pipeline driver objects |
| staging (`staging_alloc`) | 0 steady, 100-300 peak | ≥ 1 MB textures each get their own buffer, freed at retire; this is the spike at each scene load |
| **Non-GPU (dumpsys - Graphics ≈ 2,450, mostly swapped)** | | |
| guest heap, touched pages | ~1,800-2,000 | remainder; the game gets 3.2 GiB (`svc_process.cpp` info 6/21); it holds the *tiled copy of every packaged texture* (~same bytes as the GPU textures), models, archives, audio |
| guest module images + bss | ~60-80 | exefs main 28 MB / sdk 6 MB compressed, decompressed into the module range |
| `libwonder.so` resident text | ~60-120 | 173 MB r-x mapping (phone_maps.txt); file-backed, clean, reclaimable |
| shared font 5 | 23 | `hle_pl.cpp` `kernel_alloc` + memcpy of 22.9 MB shared_5.ttf (Arial Unicode based) |
| native heap | ~100 visible (+ swapped) | dumpsys Native Heap 0.07-0.11 GB at the end (0.4 GB at boot); SPIR-V words kept in `ShaderInfo` for every used shader (~35 KB each), `VkShaderModule` cache (nvn_pipeline.cpp:61), results.tsv map (2.6 MB raw), pipeline cache 6.7 MB |
| ART, system libs, stacks | ~100-150 | typical for a NativeActivity |
| RomFS caching (hle_fs.cpp) | ~0 | reads go straight to guest buffers, no cache |
| command lists, logs | < 20 | per-frame lists are recycled; logs are files |

## 4. Findings

5. **Textures exist twice.** The tiled copy stays in guest memory (the game loaded it there, and it is authoritative
   for re-uploads) and the untiled copy sits in a `VkImage`. On the phone, the guest copy is the main thing pushed
   to zram, and it compresses poorly.
6. **Fixed-size buffers sized for the Mac's worst case.** The 256 MB ring carries ~3.6 MB per frame in the course
   ("upload 3.6 MB" per frame in the log); the measured busy-submit peak was 67 MB on the Mac, and that was before
   ≥ 1 MB uploads moved to `staging_alloc`. No "upload buffer full" was logged on the phone.
7. **Scene loads spike.** Every ≥ 1 MB texture gets its own host-visible staging buffer (nvn_upload.cpp:225). A
   course load can hold hundreds of MB until the submit retires, exactly when MemAvailable is lowest.
8. **Possible image retention across scenes (hypothesis).** `MemoryPoolFinalize` (nvn_objects.cpp:114) does not drop the
   images of `Texture` objects over that pool. If Wonder frees a pool without `nvnTextureFinalize` for every
   texture, those images live until the object is re-initialised. Graphics stepping up by scene fits this, and
   also fits real content growth: item 4's live-image log decides.
9. Already fine: no ASTC/BC CPU decode on Adreno 740; `g_reusable` keeps images only 8 submits; kept/snapshot
   textures expire in 2 s; decommit does `MADV_DONTNEED`; frame-dump buffers exist only with `NX_DUMP_FRAMES`.
10. D24 → D32F costs nothing (4 B either way); D24S8 → D32S8 costs +1 B/px. Not worth the risk.

## 5. Ranked reductions

Target: dumpsys ≤ 2.5 GB and MemAvailable ≥ 2 GB. GPU cuts count 1:1 against MemAvailable; guest/native cuts count
partly (zram). Gate every change by `#ifdef __ANDROID__` or by a config default that only Android's `config.txt`
sets, so the Mac/Windows defaults do not change. Verify each one with `tools/wonder_phone_memwatch.sh <serial> out.txt`
over the same scripted pipe1 input (config in `phone_pipe1/config.txt`): compare the end-of-course TOTAL/Graphics/
MemAvailable with the baseline (3.62 GB / 1.14 GB / 0.93 GB), plus a frame-dump diff (`NX_DUMP_FRAMES`) for visual ones.

11. **Measurement first** (`memguard_android.cpp`, the `vk_bytes` counter, `wonder_phone_memwatch.sh`). Saves 0 MB, risk none.
    It makes every later number real (heap Rss+Swap, live image bytes by kind, staging peak). Verify: guard total within
    ±10 % of dumpsys TOTAL.
12. **Upload ring 256 → 64 MB on Android** (`nvn_vk.cpp:237`, new `NX_UPLOAD_RING_MB`, Android default 64). Saves
    **192 MB GPU**. Risk low: wrap waits on the oldest submit, logged as "upload buffer full". Verify: no such log line
    over the course run, fps unchanged.
13. **Release images when their pool is finalized** (`MemoryPoolFinalize`: move images of textures whose
    `memory` lies in [pool.memory, +size) to `g_grave`), only if item 11 shows growth across scenes. Saves **0-300 MB
    GPU** (unknown until measured). Risk medium (DEBUG.md #38/#42: slots sampling finalized textures; the dummy
    then applies). Verify: Graphics no longer steps up on map → course → map; frame-dump diff of 1-1.
14. **Evict cold sampled-only images** (cut 4 of ANDROID-PORT §6): images of CPU-uploaded, never-rendered
    textures not sampled for N s (e.g. 10 s, using `last_sampled`) are freed and re-uploaded from guest memory on
    the next use. Render targets are never evicted. Saves **150-300 MB GPU**. Risk medium-high: re-upload hitches,
    and a re-upload pages guest bytes back in from zram. Verify: Graphics after 2 min on the course, the log's upload
    MB/frame, frame-time spikes.
15. **Bound staging at scene loads** (`staging_alloc`, nvn_frames.cpp:544): above 64 MB staged in one submit, split
    the submit (or retire it) before allocating more; reuse a small pool of staging buffers. Saves **100-300 MB of
    peak** (none steady). Risk medium (DEBUG-2 #72 wrap bug class). Verify: Graphics at t=160/220 s no longer
    overshoots the plateau; MemAvailable minimum during the load.
16. **Native R11G11B10F on Adreno** (`nvn_format.cpp:51`, the widen paths in nvn_upload.cpp:20/125 and
    nvn_pipeline.cpp). Keep RGBA16F on MoltenVK, where Metal writes NaN/Inf. Saves **~30-60 MB GPU**. Risk medium
    (NaN bloom/blur regressions): diff the dialog blur and bloom frames.
17. **Right-size descriptor pools** (`make_descriptor_pool`, nvn_vk.cpp:414): log the per-submit peak, size to 2×
    that, and chain a second pool on exhaustion instead of failing. Saves **~20-40 MB**. Risk low. Verify: no
    "descriptor pool exhausted".
18. **Drop SPIR-V after pipeline creation** (`ShaderInfo` words in nvn_shaders.cpp; module cache nvn_pipeline.cpp:61):
    keep reflection only; on a module-cache miss, re-read `<hash>.spv` (~35 KB, cheap). Saves **~40-80 MB native**.
    Risk low-medium (patched_spirv needs the words: reload them). Verify: Native Heap, pipelines/s in the log.
19. **Smaller shared font 5**: ship a subset TTF (only the PUA/button glyphs and the ranges the game asks for) as
    `fonts/shared_5.ttf`. Data-only change, no code. Saves **~22 MB**. Risk low: check the button prompts and the menus.
20. **Swapchain**: render the present pass into a 1280×720-class buffer (`ANativeWindow_setBuffersGeometry`, the
    compositor scales) or a 3-image request the driver honours. Saves **~20-35 MB**. Risk low-medium (scaling
    quality, rotation transform 2).
21. **Guest heap (research, last).** The floor is the game's own working set. Do not shrink `TotalMemoryAvailable`
    (svc_process.cpp) without proof: the game may size fixed heaps from it and abort. Measure heap Rss+Swap first (item 11).

**Expected:** items 12+16+17+18+19+20 (low/medium risk) ≈ **330-430 MB**, of which ~260-330 MB is GPU. Adding items
13+14 gives ≈ **0.5-1.0 GB**, putting the course at ~2.6-3.1 GB dumpsys and ~1.4-1.9 GB MemAvailable. Reaching
≤ 2.5 GB dumpsys probably also needs the guest-heap story (item 21) or accepting that ~1.9 GB of it is
zram-backed SwapPss. **MemAvailable ≥ 2 GB is the better acceptance metric** for an 8 GB phone, and the GPU cuts
move it the most.
