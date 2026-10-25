# Shader containers: where Wonder's Maxwell code lives

Preparation for N5 (Maxwell → SPIR-V ahead of time). This page describes how the shader binaries of Super Mario
Bros. Wonder are packed, how to get from a RomFS file to the Maxwell instructions of one stage, and what was
measured. Nothing is translated here. Every number comes from our own cartridge dump (1.0.0, 010015100B514000) and
is reproducible with `tools/maxwell/containers.py`; derived files stay in `private/wonder/`.

Layouts were worked out from the bytes, with the public descriptions of BNSH/BFSHA/SARC in open-source tools (Switch
Toolbox, the Ryujinx and yuzu write-ups) as a map. Each fact below is marked **verified** (checked by the tool on
every instance in the game, or by hand on the bytes) or **inferred** (a reading that fits what we saw but that
nothing checks).

## The tool

```sh
P=private/venv/bin/python; T=tools/maxwell/containers.py
$P $T private/wonder/shaders/SecredUber.bfsha                  # describe: models, variations, stages, checks
$P $T <file> --list                                             # one line per stage: id, offsets, sizes, hash
$P $T <file> --reflect                                          # + names and binding slots per stage (BNSH)
$P $T <file> --extract private/wonder/sh/uber [--unique]        # .code.bin / .ctrl.bin per stage + manifest.tsv
$P $T --inventory <files or dirs...>                            # totals (private/wonder/shader_inventory.txt)
```

It opens SARC, FSHA (`.bfsha`), BNSH (`.bnsh`), VFXB (inside `.esetb.byml`), BAHS (`.sharcb`), follows nesting and
decompresses `.zs` itself. A stage's id is its path in the container tree, e.g.
`SecredUber.bfsha/SecredUber/v00940/pixel` or `Secred.sarc/GameAglShader.sharcb/ssao/v012/vertex`; ids are stable
as long as the file does not change. The manifest also gives a content hash of the code block, which is what N5
should key its cache on: the same block is shared by many variations. `--extract` refuses to write outside
`private/`.

Every stage goes through a **layout check** (magics, header and padding are zero, the control section's sizes agree
with the code block, the SPH declares the stage the container says). All 47,797 stages in the game pass.

## Where the shaders are

| RomFS file | Container chain | Stages |
|---|---|---|
| `Shader/SecredUber.Product.100.product.Nin_NX_NVN.bfsha.zs` | FSHA → 2 BNSH | 17,160 |
| `Shader/Secred.Nin_NX_NVN.release.sarc.zs` | SARC → `GameAglShader.sharcb` (BAHS) + a small `SecredUber.Nin_NX_NVN.bfsha` (FSHA → 3 BNSH) | 760 |
| `Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs` | SARC → 7 BAHS (`agl_common`, `agl_technique*`) | 8,214 |
| `Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc.zs` | SARC → `common.sharcb` (BAHS) | 431 |
| `Effect/*.Nin_NX_NVN.esetb.byml.zs` (33) | BYML → VFXB → GRSR (graphics) BNSH + GRSC (compute) BNSH | 7,702 |
| `Layout/*.Nin_NX_NVN.blarc.zs` (103 of 234) | SARC → `bgsh/__ArchiveShader.bnsh` | 13,530 |

The two `Lib/` archives were not on the first list; a sweep of the whole RomFS for the magics found them. Nothing
else carries Maxwell code (the two other hits, in a `.bfres` and a `.bwav`, are noise). `Lib/gsys` also holds two
`Gfx2` files (Wii U GX2 shaders, unused leftovers).

## Measured

From `private/wonder/shader_inventory.txt`:

| Group | Programs | Variations | Stages (vertex / pixel / compute) | Code bytes | Distinct code blocks |
|---|---|---|---|---|---|
| SecredUber.bfsha | 8,580 | 8,580 | 8,580 / 8,580 / 0 | 44.9 MB | 4,857 (18.8 MB) |
| Secred.sarc | 236 | 380 | 380 / 380 / 0 | 2.1 MB | 215 (0.76 MB) |
| Lib/agl + Lib/gsys | 242 | 4,351 | 4,294 / 4,294 / 57 | 13.9 MB | 3,552 (9.0 MB) |
| Effects (33 esetb) | – | 3,969 | 3,733 / 3,733 / 236 | 40.9 MB | 4,014 (26.4 MB) |
| Layouts (103 blarc) | – | 6,765 | 6,765 / 6,765 / 0 | 19.8 MB | 93 (0.14 MB) |
| **All** | 9,058 | 24,045 | 23,752 / 23,752 / 293 | 121.6 MB | **12,717 (55.1 MB)** |

*Programs* are what the engine names (G3d2 shading-model programs, agl programs); *variations* are compiled
programs with code; *code bytes* count a shared block once per stage that uses it. Distinct control sections: 8,496
(18.5 MB).

What this means for N5: **12,717 distinct Maxwell programs** to translate, not 47,797. Only vertex, pixel and
compute stages exist; no hull, domain or geometry shader anywhere (good news for MoltenVK, which has no geometry
shaders). The 103 Ui2d archives are almost all the same shaders: 13,530 stages, 93 distinct blocks.

## Common building blocks

The nn::util binary format (BNSH, FSHA, and in spirit VFXB) is used everywhere:

**File header** (0x20 bytes, verified on BNSH and FSHA): 8-byte magic (`"BNSH\0\0\0\0"`, `"FSHA    "`), u32 version
(BNSH 2.2.1, FSHA 9.0.0), u16 byte order mark `FEFF`, u8 log2 alignment (0x0c = 4 KiB), u8, u32 offset of the file
name (points at the characters of a string in the pool), u16 "is relocated" flag, u16 offset of the first block,
u32 offset of the relocation table, u32 file size.

**Pointers** are stored as 8-byte offsets from the start of the file they belong to (a BNSH embedded in an FSHA has
its own origin). At load time the runtime adds the load address to each one, guided by:

**Relocation table** `_RLT` (verified): u32 magic, u32 own offset, u32 section count, u32 0; sections of 0x18
(u64 0, u32 position, u32 size, u32 first entry, u32 entry count); entries of 8 (u32 position, u16 structure count,
u8 pointer count, u8 skip count), meaning "from `position`, `structure count` times: `pointer count` consecutive
pointers, then skip `skip count` 8-byte words". A section groups the pointers **by where they point**, not by where
they are: in the uber BNSH section 4 is the code pool and its 16,601 entries are the 16,600 per-stage code pointers
plus the pool's own; section 1 is the storage reserved for the 8,300 runtime program objects. This is what lets the
loader put the code pool in GPU memory and the metadata elsewhere. For N5 the table is a free cross-check: every
field we read as a pointer should be listed.

**String pool** `_STR`: block header, u32 count, then strings stored as u16 length, characters, NUL, 2-byte aligned.
String pointers point at the length (verified).

**Dictionary** (`ResDic`, verified): u32 signature (`"_DIC"` in BNSH, 0 in FSHA), s32 count, then count+1 nodes of
0x10 bytes (s32 reference bit, u16 left, u16 right, u64 key pointer). Node 0 is the root; nodes 1..count are the
entries in index order, so the key list alone gives index → name.

## BNSH (nn::gfx shader file)

Everything the other formats carry is, in the end, one or more BNSH (except agl's BAHS, below).

```
0x00  file header (0x20)                    0x20..0x60 zero
0x60  "grsc" block (ResShaderContainerData)
        +0x00 magic, u32 next block, u32 block size, u32 0
        +0x10 u16 API = 4 (NVN), u16 API version 0, u8 code type 0 (binary), 3 reserved
        +0x18 u32 compiler version (0xf02 in every file)
        +0x1c u32 variation count
        +0x20 ptr variation array
        +0x28 ptr code memory pool descriptor: u32 properties (0x61), u32 size, ptr pool start
        +0x30 u16 x4: 0x10, 0x1, 0x18, 0x11 in every file (meaning unknown)
0xc0  variations, 0x40 each: ptr source, ptr intermediate, ptr binary program, ptr parent, reserved
      (source and intermediate are null in every file: only machine code ships)
....  programs (0xa0 each):
        +0x00 u8 flags, u8 code type, u8 source format, u8, u32 binary format
        +0x08 ptr per-stage code descriptor x6: vertex, hull, domain, geometry, pixel, compute (null = absent)
        +0x60 u32 runtime object size (0x100), ptr object storage, ptr parent variation, ptr reflection
....  per-stage code descriptors (0x40 each):
        +0x00 u64 0
        +0x08 ptr control section
        +0x10 ptr code block (inside the code pool)
        +0x18 u32 code block size, +0x1c u32 control section size
        +0x28 u32, +0x2c u32 (inferred: scratch memory, see below)
....  control sections (0x880 or 0x88c bytes), reflection, dictionaries
....  runtime object storage (zero)
pool  code pool (4 KiB aligned, pool properties 0x61)
....  _STR, _RLT
```

Verified on every BNSH in the game: the stage slots, the descriptor fields at +0x08..+0x1c, the pool bounds.
**Code blocks are shared**: the distinct blocks lie back to back from the start of the pool, 0x100-aligned, and no
control section is inside the pool. The pool properties 0x61 read as CPU no-access | GPU cached | shader code if the
nn::gfx memory pool bits apply (inferred).

The descriptor words at +0x28/+0x2c are zero or a pair like 0x60000/0x600, 0x80000/0x800, 0xa0000/0xa00; in the uber
shader the second one is 0x600 + 0x20 x (local memory bytes in the SPH) when non-zero, and it equals control +0x710.
They look like the scratch (local memory) size per warp and a recommended total. Inferred.

### Reflection

Verified: the dictionaries and slot arrays parse on every BNSH stage, the sampler slots agree with the control
section (below) on all of them, and the compute work-group size agrees with the control section. The program's
reflection pointer leads to 6 stage pointers (same order as the code slots), each to 0x60 bytes:

| Offset | Field |
|---|---|
| +0x00 / 08 / 10 / 18 / 20 | dictionaries: inputs, outputs, samplers, constant (uniform) buffers, storage buffers |
| +0x28 / 2c / 30 / 34 | first slot index of outputs, samplers, constant buffers, storage buffers |
| +0x38 | ptr slot array (s32 per entry; inputs start at 0; -1 = built-in such as `gl_VertexID`) |
| +0x40 | u32 x3 compute work-group size |
| +0x4c, +0x50 | first slot index of images, image dictionary |

One stage in the whole game has no reflection (in the small bfsha of `Secred.sarc`); every other BNSH stage has it,
and no stage declares an image. So each stage says, by name, which vertex attribute, render target, texture,
uniform block and storage buffer it uses and at which NVN binding. `--reflect` prints it, e.g. for a Vfx2 particle
vertex shader: `in[sysLocalPosAttr@0, ..., gl_VertexID@-1] cbuf[NnVfx2ViewParam@5, sysEmitterStaticUniformBlock@6,
...]`. agl's BAHS does not use BNSH and has no such reflection (see below).

## The code block (verified on all 47,797 stages)

```
+0x00   0x30-byte NVN header: u32 0x12345678 (vertex/pixel) or 0x12345679 (compute), then 0x2c zero bytes
+0x30   graphics: SPH, the 0x50-byte Shader Program Header, then Maxwell instructions
+0x100  compute: Maxwell instructions (0x30..0x100 is zero); compute has no SPH
        program end (control +0x708 start, +0x6f8 length), zero padding to 0x100
        constant data (control +0x700 offset, +0x6fc size), absent in ~60% of stages
        zero padding to 0x100 = code block size (control +0x704)
```

- The SPH's shader type (word 0 bits 10..13: 1 vertex, 5 pixel) matches the stage slot for every graphics stage.
- Instructions come in Maxwell's groups of four 64-bit words: one scheduling/control word (e.g. `0x001fc000e22007f0`) then
  three instructions. Programs end with `EXIT` (`0xe30000000007000f`) and a branch to itself
  (`0xe2400fffff87000f`), then NOP fill to the program length.
- The constant data is what the compiler hoisted out of the shader (the uber pixel shader v0 has 15 floats there:
  1.0, 1.4, 0.159, ...). Where the shader reads it from (which constant bank the driver binds it to) is not known
  yet; the pixel shaders that have it also read bank 1 (see open questions).

To feed a decoder: skip `start` bytes (0x30, or 0x100 for compute), read the 0x50-byte SPH for graphics stages, then
decode `length - 0x50` (graphics) or `length` (compute) bytes of instructions.

## The NVN control section

0x880 bytes, 0x88c when it carries a 12-byte tail. What the compiler puts in it is not public; this is what we can
tie to other data:

| Offset | Value | Status |
|---|---|---|
| +0x000 | magic `0x98761234` | verified |
| +0x004..+0x010 | 1, 0x10, 0x120, 0xb in all 8,496 distinct sections | constant, meaning unknown |
| +0x014, +0x6f0, +0x794, +0x7c8 | 0x878, 0x880, 8, 0x880 in every BNSH stage; all 0 in every agl (BAHS) stage, which also has 0x0022001c instead of 0x00220014 at +0x790 | two flavours (two compiler setups?), meaning unknown |
| +0x020, +0x7e8 | 0x880 everywhere (the fixed size; +0x7e8/+0x7ec look like offset/size of the tail) | inferred |
| +0x6f8 | program length from `start` (SPH + instructions) | verified |
| +0x6fc / +0x700 | constant data size / offset in the code block | verified |
| +0x704 | code block size (= descriptor size in BNSH) | verified |
| +0x708 | program start in the code block: 0x30 graphics, 0x100 compute | verified |
| +0x710 | scratch per warp (= descriptor +0x2c) | inferred |
| +0x748..+0x750 | compute work-group size (32, 1, 1 in the effects) | verified against reflection (the 108 compute stages of `static.esetb`) |
| +0x7d0..+0x7e7 | 24 bytes that look like a hash | inferred |
| +0x7f9 (byte) | number of samplers | verified |
| +0x804.. | one byte per sampler: its binding slot, in reflection order | verified (all 38,833 BNSH stages that have reflection) |

The sampler table matters for agl's BAHS, which has no BNSH reflection: the control section alone gives the
texture bindings there.

## FSHA (G3d2 shader archive, `.bfsha`)

After the file header: +0x20 ptr archive, +0x28 ptr `_STR`, +0x30 string pool size. The archive (at 0x38): name,
path, ptr shading-model array, ptr model dictionary, ..., u16 model count at +0x40.

A shading model is 0x100 bytes. The dictionaries were checked by their contents on all 5 models in the game
(option names, attribute names, sampler names, uniform block names); the rest is marked.

| Offset | Field | Status |
|---|---|---|
| +0x00 | name | verified |
| +0x08/+0x10 | static options: array, dictionary | verified |
| +0x18/+0x20 | dynamic options | verified |
| +0x28/+0x30 | vertex attributes | verified |
| +0x38/+0x40 | samplers | verified |
| +0x48/+0x50 | images (null in all 5) | inferred |
| +0x58/+0x60 | uniform blocks | verified |
| +0x68 | uniform variables | inferred |
| +0x70/+0x78 | storage buffers (`gsys_user2`, `gsys_analyze_buffer`) | inferred from the names |
| +0x88 | program array (0x40 each) | verified (the relocation table describes 8,300 structures of 8 pointers there) |
| +0x90 | key table: (static words + dynamic words) u32 per program | verified stride (14 + 1 words) |
| +0x98 | parent archive | verified |
| +0xa8 | ptr to this model's BNSH | verified |
| +0xd8 | u64, 0x116 / 0x112 (uniform variable count?) | inferred |
| +0xe0 | s32 -1, u16 static option count, u16 dynamic option count, u16 program count, u8 static key words, u8 dynamic key words, then u8 counts: attributes, samplers, images, uniform blocks, ?, ?, ?, storage buffers, ... | counts verified against the dictionaries |

A program (0x40): +0x00, +0x08, +0x10, +0x18 pointers to per-program tables (inferred: where each sampler, image,
uniform block and storage buffer of the model sits in each stage, -1 when unused), **+0x20 pointer to the BNSH
variation** (relocated in the table's BNSH section: verified), +0x28 parent model, then u32 flags and u16 counts.
Every program points at a distinct variation (8,300 of 8,300 and 280 of 280 in the uber file), not in order.

The uber archive: `SecredUber` (8,300 programs; 185 static and 4 dynamic options; 8 attributes `_p0 _n0 _t0 _w0 _i0
_u0 _u1 _c0`; 30 samplers; 11 uniform blocks from `gsys_context` to `gsys_shape_ex`; 2 storage buffers) and
`SecredSprite` (280 programs). The small copy in `Secred.sarc` has `SecredUber` (110), `SceneMaterial` (1) and
`SecredSprite` (110). Option names are the engine's (gsys) material switches (`gsys_alpha_test_enable`,
`gsys_deferred_shading_pass`, ...); the key table encodes each program's option values, which is how the engine picks
a program at run time. N5 does not need it (it translates all variations), N6 might.

## VFXB (Vfx2 effect binary, inside `.esetb.byml`)

The `.esetb.byml` is a BYML whose keys include `Esets` and `PtclBin`; the tool does not parse BYML and finds the
`"VFXB    "` magic instead (one per file). VFXB header: magic, version bytes, BOM, alignment, u16 first section
offset (0x40) at +0x16, u32 file size at +0x1c, name at +0x20. Sections have a 0x20-byte header: magic, u32 size,
u32 child offset, u32 next offset (both relative to the section, 0xffffffff = none), u32 attribute offset, u32 data
offset, u32 0, u16 child count. The last child's "next" is not always "none" (GTNT, G3NT point at non-sections),
so a chain ends at the first entry whose magic is not four letters or digits.

Top level: ESTA (emitter sets → ESET → EMTR), GRTF (textures), PRMA, TRMA, G3PR (primitives), and **GRSN** whose
children are **GRSR** (graphics shaders) and **GRSC** (compute shaders), each holding a full BNSH at its data offset
(verified in all 33 files). `static.Nin_NX_NVN.esetb.byml`: 2,241 ESET, 6,902 EMTR, GRSR BNSH with 3,411 variations,
GRSC BNSH with 108.

## BAHS (agl shader archive binary, `.sharcb`, version 9)

agl (sead's graphics layer) does not use BNSH. No public description was found; everything here is from our 9 files
and is verified by walking every record and program of all 9 (record sizes add up to the area size exactly, every
record's code starts with the NVN header, every stage passes the layout check). The companion `GameAglShader.sharc`
(`AAHS`, version 13) holds the metadata/sources and was not needed.

```
+0x00 "BAHS", u32 version 9, u32 file size, u32 5, u32 0,
      u32 records offset (0x1000), u32 code area size, u32 code area offset
+0x150..       name pool (uniform, attribute and sampler names; seen in GameAglShader.sharcb)
records offset + 0x150 (all 9 files): u32 area size, u32 record count, records:
      u32 size, u32 stage (0 vertex, 1 pixel, 3 compute), u32 0, u32 size - 0x10,
      u32 code offset (from the code area), u32 0, u32 control size (0x880, 0x88c for 40 stages),
      s32 control offset from record + 0x10 (negative: shared with an earlier record),
      resource lists (offset/count pairs to name-slot tables after the control section; not decoded)
program table right after the records: u32 size, u32 count, programs of
      u32 size, u32 name length, u32 stage mask (bit n = record stage n: 3 = vertex+pixel, 8 = compute),
      u32 first record, name, then 3 lists (u32 size, u32 count, entries); entries of the first list are
      the variation macros: u32 size, u32 name length, u32 value count, u32 ?, name, values as strings
code area: code blocks, same layout as in BNSH
```

The code block size is not in the record; it is read from control +0x704 (verified equal to the descriptor's size
on all BNSH stages; in all 9 BAHS the distinct blocks, sized this way, lie back to back from the start of the code
area and end inside it). A program has product(value counts of its macros) variations, one record
per stage in its mask, in order: for `GameAglShader.sharcb`, `image_filter_blur` has BLUR_KERNEL x6 · BLUR_X x2 ·
BLUR_MODIFY x2 = 24 variations = records 0..47, and the 15 programs cover the 318 records exactly. This arithmetic
holding for every program of every file is what makes the reading credible; the stage bit meaning for
geometry (bit 2) is a guess, no program uses it.

## SARC

u32 `"SARC"`, u16 header size 0x14, u16 BOM, u32 file size, u32 data offset, u16 version 0x100; `SFAT` (u16 node
count at +0x1a, hash key; nodes of u32 name hash, u32 attributes (bit 24 set: low 16 bits = name offset / 4), u32
start, u32 end relative to the data offset); `SFNT` names right after the nodes. Layout `.blarc` files are SARCs:
FLYT/FLAN layouts, BNTX textures, `bgsh/__ArchiveShader.bnsh` with a `.bushvt` variation table, and in 32 of them
`.glsl` **sources** of the Ui2d combiner shaders (66 files, 9 distinct names such as
`CombinerUserShaderVariation.glsl`). Those sources are a ready-made reference for checking the translator's output
on the Ui2d shaders.

## Open questions for N5

1. **Constant bank mapping.** Reflection gives uniform-block bindings (e.g. `NnVfx2ViewParam@5`). A rough count of
   constant-bank operands in the instructions of a few effect shaders (not a real decoder) suggests binding *n* is
   read as `c[n + 3]` in graphics stages (bindings 5, 6, 7, 11 → banks 8, 9, 10, 14), and that compute shaders only
   touch banks 0 and 1 (their uniform and storage buffers presumably go through addresses loaded from bank 0).
   To confirm with the decoder; then find what banks 0..2 hold (driver constants, texture handles, the constant
   data at control +0x700?).
2. **Textures**: how a sampler binding becomes a texture handle in the code (a handle read from a constant bank, as
   the emulators describe for Maxwell?), and where image (storage texture) bindings go. No image appears in
   any reflection seen so far.
3. **Rest of the control section**: the constant words at +0x004..+0x010, the BNSH vs agl flavours (+0x014,
   +0x790, ...), the 24-byte hash, the 12-byte tail of 0x88c sections, the per-stage words between +0x70c and
   +0x7f8 (varying with the stage: vertex attributes? fragment outputs?). A diff of the 8,496 distinct sections
   against reflection should settle most of it.
4. **BAHS resource lists** (after each control section): offset/count pairs that probably give uniform/sampler
   names and slots, the equivalent of BNSH reflection for agl. Needed to bind agl's post effects (SSAO, HDR compose,
   blur).
5. **FSHA program tables** (+0x00..+0x18 of each program): which model-level sampler/uniform block lives at which
   stage binding; only needed if N6 wants names instead of the per-stage BNSH reflection.
6. **Scratch memory** (descriptor +0x28/+0x2c): meaning of the base 0x600 per warp seen in shaders with no local
   memory in the SPH.
7. **Stage coverage**: vertex, pixel and compute only. Tessellation and geometry code paths can wait.
