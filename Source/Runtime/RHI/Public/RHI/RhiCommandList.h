#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "RHI/RhiBarriers.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiPipeline.h"
#include "RHI/RhiRayTracing.h"
#include "RHI/RhiResources.h"
#include "String/Name.h"

namespace Marlin
{
enum class RhiLoadOp : uint8
{
    Load,
    Clear,
    /// The previous contents are not needed.
    Discard,
};

enum class RhiStoreOp : uint8
{
    Store,
    /// The contents are not needed after the pass; free on tile-based GPUs.
    Discard,
    /// Resolves the multisampled target into ResolveTarget, then discards it.
    Resolve,
};

/// A render target: one mip and a range of slices of a texture, viewed internally by the backend.
struct RhiRenderTarget
{
    RhiTexture Texture;
    uint32 Mip = 0;
    uint32 FirstSlice = 0;
    uint32 SliceCount = 1;
    /// Unknown uses the texture's format.
    RhiFormat Format = RhiFormat::Unknown;
    RhiLoadOp Load = RhiLoadOp::Load;
    RhiStoreOp Store = RhiStoreOp::Store;
    RhiClearValue Clear;
    RhiTexture ResolveTarget;
    uint32 ResolveMip = 0;
    uint32 ResolveSlice = 0;
};

struct RhiDepthTarget
{
    RhiTexture Texture;
    uint32 Mip = 0;
    uint32 FirstSlice = 0;
    uint32 SliceCount = 1;
    RhiLoadOp DepthLoad = RhiLoadOp::Load;
    RhiStoreOp DepthStore = RhiStoreOp::Store;
    RhiLoadOp StencilLoad = RhiLoadOp::Load;
    RhiStoreOp StencilStore = RhiStoreOp::Store;
    RhiClearValue Clear;
    /// Bound for depth testing only; the texture is in DepthStencilRead.
    bool ReadOnly = false;
};

/// Dynamic rendering: targets and their load and store ops, with no render pass objects.
struct RhiRenderingDesc
{
    Span<const RhiRenderTarget> RenderTargets;
    RhiDepthTarget Depth;
    /// A ShadingRate texture in the ShadingRateSource layout; its texels cover ShadingRateTileSize pixels.
    RhiTexture ShadingRate;
    uint32 Width = 0;
    uint32 Height = 0;
    /// Multiview: bit i renders view i into slice i; zero for none.
    uint32 ViewMask = 0;
};

struct RhiViewport
{
    float32 X = 0;
    float32 Y = 0;
    float32 Width = 0;
    float32 Height = 0;
    float32 MinDepth = 0;
    float32 MaxDepth = 1;
};

struct RhiScissor
{
    int32 X = 0;
    int32 Y = 0;
    uint32 Width = 0;
    uint32 Height = 0;
};

enum class RhiShadingRate : uint8
{
    Rate1x1,
    Rate1x2,
    Rate2x1,
    Rate2x2,
    Rate2x4,
    Rate4x2,
    Rate4x4,
};

enum class RhiShadingRateCombiner : uint8
{
    /// Keeps the earlier rate.
    Passthrough,
    /// Takes the later rate.
    Override,
    Minimum,
    Maximum,
    Sum,
};

enum class RhiQueryKind : uint8
{
    /// The time at which earlier work finished, in ticks of RhiDevice::GetTimestampFrequency.
    Timestamp,
    /// Nine uint64 counters per query, in the order of RhiPipelineStatistics.
    PipelineStatistics,
    Occlusion,
};

struct RhiPipelineStatistics
{
    uint64 InputVertices = 0;
    uint64 InputPrimitives = 0;
    uint64 VertexShaderInvocations = 0;
    uint64 ClippingInvocations = 0;
    uint64 ClippingPrimitives = 0;
    uint64 PixelShaderInvocations = 0;
    uint64 ComputeShaderInvocations = 0;
    uint64 TaskShaderInvocations = 0;
    uint64 MeshShaderInvocations = 0;
};
static_assert(sizeof(RhiPipelineStatistics) == 9 * sizeof(uint64));

struct RhiQueryPoolDesc
{
    RhiQueryKind Kind = RhiQueryKind::Timestamp;
    uint32 Count = 0;
    StringView DebugName;
};

/// Records commands for one queue type. A command list is used by one thread at a time; any number may be
/// recorded in parallel. It belongs to the device from Submit on and must not be touched again.
class RHI_API RhiCommandList
{
public:
    RhiCommandList() = default;
    virtual ~RhiCommandList() = default;
    MARLIN_NONCOPYABLE(RhiCommandList);

    [[nodiscard]] virtual RhiQueueType GetQueueType() const = 0;

    virtual void Barrier(const RhiBarriers &barriers) = 0;

    /// Hands the native command buffer (a VkCommandBuffer, an ID3D12GraphicsCommandList) to an SDK that records
    /// into it, such as an upscaler, outside BeginRendering. Until EndNativeCommands the list takes no commands;
    /// afterwards the bindless heaps are bound again, and the pipeline and shader arguments must be set again.
    [[nodiscard]] virtual void *BeginNativeCommands() = 0;
    virtual void EndNativeCommands() = 0;

    /// Opens a named region for profilers and capture tools; the name also goes into breadcrumbs.
    virtual void BeginMarker(Name name) = 0;
    virtual void EndMarker() = 0;

    /// Starts at the 1x1 shading rate, or the shading rate attachment's if there is one (SetShadingRate).
    virtual void BeginRendering(const RhiRenderingDesc &desc) = 0;
    virtual void EndRendering() = 0;

    virtual void SetPipeline(RhiPipeline pipeline) = 0;
    /// Writes into the shader arguments (the [Uniform] parameter block, MarlinLanguage §9.3), which keep their values across pipelines until rewritten.
    virtual void SetShaderArguments(Span<const uint8> data, uint32 offset = 0) = 0;

    template <typename T> void SetShaderArguments(const T &value, uint32 offset = 0)
    {
        static_assert(sizeof(T) <= RhiShaderArgumentsSize);
        SetShaderArguments(Span<const uint8>(reinterpret_cast<const uint8 *>(&value), sizeof(T)), offset);
    }

    virtual void SetViewports(Span<const RhiViewport> viewports) = 0;
    virtual void SetScissors(Span<const RhiScissor> scissors) = 0;
    virtual void SetStencilReference(uint8 reference) = 0;
    virtual void SetBlendConstants(const float32 (&constants)[4]) = 0;
    virtual void SetDepthBounds(float32 minimum, float32 maximum) = 0;
    /// The rate combines first with the primitive's rate, then with the attachment's.
    virtual void SetShadingRate(RhiShadingRate rate, RhiShadingRateCombiner primitive,
                                RhiShadingRateCombiner attachment) = 0;
    /// 16- or 32-bit indices from *buffer*.
    virtual void SetIndexBuffer(RhiBuffer buffer, uint64 offset, uint8 indexBits) = 0;

    virtual void Draw(const RhiDrawArguments &arguments) = 0;
    virtual void DrawIndexed(const RhiDrawIndexedArguments &arguments) = 0;
    /// Task shader groups, or mesh shader groups when the pipeline has no task shader.
    virtual void DrawMesh(uint32 x, uint32 y, uint32 z) = 0;
    virtual void Dispatch(uint32 x, uint32 y, uint32 z) = 0;
    virtual void DispatchRays(const RhiDispatchRaysDesc &desc) = 0;

    /// Up to *maxCount* commands from *arguments*, or the uint32 at *countBuffer* if fewer. The kind is that
    /// of the last command of *signature*; for a signature of one plain command, use the RhiCommandSignature
    /// the device made for that command.
    virtual void ExecuteIndirect(RhiCommandSignature signature, RhiBuffer arguments, uint64 argumentsOffset,
                                 uint32 maxCount, RhiBuffer countBuffer = {}, uint64 countOffset = 0) = 0;

    virtual void CopyBuffer(RhiBuffer destination, uint64 destinationOffset, RhiBuffer source, uint64 sourceOffset,
                            uint64 size) = 0;
    virtual void CopyTexture(RhiTexture destination, const RhiTextureRegion &destinationRegion, RhiTexture source,
                             const RhiTextureRegion &sourceRegion) = 0;
    virtual void CopyBufferToTexture(RhiTexture destination, const RhiTextureRegion &region, RhiBuffer source,
                                     const RhiBufferTextureLayout &layout) = 0;
    virtual void CopyTextureToBuffer(RhiBuffer destination, const RhiBufferTextureLayout &layout, RhiTexture source,
                                     const RhiTextureRegion &region) = 0;
    /// Fills with a repeated uint32; offset and size are multiples of four.
    virtual void FillBuffer(RhiBuffer buffer, uint64 offset, uint64 size, uint32 value) = 0;
    /// Clears a texture in the CopyDest layout.
    virtual void ClearTexture(RhiTexture texture, const RhiSubresourceRange &range, const RhiClearValue &value) = 0;
    virtual void ResolveTexture(RhiTexture destination, uint32 destinationMip, uint32 destinationSlice,
                                RhiTexture source, uint32 sourceMip, uint32 sourceSlice) = 0;

    virtual void BuildAccelerationStructures(Span<const RhiAccelerationStructureBuild> builds) = 0;
    virtual void CopyAccelerationStructure(RhiAccelerationStructure destination, RhiAccelerationStructure source,
                                           RhiAccelerationStructureCopyMode mode) = 0;
    /// For Serialize and Deserialize, from or into a buffer address instead of a structure.
    virtual void CopyAccelerationStructureMemory(uint64 destinationAddress, RhiAccelerationStructure structure,
                                                 RhiAccelerationStructureCopyMode mode, uint64 sourceAddress) = 0;
    /// Writes a uint64 per structure at *destination*, after their builds.
    virtual void WriteAccelerationStructureProperties(Span<const RhiAccelerationStructure> structures,
                                                      RhiAccelerationStructureProperty property, RhiBuffer destination,
                                                      uint64 offset) = 0;

    virtual void WriteTimestamp(RhiQueryPool pool, uint32 index) = 0;
    virtual void BeginQuery(RhiQueryPool pool, uint32 index) = 0;
    virtual void EndQuery(RhiQueryPool pool, uint32 index) = 0;
    /// Writes the results as uint64 values, nine per pipeline statistics query; then resets those queries.
    virtual void ResolveQueries(RhiQueryPool pool, uint32 first, uint32 count, RhiBuffer destination,
                                uint64 offset) = 0;
};
} // namespace Marlin
