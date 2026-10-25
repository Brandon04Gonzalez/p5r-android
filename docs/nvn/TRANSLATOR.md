# Shader translator: Ryujinx's, run offline (N5, plan option b)

N5 has to turn Wonder's Maxwell machine code into something the host GPU runs. [NVN.md](../NVN.md) left two options
open: (a) write our own translator, (b) start with an existing MIT translator run ahead of time and replace it step
by step. This page is the study of option (b): Ryujinx's shader translator (`Ryujinx.Graphics.Shader`, MIT) driven
from a small harness on the code blocks that `tools/maxwell/containers.py` extracts ([SHADERS.md](SHADERS.md)).

**Result in one line:** every one of Wonder's 12,717 distinct Maxwell programs (47,797 stages) translates to SPIR-V
that passes `spirv-val` for Vulkan 1.2, and all 12,717 also go through SPIRV-Cross to Metal and compile on the M1.
Ryujinx does 95.8% of the blocks (98.1% of the stages) as it is. The rest hit two bugs in its SPIR-V back end, which
the harness works around. A third back-end bug produced valid but wrong code (`else if` chains, 793 blocks); the
harness builds the translator with a patch for it.

Everything here was measured on our own cartridge dump (1.0.0, 010015100B514000). The translator source is not
vendored: it lives in `private/third_party/` like the game data, and the harness references it by path.

## Setup (macOS 14, arm64)

| Piece | Where | Notes |
|---|---|---|
| .NET SDK 10.0.401 | `private/third_party/dotnet/` | The Homebrew cask (`dotnet-sdk`) runs a `.pkg` installer through `sudo`, and the `dotnet` formula has no bottle for this OS. So we use Microsoft's tarball of the same build, unpacked without root. SHA-512 checked against Microsoft's release metadata. |
| Ryujinx source | `private/third_party/ryubing/` | Ryubing fork (the maintained continuation). Its home, `git.ryujinx.app`, did not answer from here, so this is the Codeberg archive of it (`Software_arch/Ryubing`, commit `a82350bb77`, 2026-07-17). `LICENSE.txt` is MIT. Only `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` are built. |
| `spirv-val`, `spirv-dis` | Homebrew | already installed |
| `spirv-cross` | `brew install spirv-cross` | only for the Metal check |

```sh
T=private/third_party; mkdir -p $T/dotnet $T/ryubing
curl -L https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-osx-arm64.tar.gz | tar -xz -C $T/dotnet
curl -L https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz | tar -xz -C $T/ryubing --strip-components=1
export DOTNET_ROOT=$PWD/$T/dotnet DOTNET_CLI_TELEMETRY_OPTOUT=1
$DOTNET_ROOT/dotnet build tools/maxwell/spirv_harness/SpirvHarness.csproj -c Release -m:2   # ~8 min the first time
H="$DOTNET_ROOT/dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll"
```

Build products go to `private/build/spirv_harness/` (set in the harness's `Directory.Build.props`; one `obj/<project>`
per project). `Ryujinx.Common` and `Spv.Generator` build inside their own checkout; the translator itself is built by
`translator/` (below). The build needs `patch` (macOS has it).

### Setup (Windows)

Same pieces, Windows 11 / PowerShell: the system-wide .NET SDK 10.0.401, the same Ryubing archive (`tar` ships with
Windows), `spirv-val` from the Vulkan SDK. Windows has no `patch`, so on `Windows_NT` the translator csproj runs
`tools/maxwell/apply_patch.py` instead (same arguments; unified diff after a free-text header, exact hunks with
patch's offset search, CRLF-insensitive, keeps the source's line ends); it uses `private\venv` if present, else
`python` (`-p:PatchPython=...`). The harness writes `results.tsv` with LF, strips Windows paths from stack frames
and orders manifests with `/`, so its output is the same bytes as on macOS.

```powershell
$T="private\third_party"; mkdir -Force $T\ryubing
curl.exe -L -o $T\ryubing.tar.gz https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz
tar -xzf $T\ryubing.tar.gz -C $T\ryubing --strip-components=1; rm $T\ryubing.tar.gz
dotnet build tools/maxwell/spirv_harness/SpirvHarness.csproj -c Release -m:2      # ~3 min the first time
private\venv\Scripts\python -m pip install zstandard                               # containers.py reads .zs
# extract: uber, secred, agl, gsys, effect_<name> (33), layout_<name> (the 103 blarc with shaders), as above
private\venv\Scripts\python tools\maxwell\exefs_shaders.py private\wonder\analysis\main.bin `
    --extract private\wonder\shaders_extracted\exefs                               # after the others (see below)
$env:PATH="C:\VulkanSDK\1.4.357.0\Bin;$env:PATH"
dotnet private\build\spirv_harness\bin\Release\net10.0\spirv_harness.dll --batch private\wonder\shaders_extracted `
    --out private\wonder\spirv_regen --jobs 3 --spirv-val C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe
```

**`exefs` group.** Besides the RomFS containers, `main` embeds 34 code blocks that no container holds (the glyph
shaders of *Silent miscompile* among them), hence 12,751 blocks and 47,831 stages in the batch. `exefs_shaders.py`
finds them by magic in the flat image of `tools/nso2bin.py` and pairs each with the control section whose fields
describe it (size, start, program length, constant data); it leaves out blocks the other manifests already have.

Measured (2026-09-30, while a `ninja -j6` build shared the machine): extraction 2 min (94 MB), batch with `spirv-val`
200 s with 3 jobs. All 12,751 `.spv` are byte for byte the Mac's (so the Mac set was built with `spirv-else-if` and
`spirv-fmz` only, not `spirv-nan`), and `shader_cache_test` passes on the regenerated `results.tsv`. Between two
runs `results.tsv` differs only in `ms` and, for about 24 `ok-retry` rows, in the stack frame quoted for the first
attempt's exception (`GenerateCall` or `GetLocalPointer`, depending on JIT inlining).

## The harness (`tools/maxwell/spirv_harness/`)

| File | What |
|---|---|
| `SpirvHarness.csproj` | a console program; one `ProjectReference` to `translator/Ryujinx.Graphics.Shader.csproj` |
| `translator/` | builds `Ryujinx.Graphics.Shader` from the sources in `private/third_party`, with our patches (`*.patch`) applied to copies under `private/build`; the checkout is never modified. See *Local patches* |
| `StaticGpuAccessor.cs` | our implementation of the translator's `IGpuAccessor`, the interface through which it asks "the GPU" for everything it cannot read from the code |
| `Program.cs` | single-stage and batch modes, `spirv-val`, the summary |
| `msl_check.swift` | compiles Metal source with the system Metal compiler (the MoltenVK-path check) |

```sh
# first extract (one directory per container, because each run rewrites manifest.tsv)
P=private/venv/bin/python; X=tools/maxwell/containers.py; O=private/wonder/shaders_extracted
$P $X private/wonder/shaders/SecredUber.bfsha --extract $O/uber --unique          # likewise secred, agl, gsys,
                                                                                  # effect_<name>, layout_<name>
$H $O/uber/code_<hash>.bin $O/uber/ctrl_<hash>.bin -o x.spv      # one stage; --glsl writes readable GLSL instead
$H --batch $O --out private/wonder/spirv --jobs 3                 # everything: <hash>.spv, results.tsv, summary.txt
```

Batch options: `--no-val` (skip `spirv-val`), `--target-env vulkan1.2` (the default), `--no-retry` (report the
translator exactly as Ryujinx runs it), `--debug-mode` (DebugMode from the first attempt, see below), `--limit N`.

### Feeding the translator

The translator wants three things: code at a "GPU address", a stage, and answers to its queries.

- **Code.** The harness treats the whole code block as the address space and passes the program start from the
  control section (+0x708: 0x30 for graphics, where the SPH sits; 0x100 for compute). Ryujinx reads the 0x50-byte SPH
  itself and decodes from there. The block is padded with zeros so the decoder can read past the end.
- **Stage.** Graphics stages come from the SPH (the translator reads it). Compute is flagged when the NVN header magic
  is `0x12345679`. The work-group size comes from control +0x748..+0x750.
- **Constant bank 1.** The translator reads `c[1]` directly when it follows indirect branch tables (`BRX`). The
  harness answers from the constant data at control +0x700/+0x6fc. That this data *is* `c[1]` is verified below.
- **Everything else** gets a static answer. The answers that stand in for draw-time state are listed in *What only
  the runtime knows*.

The host capabilities are those of MoltenVK on the M1, with the same choices Ryujinx's Vulkan back end makes there:
no geometry shaders, no transform feedback, no float64 (so the translator lowers doubles to floats), no shader ballot
extension, reduced precision (Ryujinx does this on MoltenVK), fragment shader interlock, subgroup size 32.

### Attempts and outputs

A block goes through the translator once. If that throws, it goes through a second time with
`TranslationFlags.DebugMode` and gets status `ok-retry` (see *Failures* for why). Each `.spv` goes through
`spirv-val --target-env vulkan1.2`. `results.tsv` has one line per distinct block: hash, container group, stage,
number of stages sharing the block, status, time, SPIR-V size, the resources it binds (in Maxwell and Vulkan terms),
translator messages, errors, and the draw-time queries it made. `summary.txt` groups all of it.

## Results

All 33 effect files, the 103 layout archives, the uber shader archive, `Secred.sarc`, `Lib/agl` and `Lib/gsys`:
12,717 distinct code blocks, used by 47,797 stages (the totals of [SHADERS.md](SHADERS.md)).

| | blocks | stages | |
|---|---|---|---|
| Ryujinx as is (`--no-retry`): valid SPIR-V | 12,183 (95.8%) | 46,890 (98.1%) | |
| &nbsp;&nbsp;exception in the SPIR-V back end | 531 | 895 | bug 1 |
| &nbsp;&nbsp;SPIR-V rejected by `spirv-val` | 3 | 12 | bug 2 |
| **with the two workarounds: valid SPIR-V** | **12,717 (100%)** | **47,797 (100%)** | |
| &nbsp;&nbsp;of which DebugMode retries | 531 | 895 | |
| SPIRV-Cross → MSL 3.0 → Metal compile on the M1 | 12,717 | | all compile |

| Group | Blocks | OK first time | OK on retry |
|---|---|---|---|
| uber (`SecredUber.bfsha`) | 4,855 | 4,855 | 0 |
| effects (33 `esetb`) | 4,014 | 3,856 | 158 |
| agl | 3,479 | 3,128 | 351 |
| secred (`GameAglShader`, small bfsha) | 203 | 191 | 12 |
| layouts (Ui2d) | 93 | 93 | 0 |
| gsys | 73 | 63 | 10 |

By stage: 7,810 fragment, 4,836 vertex, 71 compute blocks. Translation takes about 8 ms per block on average. The
whole set, with `spirv-val` on each, takes 2.5 to 5 minutes with 3 jobs, while another build was using the machine.
The SPIR-V totals 396 MB, about 30 KB per block (43 KB for the DebugMode ones).

**Decoder coverage.** A full pass in DebugMode reports every opcode the decoder does not know ("Invalid
instruction"); normal mode skips them silently. That pass found none. No block triggered any of the translator's
"instruction X is not implemented" messages either. Every Maxwell instruction Wonder's compiler emitted is decoded
and has an emitter.

**Semantic loss.** Only one translator message appears in the whole set: `Failed to find storage buffer for global
memory operation "Store"`, in one block (`gsys common.sharcb/clear_geometry_culling`, vertex). That store is dropped.
No texture access was dropped: no "failed to find handle source for bindless access" messages.

### Failures (both in Ryujinx's SPIR-V back end, not in the decoder)

1. **Call arguments** (531 blocks, 4.2%): `InvalidCastException: AstTextureOperation → AstOperand` (and two variants)
   in `CodeGen.Spirv.Instructions.GenerateCall`. The SPIR-V back end passes function arguments by pointer, so
   `StructuredProgram` copies each argument of a call into a temporary. `AstOptimizer` then propagates single-use
   temporaries back into their use, call arguments included. `GenerateCall` receives an expression where it expects
   a variable. The call is to `TextureSizeUnscale`, a helper Ryujinx inserts after every 2D texture size query
   (`TXQ`) in fragment and compute shaders for its resolution scaling. The GLSL back end has no such constraint,
   which is how the cause was found (`--glsl` on a failing block). **Workaround:** `DebugMode` turns that
   propagation off; the retry succeeds for all 531. **Proper fix** (for upstream or a local patch): skip the
   propagation in `AstOptimizer.Optimize` when a use of the variable is an `Instruction.Call`.
2. **Missing capability** (all modules with storage images: 26 agl/gsys compute blocks): when the host can read and write
   storage images without a declared format (MoltenVK can: `shaderStorageImageRead/WriteWithoutFormat`), Ryujinx
   declares them with `Unknown` format, but it never emits `OpCapability StorageImageReadWithoutFormat` /
   `StorageImageWriteWithoutFormat`. Drivers tend to accept this; `spirv-val` does not. **Workaround:** the harness
   inserts both capabilities after the module header when the shader uses images.

### Silent miscompile: `else if` chains (fixed by a local patch)

`spirv-val` cannot see this one: the module is valid, it just computes the wrong thing. Found through the dialog
text, which drew nothing (glyph vertex shaders `a106c763cf05f818` and `2fc962d8d063c1c9`, `exefs` group).

**What the Maxwell code does.** Each glyph is 64 bytes in a storage buffer; words 4..7 are its atlas rectangle
(u0, v0, u1, v1). The corner number picks which of the four words become the texture coordinate, through a
`switch` the compiler turned into a jump table in the block's own constant data (`c[1]`):

```
0x348  IADD32I R12, R12, -1            ; flag from c[1] (x: word 16 + 8 * corner, y: word 20 + 8 * corner)
0x350  IMNMX.U32 R12, R12, 2           ; clamp to the table
0x358  SHL R12, R12, 2
0x368  LDC R12, c[1][R12 + 0xc0]       ; words 48..50: 0x378, 0x390, 0x3a0
0x370  BRX R12                         ; jump to program start + R12
0x378  MOV R1, R5 ; BRK                ; case 1: v0
0x390  MOV R1, R6 ; BRK                ; case 2: u1
0x3a8  MOV R1, R7 ; BRK                ; case 3: v1
```

(flag 0 skips the switch and takes R4 = u0.) So 0x378, 0x390, 0x3a0 and 0x450, 0x430, 0x440 are **code offsets**:
`BRX` targets, in bytes from the start of the program (after the SPH). 0x450 in the second table is the `SYNC` after
the `v` switch, i.e. "keep R5". The corner flags in `c[1]` are x = {0, 2, 0, 2}, y = {1, 3, 1, 3}: corners
(u0,v0), (u1,v0), (u0,v1), (u1,v1).

**Decoder: right.** Ryujinx follows the pattern (`Decoder.FindBrxTargetRange`: `IMNMX.U32` / `SHL 2` /
`LDC c[1]`), reads the table through `IGpuAccessor.ConstantBuffer1Read` (our constant data) and finds all three
targets. `InstEmitFlowControl.Brx` turns them into `if (R12 == 0x378) ... else if (R12 == 0x390) ... else ...`. The
GLSL back end prints exactly that.

**SPIR-V back end: wrong.** With expression propagation on (not in DebugMode), `AstOptimizer.RemoveEmptyBlocks`
turns an `else` whose only content is an `if` into an `AstBlockType.ElseIf` block. `SpirvGenerator` does not know
that block type: the `If` before it branches to its merge block when false, and the `ElseIf` gets no condition and no
incoming edge. Every `else if` and the final `else` of a chain become unreachable code. Here that left only the
first case (`== 888`, `== 1072`); corners that needed u1 got a position value and those that needed v1 got v0, so
most of each glyph quad sampled empty atlas.

**Fix** (`translator/spirv-else-if.patch`, about 60 lines in `SpirvGenerator.Generate`): each `ElseIf` is emitted as a
nested selection. Its first label is the header (the previous condition's false target), it gets its own merge
block, and at the end of the chain the merge blocks branch outwards, innermost first. Upstream candidate.

**Reach.** 793 of the 12,751 blocks (8,760 stages) had unreachable blocks, all of them from this, all translated on
the first attempt (DebugMode output has no `ElseIf`): agl 378 (365 fragment), effects 367 (281 vertex, 57 fragment,
29 compute), secred 24, layouts 21, `exefs` 3 (the two glyph shaders and `395389fe95f3f4b1`). Not only jump tables:
any `if / else if` the optimizer produced. After the patch, a full batch leaves the other 11,958 `.spv` byte for byte
the same, no module has an unreachable block, all 12,751 pass `spirv-val`, and the 793 go through SPIRV-Cross to MSL
and compile with Metal. The previous files are kept next to the new ones as `private/wonder/spirv/<hash>.spv.orig`
(and `results.tsv.orig`).

Finding them again: a label that no branch, merge or phi instruction names (other than a function's first) is dead
code, and Ryujinx's back end emits none otherwise.

### Local patches

`translator/Ryujinx.Graphics.Shader.csproj` compiles the translator's sources straight from
`private/third_party/ryubing` under its own assembly name, except each file named by a `ShaderPatch` item: that one
is copied to `private/build/spirv_harness/obj/Ryujinx.Graphics.Shader/patched/`, patched with `patch`, and compiled
from there. To add a fix, add a `.patch` (paths as in the Ryujinx tree) and a `ShaderPatch` line. Bug 1 above could
move here the same way.

### Metal check

MoltenVK turns SPIR-V into Metal Shading Language with SPIRV-Cross and compiles it with the Metal compiler of the
OS. The same path, run by hand:

```sh
spirv-cross --msl --msl-version 30000 x.spv --output x.metal
swiftc -O tools/maxwell/spirv_harness/msl_check.swift -o private/build/spirv_harness/msl_check
find private/wonder/msl -name '*.metal' | private/build/spirv_harness/msl_check
```

All 12,717 convert and compile. (The `swift` interpreter gets no Metal device; compile the checker.) This uses
SPIRV-Cross's default options, not MoltenVK's exact configuration (argument buffers, its binding remap), and it
compiles each stage alone: vertex outputs are not matched against fragment inputs. It rules out shaders Metal
cannot express. It does not prove a pipeline will link.

## Where NVN puts resources (what the code reads)

The translator reports, per shader, every constant bank it reads, every storage buffer it found and where its
address came from, and every texture with the bank and word its handle came from. Set against the BNSH reflection
and the control section, this settles open questions 1 and 2 of [SHADERS.md](SHADERS.md). Status marks as there.

| Bank | Holds | Status |
|---|---|---|
| `c[0]` | driver constants. Graphics: storage buffer descriptors at byte `0x110 + 0x100 × stage + 0x10 × binding` (vertex = stage 0, fragment = stage 4); max point size at 0x610 (read by two agl vertex shaders, the only other graphics use of `c[0]`). Compute: uniform buffer descriptors at `0x210 + 0x10 × binding`, storage buffer descriptors at `0x310 + 0x10 × binding`. A descriptor is a 64-bit GPU address and a 32-bit size. | layout verified against reflection on all 513 stages that use it (every derived binding exists in the stage's reflection); point size inferred from the GLSL (`gl_PointSize = min(..., c0[0x610])`) |
| `c[1]` | the block's constant data (control +0x700, +0x6fc bytes) | verified: 6,756 blocks read `c[1]`, all have constant data; no block without constant data reads it (4 blocks with constant data never read it) |
| `c[2]` | texture handles. The handle of sampler binding *n* is the 64-bit word at byte `0x20 + 8n`. Storage image handles appear at 0x120, 0x128, 0x130 in agl compute shaders. | textures verified on all 47,797 stages: the words read are exactly `{8 + 2n}` for the control section's sampler slots (+0x804), with 12 agl stages reading a subset, 0 mismatches. Images inferred: 0x120 = 0x20 + 32 × 8, i.e. an image table after 32 texture slots. |
| `c[3 + n]` | uniform block binding *n* (graphics stages) | verified on all 26,735 BNSH graphics stages: the banks read, minus 0 and 1, equal `{n + 3}` over reflected bindings (3,259 read a subset), 0 mismatches |

The effect (BNSH) compute shaders read no `c[3+]` bank. Their uniform blocks, like their storage buffers, are
reached through addresses loaded from `c[0]`, and the translator turns those into storage buffers. The agl/gsys
compute shaders (BAHS, no reflection) do read `c[3]` directly (39 blocks): two compiler setups, as the two control
section flavours in SHADERS.md already suggested. Ryujinx's "multi-target" handling
reads the `c[0]` descriptors themselves in the shader, to decide which buffer an address falls in.

**Bindless textures from uniform blocks.** 2,263 blocks (mostly uber fragment shaders: `c[7]` word 32, i.e.
`User0` offset 0x80) sample a texture whose handle the *game* wrote into one of its uniform blocks, not into `c[2]`.
The translator recognises the pattern and gives it an ordinary binding, recording `(bank 7, word 32)`. At draw time
the runtime must read that word from the bound uniform buffer to know which texture to put there. No shader indexes
arrays of textures, and none needs the full texture pool (no pool-sized arrays were created).

Texture types seen: 2D 24,118; cube 5,716; cube array 2,132; 3D 2,102; 2D array shadow 1,053; 2D array 849; 2D
shadow 769; cube shadow 12; 2D multisample 3 (counted per texture per block). Images: 63, all 2D, in 26 compute blocks (25 agl, 1 gsys).

## How Ryujinx maps them to Vulkan (the binding contract)

The translator only asks "which (set, binding) for constant buffer *k* / storage buffer *k* / the next texture /
image". The answers are ours to choose. The harness gives the answers of Ryujinx's own Vulkan back end, so the
generated SPIR-V follows a layout already proven in practice:

| Set | Kind | Binding |
|---|---|---|
| 0 | uniform buffers | `stageSlot × 18 + 1 + k` for `c[k]`; binding 0 is the translator's **support buffer** (below) |
| 1 | storage buffers | `stageSlot × 16 + k`, *k* = the order in which the translator found them (the SPIR-V records the `c[0]` offset each one came from) |
| 2 | textures (combined image + sampler) | `stageSlot × 128 + i` in order of first use; `+ 64` for buffer textures |
| 3 | storage images | `stageSlot × 32 + i`; `+ 16` for buffer images |

`stageSlot` is 0 for vertex and compute, 1 for fragment (Ryujinx's remap puts the common stages first: geometry 2,
tessellation 3 and 4). Arrays of textures would go after all stage ranges; Ryujinx's Vulkan back end puts them in
extra sets from 4 up. Wonder has none. Example, uber pixel shader v00000: `c[1] c[3] c[5] c[6] c[8]` at set 0,
bindings 20, 22, 24, 25, 27; ten textures at set 2, bindings 128..137.

So, at each draw, our runtime binds:

- set 0: for each stage, `c[1]` = the stage's constant data (from the shader file) and `c[3+n]` = the buffer the game
  bound with `nvnCommandBufferBindUniformBuffer(stage, n, address, size)`. Binding 0 = the support buffer.
- set 1: for each storage buffer of the shader, the `c[0]` offset in its descriptor says which NVN binding it is.
  Bind the buffer the game bound to that slot (`BindStorageBuffer`). For compute this includes the uniform blocks.
- set 2: for each texture, the handle word says which NVN sampler binding it is. Bind what the game bound there
  (`BindTexture` gives a texture handle = texture id + sampler id into the game's pools). For bindless-from-UBO
  textures, read the handle from the uniform buffer word the SPIR-V recorded.
- a real `c[0]` where a shader reads it: compute shaders read the descriptor table itself (see above), so for them
  the runtime has to write GPU addresses and sizes at the NVN offsets, like the NVN driver does.

The per-shader record (`ShaderProgramInfo`: buffers with slot/offset, textures with bank/handle/type, set and
binding) is what the runtime needs next to each `.spv`. The harness prints it; the real pipeline should serialize it
into the shader cache.

The **support buffer** (set 0, binding 0, std140, shared by all stages) holds: alpha test reference, per-render-target
"is BGRA" flags, viewport inverse and size, render scale factors (1 for the render target, 64 textures, 8 images),
transform feedback emulation data. Ryujinx fills it every draw. For us: render scales stay 1.0 (no resolution
scaling), viewport values from the viewport state. About 30% of a 300-module sample access it.

## What only the runtime knows (variant keys)

These are the inputs the translator takes from draw-time state. The harness answers with fixed defaults, so the
baked SPIR-V is right when the draw matches them. Anything else needs a variant, built at run time or baked for the
combinations the game really uses. "Queried" counts come from instrumenting the accessor.

| Input | Default baked | Measured / relevance for Wonder | NVN source |
|---|---|---|---|
| vertex attribute numeric type per location (float / sint / uint; scaled if the host lacks scaled formats) | all float | every vertex shader reads attributes; integer formats (e.g. blend indices) need `Sint`/`Uint` declarations | `nvnVertexAttribStateSetFormat` |
| fragment output numeric type per render target | all float | Ryujinx specializes on this only where the host needs it, and MoltenVK is one (`needsFragmentOutputSpecialization`) | render target formats |
| alpha test (function, reference) | off | Vulkan has no alpha test; Ryujinx emits it in the shader | `nvnColorStateSetAlphaTest`, `nvnCommandBufferSetAlphaRef` (both loaded by Wonder) |
| alpha to coverage (+dither), dual-source blend, early-Z force | off | fragment only | multisample / blend / depth state |
| depth mode (-1..1 or 0..1), window origin, viewport transform disabled | 0..1, upper left | a device-wide setting, a handful of variants at most | `nvnDeviceSetDepthMode`, `nvnDeviceSetWindowOriginMode` |
| program point size, point size | off / 1.0 | only for point draws | `nvnCommandBufferSetPointSize` |
| texture coordinates normalized, per texture | yes | queried by 8,333 blocks (every sampling shader); only textures created with unnormalized coordinates would need a variant | texture descriptor |
| sampler type, when the instruction does not say (texture queries, 1D vs buffer) | 2D | queried by 505 blocks | texture descriptor |
| storage buffer alignment | 16-byte aligned | queried by 276 blocks (all storage users) | addresses bound by the game |
| base vertex/instance through `c[0]` | not through `c[0]` | asked by every vertex shader but used by none (no vertex shader reads `c[0]` 0x640+) | draw call |
| texture format for typed image access | not asked | not asked on MoltenVK: images are declared without a format | texture descriptor |
| primitive topology, tessellation mode | triangles | only matters for geometry/tessellation stages: none in Wonder | draw call |

The per-shader list of queries made is in `results.tsv` (column `draw_time_queries`). A good default for the
runtime: bake the defaults above, compute the key at draw time from the few inputs that apply to the bound program
(attribute types for its read locations, output types for its written targets, alpha test, the flags of its
textures), and fall back to a variant when the key differs.

## Recommendation

**Adopt option (b) now; keep (a) as the long-term replacement, now with a test oracle.**

- The coverage question is answered: every shader in the game translates, the decoder knows every instruction, the
  output validates and compiles for Metal. The two failures are small back-end bugs with known fixes.
- It settles the resource contract our NVN runtime needs anyway (the table above), whichever translator ends up
  producing the SPIR-V. Our own translator (a) would have to match the same bank layout. Keeping Ryujinx's Vulkan
  binding scheme lets its output and ours be swapped per shader.
- The weak spot is variants. The translator is C#, and the runtime is C++ on macOS and Android. Three ways, in
  order of preference:
  1. **Bake the variants the game uses.** Log variant keys at run time (the key is small), feed the log back to the
     offline harness, and ship the extra `.spv`. This fits the project's ahead-of-time approach and needs nothing
     at run time.
  2. **Link the translator into the runtime.** .NET NativeAOT can build `Ryujinx.Graphics.Shader` plus a C ABI entry
     point into a native library for macOS and Android arm64, so misses translate on the spot. It is a larger
     dependency, but MIT.
  3. **Make the translator's defaults cover more cases** (e.g. alpha test through the support buffer, as Ryujinx
     already does for some state) so fewer keys matter.
- Replacing it with (a) later is incremental: our decoder/IR can be checked block by block against this output (same
  bindings, same inputs), starting with the 93 Ui2d blocks whose GLSL sources ship in the layouts.

`spirv-val` and the Metal compile prove the output is well formed, not that it is right. Correctness still needs
the N5 validation plan: run shaders in isolation, and compare frames against a reference.

## Next steps

1. Fix bug 1 at the source (a one-line condition in `AstOptimizer`, offered upstream) so DebugMode is not needed.
2. Serialize `ShaderProgramInfo` next to each `.spv` (bindings plus Maxwell origin of each resource) as the runtime's
   cache format, keyed on the code hash of [SHADERS.md](SHADERS.md).
3. Link-test vertex/fragment pairs of each program in a real MoltenVK pipeline (the check above is per stage).
4. Confirm the image handle table at `c[2]` 0x120 and the point size at `c[0]` 0x610 from the NVN driver in the
   recompiled `sdk`.

Reproducible outputs in `private/wonder/`: `spirv/` (SPIR-V, `results.tsv`, `summary.txt`),
`spirv_results_noretry.tsv` and `spirv_summary_noretry.txt` (the first-pass numbers), `spirv_debug/summary.txt`
(the DebugMode pass), `msl/` and `msl_check.log`.

## Runtime side

`runtime/src/nvn/nvn_shaders.{h,cpp}` is what the pipeline builder uses. `shader_for(code_host, control)` reads the
code block size at control +0x704, hashes the block (SHA-1, first 16 hex digits), loads
`<data root>/spirv/<hash>.spv` once and returns a `ShaderInfo`: the SPIR-V words, the stage (from its execution
model), a reflection of the module, the constant data `c[1]` (control +0x700 offset, +0x6fc size) and the compute
work-group size (control +0x748). Call it when the program is set and keep the pointer: hashing is a pass over the
block.

The reflection reads the SPIR-V itself: descriptor bindings with their Vulkan type, vertex input and fragment output
locations with their component type, built-ins. The NVN meaning of each binding comes from the translator's names
and numbers (`ResourceManager.cs`), which follow the scheme above:

| SPIR-V binding | Name | NVN |
|---|---|---|
| set 0, binding 0 | `support_buffer` | the support buffer, filled by us (below) |
| set 0, `slot × 18 + 1 + k` | `<st>_c<k>` | `c[0]` driver constants, `c[1]` constant data, `c[3+n]` uniform block *n* |
| set 1, `slot × 16 + i` | `<st>_s<i>` | the buffer whose descriptor is at a `c[0]` offset: graphics `0x110 + 0x100 × stage + 0x10 × n` = storage binding *n*; compute `0x210 + 0x10 × n` = uniform block *n*, `0x310 + 0x10 × n` = storage binding *n* |
| set 2, `slot × 128 + i` | `<st>_t_tcb_<w>` | handle at `c[2]` word *w* (hex): sampler binding `(w - 8) / 2` |
| set 2 | `<st>_t_cb<k>_<w>` | a handle the game wrote into uniform block `k - 3`, at byte `4w` |
| set 3, `slot × 32 + i` | `<st>_i_cb2_<w>_<fmt>` | image binding `(w - 72) / 2` (inferred: `c[2]` 0x120 + 8n) |

`<st>` is `vp`, `fp` or `cp`. `slot` is 0 for vertex and compute, 1 for fragment. The only fact the SPIR-V does not
carry is the `c[0]` offset of a storage buffer (the name gives only the translator's slot), so the runtime reads it
from the `resources` column of `spirv/results.tsv`. Once the harness serializes `ShaderProgramInfo` (next step 2),
read that file instead.

`tests/shader_cache_test.cpp` runs the same code standalone. On all 47,797 manifest stages the size at +0x704, the
constant data fields and the hash agree with the extraction. On all 12,717 shaders the reflected bindings, described
the way the harness does, equal the `resources` column, and every binding gets a meaning. Counts: 42,315 uniform
blocks, 6,756 `c[1]`, 32 `c[0]`, 34,361 sampler-binding textures, 2,393 textures with handles in uniform blocks
(`c[4]`, `c[6]`, `c[7]`, `c[8]`), 63 images, 498 storage buffers and 145 compute uniform blocks read as storage. All
vertex inputs and fragment outputs are float `vec4` (the baked default). Every shader declares the support buffer.

**Support buffer** (std140, 0x570 bytes, layout from Ryujinx's `SupportBuffer.cs`, MIT): `alpha_test` 0x000,
`is_bgra[8]` 0x010, `viewport_inverse` 0x090, `viewport_size` 0x0a0, `frag_scale_count` 0x0b0, `render_scale[73]`
0x0c0 (x only: render target, 64 textures, 8 images), `tfe_offset` 0x550, `tfe_vertex_count` 0x560. With the baked
defaults, Wonder's shaders read only `render_scale` (3,390 shaders) and `frag_scale_count` (183 vertex shaders,
to index their texture scales). With every scale at 1.0 those reads are identities, whatever the count.
`support_buffer_defaults()` returns exactly that.

```sh
clang++ -std=c++20 -O2 -DNX_SHADERS_STANDALONE -Iruntime/src/nvn tests/shader_cache_test.cpp \
    runtime/src/nvn/nvn_shaders.cpp -o private/build/shader_cache_test && private/build/shader_cache_test private/wonder
```
