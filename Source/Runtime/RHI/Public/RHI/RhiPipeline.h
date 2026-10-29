#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EnumFlags.h"
#include "RHI/RhiFormat.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiResources.h"
#include "String/StringView.h"

namespace Marlin
{
/// Every pipeline shares one layout: these shader arguments (Vulkan push data, D3D12 root constants) plus the
/// resource and sampler heaps.
inline constexpr uint32 RhiShaderArgumentsSize = 128;

/// Specialization constant ids that hand every shader stage the device's descriptor heap strides
/// (RhiDeviceLimits::*DescriptorStride, MarlinLanguage §11.3). Shader specialization constants use ids below them.
inline constexpr uint32 RhiResourceStrideSpecializationId = 0x7FFF0000u;
inline constexpr uint32 RhiSamplerStrideSpecializationId = 0x7FFF0001u;

enum class RhiShaderStage : uint16
{
    None = 0,
    Vertex = 1 << 0,
    Pixel = 1 << 1,
    Task = 1 << 2,
    Mesh = 1 << 3,
    Compute = 1 << 4,
    RayGeneration = 1 << 5,
    Miss = 1 << 6,
    ClosestHit = 1 << 7,
    AnyHit = 1 << 8,
    Intersection = 1 << 9,
    Callable = 1 << 10,
};
MARLIN_ENUM_FLAGS(RhiShaderStage)

/// Compiled code for the device's backend (SPIR-V for Vulkan), as the shader compiler produced it.
struct RhiShader
{
    Span<const uint8> Code;
    StringView EntryPoint = "main";
    RhiShaderStage Stage = RhiShaderStage::None;
    /// Specialization constants by id, each a 32-bit value.
    Span<const uint32> SpecializationConstants;
};

enum class RhiPrimitiveTopology : uint8
{
    TriangleList,
    TriangleStrip,
    LineList,
    LineStrip,
    PointList,
};

enum class RhiCullMode : uint8
{
    None,
    Front,
    Back,
};

enum class RhiFillMode : uint8
{
    Solid,
    Wireframe,
};

struct RhiRasterState
{
    RhiFillMode Fill = RhiFillMode::Solid;
    RhiCullMode Cull = RhiCullMode::Back;
    bool FrontCounterClockwise = false;
    bool DepthClip = true;
    /// Rasterizes every pixel a primitive touches, for voxelization and visibility.
    bool Conservative = false;
    float32 DepthBias = 0;
    float32 DepthBiasClamp = 0;
    float32 DepthBiasSlope = 0;
};

enum class RhiStencilOp : uint8
{
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap,
};

struct RhiStencilFace
{
    RhiStencilOp Fail = RhiStencilOp::Keep;
    RhiStencilOp DepthFail = RhiStencilOp::Keep;
    RhiStencilOp Pass = RhiStencilOp::Keep;
    RhiCompareOp Compare = RhiCompareOp::Always;
};

/// Depth compares with reversed Z in mind: Greater is the usual test.
struct RhiDepthStencilState
{
    bool DepthTest = false;
    bool DepthWrite = false;
    RhiCompareOp DepthCompare = RhiCompareOp::GreaterEqual;
    bool StencilTest = false;
    uint8 StencilReadMask = 0xFF;
    uint8 StencilWriteMask = 0xFF;
    RhiStencilFace Front;
    RhiStencilFace Back;
    /// Rejects fragments outside the bounds set with RhiCommandList::SetDepthBounds.
    bool DepthBoundsTest = false;
};

enum class RhiBlendFactor : uint8
{
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestColor,
    InverseDestColor,
    DestAlpha,
    InverseDestAlpha,
    SourceAlphaSaturate,
    Constant,
    InverseConstant,
    Source1Color,
    InverseSource1Color,
    Source1Alpha,
    InverseSource1Alpha,
};

enum class RhiBlendOp : uint8
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum,
};

enum class RhiColorWriteMask : uint8
{
    None = 0,
    Red = 1 << 0,
    Green = 1 << 1,
    Blue = 1 << 2,
    Alpha = 1 << 3,
    All = Red | Green | Blue | Alpha, // NOLINT(bugprone-signed-bitwise)
};
MARLIN_ENUM_FLAGS(RhiColorWriteMask)

struct RhiBlendTarget
{
    bool Enabled = false;
    RhiBlendFactor SourceColor = RhiBlendFactor::One;
    RhiBlendFactor DestColor = RhiBlendFactor::Zero;
    RhiBlendOp ColorOp = RhiBlendOp::Add;
    RhiBlendFactor SourceAlpha = RhiBlendFactor::One;
    RhiBlendFactor DestAlpha = RhiBlendFactor::Zero;
    RhiBlendOp AlphaOp = RhiBlendOp::Add;
    RhiColorWriteMask WriteMask = RhiColorWriteMask::All;
};

inline constexpr uint32 RhiMaxRenderTargets = 8;

/// A graphics pipeline: Vertex, or Task (optional) and Mesh, then Pixel (optional). There is no vertex input:
/// vertices are fetched from the bindless heap.
struct RhiGraphicsPipelineDesc
{
    Span<const RhiShader> Shaders;
    RhiPrimitiveTopology Topology = RhiPrimitiveTopology::TriangleList;
    RhiRasterState Raster;
    RhiDepthStencilState DepthStencil;
    RhiBlendTarget Blend[RhiMaxRenderTargets];
    bool AlphaToCoverage = false;
    Span<const RhiFormat> RenderTargetFormats;
    RhiFormat DepthStencilFormat = RhiFormat::Unknown;
    uint32 SampleCount = 1;
    /// Multiview: bit i renders view i; zero for none.
    uint32 ViewMask = 0;
    /// It may go into an RhiIndirectPipelineSet. Vulkan's GPU-assisted validation cannot instrument such pipelines.
    bool IndirectBindable = false;
    StringView DebugName;
};

struct RhiComputePipelineDesc
{
    RhiShader Shader;
    /// It may go into an RhiIndirectPipelineSet. Vulkan's GPU-assisted validation cannot instrument such pipelines.
    bool IndirectBindable = false;
    StringView DebugName;
};

enum class RhiHitGroupKind : uint8
{
    Triangles,
    Procedural,
};

/// Shaders are named by their index in RhiRayTracingPipelineDesc::Shaders; ~0u means none.
struct RhiHitGroup
{
    RhiHitGroupKind Kind = RhiHitGroupKind::Triangles;
    uint32 ClosestHit = ~0u;
    uint32 AnyHit = ~0u;
    uint32 Intersection = ~0u;
};

/// Shader groups are numbered in this order: ray generation, miss and callable shaders by their index in
/// Shaders, then the hit groups; RhiDevice::GetShaderGroupHandles reads their handles for the binding tables.
struct RhiRayTracingPipelineDesc
{
    Span<const RhiShader> Shaders;
    Span<const RhiHitGroup> HitGroups;
    uint32 MaxPayloadBytes = 32;
    uint32 MaxAttributeBytes = 8;
    uint32 MaxRecursionDepth = 1;
    StringView DebugName;
};

enum class RhiPipelineKind : uint8
{
    Graphics,
    Compute,
    RayTracing,
};

enum class RhiPipelineState : uint8
{
    /// Still compiling on a job; commands that use it are skipped.
    Pending,
    Ready,
    Failed,
};

/// One command of an indirect command sequence; the arguments buffer holds each command's data in this order.
enum class RhiIndirectCommandKind : uint8
{
    /// Switches the pipeline to one of the RhiIndirectPipelineSet; a uint32 index. Needs the
    /// IndirectPipelineSwitch feature.
    SetPipeline,
    /// Size bytes at Offset within the shader arguments.
    ShaderArguments,
    /// A 32-bit index buffer: address, size and format.
    IndexBuffer,
    Draw,
    DrawIndexed,
    DrawMesh,
    Dispatch,
    DispatchRays,
};

struct RhiIndirectCommand
{
    RhiIndirectCommandKind Kind = RhiIndirectCommandKind::Draw;
    uint32 Offset = 0;
    uint32 Size = 0;
};

/// The layout of an indirect command sequence for RhiCommandList::ExecuteIndirect; the last command draws,
/// dispatches or traces.
struct RhiCommandSignatureDesc
{
    Span<const RhiIndirectCommand> Commands;
    /// Bytes from one sequence to the next; zero for tightly packed.
    uint32 Stride = 0;
    /// The kind of pipelines its sequences run, and the set they switch between if they do.
    RhiPipelineKind PipelineKind = RhiPipelineKind::Graphics;
    RhiIndirectPipelineSet PipelineSet;
    StringView DebugName;
};

/// Pipelines a SetPipeline indirect command picks from; they must be of one kind, IndirectBindable and not pending.
struct RhiIndirectPipelineSetDesc
{
    Span<const RhiPipeline> Pipelines;
    StringView DebugName;
};

struct RhiDrawArguments
{
    uint32 VertexCount = 0;
    uint32 InstanceCount = 1;
    uint32 FirstVertex = 0;
    uint32 FirstInstance = 0;
};

struct RhiDrawIndexedArguments
{
    uint32 IndexCount = 0;
    uint32 InstanceCount = 1;
    uint32 FirstIndex = 0;
    int32 VertexOffset = 0;
    uint32 FirstInstance = 0;
};

struct RhiDispatchArguments
{
    uint32 X = 1;
    uint32 Y = 1;
    uint32 Z = 1;
};

/// The arguments of an indirect DispatchRays as the GPU reads them.
struct RhiDispatchRaysArguments
{
    uint64 RayGenerationAddress = 0;
    uint64 RayGenerationSize = 0;
    uint64 MissAddress = 0;
    uint64 MissSize = 0;
    uint64 MissStride = 0;
    uint64 HitGroupAddress = 0;
    uint64 HitGroupSize = 0;
    uint64 HitGroupStride = 0;
    uint64 CallableAddress = 0;
    uint64 CallableSize = 0;
    uint64 CallableStride = 0;
    uint32 Width = 1;
    uint32 Height = 1;
    uint32 Depth = 1;
};
} // namespace Marlin
