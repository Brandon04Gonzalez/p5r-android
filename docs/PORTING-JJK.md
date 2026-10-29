# Jujutsu Kaisen: Cursed Clash Android Port Investigation

Status: reconnaissance and local metadata inspection only. This branch does not claim that the game is supported or bootable.

## Target

- Game: Jujutsu Kaisen: Cursed Clash (Japanese title: 呪術廻戦 戦華双乱)
- Switch application Title ID: `01002FC012548000`
- Input: user's own base-game XCI/NSP and matching keys, kept local and never committed
- Initial milestone: the shared importer recognizes the correct title and reports the actual NSO Build ID

## Why the P5R APK does not detect the JJK dump

The Android importer in `games/p5r/import/p5r_import_jni.cpp` is intentionally game-specific:

1. `kTitle` is fixed to Persona 5 Royal's Title ID (`01005CA01580E000`).
2. `kBuildId` is fixed to P5R 1.0.0's Build ID (`708BCACA873EA32B`).
3. After validation, it calls `p5r::extract_shaders`, implemented in `games/p5r/import/p5r_shaders.cpp`, which parses P5R's CRI/CPK/GSC shader-cache layout. It is not a generic Switch shader importer.
4. The launcher, package name, storage directory, native library name and game metadata also use P5R-specific names.

Therefore changing only the Title ID would not make the importer work: it would next reject the executable's Build ID, and a generic JJK shader path is not implemented.

## Local metadata inspection

A helper script now runs the repository's shared `nximport` against a user's local dump with the JJK Title ID, extracts ExeFS into a user-selected local folder, and reads the NSO Build ID header. It does not upload or commit the dump, keys, or extracted executable.

Example from the repository root in PowerShell:

```powershell
.\tools\inspect_jjk_dump.ps1 `
  -NxImport .\path\to\nximport.exe `
  -ProdKeys "$HOME\prod.keys" `
  -GameFile "$HOME\JJK.xci" `
  -OutDir "$HOME\jjk-inspect"
```

Add `-TitleKeys "$HOME\title.keys"` only if the local dump requires a separate title-keys file. The output folder contains the extracted ExeFS and `jjk-inspection-report.txt`. Keep that folder private.

The NSO header contains a 32-byte Build ID field. The current shared runtime's `main_build_id` check uses the first 8 bytes, represented as 16 hexadecimal characters. The full field is recorded in the report for traceability; do not copy the full 64-character field into `games/jjk/game.cpp`.

A successful inspection only proves that the shared importer can open the selected title and read its executable metadata. It does **not** prove that JJK boots, that its renderer matches the shared NVN runtime, or that shader translation works.

## Existing reusable pieces

- `android/wonder/importer/nxgame.h` exposes a shared `GameImage` abstraction for a title's ExeFS and RomFS.
- `runtime/src/nce/` can execute original AArch64 ExeFS code on an ARM64 host, but this does not itself provide game-specific startup, graphics compatibility, services or shaders.
- The shared NVN-to-Vulkan runtime may be reusable only if JJK's Switch build uses compatible graphics paths and the missing services can be implemented.

## Next engineering steps

1. Run the local inspection helper against the exact base-game dump and retain the report locally.
2. Inspect the executable's NPDM metadata, imports, strings, SDK expectations, graphics API usage and required system services.
3. Determine how the Switch version stores and loads shaders. Do not reuse P5R's GSC parser unless the dump proves the formats match.
4. Create a separate JJK Android importer/launcher and runtime target. Keep `games/p5r` unchanged.
5. Add a dedicated Android ARM64 build path, then run static checks and device smoke tests.

## Acceptance criteria

- The local importer recognizes the correct JJK Title ID and reports the actual Build ID from the selected base game.
- Unsupported version/update combinations produce a clear error rather than a misleading “not detected” result.
- No game dumps, keys, decrypted executable, proprietary shader data or generated game code are committed or uploaded as CI artifacts.
- “Importer recognizes the game,” “boots to title screen,” and “playable” remain separate milestones requiring separate evidence.
