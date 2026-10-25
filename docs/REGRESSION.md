# Regression suite: no build goes in that loses on Windows or on the phone

Every merge into `main` runs the same measured suite on the build before (`base`) and the build after (the
candidate), on the same machine in the same state, and goes in only with the verdict "no loss". Written for the
merge of `feat/windows-wonder` and `feat/android-wonder` into `main` (2026-10-01).

## What runs

| Part | Script | What |
|---|---|---|
| Unit tests | `tools\regress.ps1` (ctest) | `gpu_pick`, `tiling`, `memguard`, `astc`, `shader_cache` |
| Boots | `tools\regress.ps1` | 5 headless boots of 15 s on the dedicated GPU (the audio limiter crash, DEBUG-2 #74, showed here) |
| Windows, dedicated GPU | `tools\regress.ps1` → `tools\wonder_course.ps1` | the scripted run from a new game through the menus into course 1-1, 180 s headless, twice |
| Windows, integrated GPU | same, `NX_GPU=integrated` | the same run on the Intel UHD 770, twice |
| Phone | `tools\regress_phone.ps1` | the phone's scripted course 1-1 run (the blind script of `tools/wonder_phone_course.sh`), handheld, 300 s, twice, each started below 34 °C battery |

From each run: the fps per 60-frame window (the first 20 windows, boot and menus, left out), its median and 10th
percentile, the replay CPU time, the peak memory, crashes and GPU hangs, Vulkan errors, and the frames
(`NX_DUMP_FRAMES=600`) through `tools/find_glitches.py` (near-black pixels where bright scenes have none: exploded
geometry).

## The verdict (`tools\regress.ps1 -Compare base,<candidate>`)

A loss is any of:
- a unit test failing, more failed boots, any crash or GPU hang;
- median fps down more than 3%, or the 10th percentile down more than 10%;
- replay CPU up more than 10% (+0.3 ms);
- peak memory up more than 10%;
- more Vulkan errors, or more frames flagged as glitched.

The thresholds sit above the run-to-run noise measured on the laptop (a few percent), so a loss is a real one.
A loss is bisected with the switches of the change before anything goes in (for the Android optimizations:
`NX_SET_CACHE=0`, `NX_JANITOR=0`, `NX_RESOLVE_CACHE=0`, `NX_CALL_TABLE=0`, `NX_STAGING_MIN=0`).

## How to run it

```powershell
tools\regress.ps1 -Label base                  # the build in private\wonder\build, before the change
tools\regress_phone.ps1 -Label base            # the APK installed on the phone, before
# ... build the candidate (and android\build_apk.ps1 -Game wonder) ...
tools\regress.ps1 -Label merged
tools\regress_phone.ps1 -Label merged -Apk private\wonder\apk\wonder.apk
tools\regress.ps1 -Compare base,merged
```

Results: `private\wonder\regress\<label>\results.json`, `phone.json` and every run's folder (log, frames). The
Windows runs start a new game (the save is deleted; back it up first); the phone script keeps the phone's save
aside and puts it back.

Limits: Parsec or other load on the laptop moves its fps; both labels must run in the same state. The macOS build
cannot run here: Mac-only files changed in a merge are checked by building on the Mac.

## Results

See the end of this page, one entry per merge.
