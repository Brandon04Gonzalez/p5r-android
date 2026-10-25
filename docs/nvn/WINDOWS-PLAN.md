# Wonder on Windows: execution plan (2026-09-30, on the PC)

Companion to [WINDOWS-PORT.md](WINDOWS-PORT.md), which has the port's rationale, known gaps and milestones W0-W4.
This page is the order of work on the PC, the extra gaps found on it, the test plan, and a new milestone W5: the
integrated GPU. Results go into [DEBUG-2.md](DEBUG-2.md) (numbered entries) and the log at the end of this page.

## The machine

| Part | What |
|---|---|
| CPU | Intel Core i5-12450HX (8 cores, 12 threads), 16 GB RAM |
| Dedicated GPU | NVIDIA GeForce RTX 3050 6GB Laptop, driver 616.64, Vulkan 1.4 |
| Integrated GPU | Intel UHD Graphics 770 (Alder Lake-HX, 16 EU), driver 101.6129, Vulkan 1.3, shared memory |
| Toolchain | VS 2022 Build Tools 17.14 (clang-cl 19 in `VC\Tools\Llvm`), CMake + Ninja from VS, Python 3.12, Vulkan SDK 1.4.357 |
| Disk | ~26 GB free after the data: build with `-j6` and delete object files that are not needed |

Vulkan order on this laptop: **GPU0 = Intel UHD 770**, GPU1 = RTX 3050 (GPU2 is Intel again, through another
driver path). `init()` in `nvn_vk.cpp` takes the first device, so today Wonder would run on the integrated GPU.

Capabilities that differ between the two (vulkaninfo):

| | RTX 3050 | UHD 770 |
|---|---|---|
| ASTC LDR | **no** | yes |
| BC1-7 | yes | yes |
| `VK_EXT_index_type_uint8`, `depth_clip_control` | yes | yes |
| `shaderFloat64` | yes | no |
| max 2D image | 32768 | 16384 |
| Device-local heap | 5.9 GB VRAM | 7.9 GB (system RAM) |

Wonder's textures are BC (the Android pack transcodes them to ASTC for phones), so the missing ASTC on NVIDIA should
not matter. W2 checks it: any texture format the device refuses is logged once.

## Gaps found on the PC (in addition to WINDOWS-PORT.md 1-7)

8. **GPU choice.** `NX_GPU=discrete|integrated|<index>|<name part>`; default: the first discrete GPU, else the first
   one. Every device is logged with its type, and the choice. The rule lives in a small pure function
   (`nvn_gpu_pick.cpp`) with its own unit test.
9. **Headless on Windows.** `platform_win.cpp` creates the window visible even with `NX_HEADLESS`, and plays sound.
   Headless: the window is created hidden, and `audio_open` returns false with `NX_HEADLESS` or `NX_MUTE` (as on the
   Mac).
10. **`tools/capture.ps1`** looks for `HadesRecomp`. It takes the window class (`<title>Recomp`) as a parameter.
11. **Files over 700 lines** (project rule). Hand-written files on this branch that break it:
    `nvn_shaders.cpp` (823), `kernel.cpp` (855), `vk_bridge.cpp` (1019), `recomp/emit.py` (1957). Each is split
    along its existing sections, one commit each, behaviour unchanged, tested by the builds and a run.
    Generated or data files are out of scope for the rule: `vk_gen.cpp`/`vk_gen.h` (`tools/gen_vk.py`) and
    `games/hades/mods_data.h` (`tools/gen_mods.py`).

## Order of work

| Step | Milestone | Work | Test |
|---|---|---|---|
| 1 | W0 | extract the zip, venv, recompile `main` and `sdk` | `private/wonder/gen/{main,sdk}` exist |
| 2 | W1 | gaps 1-6 and 8-10 | `wonder.exe` links; unit tests pass |
| 3 | W2 | headless run on the RTX 3050, then windowed | 2,000+ presents, no crash, memory under 5 GB; title frame dumped |
| 4 | W3 | `tools/wonder_course.ps1`: the scripted course run, frames every 600 | frames match `docs/media/wonder/33-39` |
| 5 | W3 | graphics fixes: every difference from the Mac frames, and the open glitch issues (#43-#45, #47, #50, #51, #41, #42, #24, #25) seen on NVIDIA | one DEBUG-2 entry per bug, before/after frames |
| 6 | W4 | 60 fps windowed on the RTX 3050; per-frame timing | course 1-1 windowed at 60 fps |
| 7 | W5 | the same runs on the UHD 770 (`NX_GPU=integrated`): measure, profile, optimise | see W5 below |
| 8 | - | file splits (gap 11); Hades still builds (`-DNX_GAME=hades`) | builds + one run each |

## Test plan

Everything is run by scripts, so it can be repeated unattended and compared.

- **Unit tests** (`tests/`, built by CMake with `-DNX_TESTS=ON`, run with `ctest`):
  - `tiling_test --selftest` (exists);
  - `gpu_pick_test`: the GPU choice rule (new);
  - `shader_cache_test` against `private/wonder` (exists; runs when the data is there).
- **Game runs** (`tools/wonder_run.ps1`): one run = GPU, mode (headless/windowed), seconds, input script, frame
  dump interval. It writes `private/wonder/runs/<name>/` with the log, the frames (PNG) and `summary.txt`:
  presents, fps (median / 1% low from the timing lines), peak memory, crashes, Vulkan errors. Exit code 0 only when
  it passes the thresholds given.
- **Regression checks** before each commit that touches the runtime: the headless course run on the RTX 3050
  (60 fps, no crash) and a look at its frames.
- **Screens** used for comparison: title, file select, character select, world map with the player, course 1-1
  start and the tree/Goombas stretch (the Mac's `33-39`).

## W5: the integrated GPU (Intel UHD 770)

The UHD 770 in this CPU has 16 execution units, about a sixth of the RTX 3050's shader throughput and no VRAM of its
own. The Switch's own GPU (Tegra X1, 256 cores at 768 MHz docked) is in the same class, so the game's handheld
mode is the natural target.

1. **Run.** Headless and windowed with `NX_GPU=integrated`. Fix whatever breaks there only (driver differences:
   formats, limits, `shaderFloat64`, memory types).
2. **Measure.** Timing lines per 60 frames (replay, fence wait, present) and `NX_GPU_PROFILE=1` per pass, docked
   (1080p) and handheld (`NX_HANDHELD=1`, 720p).
3. **Optimise, in order of expected gain:**
   - handheld mode by default on an integrated GPU (the game renders its 3D at 720p itself: no image quality lost
     against a Switch in handheld);
   - memory: on a UMA device, host-visible device-local memory for uploads (no staging copy);
   - fewer submits and barriers where the profile shows them;
   - FIFO present when the GPU cannot hold 60 (a steady 30 is better than 35-50).
4. **Exit.** Title and menus at 60 fps; course 1-1 at a steady 30 fps or better on the UHD 770; the same images as
   the RTX 3050.

## Another PC (no build tools there)

`private/venv/Scripts/python tools/pack_pc_portable.py` writes `private/wonder/pc/WonderPC-portable.zip` (4.1 GB,
stored): `wonder.exe`, the C++ runtime DLLs (app-local), `Jogar.bat` (sets `NX_DATA_ROOT` to `data\`),
`LEIA-ME.txt` and `data\` (modules, RomFS, SPIR-V with `results.tsv`, button font, the save; `--no-save` for a new
game). Needs Windows 10/11 x64, a CPU with AVX2 and a Vulkan 1.3 GPU. Checked by extracting it outside the
repository and running it from there: 60 fps headless. Three things the LEIA-ME tells:
- extract to a short path (`C:\WonderPC`): some RomFS paths pass Windows' 260-character limit in deep folders;
- the first start takes minutes (the antivirus reads ~31,000 new files once);
- the zip is over 4 GiB: not on a FAT32 USB stick (exFAT/NTFS, or a cloud drive).

The owner's own data: never share it.

## Rules

As in WINDOWS-PORT.md: branch `feat/windows-wonder`; no AI attribution in commits; files under 700 lines; only the
owner's own dumps; no GPL code; Hades must not regress; one game process at a time with the memory guard on
(`NX_MEM_LIMIT_GB=8`); every bug a DEBUG-2 entry.

## Log

Newest last. One line per step: date, what, result.

- 2026-09-30 W0: zip extracted (19,012 RomFS files, 12,751 `.spv`, font); venv; `main` recompiled in 6 min 53 s
  (16,304,252 instructions, 0 unhandled), `sdk` in 36 s. Exit met.
- 2026-09-30 W0: **the data zip has no `spirv/results.tsv`**: `pack_phone_wonder.py` (which `pack_pc_wonder.py`
  reuses) packs only `*.spv`, so the phone lacked it too. Without it no storage buffer of a translated shader is
  bound. The packers now require it; on the PC it is regenerated with the harness (TRANSLATOR.md, "Setup (Windows)").
- 2026-09-30 W1: gaps 1-6, 8-10 done (`unistd.h`, fonts, `memguard_win.cpp`, `input_script`, headless hidden and
  mute, `NX_GPU`, `capture.ps1 -Class`). The whole runtime passes clang-cl 19 with no source change for gap 5.
  Unit tests (`-DNX_TESTS=ON`, ctest): `gpu_pick` (caught `NX_GPU=3050` read as an index: fixed), `tiling`,
  `memguard` pass; `shader_cache` waits for `results.tsv`.
- 2026-09-30 gap 11: `nvn_shaders.cpp` → + `nvn_spirv_reflect.cpp`; `kernel.cpp` → + `kernel_watch.cpp`
  (`kernel_state.h`); `vk_bridge.cpp` → + `vk_swapchain.cpp` (`vk_bridge_shared.h`); `recomp/emit.py` → mixins
  `emit_int.py`, `emit_fp.py`, `emit_neon.py`, `emit_neon_wide.py` (the `sdk` recompiled with them is byte for byte
  the same, 130 files); `DEBUG.md`'s switch table → `SWITCHES.md`. No hand-written file is over 700 lines.
- 2026-09-30 W1 exit: `wonder.exe` links with clang-cl (219 MB, first full build 18 min at -j6). The Mac build could
  not be checked from the PC: the Mac-only files are unchanged, the shared ones compile with clang.
- 2026-09-30 W2: first boot on the RTX 3050 died in the NVIDIA driver on an ASTC texture (DEBUG-2 #69): fixed with
  the format check and the CPU ASTC decoder. Headless 60 s: PASS, title at 60 fps (dips only on dumped frames), 2.9-3.1
  GB, title frame matches the Mac's but for animation.
