# Pipelines and memory on the replay thread (2026-10-18, branch opt/pipelines)

Goal: take pipeline creation and memory pressure off the replay thread so the S23 holds 60 fps (replay 10.5-12 ms at
60 fps, 15.83 ms is the game's 30 fps threshold). Tested on the PC only (Intel UHD 468b, headless, scripted run to the
Flower Kingdom intro, `NX_HANDHELD=1 NX_PIPELINED=2`, `MESA_SHADER_CACHE_DISABLE=true` so that only our own
`VkPipelineCache` counts). "Measured" = a log line of these runs; "inferred" = reasoning about the Adreno 740.

## 1. Why so many pipelines (question 1)

Measured, with a counter added to the log (`created over N frames`, `raw keys shared an existing pipeline`):

- The in-memory map (`graphics_pipeline`) never erases and is keyed by the whole `PipelineKey`, so **no pipeline is
  built twice in a process**: creations == distinct keys. The janitor and the LRU do not touch pipelines.
  The 2,167 creations of the phone session are 2,167 distinct keys, not re-creations.
- The key is finer than what Vulkan reads. It carries every attribute the game set (not only those the vertex
  shader reads), all 16 streams, blend factors of disabled blends, depth/stencil state for passes without a depth
  attachment or with the test off, and `color.alpha_test/logic_op` that the pipeline never uses. In the scripted run
  **47 of 256 new raw keys (18 %) were the same Vulkan pipeline as an earlier one**; they were compiled again.
- The remaining distinct keys are real: 165 distinct (vs, fs) pairs gave 209 pipelines (state variants of the same
  shaders: blend, depth, targets). Nothing in them changes per frame: viewport, scissor, blend constants, stencil
  reference/masks and depth bias are already dynamic, and the extended-dynamic-state on the S23 would only merge the
  cull/front-face/depth-compare variants (a handful, not measured, not done).
- So the 2,167 are mostly new shader/state combinations met once per session (the replay creates them on the first
  draw). That the pipeline cache file did not make them cheap (8 ms each on the phone, 17.7 s / 2,167) is not
  explained by the logs: on the Intel a cache hit costs 0.03 ms (255 pipelines in 8.7 ms), so whether the Adreno
  driver treats a `VkPipelineCache` hit as free is **not verified**. The worker/prewarm below does not depend on it.

## 2. What changed

| Commit | Change |
|---|---|
| canonical key + workers + prewarm | `canonical_key()` zeroes what the create info never reads, so raw keys that differ only there share one pipeline (second map by canonical key; the raw map stays the per-draw fast path). Compiles can run on worker threads (`PipeBuild` owns the create info). **Prewarm**: the canonical keys of every pipeline a session built are appended to `pipelines_<vendor>_<device>.keys` (next to the cache, saved with it every 10 windows, `WarmRec` = shader hashes + key, 16384 records max); at the next start, each submit scans 64 records, and for those whose shaders the game has already loaded (`shader_by_hash`) prepares the create info on the replay thread (<= 1 ms per submit) and sends the compile to 2 workers (`NX_PIPE_THREADS`, 0 = off; `NX_PIPE_PREWARM=0` = off). A draw that needs a pipeline in flight claims it if no worker has started it, else waits for it. `NX_PIPE_CANON=0` turns the canonical key off; `NX_PIPE_VERIFY=1` builds both create infos for every new raw key and logs differences (0 of 256 differ, apart from fields declared ignored: compare op with the depth test off, blend ops with blend off, bias without depth test). The log gains per-window lines for aliases, shader modules (ms), pipelines sent to / compiled on workers, waits, skipped draws, and the descriptor pools' peak use per submit. |
| `NX_ASYNC_PIPELINES=1` (opt-in, **off**) | A draw whose pipeline is not built yet is skipped while a worker compiles it. Cold run: 0 pipelines on the replay thread but up to **5,367 draws skipped in one 60-frame window** (79 pipelines on 2 workers at ~23 ms each: objects missing for about a second at a scene start). It changes the picture, so it stays off. |
| texture uploads through staging | `texture_stage()`: uploads of 1 MB and up (as before) and, once a submit has put a quarter of the ring in, 64 KB and up, get a staging buffer; the pitch-linear and widened paths now use it too (they always went through the ring). |
| Android upload ring 128 -> 64 MB | see 4. |

## 3. Numbers (Intel, scripted run, 115 s, same source of cache each time)

| | pipelines created on the replay thread | ms in them | compiled on workers | notes |
|---|---|---|---|---|
| before, cold cache (first launch) | 255 | 3,160 | 0 | |
| after, cold cache | 209 | 3,200 | 0 | 47 aliased; same compile cost per pipeline (driver), only fewer |
| before, warm cache (cache file only) | 255 | 8.7 | 0 | Intel cache hits are free |
| after, warm cache + keys (prewarm) | 44 | 0.7 | ~205 | 6 replay waits for a compile in flight, 14 ms in total |
| after, second warm run | 53 | 318 | | the 53 are keys not yet in the file, compiled (not cache hits) on the replay thread |

Gameplay fps is 60.0 in every run and replay ms per frame (7.3-7.9) moves with the machine's load between runs, not
with the change: **the replay ms per frame difference is not measurable on this PC**, as the Intel cache hits are
nearly free. The saving shows up on a driver where a pipeline creation costs milliseconds on every start: the
prewarm moves those off the replay thread; the canonical key removes 18 % of them.

`NX_PIPE_VERIFY=1`: 0 differences in 256 checked raw keys. Frame dumps cannot be compared byte for byte: the
uncapped scripted run is not deterministic (two runs of the *same* binary differ in every frame dump), so equality of
the create infos is the check; a dumped frame looks right.

## 4. Memory (question 3)

- **Ring size.** The scripted run's one big ring user was not many textures: it was **one 64 MB allocation**, the
  first upload of the 2048x8192 R8 glyph atlas widened to RGBA8 (`alpha_from_r`), through the ring (measured by
  logging sizes: 1 allocation of 67,108,864 bytes; backtrace upload_texture, then ensure_contents). With the ring at 64
  MB that gave 102 "upload buffer full" lines (the submit wraps over its own data: possible corruption of that load's
  draws), and 32 MB crashed. With the atlas upload in a staging buffer (freed when its submit retires): **0 "upload
  buffer full" lines at 64 and at 32 MB**, fps unchanged. Android default is now 64 MB (was 128); staging absorbs the
  difference (`staging_wanted` is relative to the ring: 71-86 staging buffers of 1-90 MB in the load window instead of
  18, transient).
- **Resident memory on the PC** (`VmHWM` sampled every 3 s) at ring 256 / 128 / 64 / 32 MB: 2.156 / 2.153 / 2.161 /
  2.134 GB: **no measurable difference on the Intel** (the iGPU's ring pages are shared memory that does not show as
  the process's resident set the way the phone's). The Android saving (64 MB less host-visible GPU memory, a
  smaller zram footprint) is inferred from the size alone.
- **Descriptor pools** are sized 32,768 sets and 65,536 / 65,536 / 16,384 / 4,096 descriptors per slot (4 slots
  pipelined). Peak use measured per submit in the intro: **563 sets, 3,668 uniform buffers, 635 samplers, 47 storage
  buffers** (under 6 % of the pool). The log now prints this every window, so the phone's course run gives the real
  peak; sizing the pools down is **not done** (exhaustion drops draws, and what Adreno allocates up front for a pool
  is unknown).
- Not done: images kept across scenes (13), SPIR-V words after module creation (18), staging spikes beyond the
  atlas. The glyph atlas itself could stay R8 with a (1,1,1,R) view swizzle instead of a 64 MB RGBA8 image (48 MB less
  GPU memory, and no widening at upload): not tried, it touches how swizzles reach `view_of`.

## 5. Expected on the S23 (inferred)

- Second and later sessions: the 1,500-2,000 pipelines of a course are compiled on 2 worker threads while the
  game loads, instead of 8 ms each on the replay thread during play; the hitches of new shaders (60-frame windows of
  100-400 ms) should mostly go. The first session after install has no keys file: as before (minus 18 %).
- Steady replay (10.5-12 ms) is **not** pipeline creation (17.7 s over 7 minutes is 0.7 ms per frame averaged); it
  will not move. What may move: the 30 fps stretches that coincide with bursts of new pipelines (Wonder effect,
  course clear).

## 6. Risks

- Android: 2 extra threads while loading (mid cores compete with the replay thread; `NX_PIPE_THREADS=1` to reduce);
  the keys file grows by ~700 bytes per new pipeline (cap 16384 records, ~11 MB); ring 64 MB wraps over its own
  submit if a load puts more than 64 MB through it (logged as "upload buffer full", glitch for that frame, no crash
  in the scripted run). `texture_stage` makes many 64 KB-1 MB staging allocations in a heavy submit (count capped by
  nothing but the driver's allocation limit, 4096 on Adreno: a load had up to 86 per window).
- Mac (MoltenVK): pipeline compile from worker threads is allowed by Vulkan but not tested here; if Metal pipeline
  creation misbehaves, `NX_PIPE_THREADS=0` restores the old behaviour. Windows: standard `std::thread`, untested.
- Prewarm compiles pipelines the session may not draw (the keys file is cumulative across courses); only those
  whose shaders the game has loaded are started.
- Not verified: anything on Adreno; Mac; Windows; a course run (only the intro); `NX_PIPE_VERIFY` does not cover
  modules built with other integer-input kinds (the module handle is compared, and it matched).
