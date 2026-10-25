# Wonder on the Mac (M1): where the frame time went, and what was done (2026-10-14)

Method: course 1-1, scripted input, headless (`private/wonder/perf_run.sh`, 290 s), warm pipeline cache; the per-60-frame
`nvn/vk: per frame` log line, `NX_GPU_PROFILE=3` (GPU busy per frame at the real load, no per-pass stalls) and `sample`
of the process in the slow stretches. Other projects were compiling on the machine, so absolute numbers move 5-10%
between runs; the differences below are far larger.

## What was slow

| Finding | Evidence |
|---|---|
| **A 2048x8192 R8 glyph cache was uploaded whole, nearly every frame.** The CPU writes a few glyphs into it; the whole 16 MB was untiled and expanded to RGBA8 (64 MB) and copied to the GPU. | `NX_TRACE_UPLOADS=1`: 5,641 uploads of that texture in 175 s; `upload` 85-107 MB/frame in the slow stretches vs ~10 MB elsewhere; `sample`: `upload_texture` the top CPU consumer outside waits |
| The replay (CPU) was on the game's critical path, so the main thread's frame went past 15.83 ms and the game's own governor locked 30 fps (DEBUG.md #43) | main-thread frames over 15.83 ms: 17.6% before, 1.7% after |
| After that, the GPU: 12.8 ms median, 16.3 ms p90, 21.9 ms max per frame at 1080p | `NX_GPU_PROFILE=3`: no headroom, any load on the machine drops frames |

## What changed

1. **Partial texture upload** (commit 5f01c9e). The write watch (page protection) already knew *that* the CPU wrote the
   glyph cache; it now also says *which pages*, and a pitch-linear texture already on the GPU uploads only the rows
   those pages cover (`write_watch_take` returns [lo, hi); `Texture::mark_dirty`; `upload_texture(lo, hi)`).
   `NX_FULL_UPLOAD=1` restores the old whole upload (bisecting).
   Course 1-1 headless: **50.4 -> 59.1 fps** mean, replay 6.3 -> 2.5 ms, windows under 35 fps 37 -> 0, upload 40 -> 11 MB/frame.
2. **FSR 1** (EASU + RCAS, AMD FidelityFX FSR 1.0, MIT) in the present pass: `NX_FSR=1`, sharpness `NX_FSR_SHARP` in
   stops (0 sharpest, 2 none, default 0.2). Shaders in `tools/shaders/fsr/`, compiled into
   `runtime/src/nvn/nvn_fsr_spirv.inc` by `tools/gen_spirv_inc.py`. Cost at 2560x1440 about 0.13 ms vs 0.11 ms for the
   plain scale. Only when the picture is upscaled; otherwise the old path.
3. **Resolution**: the game picks its render size from the operation mode (docked 1920x1080, handheld 1280x720) and
   ignores the display resolution the runtime answers (`NX_RENDER_RES` was tried: textures stay 1080p). So the steps
   are the two modes. `NX_HANDHELD=1` renders 720p: GPU **5.3 ms median** (7.3 p90), 59.7 fps.
4. `tools/wonder_mac.sh [quality|balanced|performance]` starts the game with those switches.

## Not done, with what is known

- **FSR 3 frame generation**: needs the game's motion vectors (the game has none: it is a forward 2D-style renderer
  with no velocity buffer) or an optical-flow interpolator (what FSR 3 / AFMF do without them), and a swapchain
  pacing path of its own. MetalFX frame interpolation needs macOS 26; this Mac runs macOS 14.
  With the frame already at 60 fps and the GPU at 5 ms in 720p there is little to generate; revisit if 120 Hz
  output is wanted.
- **Arbitrary resolution scale** (for example 900p): needs the host to create the game's render targets at another
  size and rescale viewports, scissors, clears and the shaders' screen-size constants. A larger change than the two
  modes; only worth it if 720p + FSR looks too soft.
- **Texture / polygon reduction**: texture memory is 3.5-4.2 GB in play; formats are the game's own compressed ones
  (ASTC, BC) sampled natively where the GPU takes them. Not measured as a cost yet.
