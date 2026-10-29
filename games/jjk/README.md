# Jujutsu Kaisen: Cursed Clash (NCE bring-up)

Targeting the Japanese base title:

- Program ID: `01002FC012548000`
- Native target/library: `jjk` / `libjjk.so`
- Import library: `libjjkimport.so`
- Android package: `io.github.jjkport`
- Default data folder: `/storage/emulated/0/Jujutsu Kaisen Cursed Clash`

## Implemented

- A separate JJK game target with no P5R-specific hooks or build-ID assumptions.
- Android launcher to select the owner's NSP/XCI and `prod.keys`.
- Generic on-device import using the shared NSP/XCI importer. It checks the selected title ID, extracts the raw ExeFS modules needed by NCE, and extracts RomFS with the importer's integrity checks and progress reporting.
- NCE nearby branch-stub pool fix for large modules.
- Android CI builds the JJK runtime, importer JNI library, and compiles the launcher Java.

## Still unverified

This is **not yet confirmed playable**. The importer is generic and does not translate JJK-specific shaders or add game-specific HLE hooks. The exact base-game `main` build ID and module list still need to be confirmed from the owner's dump and runtime logs. The target intentionally does not borrow P5R's offsets, frame pacing, shader assumptions, or hooks.

## Build

From the repository's Windows developer shell:

```powershell
android\build_apk.ps1 -Game jjk -Nce
```

The APK output path is `private\jjk\apk_nce\jjk.apk`. On first launch, choose the owner's base-game NSP/XCI and `prod.keys`; the importer will extract the data to the app's selected data folder. Keep the app open while the progress screen runs.

## Next validation steps

1. Build and install the APK on the target Android device.
2. Import the JJK JP base title and capture the first complete runtime log.
3. Confirm the extracted `main` build ID and SDK/module list.
4. Add only evidence-backed JJK-specific shader handling or HLE fixes after the next startup blocker is known.
