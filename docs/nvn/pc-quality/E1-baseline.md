# E1: baseline of the merge on the PC (regression suite, recordings, glitch and timing census)

Executor run of 2026-10-02, 00:40-03:15, RTX 3050 Laptop + UHD 770, Windows 11, `integrate` at `2dadf08`
(binaries: `private\wonder\regress\bin\wonder-base.exe` = feat/windows-wonder before the merge,
`wonder-merged.exe` = `private\wonder\build\wonder.exe`). Plan: [PLAN.md](PLAN.md).

## 1. Regression verdict (`tools\regress.ps1`, interleaved base/merged)

Five headless boots of 25 s per build, then the scripted course run (`tools\wonder_course.ps1`, 180 s, headless,
`NX_DUMP_FRAMES=600`) twice per build on the dedicated GPU and four times on the integrated one (two from the suite,
two more interleaved to check the loss). Numbers pooled over all 60-frame windows of a build after the first 20.

| | base | merged | rule | |
|---|---|---|---|---|
| unit tests failed (ctest, 5) | 0 | 0 | any | ok |
| boot failures (of 5) | 0 | 0 | more | ok |
| discrete fps median / p10 | 27.6 / 19.1 | 34.1 / 23.9 | -3% / -10% | ok |
| discrete replay ms (median) | 17.42 | 10.79 | +10% +0.3 | ok |
| discrete memory peak GB | 5.88 | 5.88 | +10% | ok |
| integrated fps median / p10 | 28.1 / 22.7 | 26.8 / 22.2 | -3% / -10% | **LOSS** (-4.6%) / ok |
| integrated replay ms (median) | 6.35 | 6.70 | +10% +0.3 | ok |
| integrated memory peak GB | 4.71 | 4.77 | +10% | ok |
| crashes/hangs, Vulkan errors, glitch frames | 0, 0, 0 | 0, 0, 0 | more | ok |

Per-run medians (fps): discrete base 27.0, 28.3 / merged 27.4, 43.5; integrated base 25.3, 27.2, 29.9, 28.8 /
merged 24.3, 27.5, 29.2, 25.5. The runs of one build spread by +-10%, more than the 3% rule: the "loss" comes from
one pair (merged-4 ran while three clang processes of another project were at 100% each).

**Bisect (group test, merged, integrated GPU, interleaved, 2 runs each):** default 26.8 and 25.4 fps (median after
20 windows) against all five Android switches off (`NX_SET_CACHE=0 NX_JANITOR=0 NX_RESOLVE_CACHE=0
NX_CALL_TABLE=0 NX_STAGING_MIN=0`) 27.4 and 26.0: +2%, inside the noise. No switch-attributable loss, so no
per-switch bisect. **Verdict: no real loss found; formally the integrated median is -4.6% under heavy, uneven
background load.** The merge also cuts the dGPU replay time (17.4 -> 10.8 ms). The phone part was not run (no
phone; `base\phone.json` only).

Notes on the suite itself:
- Headless fps is ~27 on both GPUs for both builds, against ~45-58 windowed: the headless run is not a measure
  of the 60 fps goal, only a comparison.
- `NX_DUMP_FRAMES=600` (the suite's frame dumps) stalls one present for ~0.9 s every 600 frames: the window after
  each dump drops to ~30 fps (visible as `present ~15 ms` in the timing line). Both builds pay it alike, but it
  lowers p10. E2 could measure without dumps and take the frames in a separate short run.
- `shader_cache` (ctest) took 674 s instead of 24 s on the first run: Defender (MsMpEng at 96%) scanning the
  ~40k extracted shader files cold.

Fixed in `tools\regress.ps1`: `-Compare base,merged` through `powershell -File` arrived as one string (usage
error); `CompareRuns($a,$b)` clashed with `$A/$B` (PowerShell names ignore case: the header printed whole
objects); the summary median was the median of per-run medians (with two runs, the lower one) and p10 the
minimum of per-run p10s: both now over the pooled windows; a failing ctest would have aborted the suite
(stderr under `2>&1` with `ErrorAction Stop` in PowerShell 5.1); new `-Rescore <labels>` recomputes
`results.json` from the kept run folders.

## 2. Background load

`Win32_PerfFormattedData_PerfProc_Process` (% of one core), sampled before and during the runs; nothing killed.

| When | Load |
|---|---|
| start (00:52) | brave 93+58, System 44, WmiPrvSE 24, Code 14, parsecd 14-29; MsMpEng 96 just before (ctest) |
| regress, middle (01:05-01:25) | rexglue (BatmanRecomp tool) 91-190, MsMpEng 43, parsecd 9 |
| integrated runs 3-4 (01:39-01:48) | rexglue 190, svchost 97, three clang at 99-100 each (another project's build) |
| bisect (01:50-02:02) | baa (Batman recomp, a game) 142-298, parsecd 28-39, svchost 97 |
| recordings (02:15-02:32) | baa running with a window, over the game window from 02:18 (rec-c11-2) and during rec-c11-3 |
| last runs (02:35-03:10) | baa gone; parsecd 14, WmiPrvSE 18 |

No `fh1.exe` seen. Parsec was connected the whole night. The machine was on AC, "Balanced" power plan.

## 3. Recordings

`tools\record_run.ps1` (new): a windowed run (merged build, `NX_MUTE=1`, window client 1536x864) recorded by
ffmpeg at 30 fps, 960x540, x264 ultrafast crf 30, then a frame every 0.5 s, `tools\find_glitches.py` (near-black,
threshold 20), `tools\find_lavender.py` (new: flat lavender) and contact sheets of 30 s, all looked at by eye.
`gdigrab -i title=...` gives a **white** video for the Vulkan window (GDI cannot read a flip-model swapchain); the
first two recordings grabbed the desktop over the window instead, which another program's window then covered;
from the third one the tool uses `gfxcapture` (Windows.Graphics.Capture of the window by handle), immune to
covering windows. `wonder_course.ps1` dumped frames every 600 presents by default; the tool now passes
`-DumpEvery 0` (the first recording still has the dumps).

| Run | Content | Length | Capture |
|---|---|---|---|
| rec-c11-1 | new game, `-Course11`: opening, cutscenes, map, course 1-1 (Yellow Toad) | 330 s | desktop, dumps on |
| rec-c11-2 | the same (Yellow Toad) | 330 s | desktop; covered by the Batman window from 188 s |
| rec-c11-3 | the same (Toadette) | 330 s | window |
| rec-map | owner's save (`save-backup-mapa`): menus, map, RIGHT/LEFT for 90 s | 150 s | window |
| rec-leave11 | owner's save: 1-1 entered, 90 s of RIGHT+jumps, 1 death, pause; the script's DOWN picked "Restart Course" (menu: Continue / Restart Course / Exit Course), so a second try | 250 s | window |
| rec-long | owner's save: 1-1 session 1 (entry + 4 deaths, 90 s), Exit Course, map walk 45 s, 1-1 session 2 (entry, death, game over), map walk 60 s; `dump_frames` "40 1" twice on the map | 400 s | window |
| e1-norec | `-Course11` again without recording (timing reference) | 330 s | none |

## 4. Glitch census

| Run | Video time | Screen | What |
|---|---|---|---|
| rec-c11-1 | 290.5-292.0 s (frames 582-585) | course 1-1, ~3 s after control starts, the "Onward and upward!" flower | flat lavender (143,177,245) band over the ground behind the player with two thin vertical lines to the top; the next frame the Talking Flower drawn black; then a lavender diagonal stripe across the sky. Gone from 292.5 s |
| rec-long | 79.5-81 s (frames 160-163) | course 1-1 start, session 1, same spot | black vertical columns from the sky to the ground, black slabs on the ground, a black diagonal line |
| rec-long | 112.5-116, 130.5-134, 149-152, 167.5-170 s | course 1-1 after each of the 4 respawns of session 1 | the same black geometry, ~4 s each time, at the same place (start of the course) |
| rec-long | 73.0, 83.0, 118.5, 130.5 s (frames 147, 167, 238, 262) | course 1-1 | single frames: the far castle drawn without its haze (saturated orange instead of pale pink) |
| all | first 2.5 s | boot | white window before the first present (expected) |

Clean: the opening, all cutscenes (cast scene, Bowser, castle) and the map in every run; course 1-1 in rec-c11-3
(start and a respawn), rec-leave11 (start, respawn, restart) and rec-long session 2. So the course-start glitch
showed in **2 of 6 course sessions**; when a session has it, it comes back at every respawn of that session
(5 of 5 in rec-long session 1), and a session without it stays clean. That points to state set at course load
(texture/buffer contents or a cached object reused across the load), not to frame timing. It is DEBUG-2 #76
(exploded geometry, black lines), not gone; the lavender variant is the colour of #77's flicker.
**Repro for E2:** owner's save, enter 1-1, run into the first Goomba at the brick arch, watch the respawn; reload
the course until a session shows it (rec-long: `tools\record_run.ps1 -Name rec-long -Save
private\wonder\save-backup-mapa -Seconds 400 -Script (Get-Content <evidence>\scripts\rec-long.txt)`).

**DEBUG-2 #77 (map flicker) did not reproduce:** ~5 min of map walking over 4 runs, two of them after 1-2 course
sessions (rec-long: after 3 deaths, a game over and two exits). The two `dump_frames` captures were taken on the
map without a flicker and deleted.

The near-black detector caught the black geometry but missed the lavender band (it is bright) and the black
flower (too small); `find_lavender.py` caught the band (flat share 27 and 20 per mille, 0.0 elsewhere in all runs).

## 5. Timing census (from each run's nx.log)

fps per 60-frame window; segment times are log time (sum of 60/fps), +-3 s against video time.

| Run | All after 20 windows: median / p10 / min | Opening 30-260 s | Map (walking) | Course 1-1 | >50 ms presents |
|---|---|---|---|---|---|
| rec-c11-1 | 56.3 / 34.3 / 21.5 | 57.8 / 35.4 | 44.3 (8 windows) | 41.9 / 31.0 | 80 of 15.7k (26 are dumps) |
| rec-c11-3 | 52.4 / 35.4 / 21.3 | 52.3 / 37.4 | 60.0 (9 windows) | 35.4 / 31.0 | 68 of 15.5k |
| e1-norec | 46.5 / 30.0 / 23.4 | 46.5 / 30.0 | - | 31.3 / 30.0 | 70 of 13.8k |
| rec-map | 58.5 / 31.3 / 29.8 | - | 58.0 / 30.8 | - | 13 of 7.2k |
| rec-leave11 | 47.3 / 30.4 | - | - | 35.3 / 30.3 | 31 of 9.2k |
| rec-long | 44.2 / 30.3 | - | 32.9 / 29.9 (after course) | 38.4 / 30.3, then 36.4 | 87 of 15.1k |

(rec-c11-2: 51.8 / 30.3, 793 presents over 50 ms with the Batman game sharing the GPU: left out.)

What the numbers say:
- **Course 1-1 runs at 30-42 fps on this laptop, the map at 30-60 in bursts.** The game itself asks for 30 fps:
  `nvn: N of the last 600 presents at interval >= 2` reaches 300-440 of 600 in course 1-1 and 236-258 of 600 while
  walking on the map (near 0 in the menus). The game's main thread is late: in `main-thread frame time over 600
  frames`, 40-50% of the frames take over 15.8 ms in course 1-1 and on the map (50-115 of 600 over 20 ms),
  against ~1% in the menus. Wonder
  drops to interval 2 when its frames run long, and stays there for a few seconds (windows of exactly 30-31 fps).
  The recording is not the cause: the unrecorded run e1-norec was no faster (31 fps in the course).
- Run to run spread is large (course 1-1: 42, 35, 31 fps in the three `-Course11` runs), with other agents' work
  on the same laptop and, possibly, heat after hours of load; the 60 fps goal needs a quiet machine to measure.
- Replay per frame in course 1-1: 6-12 ms, fence wait 5-10 ms, upload 10-26 MB per frame (map up to 26 MB).
- Creation per window (`created over 60 frames`), warm pipeline cache (33 MB): 1,300 pipelines per `-Course11` run
  in 220-250 ms total (worst windows 141-213 pipelines, 21-25 ms); ASTC decoding is the cost: 570 textures,
  3.1-4.0 s per run; worst windows: the cast scene 131 textures / 0.7-1.2 s, the opening's first course 70-92 /
  0.5-0.7 s, course 1-1 load 69-81 / 0.2-0.4 s (each on a loading or transition screen). rec-long, second 1-1 session: one window with
  70 pipelines in 136 ms (new variants after the exit).
- Hitches over 50 ms: 13-87 per run, mostly at loads; the frame dumps (suite and rec-c11-1) and `dump_frames`
  (rec-leave11, rec-long: windows at 2.2-2.3 fps) are our own.
- Peak memory windowed: 6.0-7.25 GB in `-Course11` runs (headless 5.9): `wonder_course.ps1`'s `MaxMemGB = 6`
  marks those windowed runs FAIL; the guard (8 GB) never tripped.

## 6. For E2

1. Course 1-1 black/lavender geometry at the start and on every respawn of an affected session (#76 is not gone).
   Bisect switches against the repro above (NX_SET_CACHE, NX_RESOLVE_CACHE, NX_JANITOR first: state kept across a
   load), with `find_lavender.py` + `find_glitches.py` on a recording.
2. The far castle losing its haze for one frame (rec-long, 4 times; rec-long frames 147/238/262).
3. 60 fps: the game's own interval-2 fallback triggered by main-thread frames over 16.7 ms (S2's subject); the
   suite's headless fps and frame dumps do not show it.
4. #77 still needs a repro; next try: longer sessions with several courses (the owner's save only has 1-1 open).

## Evidence (`private\wonder\pc-quality\evidence\`, not committed)

- `c11-1_course11-start_00582..00585.png`, `c11-1_course11-start_lavender.gif` (2.1 MB): the lavender band, black
  flower, lavender stripe.
- `long_course11_00160/00163/00227/00233/00262/00269.png`, `long_course11_respawn_black.gif` (2.2 MB, a respawn);
  `long_course11_00147/00238.png`: castle without haze.
- `logs\`: nx.log, glitches.txt and lavender.txt of every recording, nx.log of e1-norec.
- `scripts\`: the `NX_INPUT_SCRIPT` timelines of rec-map, rec-leave11 and rec-long.
- Regression: `private\wonder\regress\{base,merged}\results.json` (`base\phone.json` kept); the bisect runs'
  numbers are above (folders deleted).
