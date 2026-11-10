# P5R: 30 fps cutscenes on Android

Goal: hold 30 fps in a 2D/anime cutscene without touch input on a Vivo Y27 5G (Dimensity 6020, Mali-G57 MC2,
Android 15, 60 Hz) without changing 3D rendering or adding artificial load.

## What runs now

- ADPF reports the **present-to-present interval** by default (`NX_ADPF_REPORT=interval`). Pacing is included, so a
  frame that misses the 30 fps deadline reports more than the 33.3 ms target and asks for a clock boost. The old
  measurement (`active`, work since the previous present's pacing) excluded the sleep that precedes the frame.
- `NX_ADPF_REPORT=active` reproduces the old behaviour; `cpu` reports the session threads' own CPU time from
  `/proc/self/task/<tid>/schedstat` (honest work, but it under-reports a frame-limited game).
- `NX_ADPF_TARGET_SCALE=0.5..1.0` (default `1`; 0.7-0.8 is a useful experiment) or `NX_ADPF_TARGET_MS=<ms>` adds headroom to the target.
- `APerformanceHint_setThreads` is used on Android 14+ instead of closing and recreating the session when another
  frame thread joins.
- `[P5R_PERF]` diagnostics: `NX_PERF_DIAGNOSTICS=1` (launcher: Performance diagnostics), `perf_diag.txt` in the data
  folder, or `NX_PERF_DIAGNOSTICS=1` in the environment. One line every 2 s (or 120 presents) in `nx.log`.
- GPU timestamps are enabled only when the queue family has `timestampValidBits`; the clock is read from KGSL,
  Mali `cur_freq` or devfreq when available.

## Build and install

Native Android build (Android SDK 35, NDK 28.2, Vulkan-Headers 1.4.357, JDK 17, plus the shader translator):

```bash
bash games/p5r/tools/build_harness_android.sh   # once, in Docker
bash games/p5r/tools/build_android.sh --install
```

Unit tests (no game data):

```bash
cmake -S . -B build -DNX_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

## Measurement protocol

1. Charge the phone; let it cool to battery <= 33 C, skin <= 34 C, thermal status 0.
2. Use the same cutscene and the same route every run. Three runs per cell, 2 minutes each.
3. Cells: `IDLE` (no touch), `TOUCH` (touchscreen held/dragged), `ADPF=0`, `ADPF_REPORT=interval` (default),
   `ADPF_REPORT=active`, `ADPF_REPORT=cpu`, and `NX_ADPF_TARGET_SCALE=0.8`.
4. Between runs wait for the thermal headroom to return to its starting value.
5. Export logs from the launcher (Export logs -> `Download/p5r-log.txt`) or `adb logcat -s p5r`.

| Cell | Run | avg fps | 1% low | interval p50/p95 ms | work ms | pacing ms | gpu present ms | main frame ms | main CPU ms | vsync wait ms | ADPF report/target ms | thermal |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| IDLE, ADPF interval | 1 | | | | | | | | | | | |
| IDLE, ADPF interval | 2 | | | | | | | | | | | |
| IDLE, ADPF interval | 3 | | | | | | | | | | | |
| TOUCH, ADPF interval | 1 | | | | | | | | | | | |
| IDLE, ADPF active | 1 | | | | | | | | | | | |
| IDLE, ADPF cpu | 1 | | | | | | | | | | | |
| IDLE, ADPF off | 1 | | | | | | | | | | | |

## Reading the log

```text
adpf: target 33.33 ms (scale 1.00), report interval (default)
adpf: session made for 3 threads
[P5R_PERF] 2.00 s, 60 presents, 29.9 fps; interval avg 33.4 max 41.2 ms [...]; work avg 18.1 max 29.0,
pacing avg 15.2 max 16.0, present call avg 0.6 max 1.1 ms; waits vsync 0.0 ms/0 other 0.0 ms/0; gpu present
avg 3.0 max 5.0 ms; main frame avg 21.0 max 35.0 cpu avg 12.0 waits 4.0/60; adpf target 33.33 avg 33.4 max 41.2
over 3, 3 threads
```

- `interval` is the result; `work + pacing` should add up to it (plus waits and present call).
- `vsync` waits are the game's own display event; `other` is `WaitSynchronization` on everything else.
- If `IDLE` shows `interval` 38-40 ms, `work` ~15-18 ms and `pacing` ~15 ms, the frame is being downclocked during
  the sleep. `active` will show the same low work; `interval` should make ADPF boost on the misses.
- If `gpu present` or `main CPU` is the large term, the fix is outside ADPF (GPU/decoder/CPU work).
- If thermal headroom falls below ~0.85, repeat the A/B after cooling; do not compare across heat states.
