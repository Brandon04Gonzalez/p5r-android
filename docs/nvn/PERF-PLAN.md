# Wonder at 60 fps docked: what bounds the frame, and the plan

Read-only study (2026-09-30) of logs and code. Savings are estimates until the instrumentation below is in.

## Where the frame goes (course, docked 1080p, headless, synchronous)

- **The submit thread is the limiter, not the game thread.**
  - Wonder records frame N+1 on one thread while another submits frame N (DEBUG.md #56).
  - The main thread's frame is under 12 ms for 92-99% of course frames, and the game's governor stays at interval 1.
- **Submit thread, per frame (~22 ms):**
  - replay 8.2-8.8 ms;
  - "GPU wait" 11.7-12.1 ms, which is `vkQueueSubmit` (Metal encoding happens inside it) plus the fence;
  - ~1.4 ms outside both timers (object drain, image frees, descriptor pool reset, write-backs).
- **Per-frame counts:**
  - 594 draws replayed (156 recorded, the rest through CallList);
  - 62 passes, 10 of them clear-only;
  - 3 compute dispatches.
- **Resolution.** Handheld 720p waits 9.7 ms, so about 4-6 ms of the docked wait scales with resolution and about 7 ms
  does not (encoding, pass overhead).
- **The GPU profile inflates itself.** "Outside passes" 3 ms is mostly the clear-only passes, and each timestamp is an
  extra pass on Apple GPUs.

## Why async GPU did not help (`NX_ASYNC_GPU`)

- `frame_begin` retires the last submit that has write-backs before every replay. The course dispatches every frame,
  so every replay still starts with a fence wait.
- Waits move to game threads, under the global GPU lock:
  - `SyncWait`;
  - `QueueFinish`;
  - `MemoryPoolInvalidateMappedRange`;
  - `MemoryPoolFinalize`.
- The main thread's frame then grows, the governor drops toward 30 fps, and loads slow down.

## Plan, in order

1. **Instrumentation.**
   - Split encode (`vkQueueSubmit`) from fence time in `end_and_wait`.
   - Time the pre-replay and post-submit segments.
   - MoltenVK performance statistics into our log.
   - Counts and blocked time of the game's sync waits.
   - `NX_TRACE_PASSES=<frame>`: one frame's pass list.
2. **Low-risk CPU work** (replay ~8 → ~6 ms).
   - `apply_texel_writes` takes the registry lock exclusively for every sampled texture of every draw (~5,400 per
     frame), and each exclusive lock invalidates the per-thread lookup caches. Give the texture an atomic
     "has writes" flag instead.
   - Per-draw heap and string churn:
     - `build_sets` reserves ~130 KB per draw;
     - `sampler_for` builds a string key per binding;
     - `graphics_pipeline` builds a ~700-byte string key per draw.
   - Skip descriptor sets with no bindings (4 sets are allocated per draw).
   - Skip the empty headless present submit.
3. **Pipelined fences** (the big one: 7-10 ms of submit-thread wait).
   - Keep the replay synchronous and MoltenVK's synchronous submits, but do not wait for the fence; two frames in
     flight, throttled by slot reuse.
   - Split the command buffer after the last dispatch, so write-backs hang on the first part's fence.
   - `SyncWait` / `QueueFinish` / `Invalidate` / `Finalize` wait only when a pending write-back overlaps what they
     ask about, and never take the GPU lock.
4. **More replay CPU** (toward 4-5 ms).
   - A per-submit cache of handle → texture, image, view and sampler (the replay mark of #55/#57 makes it constant
     within a submit).
   - Dynamic uniform offsets and a cached texture set.
   - Filter redundant per-draw state.
   - Cheaper lookups and uploads:
     - `pool_at_gpu` last-hit cache;
     - dedup as a flat hash;
     - index uploads through dedup;
     - one support buffer per viewport;
     - one zero stream per submit.
5. **GPU side.**
   - Pending clears keyed per attachment (removes ~10 clear-only passes). The earlier subset experiment (#47) must
     be redone carefully.
   - Drop the depth attachment from passes that never use it.
   - MoltenVK settings to try one by one:
     - `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`;
     - `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1`;
     - a `VkPipelineCache` on disk for loads.

Expected: step 2 gives about 50 fps; step 3 reaches 60 if replay plus encode stays under ~14 ms; steps 4-5 are headroom.

## Result (2026-09-30, W7)

- Steps 1, 2 and 4 are done (patches A and C).
- Step 3 (pipelined fences) is on by default since DEBUG-2.md #61.
- Course 1-1 headless: **60.0 fps steady** (synchronous: 48-58). Windowed confirmation is pending.
- Step 5 remains headroom.
