# Super Mario Bros. Wonder: what the game is made of

Facts measured on our own cartridge dump (1.0.0, 010015100B514000). Tools and numbers here are reproducible with
the commands below; everything derived from the game stays in `private/wonder/` (never committed).

## Extraction (macOS)

nstool v1.9.2 has a macOS arm64 release (`gh release download v1.9.2 -R jakcron/nstool`). Keys from our own
console (Lockpick_RCM) in `private/keys/prod.keys`.

```sh
N=private/tools/nstool; K=private/keys/prod.keys; W=private/wonder
$N -k $K --fstree MarioWonder.xci                    # gamecard: update/ (empty), normal/, secure/
$N -k $K -x /secure $W/secure MarioWonder.xci         # 4 NCAs; the 3.7 GB one is the program
$N -k $K -x /0 $W/exefs $W/secure/<program>.nca       # main, main.npdm, rtld, sdk
$N -k $K -x /1 $W/romfs $W/secure/<program>.nca       # 3.5 GB
python3 -m venv private/venv && private/venv/bin/pip install lz4 capstone unicorn
private/venv/bin/python tools/probe.py $W/exefs
```

The virtual path is `/secure`, not `gamecard:/secure`. No update is on the cartridge, so the ExeFS is the base game.

## Modules

| Module | Size | Imports | Exports | Notes |
|---|---|---|---|---|
| `rtld` | 0x32a0 | 18 | 0 | not loaded (the runtime does its job) |
| `main` | 0x359cc28 (53.6 MB) | 1,107 | 88 | the game |
| `sdk` | 0xd4d8d0 (13.3 MB) | 13 | 27,619 | NintendoSDK 16.2.0 + NVN 16.2.0 driver |

No `subsdk*`. Load bases: `main` 0x40_0000_0000, `sdk` 0x40_1000_0000 (as for Hades).

SDK and middleware markers: `NintendoSdk_nnSdk-16_2_0`, `NintendoSDK_NVN-16_2_0` (in `sdk`); NintendoWare
`G3d2`, `Ui2d`, `Vfx2`, `Font` 16.2.0; Pia 6.30 (Clone, Common, Lan, Local, Npln, Session, Sync, Transport);
NPLN 1.16.4 with gRPC, OpenSSL, libcurl, Google APIs (online play); libz. The engine is sead (Nintendo EPD) with agl
on top of a statically linked nn::gfx.

## Imports of `main` by namespace

244 libc, 197 nn::audio, 119 nn::os, 118 C++, 53 nn::hid, 35 nn::util, 33 nn::async, 30 nn::socket, 29 nn::fs,
27 nn::prepo, 25 nn::ldn, 24 nn::fontll, 24 nn::account, 20 nn::oe, 18 nn::image, 14 nn::time, 12 nn::swkbd,
11 nn::friends, 9 nn::mem, 9 nn::err, 9 nn::crypto, 8 nn::nifm, 8 nn::ssl, 5 nn::diag, 5 nn::vi, 5 nn::ngc, 3 nn::pl,
3 nv::, 3 nn::lmem, 2 nn::nsd, **2 nvn** (`nvnBootstrapLoader`, `nvnPrivate0`), 1 each nn::codec/settings/detail.

791 of these are not imported by Hades (`private/wonder/imports_new_vs_hades.txt`). The imports resolve to `sdk`
exports, which are recompiled; what the runtime has to provide are the **services** behind them (IPC), not the
functions, except where we choose HLE (NVN, audio).

### Audio

Hades used AudioOut. Wonder uses the **AudioRenderer** (`nn::audio::*` over `audren:u`): the game builds a
renderer configuration (voices, wave buffers, ADPCM/PCM/Opus, mixes, effects: delay, reverb, I3DL2 reverb, aux,
biquad) and the service, on hardware the audio DSP, mixes it.

The SDK 16 `sdk` also carries the renderer's **server** side: `nn::audio::server::AudioRendererManagerImpl` (with
`AudioRendererImpl`, `AudioDeviceImpl`, `ExecuteAudioRendererRendering`), reachable in process through
`nn::audio::CreateAudioRendererManagerByDfc` ("direct function call") next to the usual
`CreateAudioRendererManagerByHipc`. If the DFC path can be selected, the update parsing, command generation and
possibly the mixing come recompiled for free, and only the final output needs the host. To verify during boot.

## NVN

`main` imports `nvnBootstrapLoader` from `sdk` and loads the whole C API by name (536 `nvn*` strings in `main`,
`private/wonder/nvn_names_main.txt`). By object:

149 CommandBuffer, 71 Texture, 40 Sampler, 33 Device, 33 Queue, 24 Window, 23 MultisampleState, 18 Buffer,
18 MemoryPool, 15 BlendState, 14 TextureView, 13 DepthStencilState, 11 Event, 9 PolygonState, 8 Program,
8 SamplerPool, 8 TexturePool, 7 ColorState, 5 Sync, 5 VertexAttribState, 5 VertexStreamState, 3 ChannelMaskState,
2 CommandBufferMulti, plus debug and texel read/write helpers.

Loading every name does not mean using every function: the N2 tracer tells which are called.

## Shaders

All RomFS files ending in `.zs` are plain zstd (no dictionary): `zstd -d`.

| Where | What |
|---|---|
| `Shader/SecredUber.Product.100.product.Nin_NX_NVN.bfsha.zs` | 42 MB BFSHA (`FSHA`): the G3d2 uber shader with its variations |
| `Shader/Secred.Nin_NX_NVN.release.sarc.zs` | agl shaders (`GameAglShader.sharcb`), gsys configuration |
| `Effect/static.Nin_NX_NVN.esetb.byml.zs` | 102 MB Vfx2 effect set with its shaders; 33 more per-effect `esetb` |
| `Layout/*.Nin_NX_NVN.blarc.zs` | Ui2d layouts (234) |

Shader code is Maxwell machine code inside BNSH containers (N5).

## RomFS at a glance

10,181 `bwav`, 2,934 `pack.zs`, 1,270 `bfres.zs` (models), 557 `bars.zs`, 531 `bntx.zs` (textures), 447 `bcett.byml.zs`,
447 + 91 Havok physics (`bphsc`/`bphsh`), BYML/BGYML game parameters.

## Recompilation

| | `main` | `sdk` |
|---|---|---|
| Functions | 209,629 | 40,395 |
| Instructions | 16.3M | 1.5M |
| Jump tables | 1,742 | 661 |
| Unresolved `br` (go through `nx::call`) | 11,092 | |
| Generated C++ | ~630 MB, 1,198 files | ~140 MB |

```sh
private/venv/bin/python -m recomp private/wonder/exefs/main private/wonder/gen/main --name main --base 0x4000000000
private/venv/bin/python -m recomp private/wonder/exefs/sdk  private/wonder/gen/sdk  --name sdk  --base 0x4010000000
```

The recompiler peaks at ~2.4 GB of memory on `main` (about 6 minutes on the M1).

## Host: MoltenVK on the Apple M1

`vulkaninfo` (MoltenVK 1.4.2, API 1.4.357). Present: `VK_EXT_external_memory_host`, `extended_dynamic_state`
1/2/3, `VK_KHR_dynamic_rendering`, timeline semaphores, `synchronization2`, descriptor indexing, `robustness2`,
`depth_clip_control`, `fragment_shader_interlock`, `shader_float16_int8`, ASTC LDR and BC textures, tessellation,
depth clamp. Absent: `geometryShader`, `VK_EXT_transform_feedback`, `logicOp`, `vertex_input_dynamic_state`,
`shaderFloat64`.

Consequences for the plan: memory pools can be imported directly (the PC path of N3 works on the Mac); ASTC needs
no decoder; transform feedback, geometry shaders and logic ops, if the trace shows Wonder uses them, need
workarounds (compute emulation / shader rewrites).
