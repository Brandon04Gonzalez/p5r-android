# Jujutsu Kaisen: Cursed Clash Android Port Investigation

Status: reconnaissance only. This document does not claim that the game is supported or bootable.

## Target

- Game: Jujutsu Kaisen: Cursed Clash (Japanese title: 呪術廻戦 戦華双乱)
- Switch application Title ID: `01002FC012548000`
- Input: user's own base-game XCI/NSP and matching keys, kept local and never committed
- Initial target: inspect the user's exact base-game executable and rendering path before building a JJK-specific APK

## Why the P5R APK does not detect the JJK dump

The current Android importer in `games/p5r/import/p5r_import_jni.cpp` is intentionally game-specific:

1. `kTitle` is fixed to Persona 5 Royal's Title ID (`01005CA01580E000`).
2. `kBuildId` is fixed to P5R 1.0.0's Build ID (`708BCACA873EA32B`).
3. After validation, it calls `p5r::extract_shaders`, implemented in `games/p5r/import/p5r_shaders.cpp`, which parses P5R's CRI/CPK/GSC shader-cache layout. It is not a generic Switch shader importer.
4. The launcher, package name, storage directory, native library name and game metadata also use P5R-specific names.

Therefore changing only the Title ID would not make the importer work: it would next reject the executable's Build ID, and a generic JJK shader path is not implemented.

## Existing reusable pieces

- `runtime/src/game_image.cpp` opens a game image using the title ID wildcard (`0`) and exposes its ExeFS/RomFS through the shared importer library.
- `runtime/src/nce/` can execute the original AArch64 ExeFS code on an ARM64 host, but this does not itself provide game-specific startup, graphics compatibility, services or shaders.
- The shared NVN-to-Vulkan runtime may be reusable only if JJK's Switch build uses compatible graphics paths and the missing services can be implemented.

## Safe investigation sequence

1. Add a read-only metadata inspection path to report the selected XCI's Title ID, executable Build ID, NCA/ExeFS presence and version without copying game content into the repository.
2. Use the exact local base-game dump to record its real Build ID. Do not guess the base game's Build ID from an update listing.
3. Inspect the game's ExeFS metadata and imports/strings to establish the executable type, SDK expectations, graphics API usage and required system services.
4. Determine how the Switch version stores and loads shaders. Do not reuse P5R's GSC parser unless the dump proves the formats match.
5. Only after the previous checks, create a separate `games/jjk/` target and Android launcher/importer. Keep the P5R target unchanged.
6. Build the Android ARM64 library and APK, then test on-device. A successful compile is not proof the game boots.

## Acceptance criteria

- The local importer recognizes the correct JJK Title ID and reports the actual Build ID from the selected base game.
- Unsupported version/update combinations produce a clear error rather than a misleading “not detected” result.
- No game dumps, keys, decrypted executable, proprietary shader data or generated game code are committed or uploaded as CI artifacts.
- The first APK milestone is only “JJK importer recognizes the game.” “Boots to title screen” and “playable” are separate milestones requiring device testing.
