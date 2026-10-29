#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/Jobs.h"
#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Memory/UniquePtr.h"
#include "RHI/GpuMemoryAllocator.h"
#include "RHI/RhiBackend.h"
#include "RHI/RhiBreadcrumbs.h"
#include "RHI/RhiDeferredRelease.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiObjectPool.h"
#include "RHIVulkan/VulkanApi.h"
#include "RHIVulkan/VulkanBackend.h"
#include "String/String.h"

namespace Marlin
{
class VulkanCommandList;
class VulkanDevice;

struct VulkanBuffer
{
    VkBuffer Buffer = VK_NULL_HANDLE;
    GpuAllocation Allocation;
    RhiBufferDesc Desc;
    uint64 Address = 0;
    uint8 *Mapped = nullptr;
};

/// An image view the backend made for a texture: a render target, or a native view for an SDK.
struct VulkanAttachmentView
{
    uint32 Mip = 0;
    uint32 MipCount = 1;
    uint32 FirstSlice = 0;
    uint32 SliceCount = 0;
    VkFormat Format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags Aspect = 0;
    VkImageView View = VK_NULL_HANDLE;
};

struct VulkanTexture
{
    VkImage Image = VK_NULL_HANDLE;
    GpuAllocation Allocation;
    RhiTextureDesc Desc;
    VkFormat Format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags Aspect = 0;
    /// Swapchain images belong to their swapchain.
    bool OwnsImage = true;
    /// Reserved textures: the tile shape and where the mip tail lives in the image's opaque memory.
    RhiTileInfo Tiles;
    uint64 MipTailOffset = 0;
    uint64 MipTailStride = 0;
    uint64 MipTailSize = 0;
    bool SingleMipTail = false;
    SpinLock ViewLock;
    List<VulkanAttachmentView> AttachmentViews;
};

/// A view's slot in the resource heap is its handle index; this is what Destroy needs to know.
struct VulkanView
{
    VkDescriptorType Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
};

struct VulkanSampler
{
    RhiSamplerDesc Desc;
    uint32 References = 0;
};

struct VulkanHeap
{
    GpuMemoryBlock Block;
    RhiHeapDesc Desc;
};

struct VulkanAccelerationStructure
{
    VkAccelerationStructureKHR Structure = VK_NULL_HANDLE;
    RhiAccelerationStructureKind Kind = RhiAccelerationStructureKind::BottomLevel;
    uint64 Address = 0;
    uint64 Size = 0;
};

struct VulkanShaderSource
{
    List<uint8> Code;
    String EntryPoint;
    RhiShaderStage Stage = RhiShaderStage::None;
    List<uint32> SpecializationConstants;
};

/// A pipeline and what its compile job needs, copied because the job outlives the caller's description.
struct VulkanPipeline
{
    VulkanDevice *Device = nullptr;
    RhiPipelineKind Kind = RhiPipelineKind::Graphics;
    Atomic<uint8> State{static_cast<uint8>(RhiPipelineState::Pending)};
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    List<VulkanShaderSource> Shaders;
    RhiGraphicsPipelineDesc Graphics;
    List<RhiFormat> RenderTargetFormats;
    List<RhiHitGroup> HitGroups;
    uint32 MaxRecursionDepth = 1;
    uint32 GroupCount = 0;
    bool IndirectBindable = false;
    String DebugName;
    JobCounter Compile;
};

struct VulkanFence
{
    VkSemaphore Semaphore = VK_NULL_HANDLE;
};

struct VulkanQueryPool
{
    VkQueryPool Pool = VK_NULL_HANDLE;
    RhiQueryKind Kind = RhiQueryKind::Timestamp;
    uint32 Count = 0;
};

/// A plain signature runs one indirect command; one with a layout runs device generated command sequences.
struct VulkanCommandSignature
{
    RhiIndirectCommandKind Kind = RhiIndirectCommandKind::Draw;
    uint32 Stride = 0;
    VkIndirectCommandsLayoutEXT Layout = VK_NULL_HANDLE;
    VkIndirectExecutionSetEXT Set = VK_NULL_HANDLE;
    VkShaderStageFlags Stages = 0;
};

struct VulkanIndirectPipelineSet
{
    VkIndirectExecutionSetEXT Set = VK_NULL_HANDLE;
    RhiPipelineKind Kind = RhiPipelineKind::Graphics;
    uint32 Count = 0;
};

/// The stages the first graphics submission after an acquire waits for the image at. A graphics list's barriers on
/// a swapchain image add them to their source stages, so the layout change out of Present chains after the wait.
constexpr VkPipelineStageFlags2 VulkanAcquireWaitStages =
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
/// An acquire slot's value until the graphics submission that waits for it is made.
constexpr uint64 VulkanAcquirePending = ~uint64{0};

/// An acquired image's binary semaphore, waited by the next graphics submission.
struct VulkanPendingAcquire
{
    VkSemaphore Semaphore = VK_NULL_HANDLE;
    RhiSwapchain Swapchain;
    uint32 Slot = 0;
};

struct VulkanSwapchain
{
    RhiSwapchainDesc Desc;
    VkSurfaceKHR Surface = VK_NULL_HANDLE;
    VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
    VkFormat Format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    List<RhiTexture> Images;
    /// Signaled by the queue before presenting image i, waited by the present.
    List<VkSemaphore> PresentSemaphores;
    /// Signaled by acquire; each is free again once the graphics timeline passes its value, which is
    /// VulkanAcquirePending until the graphics submission that waits for it is made.
    List<VkSemaphore> AcquireSemaphores;
    List<uint64> AcquireSemaphoreValues;
    uint32 NextAcquireSemaphore = 0;
    /// The acquire semaphore slot of the current image.
    uint32 CurrentAcquireSemaphore = 0;
    uint32 CurrentImage = ~0u;
    uint64 PresentCount = 0;
    /// The present queue's timeline value at each present, for the frame latency wait without present_wait.
    List<uint64> PresentValues;
    uint32 PresentQueue = 0;
    List<RhiColorSpace> ColorSpaces;
    bool HasHdrMetadata = false;
    RhiHdrMetadata HdrMetadata;
};

struct VulkanQueue
{
    VkQueue Queue = VK_NULL_HANDLE;
    uint32 Family = ~0u;
    VkSemaphore Timeline = VK_NULL_HANDLE;
    RhiFence Fence;
    Atomic<uint64> Submitted{0};
    bool SparseBinding = false;
    bool Present = false;
    /// Lists of a transfer-only family cannot copy query results, so ReadQueries reads them on the CPU.
    bool ResolvesQueries = false;
    uint32 TimestampValidBits = 0;
    /// Timeline waits the next submission on this queue must make, such as after a tile mapping update; at most one
    /// per semaphore, a later wait on the same timeline replaces the earlier one.
    List<VkSemaphoreSubmitInfo> PendingWaits;
    /// Graphics only: the acquired images' binary semaphores, waited by the next submission, which then gives their
    /// slots its timeline value.
    List<VulkanPendingAcquire> PendingAcquires;
};

/// Things a command list made that are released once its submission is done.
struct VulkanListRelease
{
    VkQueryPool QueryPool = VK_NULL_HANDLE;
    RhiBuffer Buffer;
};

/// The Vulkan device: one VkDevice, its queues, the bindless heaps and every object pool.
class VulkanDevice final : public RhiDevice, public IGpuMemorySource
{
public:
    VulkanDevice(VulkanBackend &backend, const VulkanAdapter &adapter, const RhiDeviceDesc &desc);
    ~VulkanDevice() override;
    MARLIN_NONCOPYABLE(VulkanDevice);

    /// Creates the VkDevice and everything on it; the device is unusable if this fails.
    Result<void, RhiError> Initialize(Span<const char *const> requestedExtensions,
                                      Span<VkBaseOutStructure *const> chained);

    [[nodiscard]] Name GetBackendName() const override;
    [[nodiscard]] const RhiAdapterInfo &GetAdapterInfo() const override
    {
        return _adapter.Info;
    }
    [[nodiscard]] const RhiDeviceLimits &GetLimits() const override
    {
        return _limits;
    }
    [[nodiscard]] Span<const Name> GetFeatures() const override
    {
        return _features;
    }
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

    Result<GpuMemoryBlock, RhiError> AllocateBlock(uint32 pool, uint64 size, const void *dedicatedResource) override;
    void FreeBlock(const GpuMemoryBlock &block) override;

    [[nodiscard]] VkDevice GetDevice() const
    {
        return _device;
    }

    [[nodiscard]] const VulkanBuffer *FindBuffer(RhiBuffer buffer) const
    {
        return _buffers.Find(buffer);
    }

    [[nodiscard]] VulkanTexture *FindTexture(RhiTexture texture) const
    {
        return _textures.Find(texture);
    }

    [[nodiscard]] VulkanPipeline *FindPipeline(RhiPipeline pipeline) const
    {
        return _pipelines.Find(pipeline);
    }

    [[nodiscard]] const VulkanAccelerationStructure *FindAccelerationStructure(RhiAccelerationStructure s) const
    {
        return _accelerationStructures.Find(s);
    }

    [[nodiscard]] const VulkanQueryPool *FindQueryPool(RhiQueryPool pool) const
    {
        return _queryPools.Find(pool);
    }

    [[nodiscard]] const VulkanCommandSignature *FindCommandSignature(RhiCommandSignature signature) const
    {
        return _commandSignatures.Find(signature);
    }

    /// The image view a render target uses, made on first use and kept with the texture.
    VkImageView GetAttachmentView(VulkanTexture &texture, uint32 mip, uint32 firstSlice, uint32 sliceCount,
                                  RhiFormat format);
    /// A view of any range and aspects, kept with the texture like attachment views.
    VkImageView GetTextureView(VulkanTexture &texture, uint32 mip, uint32 mipCount, uint32 firstSlice,
                               uint32 sliceCount, VkFormat format, VkImageAspectFlags aspect);
    void BindHeaps(VkCommandBuffer commandBuffer) const;
    [[nodiscard]] RhiBreadcrumbs *GetBreadcrumbs() const
    {
        return _breadcrumbs.Get();
    }
    [[nodiscard]] VkBuffer GetBreadcrumbBuffer() const;
    [[nodiscard]] bool HasDebugNames() const
    {
        return _debugNames;
    }
    [[nodiscard]] bool HasVariableRateShading() const
    {
        return _variableRateShading;
    }
    /// Preprocess memory for generated commands; each command list keeps one and reuses it.
    RhiBuffer CreatePreprocessBuffer(uint64 size);
    /// The preprocess memory one ExecuteIndirect of a generated command signature needs; zero size if none.
    void GetPreprocessRequirements(const VulkanCommandSignature &signature, uint32 maxCount, uint64 &size,
                                   uint64 &alignment) const;
    /// The stages generated commands may bind pipelines of *kind* for; zero without device generated commands.
    [[nodiscard]] VkShaderStageFlags GetGeneratedCommandStages(RhiPipelineKind kind) const;
    /// Notes device loss from a result: calls the device-lost handler once.
    bool CheckLost(VkResult result);
    void SetObjectName(VkObjectType type, uint64 handle, StringView name) const;

    static void CompilePipeline(void *pipeline);

private:
    friend class VulkanCommandList;

    struct QueueFamilies
    {
        uint32 Graphics = ~0u;
        uint32 Compute = ~0u;
        uint32 Copy = ~0u;
    };

    bool CreateDevice(Span<const char *const> requestedExtensions, Span<VkBaseOutStructure *const> chained);
    void ReadLimits();
    void CreateMemoryPools();
    Result<void, RhiError> CreateDescriptorHeaps();
    Result<void, RhiError> CreateBreadcrumbBuffer();
    /// *staging*: an Upload buffer only copied from, kept out of GPU memory unless the memory is unified.
    uint32 FindMemoryType(uint32 typeBits, RhiMemoryLocation location, bool staging = false) const;
    /// *usage2* replaces the usage with 64-bit flags, for usages only VkBufferUsageFlags2 has.
    Result<RhiBuffer, RhiError> CreateRawBuffer(const RhiBufferDesc &desc, VkBufferUsageFlags usage,
                                                VkBufferUsageFlags2 usage2 = 0);
    uint32 GetPool(uint32 memoryType, bool optimalTiling) const
    {
        return memoryType * 2 + (optimalTiling ? 1 : 0);
    }
    void WriteResourceDescriptor(uint32 slot, const VkResourceDescriptorInfoEXT &info);
    Result<RhiPipeline, RhiError> AddPipeline(VulkanPipeline *&pipeline);
    void NotifyPipelineCreated(Span<const RhiShader> shaders, StringView debugName) const;
    void Compile(VulkanPipeline &pipeline);
    VkResult CompileGraphics(VulkanPipeline &pipeline);
    VkResult CompileCompute(VulkanPipeline &pipeline);
    VkResult CompileRayTracing(VulkanPipeline &pipeline);
    Result<void, RhiError> CreateSwapchainImages(VulkanSwapchain &swapchain, uint32 width, uint32 height,
                                                 VkSwapchainKHR oldSwapchain);
    void DestroySwapchainImages(VulkanSwapchain &swapchain);
    void ApplyHdrMetadata(const VulkanSwapchain &swapchain) const;
    /// Submits only the queue's pending waits, signaling its timeline and *signalBinary* if given.
    Result<void, RhiError> SubmitEmpty(VulkanQueue &queue, VkSemaphore signalBinary, uint64 &signaledValue);
    /// Adds a timeline wait to the queue's next submission, merged with a pending wait on the same semaphore.
    static void AddPendingWait(VulkanQueue &queue, VkSemaphore semaphore, uint64 value, VkPipelineStageFlags2 stages);
    /// After a submission on *queue* that waited its pending acquires: gives their slots the submission's value.
    void ResolvePendingAcquires(VulkanQueue &queue, uint64 value);
    /// Submits the graphics queue's pending acquire waits now, if it has any.
    Result<void, RhiError> FlushPendingAcquires();
    template <typename THandle> void DeferRelease(THandle handle, RhiDeferredReleaseQueue::ReleaseFunction release);
    void Recycle(VulkanCommandList *list);

    VulkanBackend &_backend;
    const VulkanAdapter &_adapter;
    RhiDeviceDesc _desc;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties _memoryProperties{};
    QueueFamilies _families;
    VulkanQueue _queues[RhiQueueTypeCount];
    RhiDeviceLimits _limits;
    List<Name> _features;
    bool _debugNames = false;
    bool _breadcrumbMarkers = false;
    bool _memoryBudget = false;
    bool _calibratedTimestamps = false;
    bool _presentWait = false;
    bool _variableRateShading = false;
    bool _conservativeRasterization = false;
    bool _traceRaysIndirect2 = false;
    bool _generatedCommands = false;
    VkShaderStageFlags _generatedCommandStages = 0;
    bool _reBar = false;
    Atomic<bool> _lost{false};

    UniquePtr<GpuMemoryAllocator> _allocator;
    RhiDeferredReleaseQueue _releases;

    RhiObjectPool<VulkanBuffer, RhiBuffer> _buffers{1 << 20};
    RhiObjectPool<VulkanTexture, RhiTexture> _textures{1 << 20};
    RhiObjectPool<VulkanView, RhiResourceView> *_views = nullptr;
    RhiObjectPool<VulkanSampler, RhiSampler, 64> *_samplers = nullptr;
    RhiObjectPool<VulkanHeap, RhiHeap> _heaps{1 << 16};
    RhiObjectPool<VulkanAccelerationStructure, RhiAccelerationStructure> _accelerationStructures{1 << 20};
    RhiObjectPool<VulkanPipeline, RhiPipeline> _pipelines{1 << 18};
    RhiObjectPool<VulkanFence, RhiFence> _fences{1 << 12};
    RhiObjectPool<VulkanQueryPool, RhiQueryPool> _queryPools{1 << 12};
    RhiObjectPool<VulkanCommandSignature, RhiCommandSignature> _commandSignatures{1 << 12};
    RhiObjectPool<VulkanIndirectPipelineSet, RhiIndirectPipelineSet> _indirectPipelineSets{1 << 10};
    RhiObjectPool<VulkanSwapchain, RhiSwapchain> _swapchains{64};
    RhiCommandSignature _plainSignatures[static_cast<uint32>(RhiIndirectCommandKind::DispatchRays) + 1];

    RhiBuffer _resourceHeap;
    RhiBuffer _samplerHeap;
    uint64 _resourceHeapReserved = 0;
    uint64 _samplerHeapReserved = 0;
    uint32 _resourceStride = 0;
    uint32 _samplerStride = 0;
    SpinLock _samplerLock;

    RhiBuffer _breadcrumbBuffer;
    UniquePtr<RhiBreadcrumbs> _breadcrumbs;

    VkPipelineCache _pipelineCache = VK_NULL_HANDLE;

    SpinLock _listLock;
    List<VulkanCommandList *> _freeLists[RhiQueueTypeCount];
    struct InFlightList
    {
        VulkanCommandList *List = nullptr;
        uint64 Value = 0;
    };
    List<InFlightList> _inFlight[RhiQueueTypeCount];
};
} // namespace Marlin
