# Hades (Switch) — first findings

Title 0100535012974000, update v589824 (0100535012974800). SDK 9.3.0, NCA SdkAddon 9.6.0.

## Extraction (Windows)

- LibHac/hactoolnet is gone from GitHub; hactool 1.4.0 chokes on current prod.keys. Use **nstool v1.9.2** (jakcron).
- `nstool -x / <out> game.nsp` unpacks the NSP (no keys needed).
- `nstool -k prod.keys --tik <update .tik> -x /0 <out> <update program .nca>` gives the full ExeFS (the update's ExeFS is complete, not a delta). RomFS (partition 1) needs `--basenca`.
- `tools/nso2bin.py` decompresses an NSO (LZ4) into a flat image.

## Modules (ExeFS of the update)

| module  | text     | what it is |
|---------|----------|------------|
| main    | 0x6254e0 | the game: Supergiant engine on The Forge, FMOD Studio, Bink, Lua 5.x, statically linked |
| sdk     | 0x53f9c0 | nnSdk: musl libc, libc++, nn::* (IPC clients), NVN; 21.5k exported functions |
| subsdk0 | 0x6c040  | Nintendo's Vulkan driver (NintendoSDK_Vulkan) |
| subsdk1 | —        | Nintendo's OpenGL (NintendoSDK_OpenGL), 2.6k exports |
| rtld    | tiny     | dynamic loader |

## main's imports (631, all by name)

244 libc, 68 other C++ (libc++/cxxabi), **100 Vulkan `vk*`**, and nn::: os 44, hid 30, fs 29, audio 22, socket 19, account 14,
nifm 8, oe 8, ro 7, mem 6, web 5, vi 4, crypto 4, swkbd 4, time 3, ssl 3, plus nv::InitializeGraphics.

Consequence: the game talks to the GPU only through Vulkan. Recompile `main` alone, and implement its imports natively
(Vulkan passed through to the host driver, nn::* as HLE on the host), so there is no Maxwell/NVN emulation. It is the ReXGlue model
(kernel exports) with a much smaller surface. Both platforms are little endian: no byte swapping.
