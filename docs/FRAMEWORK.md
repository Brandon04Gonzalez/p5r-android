# SwitchGamesRecomp

Static recompilation of Nintendo Switch 1 games (AArch64) to native code, CPU and GPU. No emulator runs: the games'
machine code becomes C++ that is compiled for the host, and their system and graphics calls are implemented
natively.

| Game | Title ID | Host | Graphics path | State |
|---|---|---|---|---|
| **Hades** (Supergiant) | 0100535012974000 | Windows, Android | the game's own Vulkan, passed to the host driver | boot to gameplay |
| **Super Mario Bros. Wonder** (Nintendo) | 010015100B514000 | macOS (Apple M1) | NVN implemented over Vulkan (MoltenVK) | title, menus, world map and course 1-1; 60 fps headless |

No game data here. Everything derived from a game, keys or firmware stays in `private/` (gitignored). Bring your own
legally dumped copy, keys and firmware.

## Super Mario Bros. Wonder

| Title | Character select |
|---|---|
| ![title](docs/media/wonder/37-title.png) | ![select](docs/media/wonder/35-character-select-samplers.png) |
| **World map** | **Course 1-1, 60 fps** |
| ![map](docs/media/wonder/39-world-map-player.png) | ![course](docs/media/wonder/38-course-60fps-pipelined.png) |

### What works

- **CPU:** `main` and the SDK are recompiled: 17.8M AArch64 instructions, 250k functions, about 1.4 GB of generated C++.
  The game runs its whole frame loop natively on macOS.
- **Graphics:** Wonder draws through **NVN**, Nintendo's own graphics API. The `nvn*` functions are implemented on
  the host: recorded command buffers are replayed into Vulkan, running on Metal through MoltenVK.
  - All 12,717 Maxwell shader blocks were translated to SPIR-V ahead of time, with the Ryujinx translator (MIT).
  - Textures are untiled from the block-linear layout. ASTC and BCn are sampled natively.
  - Render targets, samplers, views, clears, compute and write-backs are replayed per submit.
- **Screens:** title, file and player select, character select, the opening, the world map and course 1-1 render
  correctly.
- **Speed:** 60 fps in the menus and in course 1-1 (headless test runs). Submits are pipelined: the frame's GPU
  work is not waited for.
- **Button icons:** our own glyph font for the NintendoExtended codepoints (`tools/fonts/make_button_font.py`).
- **Audio:** the sdk's own AudioRenderer (server and DSP code) runs in process.
- **Memory:** 3.5 to 4.2 GB in play.

### Open

The details are in the GitHub milestones.

- **Windowed stretches at 35-40 fps (#51):** the GPU goes past the frame budget only when a window is visible.
- **Windowed confirmation of the W7 glitch fixes (#43-#45, #47):** horizon band, silhouettes, present mode.
- **Character-select blur:** needs a reference from a real console.
- **Android port:** planned in [docs/nvn/ANDROID-PORT.md](docs/nvn/ANDROID-PORT.md), in progress on branch
  `feat/android-wonder`. See below.
- **Windows port:** planned in [docs/nvn/WINDOWS-PORT.md](docs/nvn/WINDOWS-PORT.md), branch `feat/windows-wonder`.

### Android (in progress, branch `feat/android-wonder`)

The same recompiled code runs on arm64 phones with a native Vulkan 1.3 driver. On a Galaxy S23 (Snapdragon 8 Gen 2)
the game runs at 40-60 fps at the title. Since then these were added:
- the present pass: rotated to the display, 16:9 with bars, alpha 1;
- drawn touch controls;
- a pipeline cache on disk;
- ADPF performance hints.

They were checked on the Mac, with the phone check pending. Progress and the test list are in ANDROID-PORT.md.

Build and install (macOS or Linux, Android SDK with NDK 28.2):

```sh
private/venv/bin/python tools/pack_phone_wonder.py        # your data -> private/wonder/Wonder-data.zip (~4 GB)
CMAKE_BUILD_PARALLEL_LEVEL=4 bash android/build_apk.sh --game wonder   # -> private/wonder/apk/wonder.apk
bash tools/wonder_push_android.sh <adb serial>             # install + data to Internal storage + all-files access
bash tools/wonder_run_android.sh <adb serial> 90 run1      # start, wait, save the log, pull frame dumps
```

Without a computer, copy the APK and the zip to the phone:
1. Extract the zip into Internal storage. You get `Super Mario Bros. Wonder/`.
2. Install the APK.
3. Allow "all files access" when the app asks.

The APK contains recompiled game code built from your own dump. It is for you only: never share or publish it.

Touch controls:
- **Right side:** A (jump, big), B, Y (dash), X.
- **Top:** ZR, R, L, + and -.
- **Left half:** a stick that appears where your thumb lands; pushed all the way, it also dashes.

A Bluetooth or USB controller works too, and the touch controls hide while it is in use.

### Build and run (macOS, Apple Silicon)

Requirements: Xcode command line tools, CMake, Ninja, Homebrew `sdl3` + `vulkan-headers` + `vulkan-loader` + `molten-vk`, Python 3 venv
in `private/venv`.

1. **Dump.** Dump your own cartridge (nxdumptool) and keys (Lockpick_RCM). Extract to `private/wonder/exefs` and
   `private/wonder/romfs` with `nstool`.
2. **Recompile the CPU code.** This takes about 6 minutes and 2.4 GB of memory for `main`:
   ```sh
   private/venv/bin/python -m recomp private/wonder/exefs/main private/wonder/gen/main --name main --base 0x4000000000
   private/venv/bin/python -m recomp private/wonder/exefs/sdk  private/wonder/gen/sdk  --name sdk  --base 0x4010000000
   ```
3. **Translate the shaders.** Extract the shader containers with `tools/maxwell/containers.py`, then translate them
   with the harness in `tools/maxwell/spirv_harness`. This writes `private/wonder/spirv/<hash>.spv`. See
   [docs/nvn/TRANSLATOR.md](docs/nvn/TRANSLATOR.md).
4. **Build the button font.** It is local only and never committed:
   ```sh
   private/venv/bin/python tools/fonts/make_button_font.py
   ```
5. **Build.** The first build takes about 15 minutes:
   ```sh
   cmake -S . -B private/wonder/build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release
   ninja -C private/wonder/build -j6 wonder
   ```
6. **Run:**
   ```sh
   NX_LOG=private/wonder/nx.log private/wonder/build/wonder
   ```
   It plays with a gamepad (SDL) or the keyboard.

Useful switches:

| Switch | What it does |
|---|---|
| `NX_MEM_LIMIT_GB=<n>` | memory guard, ends the process past this footprint (default 8) |
| `NX_HEADLESS=1` | no window, for unattended test runs |
| `NX_MUTE=1` | no sound |
| `NX_HANDHELD=1` | handheld mode, 720p |
| `NX_PIPELINED=0` | waits for every submit, as before 60 fps (diagnosis) |

Every debug switch (draw traces, snapshots, bisecting toggles) is listed in [docs/nvn/SWITCHES.md](docs/nvn/SWITCHES.md).

### How it works, and where it is written down

| Document | What |
|---|---|
| [PROGRESS.md](PROGRESS.md) | living log, newest first, with screenshots |
| [docs/NVN.md](docs/NVN.md) | the NVN → Vulkan plan: phases N0-N7, where to cut, ground rules |
| [docs/nvn/WONDER.md](docs/nvn/WONDER.md) | facts about the game: modules, RomFS, recompilation numbers, the host GPU |
| [docs/nvn/SHADERS.md](docs/nvn/SHADERS.md), [TRANSLATOR.md](docs/nvn/TRANSLATOR.md) | shader containers, and Maxwell → SPIR-V |
| [docs/nvn/TILING.md](docs/nvn/TILING.md) | block-linear texture layouts |
| [docs/nvn/DEBUG.md](docs/nvn/DEBUG.md), [DEBUG-2.md](docs/nvn/DEBUG-2.md) | every rendering bug found so far (67 entries): symptom, cause, fix, switch |
| [docs/nvn/GLITCH-PLAN.md](docs/nvn/GLITCH-PLAN.md), [PERF-PLAN.md](docs/nvn/PERF-PLAN.md) | glitch and performance studies and plans |
| [docs/nvn/ANDROID-PORT.md](docs/nvn/ANDROID-PORT.md) | study for the Android port: gaps, risks, milestones A0-A7 |

Ground rules for this track:
- only our own dumps, keys and firmware;
- no leaked NVN headers: the API is reverse engineered from binaries;
- no GPL code (Ryujinx, MIT, is used as a reference and for the offline shader translator);
- Hades must not regress.

## Hades

**Hades runs natively on PC, from boot to gameplay**: the CPU code is recompiled (main + Nintendo's SDK, 3.0M AArch64
instructions), the game's own Vulkan renderer (The Forge) runs on the host GPU through generated bridges, no
emulation anywhere. RTX 3050 laptop, 1280x720, 143 fps (vsync). It also runs on Android phones: see
[docs/ANDROID.md](docs/ANDROID.md).

| Logo | Main menu |
|---|---|
| ![logo](docs/media/first-frame-logo.png) | ![menu](docs/media/main-menu.png) |
| **Intro** | **Gameplay** |
| ![intro](docs/media/intro.png) | ![gameplay](docs/media/gameplay.png) |

### Approach

Hades' `main` module imports everything it needs from the system by name: libc/libc++, `nn::*` SDK functions and
**Vulkan** (100 `vk*` entry points; `subsdk0` is Nintendo's Vulkan driver). So:

1. **CPU:** recompile `main` (AArch64) to C++ with our own recompiler (`recomp/`).
2. **System:** implement main's imports natively on the host (`nn::os`, `nn::fs`, `nn::hid`, `nn::audio`, ...),
   like ReXGlue does with Xbox 360 kernel exports.
3. **GPU:** pass Vulkan calls through to the host driver. No Maxwell/NVN emulation.

Both sides are little endian, so no byte swapping. Details in [docs/FINDINGS.md](docs/FINDINGS.md),
[docs/RECOMP.md](docs/RECOMP.md) and [docs/RUNTIME.md](docs/RUNTIME.md).

### Phases

| Phase | What |
|---|---|
| 0 | Extraction and analysis |
| 1 | AArch64 → C++ recompiler: functions, jump tables, NEON, atomics |
| 2 | Runtime: loader, memory, threads, nn:: HLE |
| 3 | Vulkan passthrough: first native pixels |
| 4 | Audio, input, stability, full game |

### Extraction

```powershell
nstool -x / private\extract\upd_nsp "private\update\<update>.nsp"
nstool -k private\keys\prod.keys --tik <update .tik> -x /0 private\extract\exefs <update program .nca>
python tools\nso2bin.py private\extract\exefs\main private\analysis\main.bin
```

## Repository layout

| Path | What |
|---|---|
| `recomp/` | the AArch64 → C++ recompiler (Python) |
| `runtime/` | shared runtime: kernel, IPC, `nn::*` HLE, memory, platform layers (Windows, macOS, Android) |
| `runtime/src/nvn/` | NVN over Vulkan (Wonder) |
| `games/<game>/` | per-game glue (entry, patches, mods) |
| `tools/` | extraction, shader and font tools, packaging |
| `android/` | Android project and build script |
| `docs/` | findings, plans, debug history, screenshots (`docs/media/`) |
