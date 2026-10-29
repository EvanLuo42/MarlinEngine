#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "RHI/RhiCommandList.h"
#include "Templates/Optional.h"

namespace Marlin
{
class ValidationDevice;

/// Checks the use of one command list: one thread at a time, nothing after Submit, commands that belong to its
/// queue type and to the inside or outside of a render pass, live handles and balanced markers.
class ValidationCommandList final : public RhiCommandList
{
public:
    ValidationCommandList(ValidationDevice &device, RhiCommandList &inner);
    ~ValidationCommandList() override = default;
    MARLIN_NONCOPYABLE(ValidationCommandList);

    [[nodiscard]] RhiCommandList &GetInner() const
    {
        return _inner;
    }

    /// Checks that the list can be submitted, and marks it as submitted.
    bool PrepareSubmit(const char *call);

    [[nodiscard]] RhiQueueType GetQueueType() const override;
    void Barrier(const RhiBarriers &barriers) override;
    [[nodiscard]] void *BeginNativeCommands() override;
    void EndNativeCommands() override;
    void BeginMarker(Name name) override;
    void EndMarker() override;
    void BeginRendering(const RhiRenderingDesc &desc) override;
    void EndRendering() override;
    void SetPipeline(RhiPipeline pipeline) override;
    void SetShaderArguments(Span<const uint8> data, uint32 offset) override;
    void SetViewports(Span<const RhiViewport> viewports) override;
    void SetScissors(Span<const RhiScissor> scissors) override;
    void SetStencilReference(uint8 reference) override;
    void SetBlendConstants(const float32 (&constants)[4]) override;
    void SetDepthBounds(float32 minimum, float32 maximum) override;
    void SetShadingRate(RhiShadingRate rate, RhiShadingRateCombiner primitive,
                        RhiShadingRateCombiner attachment) override;
    void SetIndexBuffer(RhiBuffer buffer, uint64 offset, uint8 indexBits) override;
    void Draw(const RhiDrawArguments &arguments) override;
    void DrawIndexed(const RhiDrawIndexedArguments &arguments) override;
    void DrawMesh(uint32 x, uint32 y, uint32 z) override;
    void Dispatch(uint32 x, uint32 y, uint32 z) override;
    void DispatchRays(const RhiDispatchRaysDesc &desc) override;
    void ExecuteIndirect(RhiCommandSignature signature, RhiBuffer arguments, uint64 argumentsOffset, uint32 maxCount,
                         RhiBuffer countBuffer, uint64 countOffset) override;
    void CopyBuffer(RhiBuffer destination, uint64 destinationOffset, RhiBuffer source, uint64 sourceOffset,
                    uint64 size) override;
    void CopyTexture(RhiTexture destination, const RhiTextureRegion &destinationRegion, RhiTexture source,
                     const RhiTextureRegion &sourceRegion) override;
    void CopyBufferToTexture(RhiTexture destination, const RhiTextureRegion &region, RhiBuffer source,
                             const RhiBufferTextureLayout &layout) override;
    void CopyTextureToBuffer(RhiBuffer destination, const RhiBufferTextureLayout &layout, RhiTexture source,
                             const RhiTextureRegion &region) override;
    void FillBuffer(RhiBuffer buffer, uint64 offset, uint64 size, uint32 value) override;
    void ClearTexture(RhiTexture texture, const RhiSubresourceRange &range, const RhiClearValue &value) override;
    void ResolveTexture(RhiTexture destination, uint32 destinationMip, uint32 destinationSlice, RhiTexture source,
                        uint32 sourceMip, uint32 sourceSlice) override;
    void BuildAccelerationStructures(Span<const RhiAccelerationStructureBuild> builds) override;
    void CopyAccelerationStructure(RhiAccelerationStructure destination, RhiAccelerationStructure source,
                                   RhiAccelerationStructureCopyMode mode) override;
    void CopyAccelerationStructureMemory(uint64 destinationAddress, RhiAccelerationStructure structure,
                                         RhiAccelerationStructureCopyMode mode, uint64 sourceAddress) override;
    void WriteAccelerationStructureProperties(Span<const RhiAccelerationStructure> structures,
                                              RhiAccelerationStructureProperty property, RhiBuffer destination,
                                              uint64 offset) override;
    void WriteTimestamp(RhiQueryPool pool, uint32 index) override;
    void BeginQuery(RhiQueryPool pool, uint32 index) override;
    void EndQuery(RhiQueryPool pool, uint32 index) override;
    void ResolveQueries(RhiQueryPool pool, uint32 first, uint32 count, RhiBuffer destination, uint64 offset) override;

private:
    /// Marks the list as in use by the calling thread for one call; reports concurrent use and use after Submit.
    class Use
    {
    public:
        Use(ValidationCommandList &list, const char *call);
        ~Use();
        MARLIN_NONCOPYABLE(Use);

    private:
        ValidationCommandList &_list;
    };

    void RequireQueue(bool graphics, bool compute, bool copy, const char *call) const;
    void RequireRendering(bool inside, const char *call) const;
    void RequirePipeline(RhiPipelineKind kind, const char *call) const;
    Optional<RhiBufferDesc> RequireBuffer(RhiBuffer buffer, const char *call) const;
    Optional<RhiTextureDesc> RequireTexture(RhiTexture texture, const char *call) const;
    void CheckBufferRange(const RhiBufferDesc &desc, uint64 offset, uint64 size, const char *call) const;
    void CheckRegion(const RhiTextureDesc &desc, const RhiTextureRegion &region, const char *call) const;

    ValidationDevice &_device;
    RhiCommandList &_inner;
    RhiQueueType _queue;
    Atomic<uint32> _user{0};
    bool _submitted = false;
    bool _rendering = false;
    bool _hasPipeline = false;
    bool _native = false;
    RhiPipelineKind _pipelineKind = RhiPipelineKind::Graphics;
    uint32 _markerDepth = 0;
    uint32 _openQueries = 0;
};
} // namespace Marlin
