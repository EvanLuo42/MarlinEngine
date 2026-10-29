#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiBreadcrumbs.h"
#include "RHI/RhiCommandList.h"
#include "RHIVulkan/VulkanApi.h"

namespace Marlin
{
class VulkanDevice;
struct VulkanCommandSignature;
struct VulkanListRelease;

/// A command buffer with its own pool, so lists record on any thread without locks; the device recycles it once
/// its submission is done.
class VulkanCommandList final : public RhiCommandList
{
public:
    VulkanCommandList(VulkanDevice &device, RhiQueueType queue, uint32 family);
    ~VulkanCommandList() override;
    MARLIN_NONCOPYABLE(VulkanCommandList);

    /// Starts recording; *trail* is the list's breadcrumbs, or null.
    bool Begin(RhiBreadcrumbs::Trail *trail);
    VkResult End();
    void Reset();

    [[nodiscard]] VkCommandBuffer GetCommandBuffer() const
    {
        return _commandBuffer;
    }

    [[nodiscard]] RhiBreadcrumbs::Trail *GetTrail() const
    {
        return _trail;
    }

    void TakeTrail()
    {
        _trail = nullptr;
    }

    [[nodiscard]] List<VulkanListRelease> &GetReleases()
    {
        return _releases;
    }

    [[nodiscard]] uint32 GetSkippedCommands() const
    {
        return _skipped;
    }

    [[nodiscard]] RhiQueueType GetQueueType() const override
    {
        return _queue;
    }

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
    /// False when the bound pipeline is not ready; the command is then skipped and counted.
    bool CanExecute();
    void WriteBreadcrumb(uint32 value);
    /// A range of the list's preprocess memory for one execute of a generated command signature; zero *size* if it
    /// needs none or none could be made. The memory is reused once the list's submission is done.
    void AllocatePreprocess(RhiCommandSignature handle, const VulkanCommandSignature &signature, uint32 maxCount,
                            uint64 &address, uint64 &size);
    /// *count* queries of the list's pool for *type*, reset on the host; the pool is reused like preprocess memory.
    VkQueryPool AllocatePropertyQueries(VkQueryType type, uint32 count, uint32 &first);

    /// The preprocess memory a generated command signature needs for a maxCount, as last asked of the driver.
    struct PreprocessRequirements
    {
        RhiCommandSignature Signature;
        uint32 MaxCount = 0;
        uint64 Size = 0;
        uint64 Alignment = 0;
    };

    /// Acceleration structure property queries: compacted size, serialization size.
    struct PropertyQueries
    {
        VkQueryPool Pool = VK_NULL_HANDLE;
        uint32 Capacity = 0;
        uint32 Used = 0;
    };

    VulkanDevice &_device;
    RhiQueueType _queue;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _commandBuffer = VK_NULL_HANDLE;
    RhiBreadcrumbs::Trail *_trail = nullptr;
    List<VulkanListRelease> _releases;
    VkPipelineBindPoint _bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    bool _pipelineReady = false;
    bool _inRendering = false;
    uint32 _skipped = 0;
    uint32 _markerDepth = 0;
    RhiBuffer _preprocess;
    uint64 _preprocessAddress = 0;
    uint64 _preprocessCapacity = 0;
    uint64 _preprocessUsed = 0;
    PreprocessRequirements _requirements[4];
    uint32 _nextRequirements = 0;
    PropertyQueries _propertyQueries[2];
};
} // namespace Marlin
