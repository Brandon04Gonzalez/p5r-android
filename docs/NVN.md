# NVN → Vulkan (plan)

Goal: run games that draw through **NVN** (Nintendo's own graphics API, what almost every Switch game uses) on the
host GPU through Vulkan, the same way Hades' Vulkan calls already reach the host driver. First target: **Super Mario
Bros. Wonder** (010015100B514000), from a dump of our own cartridge.

Nothing is emulated at the GPU level: no Maxwell command processor, no nvhost/GPFIFO. The game's NVN calls are
implemented on the host, like its `nn::*` imports.

## Where to cut

| Level | What we would implement | For | Against |
|---|---|---|---|
| **NVN API (chosen)** | the `nvn*` functions, over Vulkan | objects arrive with their meaning (format, size, usage, state); no command stream to parse; small per-frame cost | ~hundreds of functions to reverse; object structs are allocated by the game at the real driver's sizes |
| nvhost + Maxwell methods | the kernel driver and the 3D engine (what yuzu/Ryujinx do) | the SDK's NVN driver is recompiled for free; works for NVN, OpenGL and Vulkan games alike | meaning is lost: texture caches and memory tracking by heuristics, the hardest and slowest part of the emulators |

The NVN boundary is clean: the SDK exports one entry point, `nvnBootstrapLoader` (seen in Hades' sdk,
`NintendoSDK_NVN-9_3_0`). The game asks it for `nvnDeviceGetProcAddress` and gets every other function as a
pointer by name, the same shape as `vkGetInstanceProcAddr`. We register `nvnBootstrapLoader` as HLE and hand out our
own functions; the SDK's NVN driver never runs. Engines layered on top (nn::gfx, Nintendo's own) stay recompiled
guest code and need nothing.

Shaders need translating whichever level is chosen: games ship Maxwell machine code.

## Ground rules

- **Only our own dumps and keys** (nxdumptool, Lockpick_RCM). Nothing downloaded.
- **No leaked NVN headers or documentation.** The API is reverse engineered from binaries: names from the
  game's loader, signatures from call sites and from the SDK's NVN driver (recompiled, readable, ours to study).
  Everything learned goes into `docs/nvn/`, which is the part of this project that teaches.
- Emulator code as reference: Ryujinx is MIT (the original repo is gone, forks remain), yuzu is GPL. Study both;
  copy from GPL code only if this repository is GPL. See *Decisions*.
- **Hades must not regress.** Every change to the shared runtime is checked on Hades (boot → menu → gameplay)
  before merge.

## Phases

### N0: groundwork (no game needed)

- Split the repository into a shared core and per-game parts: `runtime/` (kernel, IPC, HLE, Vulkan, platform),
  `games/hades/` (settings, widescreen, mods, program id, names), `games/wonder/`, `private/<game>/`. One CMake
  target per game. Hades builds the same APK before and after.
- `tools/probe.py <exefs>`: modules, SDK version, graphics API (NVN / Vulkan / OpenGL), imports per `nn::*` service,
  engine hints.
- **Done when:** Hades unchanged on PC and Android; probe reports Hades as Vulkan.

### N1: Wonder's CPU side

- Recompile `main`, `sdk` and any `subsdk*`; check the report (unhandled instructions, jump tables).
- Wonder's SDK is years newer than Hades' 9.3: expect TIPC for `sm:` (12.0+), new SVCs, newer IPC commands, and
  more services (friends, NEX, bcat, prepo, ...). Stub what is only online.
- **Done when:** boot reaches the first NVN call (`nvnBootstrapLoader`).

### N2: NVN reconnaissance

- `nvnBootstrapLoader` + `nvnDeviceGetProcAddress` returning a **logging thunk per name**: every function the
  game asks for, then every call with its arguments, in order (an apitrace for NVN).
- For each function: signature (from call sites and the SDK driver), object struct sizes (the game allocates
  `NVNdevice`, `NVNtexture`, ... itself, so ours must fit in them), enum values.
- Stubs return plausible values so the trace runs as far as possible.
- **Deliverables:** `docs/nvn/API.md` (functions, signatures, objects, enums), `private/wonder/nvn_trace.txt`.
- **Done when:** the trace covers boot up to the first present with no crash in the stubs.

### N3: device, memory, sync, window: first NVN pixel

- Device, queue, command buffers recorded into **our own command list** (not Maxwell methods) and replayed into
  Vulkan on `nvnQueueSubmitCommands`; a handle can be submitted many times.
- **Memory pools** are the core problem: the game writes buffers and textures straight into pool memory and passes
  **GPU addresses** (`nvnBufferGetAddress`) to commands.
  - Our own GPU address allocator; address → (pool, offset) → (VkBuffer, offset).
  - PC: `VK_EXT_external_memory_host` imports the guest pool itself as Vulkan memory. Guest memory is host memory,
    so there is no copy and no coherence problem.
  - Android (not all drivers have the extension): shadow buffers with dirty tracking (write-protected pages).
- Sync and events → timeline semaphores and fences; `nvnQueueFinish`.
- Window: `nvnWindowAcquireTexture` / `nvnQueuePresentTexture` over the present pass Hades already uses (rotation,
  scaling, surface loss).
- **Done when:** a clear colour presented through NVN.

### N4: textures and samplers

- Format table NVN → VkFormat. ASTC: native on Android; on PC decoded by a compute shader (desktop GPUs lack it).
- **Block linear**: Maxwell's tiled layout. Nintendo games load pre-tiled BNTX data into pools, so textures are
  untiled on upload (compute), and tiled again only if the game reads one back.
- Texture and sampler pools (descriptor tables the game keeps in memory, `NVNtextureHandle` = texture id | sampler
  id) → one bindless descriptor array per pool (descriptor indexing).
- Render targets, depth, MSAA resolve, copies and blits.
- **Done when:** the Nintendo logo and 2D menus.

### N5: shaders (the largest phase)

- Games carry Maxwell machine code (the SDK's offline compiler output: code plus a control section with the
  bindings). Every shader is in the ExeFS/RomFS, so they can be **translated ahead of time**, like the CPU code.
- `tools/maxwell/`: extraction (BNSH / program containers) → decoder → IR → SPIR-V, plus a runtime cache for
  variants that depend on draw-time state (vertex formats, texture types).
- Options for the translator: (a) our own, the most valuable to teach; (b) start by running an existing MIT
  translator offline (Ryujinx's shader project) and replace it step by step. See *Decisions*.
- Maxwell references: envytools, Mesa (nouveau codegen for GM107, NAK), the yuzu and Ryujinx decoders.
- Validation: per shader, compile and run SPIR-V in isolation against expected outputs; per frame, compare
  screenshots with a reference (the game on real hardware, or an emulator frame).
- **Done when:** the title screen matches the reference.

### N6: pipeline state and draws

- NVN state objects (blend, colour, depth/stencil, polygon, multisample, vertex attributes and streams) are set
  at any time; Vulkan wants pipelines. Use dynamic state wherever the driver supports it
  (`VK_EXT_extended_dynamic_state` 1–3, `vertex_input_dynamic_state`), with a pipeline cache for the rest.
  Every shader is known in advance, so pipelines can be built before gameplay.
- Draws (direct, indexed, indirect), compute dispatch, queries, barriers (`vkCmdPipelineBarrier2`), render target
  changes in the middle of a command buffer (dynamic rendering).
- Conventions: depth range (-1..1 vs 0..1), window origin, viewport flips.
- **Done when:** World 1-1 playable on PC.

### NA: AudioRenderer (found in N1)

- Wonder mixes through `nn::audio`'s **AudioRenderer** (`audren:u`), not AudioOut like Hades: the game describes
  voices, wave buffers (PCM, ADPCM, Opus), sub/final mixes and effects (delay, reverb, I3DL2 reverb, aux, biquad)
  and the service mixes them on the audio DSP. The service side is ours: parse the renderer's update buffers,
  decode, mix at 48 kHz, apply effects, hand the result to the platform audio device.
- Independent from the graphics phases: the game can run silent (a renderer that consumes buffers and reports
  them played) until this lands.

### N7: whole game, performance, Android

- Full playthrough; the tail of the API, rare formats, readbacks (GPU-written data the CPU reads).
- Profiling: GPU address translation, descriptor updates, pipeline hitches.
- Android: shadow pool path, the Adreno/Mali quirks already found with Hades.
- Audio, input and saves come from the shared runtime.

## Risks

| Risk | Mitigation |
|---|---|
| The game reads fields inside NVN objects (inlined helpers, nn::gfx) | the trace in N2 shows it; keep real field offsets for what is read |
| Shader translation is the whole project | ahead-of-time translation of a known, finite set; a reference translator to start |
| Pools written by the CPU while the GPU uses them | imported host memory (PC); page tracking (Android) |
| Readbacks of GPU data | tracked per resource; synchronous copy back when the CPU touches it |
| Newer SDK services and online-only features | stubs, offline mode like Hades |
| Legal exposure | own dumps only, no leaked material, repository stays private until decided otherwise |

## Decisions

1. **License of this repository** (open, none yet): MIT keeps it simple; GPL allows reusing yuzu code.
2. **Shader translator: an existing MIT translator first** (decided 2026-09-28). Ryujinx's shader translator, run
   offline, turns every one of Wonder's 12,717 Maxwell blocks into valid SPIR-V (docs/nvn/TRANSLATOR.md). Our own
   translator stays the long-term goal and can be checked shader by shader against it.
3. **First platform: macOS** (decided 2026-09-28). Apple M1 through MoltenVK. It has
   `VK_EXT_external_memory_host` (the "PC" pool path of N3 applies), extended dynamic state 1-3, dynamic rendering,
   timeline semaphores, descriptor indexing and native ASTC/BC. It lacks geometry shaders, transform feedback,
   logicOp and `vertex_input_dynamic_state`. Details in [nvn/WONDER.md](nvn/WONDER.md). Android follows.

Progress is logged in [../PROGRESS.md](../PROGRESS.md).
