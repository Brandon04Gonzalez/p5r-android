# Wonder on Android, phase 4: the periodic hitch, GPU at real load, replay ≤ 11 ms (Galaxy S23)

This is research and planning only (2026-10-01). No runtime code was changed.

Inputs: `PLAN.md`, `PHASE3.md`, `MEASUREMENTS.md`, `MEMORY.md`; `private/wonder/phone_{c2,c1,spin1,heap1}` (nx.log,
profile.txt); the scratchpad tools `bases.py`, `full1.py`, `agg2.py`, `own2.py`, and two new ones, `hspace.py` (hitch
spacing) and `guardper.py` (memory-guard period); the code at commit 9f23df1, which the line numbers refer to. "Est."
marks an estimate. Every step below is one phone run (or one A/B set) that tests one thing, in order.

## 0. Summary

| # | Step | Expected | Phone runs |
|---|---|---|---|
| T0 | Tooling: thermal gate, MEMWATCH=0, run summary, build-id-checked profile report | measurement hygiene | 0 |
| H0 | Hitch instrumentation + `NX_MEMGUARD=ab` (guard source alternates inside one run) | confirms the cause | 1 |
| H1 | Memory guard from `/proc/self/statm` (no page-table walk) by default on Android | hitches 40 → < 5 per 600; course +3 to +6 fps | 2 (A/B) |
| D1 | simpleperf: minor/major faults and context switches with call chains (diagnostic) | decides U1, M1, K* | 1 |
| G0 | GPU profile without serialisation (modes 2, 3) + GPU clock | GPU ms at real load | 2 |
| C2b | Command-pool and descriptor-pool resets on a janitor thread | submit thread −0.8 to −1.1 ms | 4 (ABBA) |
| U1 | Steady-state ≥ 1 MB uploads through the ring, not a fresh staging buffer | −0.3 to −0.5 ms; replay faults −50% (est.) | 4 |
| C1r | Re-test `NX_RENDER_PASS_OBJECTS=1`, cooled, ABBA | −1.0 to −1.8 ms (the profile says so) | 4 |
| C4 | Per-submit object resolve cache | −0.8 to −1.2 ms | 4 |
| C5 | CallList closure once per submit | −0.3 to −0.6 ms | 4 |
| C6 | Last-hit caches (pool, guest bytes, handle history) | −0.3 to −0.5 ms | 4 |
| G5A | Clear-only passes as transfer clears (Android) | sub −0.3 to −0.6; GPU −0.1 to −0.3 | 4 |
| G1a | MUTABLE_FORMAT only with a format list | GPU −0.5 to −2 (est.) | 4 |
| G2 | Native B10G11R11_UFLOAT for NVN 0x3f on Adreno | GPU −0.8 to −2 (est.); −30-60 MB | 4 |
| G4 | Pass-end barrier without vertex stages | GPU −0.5 to −2 (est.) | 4 |
| G1b, C7, K*, M1 | Conditional on G0, D1 and H0 data | see §5 | |

Order: T0, H0, H1, D1, G0 first. Then it branches on G0's decision (§3): if the GPU is ≥ 15.5 ms busy at load,
G1a/G2/G5A/G4 move ahead of C4-C6.

**Most likely hitch cause** (§1.1): the Android memory guard (`memguard_android.cpp`) reads `/proc/self/smaps_rollup`
every 250 ms. On a process with ~3 GB resident or swapped in one guest-heap VMA, that read is a 40-60 ms page-table
walk under `mmap_lock`. Every `mprotect`/`mmap`/`munmap` blocks behind it: the write watch, scudo, the driver's
per-pass allocations and `vkResetCommandPool`. So do page faults queued behind such a writer. The main thread stalls
for the rest of the walk.

## 1. Findings

### 1.1 The periodic hitch follows the memory guard, not the frame

**Hitch spacing in time** (`hspace.py`). Consecutive `hitch:` lines are spaced 14-17 frames apart. Converted with the
fps of their 60-frame window, that is a fixed time:

| fps window | c2 spacing, median (frames → ms) | c1 | spin1 | hitch length, median |
|---|---|---|---|---|
| 40-45 | 14 → 313 ms | 14 → 318 ms | 13 → 308 ms | 48-57 ms |
| 45-50 | 14 → 304 ms | 15 → 309 ms | 15 → 304 ms | 49-50 ms |
| 50-55 | 16 → 295 ms | 15 → 294 ms | 15 → 288 ms | 42-50 ms |
| 55-60 | 17 → 290 ms | 15 → 256 ms | - | 38-46 ms |

The spacing in frames grows with fps, so the cause runs on a timer and is not frame-locked game work.

**The guard's real period** (`guardper.py`). The guard sleeps 250 ms, reads, and logs every 120 reads. The
fps-weighted wall time between its log lines is 34.0-37.6 s in all four runs, so each loop takes **288-313 ms**:

| Run | Per guard loop (ms), over the run |
|---|---|
| c2 | 295 288 288 287 292 293 306 303 |
| c1 | 295 294 292 291 287 301 306 304 |
| spin1 | 295 297 293 295 290 300 300 301 |
| heap1 | 296 295 293 284 297 310 301 313 |

So **one `smaps_rollup` read takes 38-63 ms**, and it grows late in the run as RSS and swap grow. Period and length
both match the hitch: ~300 ms apart, 45-60 ms long.

**Why the read blocks other threads:**
- `smaps_rollup` walks every PTE of every VMA (`Rss`, `Pss`, `Swap` accounting) with `mmap_lock` held for read. The
  5.10+ "release the lock if contended" logic only applies *between* VMAs. The guest heap is committed by `mprotect`
  in one fixed reservation (`platform_android.cpp:68-87`), so it is one multi-GB VMA: ~0.8 M PTEs, ~50 ns each.
- Writers need the lock exclusively:
  - write-watch faults (`write_watch.cpp:90`, game threads writing the glyph cache);
  - `write_watch_take` (`:76`, the replay thread);
  - scudo's secondary `mmap`/`munmap`;
  - the driver's per-pass blocks and their frees in `vkResetCommandPool`;
  - `staging_alloc` (`nvn_frames.cpp:609`).
- While a writer waits, new readers queue behind it, and readers include every page fault. The main thread takes ~40
  minor faults a frame.
- The harness's `dumpsys meminfo` every 20 s (full `smaps`) gave the 110-300 ms stalls PHASE3 §0.4 recorded: the same
  mechanism, larger.

**Ruled out:**

| Suspect | Why not |
|---|---|
| Waits on the replay | I1 shows 0.0 ms at every site |
| vsync thread (`hle_vi.cpp:46-56`) | 16.67 ms free-running; no ~300 ms component |
| Audio (`hle_audio.cpp:402-436`) | renders every 5 ms (240 samples); update pacing is per render; 0 underruns; no 240-300 ms cadence |
| Job-system yields | `NX_YIELD=spin` (spin1) left the hitch spacing unchanged |
| SleepThread | `nanosleep`, µs-precise |
| Game GC/streaming | would be frame-locked, but the spacing is time-locked |
| Frame dumps | 1 per 600 frames |

H0 below confirms the cause in one run; H1 is the fix.

### 1.2 What the profiles can and cannot say

- **Symbol mismatch.** `ba/libwonder.so` (built 14:21) is newer than the c2 (14:13) and c1 (14:20) runs. Their
  own-code names are wrong ("snprintf 6.8%", "texture_race_old 3.4%", chains through `operator delete`).
  - Only the driver and libc entries resolve correctly, because those libraries did not change.
  - spin1 matches the build, but its profile window was a menu (replay 0.7-4.6 ms).
  - Fix: T0 records a build id per run.
- **Sample rate.** The in-app sampler uses per-thread CPU-time timers at 1 ms. The kernel fires them at tick
  granularity (HZ 250, inferred from the counts), so **one sample ≈ 4 ms of CPU**. c2's replay thread: 1,517 samples in 10 s at ~44 fps
  (course) → ~13.8 ms of CPU per frame. **1% ≈ 0.14 ms/frame.**

**c2, replay thread, by outermost Adreno entry point** (valid):

| Entry | Share | ms/frame |
|---|---|---|
| `vkCmdBeginRendering` | 11.4% | ~1.6 |
| `vkResetCommandPool` (frame_begin, `nvn_frames.cpp:352`) | 8.9% | **~1.2, on the critical path** |
| `vkCmdDrawIndexed` | 3.2% | ~0.45 |
| `vkAllocateMemory` (kgsl ioctl; inferred `staging_alloc`: only ~1,000 images per run) | 2.6% | ~0.37 |
| `vkUpdateDescriptorSets` + `vkAllocateDescriptorSets` + `vkCmdBindDescriptorSets` | 4.1% | ~0.57 |
| `vkEndCommandBuffer`, `vkQueueSubmit`, barriers, fences | ~4.5% | ~0.6 |

**c1 (`NX_RENDER_PASS_OBJECTS=1`), same thread and window kind:**
- `vkCmdBeginRenderPass` 46 samples (against 173 for BeginRendering), `vkResetCommandPool` 55 (against 135).
- The driver lost ~207 of ~1,500 samples (**−1.9 ms of CPU per frame**). Yet the measured replay rose and total
  samples stayed the same: the c1 run was warmer (headroom falling), and its own code took ~130 more samples at lower
  clocks.
- **C1 was rejected on a thermally confounded run.** It must be re-tested cooled and ABBA (C1r).

**Own code:** heap1 (PHASE3 §0.1, matching build) stays the reference: `get<Texture>` 8.5%, `replay_list` 6.1%,
`dedup_upload` 5.4%, `count_dispatches` 3.8%, `lookup_handle` 2.9%, `guest_bytes`/`pool_at_gpu` 2.5%.

### 1.3 Parts of the course are GPU-bound

- **Fence waits.** In c2's profile window the fence wait was 2.0-4.3 ms per frame at 40-46 fps. The submit thread waits
  on the GPU (two submits in flight), so there the GPU frame is ≈ 20-22 ms at the clocks DCVS chose.
- **Present intervals.** In course, 109-274 of 600 sit at 19-25 ms.
- **Consequence.** After H1 the course mean is bounded by whichever of CPU (~17 ms submit thread) or GPU is slower. G0
  must come before the CPU series.

### 1.4 Faults and switches (for D1)

- **Replay thread.**
  - ~80 minor faults a frame. Likely sources: first-touch of every fresh `staging_alloc` buffer (256 faults/MB) and
    the driver's per-pass heap blocks.
  - Majors spike to 2,500-7,700 per 600 frames in some windows: zram swap-ins of guest texture/vertex bytes the replay
    reads.
- **Main thread.** ~52 voluntary switches a frame. `sched_yield` counts as involuntary, so these are real blocks:
  futex waits on the kernel's condvars, contention on `g_k` (the single kernel `recursive_mutex`), and nanosleep.
  H0's per-kind accounting splits them.
- **Wake-up storms.** `notify_all()` (`kernel.cpp:67`) wakes every thread in `WaitSynchronization` on every signal:
  vsync at 60 Hz, the audio render thread at 200 Hz, SignalEvent. Cheap, but it contends on `g_k`. K1 if H0 shows it.

## 2. Measurement protocol

### 2.1 RUN, ABBA, decisions

```
RUN <name> [NX_...] :=
  REPLAY=private/wonder/phone_rec/replay.txt MEMWATCH=0 bash tools/wonder_phone_course.sh RXCX402B6JX <name> \
      NX_PIPELINED=1 NX_AFFINITY=1 NX_MAX_INTERVAL=1 [NX_...]
```

**Build and install** (each code step):
```
bash android/build_apk.sh --game wonder
adb -s RXCX402B6JX install -r private/wonder/apk/wonder.apk
```

**Repetitions:**
- **Large effects** (H1: hitches, ≥ 5× the noise): one A/B pair, each cooled; H0's in-run alternation has already
  shown the direction.
- **CPU/GPU items of 0.3-2 ms** (noise ±1 ms on replay, ±0.3 ms per GPU label): ABBA, i.e. A1 B1 B2 A2, every run
  after the thermal gate.
  - Metric: the median over course windows (`per frame over 60` lines with replay ≥ 8 ms), plus their IQR.
  - **Accept** if B beats A by ≥ 0.4 ms in *both* pairs and the course-mean headroom of the four runs spans ≤ 0.04.
  - Otherwise repeat the pair once; then reject or keep it off by default.
  - Also compare replay normalised by the replay thread's core clock (T0 logs it): `replay_ms × MHz / 1000` (mcycles).
    That is the thermal-proof number.
- **Image check** for every item: the frames every 600 (`tools/frames.py sheet`), `tools/find_glitches.py` and
  `tools/detect_explode.py` on `phone_<name>/frames`.

**Mac regression** (each item not gated to Android, and a smoke run for gated ones):
```
ninja -C private/wonder/build -j6 wonder
bash private/wonder/course.sh <name> [NX_...]          # prints the per-60 fps tail: expect 60.0 throughout
python3 tools/detect_explode.py private/wonder/frames_<name>
python3 tools/find_glitches.py private/wonder/frames_<name>
python3 tools/frames.py diff private/wonder/frames_<ref>/<N>.png private/wonder/frames_<name>/<N>.png
```

### 2.2 Cooling and thermal check

New `tools/phone_thermal_wait.py <serial> [--battery 33.0] [--skin 34.0] [--timeout 900] [--interval 30] [--once]`
(Python; no shell heredocs). It loops:
1. Read `adb -s <serial> shell dumpsys thermalservice`:
   - `Thermal Status: (\d)`;
   - each `Temperature{mValue=V, mType=T, mName=N, mStatus=S}` under "Current temperatures from HAL" (type 0 CPU,
     1 GPU, 2 battery, 3 skin; Samsung names AP/BAT/SKIN as in MEASUREMENTS.md);
   - opportunistically, any line containing "headroom".
2. Read `adb -s <serial> shell dumpsys battery`: `temperature: NNN` (tenths of °C) and `level`.
3. Print one line `thermal: status S, battery B °C, skin K °C, cpu C °C, gpu G °C, level L%`.
4. Exit 0 when status == 0, battery ≤ 33.0 and skin ≤ 34.0. Otherwise sleep `--interval` and repeat, up to
   `--timeout`, then exit 2 (`--once`: read and print, exit 0).

**Rules:**
- The app is force-stopped during the wait.
- Same cable and charge state for every run, battery ≥ 50%, case off, same surface and orientation, brightness fixed.
- Typical wait after a 5-minute run: 6-10 min.
- The runtime already logs `thermal headroom` every 600 frames. T0 adds one line at the first frame tick
  (`threads: thermal headroom at start X`).
- The summary flags a run whose course-mean headroom is ≥ 0.85 as "warm": do not use it in an A/B.

### 2.3 Extending `tools/wonder_phone_course.sh` (T0)

1. **Switches** after `OUT=...`: `MEMWATCH=${MEMWATCH:-1}` (1: dumpsys every 20 s as today; 0: none; `end`: one
   dumpsys after the loop), `THERMAL=${THERMAL:-1}`, `PROFILE_AT=${PROFILE_AT:-200}` (a multiple of 20),
   `GPUWATCH=${GPUWATCH:-0}`.
2. **Thermal gate.** Move `adb shell am force-stop "$PKG"` before the config push; then, if `THERMAL=1`,
   `python3 tools/phone_thermal_wait.py "$S" | tee "$OUT/thermal_start.txt"`. On exit code 2, print "thermal: still
   warm" and append `# WARM START` to `config.txt`. `THERMAL=0` only records a `--once` reading.
3. **Loop:** wrap the dumpsys block (`M=$(...)` through the `memory.txt` append) in `if [ "$MEMWATCH" = 1 ]`; keep
   `sleep 20`; `[ "$t" = 200 ]` becomes `[ "$t" = "$PROFILE_AT" ]`. With `GPUWATCH=1`, append `$((t+20))` and
   `adb -s "$S" shell cat /sys/class/kgsl/kgsl-3d0/gpuclk /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage` to
   `$OUT/gpu.txt` (sysfs only: no stall; the shell user can usually read it when the app cannot).
4. **After the loop:** with `MEMWATCH=end`, one dumpsys sample before `force-stop`; then
   `python3 tools/phone_thermal_wait.py "$S" --once > "$OUT/thermal_end.txt"`.
5. **Build id:** `llvm-readelf -n private/wonder/ba/libwonder.so | grep 'Build ID'` (NDK llvm bin) into
   `$OUT/build_id.txt`, and `llvm-nm -n --defined-only` of the runtime's symbols (`grep -v ' main_\| sdk_'`), gzipped,
   into `$OUT/libwonder.syms.gz` (a few MB): profiles stay symbolizable after the next build.
6. **Summary:** the closing `grep ... awk` line becomes `python3 tools/wonder_run_summary.py "$OUT"`.

New `tools/wonder_run_summary.py <OUT>` (< 250 lines):
- **Course windows:** the per-60 lines with replay ≥ 8 ms.
- **Medians and IQR:** fps, replay, other, encode, present, fence wait, upload MB, replay mcycles.
- **Totals over course 600-blocks:** main-thread histogram; present intervals (36-50 and > 50 ms).
- **Hitches:** `hitch:` count per 600 course frames, and the share whose extended fields (H0) show guard overlap ≥ 20
  ms or off-CPU-not-in-waits ≥ 20 ms.
- **Per guard mode** (`NX_MEMGUARD=ab`): the hitch count per 600 frames in each mode's frame ranges.
- **Thermal:** headroom min/mean in course, and the start/end thermal readings.
- **Other:** guard read times; staging buffers and MB per 600 (U1); whether the profile window was in course (median
  replay ≥ 8 ms in the 10 s after `profile: sampling`, else "profile not in course: discard").

`tools/android_profile_report.py`:
- Fold in `bases.py` and `full1.py`: PAC-masked frames, per-library base search, and the outermost `qgl::vk*` entry
  with the first runtime caller. Also `agg2.py`/`own2.py` as subcommands (`entries`, `own`, `chain <name>`).
- Before symbolizing, compare `build_id.txt` with `private/wonder/ba/libwonder.so`. On a mismatch, use
  `libwonder.syms.gz` (function-level only), or stop with "build mismatch: names would be wrong". That is §1.2's trap.
- 4 ms/sample: print ms/frame using the run's course fps.
- Keep `libc.syms`/`adreno.syms` under `private/wonder/phone_libs/`.

## 3. Steps: hitches first, then measurement

### H0. Hitch instrumentation and the in-run A/B of the guard source (1 run)

**Goal:** one run that names what each > 33 ms main-thread frame waited on, and that switches the guard source back and
forth to show cause and effect.

**Files and changes:**

1. **`runtime/src/memguard_android.cpp`** (62 lines).
   - **`NX_MEMGUARD` modes:** `smaps` (today; the H0 default); `statm` (new: field 2 of `/proc/self/statm`, resident
     pages × page size; mm counters, no page-table walk, no `mmap_lock`; the default after H1); `off`; `ab`
     (alternate `smaps`/`statm` every 160 reads, ~40-48 s, logging `memory: guard source smaps -> statm at frame N`
     from `nx::g_frame`).
   - **Timing:** each read timed with `monotonic_ns()`; the last 16 reads in a ring of `{begin, end, mode}` atomics;
     new `uint64_t plat::memguard_overlap_ns(uint64_t from, uint64_t to)` (summed overlap with a window). The
     120-read log line adds `source S, read avg A ms, max M ms`, plus `VmSwap` from `/proc/self/status` (log only,
     timed; dropped unless its read is ≪ 1 ms).
2. **`runtime/src/frame_stats.cpp`** (194 lines).
   - Export `struct ThreadCounters { uint64_t cpu_ns; long majflt, minflt, nvcsw, nivcsw; }` and
     `ThreadCounters plat::thread_counters()` (from `thread_cpu_ns()` :34 and `thread_usage()` :133).
   - `frame_thread_tick` (:145): at the first tick log `threads: thermal headroom at start %.2f`; every 60 ticks of
     role 0, the replay thread's core clock (`/sys/devices/system/cpu/cpu<sched_getcpu()>/cpufreq/scaling_cur_freq`,
     a few samples averaged), for the summary's mcycles.
3. **New `runtime/src/kernel_waits.cpp`** (~150 lines), plus hooks.
   - **`kernel_state.h`:** `enum WaitKind { kKLock, kMutex, kCondVar, kAddress, kSync, kSleep, kYield, kWaitKinds }`;
     `void kwait_add(KThread*, WaitKind, uint64_t obj, uint64_t ns)`; `std::unique_lock<std::recursive_mutex>
     klock()` (`try_lock` first; on failure time the blocking `lock()` and `kwait_add(t_self, kKLock, 0, ns)`);
     `extern std::atomic<uint64_t> g_last_notifier`.
   - **`kernel.h` `KThread`** (:16-38): `uint64_t wait_ns[7]; uint32_t wait_n[7]; uint64_t longest_ns, longest_obj,
     longest_waker; uint8_t longest_kind; uint64_t woken_by;`.
   - **Edits in `kernel.cpp`:**
     - `block()` (:116-123) takes `(WaitKind, obj)`, times the wait and calls `kwait_add`. Callers: :171 (kMutex,
       addr), :192 (kCondVar, key), :267 (kAddress, addr).
     - `wake()` (:126-130) sets `t->woken_by = t_self ? t_self->id : 0` (0 = a host thread: vsync, audio device,
       runtime); `notify_all()` (:67-70) sets `g_last_notifier` the same way.
     - `svc_wait_synchronization` (:352-355): time each `cv.wait`/`wait_until` (kSync, obj = the first handle) and set
       `woken_by = g_last_notifier`.
     - SleepThread (:490-509): time `sleep_ns` (kSleep, obj = requested ns); count and time yields (kYield).
     - The seven `std::unique_lock lk(g_k)` in the svc functions (:164, :176, :182, :204, :245, :293, :320) become
       `auto lk = klock();`.
     - **Line budget:** move `spin_naps()` and `hle_get_system_tick` (:80-111) into kernel_waits.cpp.
   - **API:** `KFrameWaits kernel_take_frame_waits()` returns and zeroes t_self's counters and longest wait;
     `kernel_log_waits(600)` logs the top 8 guest threads by blocked ms per kind (job workers 0x7a-0x7e, audio 0x6c,
     ...).
4. **`runtime/src/nvn/nvn_queue.cpp`** (359 lines): `frame_time_probe` (:235) computes per call the loop period, the
   CPU, fault and switch deltas (`thread_counters()`), `kernel_take_frame_waits()` and the guard overlap over
   [now − ms, now].
   - **Hitch line** (cap `NX_HITCH_LINES`, default 2000 in H0):
     ```
     hitch: frame F, main thread M ms (loop L, cpu C, off-cpu not in waits X), waits ms: klock a mutex b cv c addr d
     sync e sleep f yields n; nvn sync/finish/invalidate/finalize g/h/i/j; longest KIND obj 0x.. W ms woken by 0x..;
     faults maj/min, switches vol/invol; guard overlap O ms (read R ms, SRC); replay backlog B; dump D
     ```
     - `off-cpu not in waits` = loop − cpu − Σwaits − nvn frame_ns;
     - backlog = `g_sub_called − g_sub_done`;
     - D = 1 if `g_frame % 600 == 0`.
   - **Baseline:** every 600 frames, the same fields averaged over all frames (`main thread per frame: ...`). Then call
     `kernel_log_waits`.
5. **`runtime/src/nvn/nvn_frames.cpp`:** count `staging_alloc` calls and bytes, and log them in `frame_log` (:486):
   `staging: N buffers, M MB over 60 frames`. This sizes U1.

**Overhead:** two `clock_gettime` per wait (~50 ns), one `getrusage` per frame, an uncontended `try_lock`. Invisible
next to futex waits.

**Phone:** `RUN h0 NX_MEMGUARD=ab NX_HITCH_LINES=2000`.

**Pass criteria (the cause is confirmed if all hold):**
- In `smaps` windows, ≥ 80% of hitch lines show guard overlap ≥ 20 ms and "off-cpu not in waits" ≥ 20 ms.
- In `statm` windows, hitches drop below 5 per 600 frames (the summary's per-mode split).
- The guard's logged smaps read time is 35-65 ms; statm's is < 0.1 ms.

**If it fails:** the waits-by-kind and waker fields name the alternative, and the K/M branch (§5.2) applies.

**Mac:** `course.sh h0` at 60.0 fps; frames identical (instrumentation only). `NX_MEMGUARD` is Android-only.

### H1. The guard without a page-table walk (default on Android)

**Goal:** remove the periodic stall.

**Change:**
- `memguard_android.cpp`: default `NX_MEMGUARD=statm`. The limit check uses resident × page size.
  - RSS ≥ Pss, so the guard stays conservative.
  - RSS includes the resident part of libwonder's text (~60-120 MB), well inside the 5 GB / 60%-of-RAM limits.
- Keep `smaps` as an opt-in (`NX_MEMGUARD=smaps`) for memory studies, and an on-demand `meminfo` request file, read
  by the watchdog in `kernel_watch.cpp` like `profile`, for a one-off `smaps_rollup` + per-region `smaps` split.
- **MEMORY.md item 11** (a per-region `smaps` read every 30 s) is replaced by that request file: periodic smaps reads
  are banned on Android. For the dumpsys-equivalent total use statm RSS + `vk_bytes` (the counter MEMORY.md item 11
  proposes) + `VmSwap` (status, only if H0 showed it is cheap).

- **Expected:** main-thread frames > 33 ms ~40 → < 5 per 600; present intervals > 50 ms 25-38 → < 5 per 600 in
  course; course mean +3 to +6 fps (~48 → ~51-54; the submit thread's ~17 ms and the GPU sections bound the rest).
- **Risk:** low: the guard still kills before the LMK on a leak (RSS grows with a leak just like Pss).
- **Phone:** cooled between them, `RUN h1a NX_MEMGUARD=smaps`, then `RUN h1b`. **Pass:** h1b hitches < 5 per 600
  course frames and > 50 ms intervals < 5 per 600; h1a reproduces ~40.
- **Mac:** `course.sh h1` (unchanged code path) at 60.0.

### D1. Where the faults and the voluntary switches come from (diagnostic, 1 run)

**Goal:** call sites, not guesses, for §1.4. Run it after H1, so the guard's stalls no longer dominate.

**How:** the APK is debuggable (`games/wonder/AndroidManifest.xml:24`), so simpleperf can attach.
1. `RUN d1` with `PROFILE_AT=300` (moves the in-app profile out of the way).
2. At ~200 s (in course), from a second terminal:
   ```
   NDK=$(ls -d ~/Library/Android/sdk/ndk/* | tail -1)
   python3 $NDK/simpleperf/app_profiler.py -p io.github.belmantegu.wonderrecomp -lib private/wonder/ba \
       -r "-e minor-faults,major-faults -c 1 --call-graph fp --duration 10" -o private/wonder/phone_d1/faults.data
   python3 $NDK/simpleperf/report_html.py -i private/wonder/phone_d1/faults.data -o private/wonder/phone_d1/faults.html
   ```
3. Then a second capture with `-r "-e context-switches -c 1 --call-graph fp --duration 10"` (where the main thread
   blocks: guest functions in libwonder, the svc site, and our `kernel.cpp` frames).

**Read per thread** (the replay thread's tid is in `threads:` lines):
- the fault share under `staging_alloc`/`memcpy` (U1), under `qgl::*`→scudo (C1r/C8), under `write_watch`, and under
  guest-memory reads (majors: zram);
- for the main thread: faults under `CommandBufferEndRecording`/vector growth (M1) and switches by svc kind (K*).

**Pass:** the report exists; each fault source above ≥ 5% is named in MEASUREMENTS.md.

**Mac:** none.

### G0. GPU time without serialising the frame (2 runs)

**Goal:** the GPU's busy time per frame at the 60-fps load, plus its clock. This decides whether G-items or C-items
come next.

**Files and changes:**
- **New `runtime/src/nvn/nvn_gpu_profile.cpp`** (~220 lines). Move the GPU profile from `nvn_debug.cpp:319-386` (the
  same API in `nvn_vk.h:232-237`) and add `int gpu_profile_mode()` (NX_GPU_PROFILE = 1, 2 or 3; 0 off).
- **Mode 1:** today's behaviour (serialised).
- **Mode 2:** per-pass labels without whole-frame waits.
  - A `VkQueryPool` (1,024 timestamps) and a label vector per slot, indexed by a new accessor
    `uint32_t frame_current_slot()` in `nvn_frames.cpp`.
  - `frame_begin` (`nvn_frames.cpp:346-367`), after `begin_part`: `gpu_profile_slot_begin(slot, cb)` resets that
    slot's pool (outside a pass).
  - `gpu_profile_mark` writes into the current slot's pool.
  - `next_part` (:271-280) and `frame_submit` (:428) write a mark "(gap until next part)" at the end of a part, so CPU
    lateness between parts is its own label.
  - `retire_oldest` (:165-182), after `wait_fences`: `gpu_profile_slot_retired(slot)` reads with
    `vkGetQueryPoolResults(..., VK_QUERY_RESULT_64_BIT)` and no WAIT (the fence passed); `VK_NOT_READY` is counted
    and skipped.
- **Mode 3:** busy time only. `TOP_OF_PIPE` at each part's start and `BOTTOM_OF_PIPE` at its end; no per-pass marks
  (`gpu_profile_mark` returns early).
  - Logged every 60 frames: `gpu busy X ms/frame (parts P), span S ms, idle before next frame I ms`.
  - Busy = Σ per-part (end − start), span = last end − first start, idle = next frame's first start − this last end.
- **Clock:** every 60 frames (modes 2 and 3), read `/sys/class/kgsl/kgsl-3d0/gpuclk` (Hz),
  `/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage`, else `/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq`. On EACCES, log
  once (`GPUWATCH=1` in the script is the fallback).
- **Timestamp support:** check `timestampValidBits` of the queue family once and log it.
- **`nvn_vk.cpp`** (698 lines: in-place edits only):
  - :554 `gpu_profile_on()` → `gpu_profile_mode() == 1`;
  - :571 the same in the `frame_submit` wait flag;
  - `gpu_profile_begin` (:550) and `gpu_profile_collect` (:575) become no-ops unless mode 1, inside their new file.
- **Other `gpu_profile_on()` call sites** stay: they guard the label strings (`nvn_draw.cpp:214/216/304/498`,
  `nvn_replay.cpp:53/55`, `nvn_upload.cpp:114`).

- **Expected:** a measurement (mode 3 perturbs the least). **Risk:** none (debug only).
- **Phone:** `RUN g0a NX_GPU_PROFILE=3`, cool, then `RUN g0b NX_GPU_PROFILE=2`. **Pass:** course fps within 1.5 of
  h1b and fence wait ≤ h1b + 0.3 ms; mode 2 total ≈ mode 3 busy ± 10%; a GPU clock line or a logged reason.
- **Decision:** if mode-3 busy at load is ≥ 15.5 ms in course windows, the GPU is a 60-fps blocker: G1a, G2, G5A and
  G4 come right after C2b. Otherwise continue with the CPU order.
- **Mac:** `course.sh g0 NX_GPU_PROFILE=2` logs labels and stays at 60.0 (Metal samples timestamps at encoder
  boundaries, so mode 3 is coarse there).

## 4. Steps: replay and submit-thread CPU (target: submit thread ≤ 14 ms, replay ≤ 11 ms)

### C2b. Pool resets off the critical path (janitor thread)

**Today:**
- `frame_begin` (`nvn_frames.cpp:352`) calls `vkResetCommandPool(s.pool)` when the slot is reused: 8.9% of the replay
  thread, ~1.2 ms/frame, inside "other".
- `retire_oldest` (:174) resets the slot's descriptor pool inline.

**Change** (new `runtime/src/nvn/nvn_janitor.cpp`, ~100 lines):
- **The thread:** one janitor thread with a queue of slot indices. On NX_AFFINITY it is placed on the middle cluster,
  role 2 in `frame_stats.cpp` `place()`.
- **Slot state:** add `std::atomic<bool> reset_done{true}` and `bool reset_queued` to `Slot` (:58-81).
- **`retire_oldest`:** after the fences and `apply_write_backs`, instead of the inline descriptor-pool reset, set
  `reset_done = false` and push the slot.
- **The janitor's work:** `vkResetCommandPool(s.pool, 0)` if `pool_used`, then `vkResetDescriptorPool(s.descriptors)`
  if `descriptors_used`, clear both flags, then `reset_done.store(true, release)` and notify.
- **`frame_begin`:** replace :352 with a wait for `reset_done` (spin 50 µs, then a condition variable). It is normally
  already set: the slot retired ≥ 1 frame earlier.
- **Fallback:** `NX_JANITOR=0` keeps today's inline resets.

- **Vulkan rules:** pool resets need external synchronisation of the pool and its command buffers and sets; while
  queued, the slot is retired and not yet begun, so no other thread touches it. `frame_wait_all` (:482), `gpu_hung`
  and shutdown drain the janitor (`janitor_drain()`).
- **Expected:** "other" 2.3 → ~1.2 ms: **submit thread −0.8 to −1.1 ms** (total CPU unchanged, off the critical path).
- **Risk:** low-med: a driver-internal lock could serialise the janitor's frees with recording; the A/B shows it.
- **Phone:** ABBA `RUN c2b-A NX_JANITOR=0` / `RUN c2b-B`. **Pass:** median "other" −0.6 ms or more in both pairs,
  replay not up by more than 0.3 ms.
- **Mac:** `course.sh c2b` at 60.0; frames diff clean (MoltenVK resets are cheap, but the code path is shared).

### U1. Steady-state big uploads through the ring

**Today:**
- `upload_texture` (`nvn_upload.cpp:225-229`) gives every upload ≥ 1 MB its own `staging_alloc` buffer
  (`nvn_frames.cpp:609-637`): vkCreateBuffer, vkAllocateMemory (a kgsl ioctl that zero-fills), map, first-touch
  faults on every page of the memcpy, then destroy and free at retire.
- That is 2.6% of the replay thread in course, ~0.37 ms, plus the faults and a `munmap` under the mmap lock.
- The 128 MB ring carries ~20 MB per frame in course.

**Change:**
- New accessor `uint64_t frame_ring_size()`.
- The condition at :225 becomes:
  `linear.size() >= (1u << 20) && (frame_upload_used() + linear.size() > frame_ring_size() / 4 || linear.size() > frame_ring_size() / 8)`.
  - Scene loads, which stage hundreds of MB (DEBUG-2 #72), keep their own buffers.
  - Course frames use the ring.
- `NX_STAGING_RING=1` is unchanged (always the ring). The new `NX_STAGING_MIN=0` restores today's rule.

- **Expected:** −0.3 to −0.5 ms of replay; replay-thread minor faults −50% or more (est.; H0's staging counter and
  D1 size it). **Risk:** low: `frame_upload_alloc` (:500) already handles wrap, and the quarter-ring bound keeps
  headroom.
- **Phone:** ABBA `NX_STAGING_MIN=0` against the default. **Pass:** `staging: N buffers` in course → ~0; replay
  −0.3 ms in both pairs; replay-thread minor faults per 600 down; no "upload buffer full"; frames clean at the load.
- **Mac:** `course.sh u1`, plus the scene-load frames (the #72 class): no garbage band.

### C1r. Render pass objects, re-tested properly

- **Why:** §1.2: the c1 profile shows the driver's BeginRendering + ResetCommandPool share falling by ~207 samples
  (−1.9 ms CPU/frame), on a warm run.
- **Change:** none (`NX_RENDER_PASS_OBJECTS` exists). Run it after C2b: with the janitor, the reset part of C1's gain
  is already off the critical path, so C1r measures the BeginRendering part. If it passes, make it the Android
  default where `nvn_renderpass.cpp` reads the switch.
- **Expected:** −1.0 to −1.8 ms (est. from the samples). **Risk:** as PHASE3 C1; the pipeline cache's driver blobs
  change, so make one warm-up run before the ABBA set.
- **Phone:** ABBA `NX_RENDER_PASS_OBJECTS=0/1`, every run cooled. **Pass:** replay median −0.8 ms or more in both
  pairs at matched headroom (mcycles too); frames clean; no new validation or log errors.
- **Mac:** `course.sh c1r NX_RENDER_PASS_OBJECTS=1` at 60.0, frames diff clean (it stays off by default there).

### C4. Per-submit object resolve cache

**Today:** `get<T>` (`nvn.h:92-110`) is valid only while `objects_lock().generation` holds. Game threads bump it
~180 times a frame, so during a replay it mostly misses: a shared lock, a hash map, the incarnation map and
`dynamic_cast`. It was 8.5% in heap1.

**Change:**
- While `t_replay_mark != 0`, use a thread-local open-addressing table (4,096 entries, power of two) from
  `(guest, type tag)` to `Object*`. Clear it in `draw_begin_submit` (`nvn_draw.cpp:439`).
- Add an `uint8_t type` tag in `Object`, set in `make<T>`, to replace `dynamic_cast`.
- Do not cache nulls (#64).
- `find_object(guest, mark)` (`nvn_incarnation.cpp`) stays the source of truth. Retired objects are drained only
  between replays, so pointers are stable for one replay.
- `NX_RESOLVE_CACHE=0` turns it off.

- **Expected:** −0.8 to −1.2 ms. **Risk:** low-med, the #55/#64 class: the cache must return exactly
  `find_object`'s answer. Debug check `NX_RESOLVE_CHECK=1` compares every hit with a fresh lookup and logs mismatches.
- **Phone:** ABBA. **Pass:** −0.6 ms in both pairs; `replay lookups resolved a texture re-initialized` per 600
  unchanged; frames clean.
- **Mac:** `course.sh c4 NX_RESOLVE_CHECK=1`: 0 mismatches; then `course.sh c4` at 60.0 and the frames diff.

### C5. CallList closure once per submit

**Change:**
- In `gpu_submit` (`nvn_vk.cpp:531`, edit in place; the helper goes in `nvn_replay.cpp`, 66 lines), walk the
  submitted lists' `CallList` graph once.
  - `command_list()` (`nvn_cmd.cpp:336`) costs a mutex and a map lookup per call.
  - The walk builds a per-submit table, handle → `{shared_ptr<CmdList>, own_dispatches}`.
- `count_dispatches` (`nvn_frames.cpp:247-262`) and the `CallList` case of `replay_list` (`nvn_replay.cpp:42`) read the
  table. Lists are still resolved at submit time, as today.
- `NX_CALL_TABLE=0` turns it off.

- **Expected:** −0.3 to −0.6 ms (`count_dispatches` 1.7-3.8% plus the replay loop's lookups). **Risk:** low.
- **Phone:** ABBA. **Mac:** `course.sh c5`, 60.0, frames diff.

### C6. Last-hit caches

**Change:**
- **`pool_at_gpu`** (`nvn_objects.cpp:48`) and **`guest_bytes`** (`nvn_draw.cpp:27`): a thread-local
  `{begin, end, pool, generation}` last hit, checked before the `upper_bound`.
- **`lookup_handle`** (`nvn_sets.cpp:46`): consult the pool history (`lower_bound`) only when the slot's epoch changed
  after the replay mark; otherwise use the current entry.
- **Size:** `nvn_objects.cpp` is at 700 lines, so move `pool_at_gpu` and its helpers to a new `nvn_pools.cpp` first.

- **Expected:** −0.3 to −0.5 ms. **Risk:** low. **Phone:** ABBA. **Mac:** `course.sh c6`, frames diff.

### G5A. Clear-only passes as transfer clears (Android)

**Today:** `clears_flush` (`nvn_clears.cpp:119-125`) opens and closes an empty pass, ~11 per frame. On Adreno each one
creates the driver's per-pass objects (~40-57 µs).

**Change** (Android, unless `NX_CLEARS_PASS=1`). For each pending colour slot:
- `vkCmdClearColorImage(cb, img, GENERAL, &value, 1, &range)` with the slot's level and layer from `t.views`.
- If the view format is the sRGB variant of a UNORM image (0x25 viewed as 0x38), encode r, g, b linear → sRGB first.
  A loadOp clear through an sRGB view encodes; a transfer clear writes raw.
- Depth/stencil: `vkCmdClearDepthStencilImage` with the aspects in `g_pending.ds`.
- Mirror `begin_rendering`'s side effects on each `GpuImage` (layout GENERAL, `rendered = true`) and the trace line.
- End with a memory barrier, `TRANSFER_WRITE` → `MEMORY_READ|WRITE` (dst stage ALL_COMMANDS), and the
  `gpu_profile_mark`.

- **Expected:** sub −0.3 to −0.6 ms (11 passes), GPU −0.1 to −0.3 ms. **Risk:** low-med: the sRGB clear value and
  the image-state flags.
- **Phone:** ABBA, plus one mode-2 run for the "outside passes" label. **Pass:** `nvn clears: ... alone` unchanged in
  count but no empty passes (log them); frames clean (cleared targets, HUD).
- **Mac:** gated off (MoltenVK encoder cost, #31); `course.sh g5a` must be identical.

## 5. GPU items and conditional items

### 5.1 GPU (after G0; each one ABBA with `NX_GPU_PROFILE=3`, plus one mode-2 run for labels)

**G1a. MUTABLE_FORMAT only where views reinterpret**

- **Today:** `nvn_image.cpp:153` sets `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` on every colour image. On the Adreno 740
  that can disable UBWC. Every run logs a single reinterpretation: `0x25 viewed as 0x38` (17 of 17 logs).
- **`nvn_image.cpp`:**
  - Set MUTABLE only when the image's NVN format is in a set of known view pairs. Seed it with {0x25↔0x38, the BGRA8
    UNORM/SRGB pair}; the set is persisted (below).
  - Chain a `VkImageFormatListCreateInfo` with {the image format, its view formats}.
  - Add `bool mutable_format` to `GpuImage` (`nvn_vk.h`).
- **`view_format`** (`nvn_pipeline.cpp:22-39`): when `nvn != img.nvn_format` and `!img.mutable_format`, log once
  `viewed as X but not mutable: ignored (recorded)`, add the pair to the set, and return `img.format`.
- **Persistence:** the set lives in `view_formats.txt` next to the pipeline cache (data root), read at init, so the next
  run makes those images mutable.
- **Switch:** `NX_UBWC_FLAGS=0` restores today's flags.
- **Expected:** GPU −0.5 to −2 ms (est.).
- **Risk:**
  - low-med: a first-seen pair renders with the wrong sRGB-ness for one run (logged).
  - Check that every view creation (sampled, attachment, storage) goes through `view_format`; audit `view_of`.
- **Pass:** "in all"/busy −0.5 ms in both pairs; the contact sheet's UI colours unchanged; no new "not mutable" lines on
  the second run.
- **Mac:** also enabled (Metal loses lossless compression on mutable textures): `course.sh g1a`, 60.0, frames diff.

**G2. Native R11G11B10F (NVN 0x3f) on Adreno**

- **`vk_format()`** (`nvn_format.cpp`): for 0x3f return `VK_FORMAT_B10G11R11_UFLOAT_PACK32` when
  - not MoltenVK,
  - not `NX_R11G11B10_EMULATE=1`,
  - and its optimal-tiling features include COLOR_ATTACHMENT | COLOR_ATTACHMENT_BLEND | SAMPLED_IMAGE |
    SAMPLED_IMAGE_FILTER_LINEAR | TRANSFER_SRC | TRANSFER_DST (checked once and logged).

  The table entry at :51 stays RGBA16F for the emulated path.
- **Byte layout:** NVN's R11G11B10F matches `B10G11R11_UFLOAT_PACK32` bit for bit (R in bits 0-10, B in 22-31).
- **What adapts by itself:**
  - The widen path (`nvn_upload.cpp:130`) is gated on `img.format == RGBA16F`, so native textures upload raw.
  - `view_format`'s byte rule (`nvn_pipeline.cpp:29-35`) admits 4-byte views again.
  - The blend-factor substitution (`nvn_pipeline.cpp:370-385`) and the alpha-reads-ONE swizzle (:578) become
    redundant but harmless: a format without alpha already reads alpha 1 and blends with destination alpha 1.
- **Copies:** audit `nvn_copy.cpp` copies to and from buffers for 0x3f.
- **Accuracy:** Vulkan converts negative results to 0 (as Maxwell does; RGBA16F kept negatives), and NaN stays NaN.
- **Expected:**
  - GPU −0.8 to −2 ms (est.): half the bytes per load, store and sample of the 960x540 HDR chain, and fewer GMEM bins
    for the MRT passes;
  - −30-60 MB GPU memory.
- **Risk:** medium: the #50 NaN class, the bloom and the dialog blur, and the glow flicker (PLAN.md §7 E4).
- **Pass:** busy −0.8 ms; bloom, sky and dialog-blur frames match the emulated run (`frames.py diff`, p99 ≤ 8).
- **Mac:** gated off; `course.sh g2` identical.

**G4. Pass-end barrier lets binning overlap**

- **`pass_end_only`** (`nvn_draw.cpp:477-498`): the barrier at :486-494 gets dst stages FRAGMENT_SHADER |
  EARLY/LATE_FRAGMENT_TESTS | COLOR_ATTACHMENT_OUTPUT | TRANSFER | COMPUTE_SHADER, with matching access, instead of
  ALL_COMMANDS.
- **Vertex readers:**
  - Add a `rendered_at` serial on `GpuImage`, set at pass end for the pass's attachments, and a global
    `g_vertex_visible_at`.
  - When `gather_textures` (`nvn_sets.cpp`) binds an image to a vertex-stage slot (reflection knows the stage) with
    `rendered_at > g_vertex_visible_at`, it returns kNeedBreak. That pass then ends with today's full barrier and
    advances `g_vertex_visible_at`.
- **Unchanged:** compute keeps its own barrier (`nvn_draw.cpp:187-216`), and `begin_part` keeps ALL→ALL.
- **Switch:** `NX_PASS_BARRIER_FULL=1` restores today's barrier.
- **Expected:** GPU −0.5 to −2 ms (est.).
- **Risk:** medium, the #71 class (half-written tiles): check the HUD and the course on Windows.
- **Pass:** busy −0.5 ms in both pairs; frames clean.

**G1b. STORAGE on demand**

Only if G1a's gain suggests UBWC matters and H0's new counter (`storage views of real images per 600`, in
`storage_view`, `nvn_pipeline.cpp:613`) shows few images. Same design as PHASE3 G1 step 3.

### 5.2 Conditional items (decided by H0 and D1 data)

- **C7** (dedup open addressing + R6 padding, `nvn_dedup.cpp`, `nvn_sets.cpp` `buffer_of`): −0.3 to −0.6 ms. Do it
  after C4-C6 if replay is still > 11 ms.
- **C8** (scudo secondary cache, next to the `mallopt` calls at `platform_android.cpp:576`: `mallopt(M_CACHE_COUNT_MAX, 64)`,
  `mallopt(M_CACHE_SIZE_MAX, 4 << 20)`): only if D1 shows scudo `mmap`/`munmap`/`madvise` under the driver after C1r.
- **K1** (only if H0 shows `sync` waits with spurious wakes, or `klock` > 0.3 ms per frame):
  - Per-object waiter lists. `KEvent`/`KThread` keep their waiters; `notify_all()` becomes `notify(obj)` in
    SignalEvent, the vsync thread (`hle_vi.cpp:54`), the audio thread (`hle_audio.cpp:434`), thread exit and cancel.
  - Wakes only that object's waiters instead of every `WaitSynchronization` thread at 60 + 200 Hz.
- **K2** (if `klock` dominates): shard `g_k`. Use a handle-table mutex, and arbiter, condvar and mutex waiters in
  address-hashed buckets with per-bucket locks. Higher risk; lost-wakeup tests with `NX_TRACE_ADDR`.
- **M1** (if D1 puts main-thread faults under command recording):
  - Recycle `CmdList` storage. `CommandBufferBeginRecording` (`nvn_cmd.cpp:72-77`) takes a recycled list with its
    `cmds` capacity; lists dropped from the handle table return to a small free list.
  - −50% of the main thread's minor faults (est.). The main thread is not the bottleneck, so this is low priority.
- **Pacing** (PHASE3 §6): `NX_GOVERNOR=1` once the submit thread is ≤ 15 ms; P2/P3 at PLAN.md §1's acceptance.

## 6. Memory: cheap wins that also help frame time

1. **H1 itself** (the guard source). Never read `smaps`/`smaps_rollup` periodically on Android. That includes the
   harness: `MEMWATCH=0` for performance runs, `MEMWATCH=1` only for memory studies.
2. **U1** (no fresh staging buffers in steady state): fewer kgsl allocations, faults and `munmap`s.
3. **G2** (−30-60 MB GPU) and **ring 128 → 64 MB** (`NX_UPLOAD_RING_MB`, MEMORY.md #12, −64 MB GPU). These lower the
   zram pressure behind the replay thread's major-fault bursts (2,500-7,700 per 600 frames in some windows; a zram
   swap-in costs ~20-40 µs). Do the ring change together with G2, as a 2-run A/B on MemAvailable and replay-thread
   majors.
4. **MEMORY.md #15** (bound staging at scene loads) pairs with U1. It cuts load-time peaks and has no steady-state
   frame-time effect.

## 7. Expected totals and checkpoints

**CPU, submit thread per frame:**

| | Replay | Other | Encode | Present | Total |
|---|---|---|---|---|---|
| Today (c2) | 13.5 | 2.3 | 0.6 | 0.4 | ~16.8 ms |
| After C2b (−1.0), U1 (−0.4), C1r (−1.4), C4 (−1.0), C5 (−0.4), C6 (−0.4), G5A (−0.4) | **~10-10.5** | | | | **~11.5-12.5 ms** (est.) |

**GPU:** G0 gives the starting point. G1a + G2 + G5A + G4 are worth −2 to −6 ms (est.) against ~20 ms in the heavy
sections (§1.3).

**Pacing:** H1 removes the ~300 ms-periodic 45-60 ms stall. Then the course mean follows max(CPU, GPU).

| Checkpoint | Target |
|---|---|
| After H1 | course > 50 ms intervals < 5/600; course mean ≥ 51 fps |
| After G0 | GPU busy (mode 3) at load recorded with its clock |
| After C2b + U1 + C1r | submit thread ≤ 14.5 ms |
| After C4-C6 + G5A | replay ≤ 11 ms, submit thread ≤ 13 ms |
| GPU series | mode-3 busy ≤ 14 ms in every course window |
| Then | PLAN.md §1 acceptance and the pacing items |

Record each checkpoint in MEASUREMENTS.md:
- course fps (median and IQR);
- replay/other/encode/present/fence;
- > 50 ms intervals and hitches per 600;
- G0 busy and clock;
- start/end temperatures and course headroom;
- the top 6 driver entries with ms/frame (build id checked).

## 8. References

- Linux `fs/proc/task_mmu.c`: `show_smaps_rollup` holds `mmap_read_lock` and releases it only between VMAs when
  contended ("proc: smaps_rollup: do not stall write attempts on mmap_lock", v5.10); `task_statm` reads counters
  only. https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/proc/task_mmu.c
- Vulkan spec, unsigned 11/10-bit float conversion (negative → 0, NaN → NaN):
  https://docs.vulkan.org/spec/latest/chapters/fundamentals.html
- Android simpleperf (debuggable apps, `app_profiler.py`, `report_html.py`):
  https://android.googlesource.com/platform/system/extras/+/main/simpleperf/doc/README.md
- Thermal: `dumpsys thermalservice`; NDK AThermal (`AThermal_getThermalHeadroom`):
  https://developer.android.com/ndk/reference/group/thermal
- Qualcomm Adreno best practices (MUTABLE_FORMAT and UBWC before Adreno 750):
  https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html
- PHASE3.md §10 for the remaining references (Khronos samples, Arm barriers, Turnip notes, MoltenVK).
