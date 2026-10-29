# Jujutsu Kaisen: Cursed Clash Android port notes

Status: executable inspection only. This branch is not a playable port and must not be presented as an APK.

## Verified from the user's own local dump

- Title ID supplied for the target: `01002FC012548000`.
- The on-device `nximport-arm64` inspection CLI completed ROMFS extraction successfully.
- ROMFS report: 43 files, approximately 6.11 GB.
- ExeFS files extracted: `main` (~51 MB), `sdk` (~5.9 MB), `subsdk0` (~3.3 MB).
- The NSO Build ID field starts with `A2834C16F37633B4` (the 8-byte identifier currently checked by the shared runtime). The helper printed additional bytes from the 32-byte field; do not use the entire printed field as the runtime's 8-byte ID.
- No JJK runtime launch or graphics test has been performed.

## Why the P5R target cannot simply be renamed

- `games/p5r/game.cpp` contains P5R-specific program metadata and hard-coded guest-memory offsets/hooks.
- `games/p5r/import/p5r_import_jni.cpp` rejects anything other than P5R Title ID `01005CA01580E000` and Build ID `708BCACA873EA32B`.
- `games/p5r/import/p5r_shaders.cpp` extracts P5R's GFD shader caches. JJK's UE4-style content layout (`Abrams/Content/Paks/Abrams-Switch.ucas`) is not evidence that those shader routines are applicable.
- The P5R Java launcher, manifest, package names, data directory, and settings also contain P5R-specific assumptions.
- The shared runtime exposes a per-game contract in `runtime/include/nx/game.h`; game-specific hooks must be validated against JJK before they can be implemented safely.

## Porting sequence

1. Preserve `games/p5r` unchanged. Add a separate `games/jjk` target only after its metadata and runtime contract are defined.
2. Audit the target's NSO header, NPDM metadata, imported symbols, and executable entry path. Keep local dumps and keys on the user's device; never commit them.
3. Determine renderer and graphics API requirements from the executable and local content. Do not reuse P5R's NVN/GFD shader hooks without evidence.
4. Implement a JJK-specific importer/launcher that validates the target Title ID and the exact supported Build ID. Do not transplant the P5R shader translator.
5. Add an Android ARM64 build job for the new target, then run static checks and device smoke tests before calling it playable.

## Next useful on-device checks

From Termux, inside the cloned repository:

```sh
python tools/nso_build_id.py "$HOME/jjk-inspect/exefs/main"
```

Keep the complete output for analysis, but use the first 16 hexadecimal characters as the current runtime's 8-byte `main_build_id` check value. The inspection output alone does not establish compatibility or playability.
