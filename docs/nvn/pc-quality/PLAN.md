# Wonder on PC: zero glitches and a steady 60 fps (plan, 2026-10-02 night)

Goal set by the owner for the morning, on the RTX 3050 laptop (Windows, synchronous submits, the merged
`integrate` branch: Windows + Android work):
1. **No glitch** in the opening, on the world map (walking included) and in course 1-1, checked on **recordings**
   of windowed play, not only on frame dumps.
2. **60 fps steady** in course 1-1 and on the map (windowed, 1080p docked): no window of 60 frames under 58 fps
   after loading, and no single hitch over 50 ms outside screen transitions.
3. No loss against the build before (`docs/REGRESSION.md`), and no heavy files left behind.

## What is known

| Item | State | Where |
|---|---|---|
| Exploded meshes, black lines (course 1-1) | pipelined off by default off macOS (#76); the per-session rest was command lists dropped across memory pools, fixed | DEBUG-2 #76, #90 |
| Map: every other frame lavender + black triangles while walking | open; seen after a long session, not from a boot into the map | DEBUG-2 #77 |
| HUD in blocks, black sky | fixed: barrier after each pass | DEBUG-2 #71 |
| Characters black/green at the cast scene | fixed: staging buffers for big uploads | DEBUG-2 #72 |
| Hitches when objects come in | pipelines compiled in the replay (cache on disk since #73), ASTC decoded on the replay thread (#75) | DEBUG-2 #73, #75 |
| Android optimizations now on Windows | set cache, janitor, resolve cache, call table, ring uploads: untested on Windows | android-perf/MEASUREMENTS.md |

## Work

| # | Who | What | Output |
|---|---|---|---|
| S1 | study (read only) | map flicker: causes from the code and the owner's recording (`private/wonder/shadow`), the instrumentation that would prove each | `S1-map-flicker.md` |
| S2 | study (read only) | 60 fps: every source of a long frame on the PC and its fix, ranked | `S2-steady-60.md` |
| S3 | study (read only) | the five Android optimizations on Windows: risks, what to measure, switches | `S3-android-opts-on-pc.md` |
| E1 | executor (runs the game) | the regression suite on the merge; windowed recordings (ffmpeg) of opening, map and course 1-1; glitch and hitch census | `E1-baseline.md` |
| E2+ | executor | fixes from S1-S3 and E1, one at a time: each with a DEBUG-2 entry, a recording before and after, and the regression suite | DEBUG-2, commits |

Only one process of the game at a time (the memory guard and the GPU): the studies never run it, the executors
take turns. The owner's save is backed up before and put back after.

## Recordings

`ffmpeg -f gdigrab -framerate 60 -i title="<window title>"` over a windowed scripted run
(`tools/wonder_course.ps1 -Windowed -Course11`): opening, map, course 1-1. Frames are pulled every 0.5 s and
through `tools/find_glitches.py`; fps and hitches come from the log. The videos are deleted once analysed: only
the census, a few PNGs and short GIFs of a glitch stay (`private/`, never committed).

## Log

Newest last.

- 2026-10-02 03:20, E1 ([E1-baseline.md](E1-baseline.md)): regression suite base/merged: no switch-attributable
  loss (integrated median 28.1 -> 26.8 fps, -4.6%, inside a +-10% run spread under other projects' load; all five
  Android switches off: +2%, noise); dGPU replay 17.4 -> 10.8 ms. Recordings (new `tools/record_run.ps1`, window
  capture): course 1-1 shows black or lavender exploded geometry at its start and on every respawn in 2 of 6
  course sessions (#76 not gone); opening, cutscenes and map clean; #77 not reproduced. Course 1-1 runs at
  30-42 fps windowed: the game asks for present interval 2 when its main thread runs over 16.7 ms.
- 2026-10-02 05:05, E2 (DEBUG-2 #90): the course 1-1 exploded geometry (#76's per-session rest) was command lists
  dropped because command memory was keyed by the `NVNmemoryPool*` struct address plus offset (lists of one pool
  "overlapped" another's); now by the pool's GPU address (`NX_CMD_REGION_OLD=1`: old). Found with the new
  `NX_CHECK_VERTS=1`; the log line `called handle ... is unknown` marked every glitchy run. Repro (8 course entries
  per process, recorded): before, unknown handles in 5 of 7 processes; after, 16 entries and a 330 s `-Course11`
  recording clean; regress (dGPU, 2 runs) no loss. The map's blown-up rock on returning from the course, seen in the
  same processes, went with it.
