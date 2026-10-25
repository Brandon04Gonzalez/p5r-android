# Android on the Galaxy S23, 2026-10-18: what makes gameplay heavy

Two sources, both with the player's settings (`NX_HANDHELD=1`, `NX_ULTRAWIDE=1`, `render_scale=0.67`,
`NX_GPU_TURBO=1`; pipelined submits by default, 2 in flight; Adreno 740), a build of `dev` at 010ed7a:

- **play**: the player's own session (about 50 windows of 60 frames, title to the Flower Kingdom course).
- **base1**: a scripted course run (`tools/wonder_phone_course.sh`, 151 windows), with the new memory log below.

"Measured" is a log line; "inferred" is marked.

## 1. The GPU is not the limit

| | menus | course |
|---|---|---|
| fps (play) | 60 | 30-50, dips to 20-25, one window at 11 |
| CPU replay per frame (play) | 1-6 ms | **17-18 ms**, peaks 25, 34, 64 ms |
| fence wait per frame (play) | 0 ms | 0.1-2 ms |

The replay (render thread, NVN to Vulkan) crosses the game's 15.83 ms rule and the game drops to its own 30 fps
mode. The GPU waits are short: `render_scale=0.67` buys nothing here and `1` should cost little (not measured).

## 2. Android swaps the game out while it plays

New in this build (`memguard_android.cpp`, every 30 s, from `/proc/self/status` and `/proc/meminfo` only, so no
page-table walk): `memory: anon, file, swapped (VmSwap) | MemAvailable, SwapFree`. base1, first 10 samples:

| t | anon resident | file resident | swapped (zram) | MemAvailable |
|---|---|---|---|---|
| 30 s | 1018 MB | 119 MB | 249 MB | 1176 MB |
| 90 s | 768 MB | 87 MB | 1148 MB | 849 MB |
| 150 s | 490 MB | 70 MB | 1454 MB | 955 MB |
| 210 s | 377 MB | 44 MB | 1774 MB | 1034 MB |
| 300 s | 645 MB | 65 MB | 1634 MB | 966 MB |

The phone keeps about 1 GB available and pushes up to **1.8 GB of the game's anonymous memory to zram**. Every
touch of a swapped page is a major fault: the replay thread's major faults per 600 frames went 81, 57, 114 ... 4704,
9408, 36439 over the play session (about 60 per frame at the end), and up to 7759 in base1, which also has fps
dips to 24-30 in the course (that they are the same windows is inferred, not checked window by window). MEMORY.md (2026-10) predicted this (~1.9 GB SwapPss); it is now measured.

Also measured (play): the replay thread sits on cluster 1 (mid cores) 340-490 of 600 samples and never on cluster 3
(the X3); thermal headroom 0.89.

## 3. New shaders: pipeline creation hitches

base1 created **905 pipelines in 6.9 s** even with a 4.4 MB pipeline cache loaded (worst 60-frame window: 98 ms);
the play session had single windows of 100-400 ms. This is the drop the player sees "when heavy animations with new
shaders appear": the first time a shader/state combination is drawn, the Adreno driver compiles it on the replay
thread. The cache makes the second time free. Options (not done): load every cached pipeline at start, and compile
new ones on a worker thread while the frame draws without them.

## 3b. A longer play session (7 min screen recording, same settings, 280 windows)

The player's recording (opening course with its Wonder effect, then "Piranha Plants on Parade", saving, world
map) and its nx.log: no crash; the picture is right throughout (no black silhouettes; widescreen fills the screen).

- At 60 fps the replay takes **10.5-12 ms** per frame and the GPU waits under 0.1 ms. When the replay goes past about
  15 ms the game switches to its 30 fps mode and stays there for 10-20 windows (the Wonder effect, parts of the
  Piranha Plant course, the course-clear screens), then comes back to 60.
- In the 30 fps stretches the GPU still waits only 0.1-5 ms: the limit stays the CPU.
- **2,167 pipelines were created in 17.7 s** of replay-thread time during the session, despite the loaded cache:
  pipeline creation is part of the replay cost, not only a first-time hitch.
- Memory: swapped 536 → 1818 MB, anon resident 1045 → 574 MB, MemAvailable 790-1000 MB.

So two levers matter most on the S23: keep the game out of zram, and take pipeline creation off the replay thread.

## 4. Changes in this round

| Change | State |
|---|---|
| Memory log (anon/file/VmSwap, MemAvailable/SwapFree), MEMORY.md item 11 | in, ran on the phone (table above) |
| `NX_UPLOAD_RING_MB=<16..1024>` overrides the upload ring size | in; defaults unchanged |
| `tools/wonder_phone_course.sh`: `ADB=`, `DATA=` (game imported into the app folder), `ACT=`, NDK optional | in |

Correction to MEMORY.md: the Android upload ring is already **128 MB** (not 256); the log prints it
(`nvn/vk: upload ring 128 MB`).

## 5. Next (measured one by one, ABBA with cool-down, as planned in MEMORY.md / PHASE4.md)

1. Smaller upload ring on Android (`NX_UPLOAD_RING_MB=64`), then image retention across scenes and the staging
   spikes at scene loads (MEMORY.md items 12-13).
2. React to MemAvailable (trim reusable images and idle descriptor pools when it falls).
3. `NX_AFFINITY` 0/1/2 (the replay thread on the fast cores).
4. Pipelines: pre-warm from the cache at start; compile new ones off the replay thread.
5. `render_scale` 1 vs 0.67 (the GPU has room).

The work stopped here at the player's request; nothing of 1-5 was measured.
