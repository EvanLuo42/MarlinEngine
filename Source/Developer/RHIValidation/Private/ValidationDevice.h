#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformSync.h"
#include "RHI/RhiDevice.h"
#include "Templates/Optional.h"

MARLIN_DECLARE_LOG_CATEGORY(RHIVALIDATION_API, LogRhiValidation, Info, Error);

namespace Marlin
{
class ValidationCommandList;

/// Reports a broken RHI contract: logs it and breaks into an attached debugger.
#define MARLIN_RHI_VALIDATION_ERROR(...)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        MARLIN_LOG(LogRhiValidation, Error, __VA_ARGS__);                                                              \
        ::Marlin::Private::BreakOnValidationError();                                                                   \
    } while (false)

namespace Private
{
void BreakOnValidationError();
} // namespace Private

template <typename TTag> uint64 HandleKey(RhiHandle<TTag> handle)
{
    return (uint64{handle.Generation} << 32) | handle.Index;
}

/// A device that checks every call against the RHI contract before forwarding it to the backend's device: stale
/// and doubly destroyed handles, descriptions, views and barriers out of range, the submit thread, and fence values
/// that go backwards. Command lists it hands out check their own use (ValidationCommandList).
class RHIVALIDATION_API ValidationDevice final : public RhiDevice
{
public:
    explicit ValidationDevice(RhiDevice &inner);
    ~ValidationDevice() override;
    MARLIN_NONCOPYABLE(ValidationDevice);

    [[nodiscard]] Name GetBackendName() const override;
    [[nodiscard]] const RhiAdapterInfo &GetAdapterInfo() const override;
    [[nodiscard]] const RhiDeviceLimits &GetLimits() const override;
    [[nodiscard]] Span<const Name> GetFeatures() const override;
    [[nodiscard]] RhiNativeHandles GetNativeHandles() const override;
    [[nodiscard]] RhiMemoryBudget GetMemoryBudget(RhiMemoryLocation location) const override;
    void GetMemoryStats(List<RhiMemoryPoolStats> &pools) const override;

    [[nodiscard]] Result<RhiHeap, RhiError> CreateHeap(const RhiHeapDesc &desc) override;
    void Destroy(RhiHeap heap) override;
    [[nodiscard]] RhiAllocationInfo GetAllocationInfo(const RhiBufferDesc &desc) const override;
    [[nodiscard]] RhiAllocationInfo GetAllocationInfo(const RhiTextureDesc &desc) const override;

    [[nodiscard]] Result<RhiBuffer, RhiError> CreateBuffer(const RhiBufferDesc &desc) override;
    void Destroy(RhiBuffer buffer) override;
    [[nodiscard]] uint8 *GetMappedData(RhiBuffer buffer) const override;
    [[nodiscard]] uint64 GetBufferAddress(RhiBuffer buffer) const override;

    [[nodiscard]] Result<RhiTexture, RhiError> CreateTexture(const RhiTextureDesc &desc) override;
    void Destroy(RhiTexture texture) override;
    [[nodiscard]] const RhiTextureDesc &GetTextureDesc(RhiTexture texture) const override;
    [[nodiscard]] RhiNativeTexture GetNativeTexture(RhiTexture texture, const RhiSubresourceRange &range,
                                                    RhiFormat viewFormat) override;
    [[nodiscard]] RhiNativeBuffer GetNativeBuffer(RhiBuffer buffer) const override;
    [[nodiscard]] RhiTileInfo GetTileInfo(RhiTexture texture) const override;

    [[nodiscard]] Result<RhiResourceView, RhiError> CreateView(const RhiTextureViewDesc &desc) override;
    [[nodiscard]] Result<RhiResourceView, RhiError> CreateView(const RhiBufferViewDesc &desc) override;
    [[nodiscard]] Result<RhiResourceView, RhiError> CreateView(RhiAccelerationStructure structure) override;
    void Destroy(RhiResourceView view) override;

    [[nodiscard]] Result<RhiSampler, RhiError> CreateSampler(const RhiSamplerDesc &desc) override;
    void Destroy(RhiSampler sampler) override;

    [[nodiscard]] RhiAccelerationStructureSizes GetAccelerationStructureSizes(
        const RhiAccelerationStructureInputs &inputs) const override;
    [[nodiscard]] Result<RhiAccelerationStructure, RhiError> CreateAccelerationStructure(
        const RhiAccelerationStructureDesc &desc) override;
    void Destroy(RhiAccelerationStructure structure) override;
    [[nodiscard]] uint64 GetAccelerationStructureAddress(RhiAccelerationStructure structure) const override;

    [[nodiscard]] RhiPipeline CreatePipeline(const RhiGraphicsPipelineDesc &desc) override;
    [[nodiscard]] RhiPipeline CreatePipeline(const RhiComputePipelineDesc &desc) override;
    [[nodiscard]] RhiPipeline CreatePipeline(const RhiRayTracingPipelineDesc &desc) override;
    void Destroy(RhiPipeline pipeline) override;
    [[nodiscard]] RhiPipelineState GetPipelineState(RhiPipeline pipeline) const override;
    RhiPipelineState WaitForPipeline(RhiPipeline pipeline) override;
    void GetShaderGroupHandles(RhiPipeline pipeline, uint32 first, uint32 count, Span<uint8> output) const override;
    [[nodiscard]] List<uint8> GetPipelineCacheData() const override;

    [[nodiscard]] Result<RhiIndirectPipelineSet, RhiError> CreateIndirectPipelineSet(
        const RhiIndirectPipelineSetDesc &desc) override;
    void Destroy(RhiIndirectPipelineSet set) override;
    [[nodiscard]] Result<RhiCommandSignature, RhiError> CreateCommandSignature(
        const RhiCommandSignatureDesc &desc) override;
    void Destroy(RhiCommandSignature signature) override;
    [[nodiscard]] RhiCommandSignature GetCommandSignature(RhiIndirectCommandKind kind) const override;

    [[nodiscard]] Result<RhiQueryPool, RhiError> CreateQueryPool(const RhiQueryPoolDesc &desc) override;
    void Destroy(RhiQueryPool pool) override;
    bool ReadQueries(RhiQueryPool pool, uint32 first, uint32 count, Span<uint64> output) override;
    void GetCalibratedTimestamps(RhiQueueType queue, uint64 &cpuTicks, uint64 &gpuTicks) const override;

    [[nodiscard]] Result<RhiFence, RhiError> CreateFence(uint64 initialValue, StringView debugName) override;
    void Destroy(RhiFence fence) override;
    [[nodiscard]] uint64 GetCompletedValue(RhiFence fence) const override;
    bool Wait(RhiFence fence, uint64 value, uint32 timeout) override;
    void Signal(RhiFence fence, uint64 value) override;
    [[nodiscard]] RhiFence GetQueueFence(RhiQueueType queue) const override;
    [[nodiscard]] RhiQueueValues GetSubmittedValues() const override;
    [[nodiscard]] RhiQueueValues GetCompletedValues() const override;

    [[nodiscard]] RhiCommandList *BeginCommandList(RhiQueueType queue) override;
    void DiscardCommandList(RhiCommandList *list) override;
    Result<uint64, RhiError> Submit(RhiQueueType queue, const RhiSubmitDesc &desc) override;
    Result<void, RhiError> UpdateTileMappings(RhiQueueType queue, Span<const RhiTileMapping> mappings) override;
    void WaitIdle() override;
    void ProcessCompletions() override;

    [[nodiscard]] Result<RhiSwapchain, RhiError> CreateSwapchain(const RhiSwapchainDesc &desc) override;
    void Destroy(RhiSwapchain swapchain) override;
    Result<void, RhiError> ResizeSwapchain(RhiSwapchain swapchain, uint32 width, uint32 height) override;
    Result<void, RhiError> SetPresentMode(RhiSwapchain swapchain, RhiPresentMode mode) override;
    [[nodiscard]] Span<const RhiColorSpace> GetSupportedColorSpaces(RhiSwapchain swapchain) const override;
    Result<void, RhiError> SetColorSpace(RhiSwapchain swapchain, RhiColorSpace colorSpace, RhiFormat format) override;
    Result<void, RhiError> SetHdrMetadata(RhiSwapchain swapchain, const RhiHdrMetadata &metadata) override;
    void WaitForPresent(RhiSwapchain swapchain) override;
    Result<RhiTexture, RhiError> AcquireNextImage(RhiSwapchain swapchain) override;
    Result<void, RhiError> Present(RhiSwapchain swapchain, RhiQueueType queue) override;

    void WriteCrashReport(StringWriter &output) const override;
    void SetDebugName(RhiBuffer buffer, StringView name) override;
    void SetDebugName(RhiTexture texture, StringView name) override;

    /// What command lists check handles against, copied out under the lock; nothing if the handle is not live.
    [[nodiscard]] Optional<RhiBufferDesc> FindBuffer(RhiBuffer buffer) const;
    [[nodiscard]] Optional<RhiTextureDesc> FindTexture(RhiTexture texture) const;
    [[nodiscard]] bool IsLive(RhiPipeline pipeline) const;
    [[nodiscard]] bool IsLive(RhiAccelerationStructure structure) const;
    [[nodiscard]] Optional<RhiQueryPoolDesc> FindQueryPool(RhiQueryPool pool) const;
    [[nodiscard]] Optional<RhiIndirectCommandKind> FindCommandSignature(RhiCommandSignature signature) const;
    /// The kind of a live pipeline, in one lookup; nothing if it is stale.
    [[nodiscard]] Optional<RhiPipelineKind> FindPipelineKind(RhiPipeline pipeline) const;

private:
    /// Marks a submit-side call (Submit, Present, AcquireNextImage and the like) while it runs. They belong to one
    /// submitter at a time (07 §1.2), so a call that overlaps another thread's is an error.
    class SubmitScope
    {
    public:
        SubmitScope(ValidationDevice &device, const char *call);
        ~SubmitScope();
        MARLIN_NONCOPYABLE(SubmitScope);

    private:
        ValidationDevice &_device;
        bool _owner = false;
    };
    RhiCommandList *Unwrap(RhiCommandList *list, RhiQueueType queue, const char *call);
    bool CheckTextureDesc(const RhiTextureDesc &desc) const;

    RhiDevice &_inner;
    mutable ReadWriteLock _lock;
    HashMap<uint64, RhiBufferDesc> _buffers;
    HashMap<uint64, RhiTextureDesc> _textures;
    HashMap<uint64, uint8> _views;
    HashMap<uint64, uint32> _samplers;
    HashMap<uint64, uint8> _heaps;
    HashMap<uint64, uint8> _accelerationStructures;
    HashMap<uint64, RhiPipelineKind> _pipelines;
    HashMap<uint64, uint64> _fences;
    HashMap<uint64, RhiQueryPoolDesc> _queryPools;
    HashMap<uint64, RhiIndirectCommandKind> _commandSignatures;
    /// The device's signatures of the plain commands, which are not destroyed.
    HashSet<uint64> _builtInSignatures;
    HashMap<uint64, uint8> _swapchains;
    HashMap<uint64, uint8> _swapchainImages;
    /// The thread inside a submit-side call, or zero.
    Atomic<uint32> _submitThread{0};
    Atomic<uint32> _liveLists{0};
};
} // namespace Marlin
