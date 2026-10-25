// What Ryujinx's shader translator asks of "the GPU", answered from the shader file alone.
//
// In the emulator these queries read live Maxwell state (bound constant buffers, the texture pool, the 3D engine
// registers). Offline we only have the code block and its NVN control section, so every answer is either read from
// those or a fixed default. The defaults that stand in for draw-time state are the ones our runtime will have to key
// a variant cache on; docs/nvn/TRANSLATOR.md lists them.

using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen;
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace SpirvHarness
{
    sealed class StaticGpuAccessor : IGpuAccessor
    {
        // Vulkan descriptor layout, identical to Ryujinx's Vulkan backend (PipelineBase / Constants.cs there):
        // one set per resource kind, and a fixed binding range per stage inside each set.
        public const int UniformSet = 0, StorageSet = 1, TextureSet = 2, ImageSet = 3;
        public const int MaxUniformBuffersPerStage = 18;   // c[0]..c[17]
        public const int MaxStorageBuffersPerStage = 16;
        public const int MaxTexturesPerStage = 64;
        public const int MaxImagesPerStage = 16;
        const int ShaderStages = 5;                         // vertex, tess ctrl, tess eval, geometry, fragment
        const int ReservedUniformBuffers = 1;               // binding 0 of each stage range: the translator's support buffer

        readonly byte[] _code;        // the whole code block (0x30-byte NVN header, SPH, instructions, constant data)
        readonly int _constOff, _constSize;
        readonly int _stageSlot;      // 0 vertex/compute, 1 fragment (Ryujinx's GetStageIndex remap)
        readonly int _localX, _localY, _localZ, _sharedSize;
        int _staticTextures, _staticImages, _arrayTextures, _arrayImages;

        public readonly List<string> Messages = new();
        // Draw-time queries the translator made for this shader (what a runtime variant would have to key on).
        public readonly SortedSet<string> DrawTimeQueries = new();

        public StaticGpuAccessor(byte[] code, int constOff, int constSize, ShaderStage stage, byte[] ctrl)
        {
            // Pad so that the decoder may read past the program end (it asks for minimum sizes) without bounds errors.
            _code = new byte[(code.Length + 0x1000 + 7) & ~7];
            code.CopyTo(_code, 0);
            _constOff = constOff;
            _constSize = constSize;
            _stageSlot = stage == ShaderStage.Fragment ? 1 : 0;
            // Compute work-group size: control section +0x748..+0x750 (verified against BNSH reflection, SHADERS.md).
            _localX = _localY = _localZ = 1;
            if (ctrl != null && ctrl.Length >= 0x754)
            {
                _localX = Math.Max(1, BitConverter.ToInt32(ctrl, 0x748));
                _localY = Math.Max(1, BitConverter.ToInt32(ctrl, 0x74c));
                _localZ = Math.Max(1, BitConverter.ToInt32(ctrl, 0x750));
            }
            // Shared memory bytes: control +0x754 (0 exactly for the compute shaders without shared memory). Without
            // it the translator declares its default, 0xc000, past the 32 KB Metal allows (docs/nvn/DEBUG.md #23).
            if (ctrl != null && ctrl.Length >= 0x758) _sharedSize = BitConverter.ToInt32(ctrl, 0x754);
        }

        public void Log(string message)
        {
            lock (Messages) Messages.Add(message);
        }

        // "GPU addresses" are offsets in the code block: the harness passes the program start (0x30 or 0x100).
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize)
        {
            return MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code, (int)address, _code.Length - (int)address));
        }

        // Hypothesis tested by this harness: the constant data the compiler appended to the block (control +0x700)
        // is what NVN binds as c[1]. The translator reads c[1] to follow indirect branch (BRX) jump tables.
        public uint ConstantBuffer1Read(int offset)
        {
            return offset >= 0 && offset + 4 <= _constSize ? BitConverter.ToUInt32(_code, _constOff + offset) : 0;
        }

        public SetBindingPair CreateConstantBufferBinding(int index)
        {
            return new(UniformSet, _stageSlot * MaxUniformBuffersPerStage + index + ReservedUniformBuffers);
        }

        public SetBindingPair CreateStorageBufferBinding(int index)
        {
            return new(StorageSet, _stageSlot * MaxStorageBuffersPerStage + index);
        }

        // Single textures: per stage, 2 x max slots (plain textures first, buffer textures in the upper half).
        // Arrays (bindless turned into an array of textures): after every stage's range, counted from there.
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer)
        {
            int binding = count == 1
                ? _stageSlot * MaxTexturesPerStage * 2 + (_staticTextures++) + (isBuffer ? MaxTexturesPerStage : 0)
                : MaxTexturesPerStage * ShaderStages * 2 + _arrayTextures++;
            return new(TextureSet, binding);
        }

        public SetBindingPair CreateImageBinding(int count, bool isBuffer)
        {
            int binding = count == 1
                ? _stageSlot * MaxImagesPerStage * 2 + (_staticImages++) + (isBuffer ? MaxImagesPerStage : 0)
                : MaxImagesPerStage * ShaderStages * 2 + _arrayImages++;
            return new(ImageSet, binding);
        }

        // Per-texture state that lives in the texture/sampler descriptors at draw time. Defaults: 2D, normalized
        // coordinates, RGBA8 (the last one only matters for typed image access).
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            DrawTimeQueries.Add("samplerType");
            return SamplerType.Texture2D;
        }

        public bool QueryTextureCoordNormalized(int handle, int cbufSlot = -1)
        {
            DrawTimeQueries.Add("coordNormalized");
            return true;
        }

        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            DrawTimeQueries.Add("textureFormat");
            return TextureFormat.R8G8B8A8Unorm;
        }

        public bool QueryHasUnalignedStorageBuffer()
        {
            DrawTimeQueries.Add("unalignedStorage");
            return false;
        }

        public bool QueryHasConstantBufferDrawParameters()
        {
            DrawTimeQueries.Add("drawParameters");
            return false;
        }

        public int QueryComputeLocalSizeX() => _localX;
        public int QueryComputeLocalSizeY() => _localY;
        public int QueryComputeLocalSizeZ() => _localZ;
        public int QueryComputeSharedMemorySize() => _sharedSize;  // 0: the translator uses 4 KB (never happens here)

        // Sizes of bindless arrays: pool sizes are draw-time state; a fixed bound makes the SPIR-V valid.
        public int QuerySamplerArrayLengthFromPool() { DrawTimeQueries.Add("poolSize"); return 4096; }
        public int QueryTextureArrayLengthFromPool() { DrawTimeQueries.Add("poolSize"); return 4096; }
        public int QueryTextureArrayLengthFromBuffer(int slot) { DrawTimeQueries.Add("cbufSize"); return 32; }

        // Default graphics state: triangles, no alpha test (emulated in the shader on Vulkan), every vertex attribute
        // and render target float, origin upper left (what Ryujinx passes on Vulkan). All of it is draw-time state.
        public GpuGraphicsState QueryGraphicsState()
        {
            return new GpuGraphicsState(
                earlyZForce: false,
                topology: InputTopology.Triangles,
                tessCw: false,
                tessPatchType: TessPatchType.Triangles,
                tessSpacing: TessSpacing.EqualSpacing,
                alphaToCoverageEnable: false,
                alphaToCoverageDitherEnable: false,
                viewportTransformDisable: false,
                depthMode: false,
                programPointSizeEnable: false,
                pointSize: 1f,
                alphaTestCompare: AlphaTestOp.Always,
                alphaTestReference: 0f,
                attributeTypes: default,
                hasConstantBufferDrawParameters: false,
                fragmentOutputTypes: default,
                dualSourceBlendEnable: false,
                yNegateEnabled: false,
                originUpperLeft: true,
                halvePrimitiveId: false);
        }

        // Host = MoltenVK on the Apple M1 (docs/nvn/WONDER.md), with the choices Ryujinx's Vulkan backend makes.
        public bool QueryHostReducedPrecision() => true;                 // Ryujinx does this on MoltenVK
        public bool QueryHostSupportsGeometryShader() => false;
        public bool QueryHostSupportsGeometryShaderPassthrough() => false;
        public bool QueryHostSupportsTransformFeedback() => false;
        public bool QueryHostSupportsShaderFloat64() => false;
        public bool QueryHostSupportsShaderBallot() => false;
        public bool QueryHostSupportsViewportMask() => false;
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        public bool QueryHostSupportsTextureShadowLod() => false;
        public bool QueryHostSupportsFragmentShaderInterlock() => true;
        public bool QueryHostSupportsShaderNonUniformIndexing() => true;
        public int QueryHostSubgroupSize() => 32;
    }
}
