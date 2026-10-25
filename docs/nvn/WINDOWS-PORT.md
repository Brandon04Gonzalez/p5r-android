# Wonder on Windows: port plan

Branch `feat/windows-wonder`. Target: Super Mario Bros. Wonder 1.0.0 on a Windows 10/11 PC with an NVIDIA GPU. Today
it runs on macOS (Apple M1, MoltenVK) and boots on Android (Galaxy S23). This page is written to be picked up on
the PC with no other context: read it, then [../../README.md](../../README.md), [../NVN.md](../NVN.md),
[DEBUG.md](DEBUG.md) and [DEBUG-2.md](DEBUG-2.md). The order of work on the PC, the gaps found there, the test
plan and milestone W5 (integrated GPU) are in [WINDOWS-PLAN.md](WINDOWS-PLAN.md).

## Why it is a short port

- **The runtime runs on Windows already.** Hades plays there from boot to gameplay: `platform_win.cpp` has the
  window, XInput, XAudio2, the Vulkan loader and the Win32 surface.
- **The recompiled code is portable C++.** Windows uses the same guest layout as Android (`runtime.h`:
  `NX_BASE` 0x40_0000_0000, heap 0x50, alias 0x58, stacks 0x68), and Wonder is recompiled at that base.
- **The NVN layer talks only generic Vulkan** (`runtime/src/nvn/*`, through `vk_gen`'s function pointers). It has
  no Metal code. An NVIDIA GPU is closer to the Switch (itself NVIDIA) than the Mac:
  - BC textures, D24, MAILBOX and depth clip control are native;
  - no MoltenVK translation layer.

  Several Mac workarounds become unnecessary there, but they stay: they are correct on any GPU. Clean them up later,
  per device capability, not with `#ifdef`.

## Known gaps (from reading the code, 2026-09-30)

1. `runtime/src/nvn/nvn_vk.cpp` includes `<unistd.h>`, which Windows lacks. Find what it uses and replace it or put
   it behind `#ifndef _WIN32`.
2. **Shared fonts.** `hle_pl.cpp` falls back to Arial Unicode on macOS and Roboto on Android, and has nothing on
   Windows: the game's text would be empty. Add `C:\Windows\Fonts\arialuni.ttf` if present, else
   `C:\Windows\Fonts\arial.ttf`. The button glyphs come from `private/wonder/fonts/shared_5.ttf` (packed, see W0).
3. **Memory guard.** `start_memory_guard()` exists for macOS and Android only.
   - Add `memguard_win.cpp`: `GetProcessMemoryInfo` → `PrivateUsage` every 250 ms, a log line every 30 s, and exit
     past `NX_MEM_LIMIT_GB` (default 8), as `memguard_mac.cpp` does.
   - Call it from `platform_win.cpp`'s `main` before `boot()`, and add it to the WIN32 branch of `CMakeLists.txt`.
   - This is a hard rule of the project: a leak once took a machine to 85 GB.
4. **Scripted input** (`NX_INPUT_SCRIPT`, `input_script.cpp`) is not called by `platform_win.cpp`'s `read_pad`.
   Call `input_script(p)` at the end of it, as `platform_mac.cpp:345` does. Unattended test runs need it.
5. **Compiler.** The recompiler targets clang (`docs/RECOMP.md`: "builds with clang-cl"). The NVN layer has only
   been built with Apple clang and NDK clang. Expect a few clang-cl / MSVC-STL differences:
   - `std::atomic` in aggregates;
   - `__builtin_*`;
   - `ssize_t`;
   - designated initializers in C++20 mode.

   Fix each one at its source, keeping the Mac and Android builds green.
6. **`NX_HEADLESS`** exists on Windows (`platform_win.cpp:225`). Check that the NVN present path honours it there
   (`present_headless` in `nvn_vk.cpp` keys on `plat::headless()`).
7. **Swapchain.** `make_swapchain` in `nvn_vk.cpp` is shared. It picks MAILBOX, else IMMEDIATE, else FIFO, and logs
   the mode. On a 60 Hz monitor with MAILBOX, pacing stays with the game's vsync event (`hle_vi.cpp`) and
   `pace_present`, as on the Mac.

## Milestones

| # | Milestone | Work | Exit criteria |
|---|---|---|---|
| W0 | Data and toolchain | on the Mac: `tools/pack_pc_wonder.py` → `private/wonder/pc/Wonder-pc-data.zip`; on the PC: extract it into the repo root, install the tools below, recompile `main` and `sdk` | `private/wonder/gen/{main,sdk}` exist on the PC |
| W1 | It builds | gaps 1-5 above; `cmake -G Ninja -DNX_GAME=wonder` with clang-cl | `private/wonder/build/wonder.exe` links; the Mac build still builds |
| W2 | It boots | run headless (`NX_HEADLESS=1 NX_MUTE=1`), then windowed | 2,000+ presents, no crash, `memory:` lines under 5 GB; the title screen appears |
| W3 | It matches the Mac | the scripted course run (`private/wonder/course.sh` has the script; port it to a `.ps1`), frames every 600 | frames match the Mac's `docs/media/wonder/33-39` screens (title, character select, map with the player, course 1-1) |
| W4 | 60 fps and polish | per-frame timing log; FIFO/MAILBOX choice; NVIDIA-native formats (D24, R11G11B10F) behind capability checks | course 1-1 windowed at 60 fps; no glitch from the W7/W8 lists |

## W0 in detail

On the Mac (this repository, branch `feat/windows-wonder`):

```sh
private/venv/bin/python tools/pack_pc_wonder.py   # → private/wonder/pc/Wonder-pc-data.zip (~4 GB, stored)
```

Copy the zip to the PC. It is the owner's own game: never upload or share it.

On the PC:
1. **Install the tools:**
   - Git;
   - Visual Studio 2022 Build Tools with "Desktop development with C++" and "C++ Clang tools for Windows" (clang-cl);
   - CMake 3.28+ and Ninja;
   - Python 3.11+;
   - the LunarG Vulkan SDK (1.4.x), which sets `VULKAN_SDK`;
   - a current NVIDIA driver.
2. **Clone the repository and check out the branch:** `git checkout feat/windows-wonder`.
3. **Extract the zip into the repository root.** It creates `private\wonder\...`.
4. **Recompile.** It takes about 6 minutes and 2.4 GB of memory for `main`:
   ```powershell
   python -m venv private\venv; private\venv\Scripts\pip install capstone lz4
   private\venv\Scripts\python -m recomp private\wonder\exefs\main private\wonder\gen\main --name main --base 0x4000000000
   private\venv\Scripts\python -m recomp private\wonder\exefs\sdk  private\wonder\gen\sdk  --name sdk  --base 0x4010000000
   ```
5. **Build** from a "x64 Native Tools Command Prompt for VS 2022":
   ```powershell
   cmake -S . -B private\wonder\build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release `
         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   ninja -C private\wonder\build -j6 wonder
   ```
   The generated code is ~1.4 GB of C++. Keep `-j` at or below (RAM in GB) / 2.
6. **Run:**
   ```powershell
   $env:NX_LOG="private\wonder\nx.log"; $env:NX_MEM_LIMIT_GB="8"; private\wonder\build\wonder.exe
   ```

## Rules (same as the rest of the project)

- Work on `feat/windows-wonder`, never on `main`. Commit messages carry no AI attribution. Files stay under 700
  lines.
- Only the owner's own dumps and keys. No leaked NVN headers. No GPL code (Ryujinx, MIT, is fine as a reference).
- Hades must not regress: `platform_win.cpp` and the shared runtime also build Hades. Build `-DNX_GAME=hades` once
  before merging.
- One game process at a time, with the memory guard on.
- Every bug found goes into `docs/nvn/DEBUG-2.md`, as a numbered entry: symptom, cause, fix, switch. Screenshots go
  in `docs/media/wonder/`.
- The Mac and Android work continues on `feat/android-wonder`. Merge `main` into this branch when PR #53 lands, and
  keep the shared files (`runtime/src/nvn/*`, `runtime/include`) conflict-free by rebasing often.
