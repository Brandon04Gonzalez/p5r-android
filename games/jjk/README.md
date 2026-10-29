# Jujutsu Kaisen: Cursed Clash (NCE bring-up)

This target is a clean starting point for the Japanese base title:

- Program ID: `01002FC012548000`
- Target/library: `jjk` / `libjjk.so`
- Android package: `io.github.jjkport`
- Default data folder: `/storage/emulated/0/Jujutsu Kaisen: Cursed Clash`

## Current status

The Android launcher and generic NCE runtime target are wired up. The launcher can select the owner's game dump and `prod.keys`, then pass the game file descriptor to the runtime. The NCE branch-range fix is also included.

This is **not yet a playable JJK port**. The app currently has no JJK-specific importer or shader translator. It intentionally does not copy Persona 5 Royal's hooks, frame pacing, shader assumptions, build ID, or shader extraction pipeline.

The NCE runtime still expects the game's extracted modules under the chosen data root, including `exefs/main` (and other ExeFS modules required by the title). The selected NSP/XCI supplies the game image/RomFS to the shared runtime; it does not automatically extract the NCE module files in this target yet. If those files are not present, boot will stop before the game starts.

The exact JJK base-game `main` build ID and SDK/module list still need to be confirmed from the owner's dump and device log. The build ID is intentionally unset until verified.

## Build

From the repository's Windows developer shell:

```powershell
android\build_apk.ps1 -Game jjk -Nce
```

The APK output path is `private\jjk\apk_nce\jjk.apk`. The build script compiles the `jjk` native library and packages the Java launcher.

## Next validation steps

1. Confirm the extracted `exefs/main` and module set correspond to the JJK JP base title, not an update.
2. Capture the JJK base dump's main NSO build ID and SDK/module list from the import output.
3. Add a generic importer path for JJK that extracts the correct modules without P5R-specific shader processing.
4. Boot the NCE target and collect the first full logcat/runtime log.
5. Add only evidence-backed JJK-specific HLE hooks and shader handling after the first boot identifies the next blocker.
