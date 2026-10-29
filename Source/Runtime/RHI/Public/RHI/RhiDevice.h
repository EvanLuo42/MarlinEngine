#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "RHI/RhiCommandList.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiPipeline.h"
#include "RHI/RhiRayTracing.h"
#include "RHI/RhiResources.h"
#include "String/Name.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Result.h"

namespace Marlin
{
class StringWriter;

/// Numeric limits of the device; optional capabilities are features, named in RHI/RhiFeatures.h.
struct RhiDeviceLimits
{
    uint32 MaxTextureSize2D = 0;
    uint32 MaxTextureSize3D = 0;
    uint32 MaxTextureSizeCube = 0;
    uint32 MaxTextureArraySize = 0;
    uint32 MaxResourceDescriptors = 0;
    uint32 MaxSamplerDescriptors = 0;
    uint32 MinWaveSize = 0;
    uint32 MaxWaveSize = 0;
    /// Bytes between descriptors in the resource heap, the same for every kind; shaders scale indices by it.
    uint32 ResourceDescriptorStride = 0;
    uint32 SamplerDescriptorStride = 0;
    uint32 BufferPlacementAlignment = 0;
    uint32 TexturePlacementAlignment = 0;
    uint32 ConstantDataAlignment = 0;
    uint32 StructuredBufferOffsetAlignment = 0;
    /// Row pitch and offset alignment of buffer-texture copies.
    uint32 CopyRowPitchAlignment = 0;
    uint32 CopyOffsetAlignment = 0;
    uint32 ShaderGroupHandleSize = 0;
    uint32 ShaderGroupHandleAlignment = 0;
    uint32 ShaderTableAlignment = 0;
    uint32 MaxRayRecursionDepth = 0;
    uint32 ShadingRateTileSize = 0;
    uint32 MaxMeshOutputVertices = 0;
    uint32 MaxMeshOutputPrimitives = 0;
    uint32 MaxTaskPayloadBytes = 0;
    uint32 MaxComputeGroupSize[3] = {};
    uint32 MaxComputeGroupInvocations = 0;
    uint32 MaxComputeSharedMemory = 0;
    /// Ticks per second of timestamps, per queue type; zero where the queue cannot write timestamps.
    uint64 TimestampFrequency[RhiQueueTypeCount] = {};
    /// Whether command lists of each queue type can ResolveQueries. Where they cannot, as on Vulkan's transfer-only
    /// queues, RhiDevice::ReadQueries reads the results on the CPU.
    bool ResolvesQueries[RhiQueueTypeCount] = {};
};

enum class RhiAdapterKind : uint8
{
    Discrete,
    Integrated,
    Software,
    Other,
};

struct RhiAdapterInfo
{
    String Name;
    uint32 VendorId = 0;
    uint32 DeviceId = 0;
    RhiAdapterKind Kind = RhiAdapterKind::Other;
    String DriverVersion;
    uint64 DeviceMemoryBytes = 0;
    /// The device memory the CPU can map: all of it with ReBAR, a small window or none without.
    uint64 HostVisibleDeviceMemoryBytes = 0;
    /// CPU and GPU share memory, as on consoles and Apple silicon.
    bool UnifiedMemory = false;
};

struct RhiMemoryBudget
{
    uint64 Budget = 0;
    uint64 Usage = 0;
};

/// One pool of the device's memory allocator: the blocks it took from the driver and how full they are.
struct RhiMemoryPoolStats
{
    RhiMemoryLocation Location = RhiMemoryLocation::Device;
    /// Optimally tiled resources, textures, rather than buffers.
    bool Textures = false;
    uint32 BlockCount = 0;
    uint32 AllocationCount = 0;
    uint64 BlockBytes = 0;
    uint64 UsedBytes = 0;
};

struct RhiFenceWait
{
    RhiFence Fence;
    uint64 Value = 0;
};

using RhiFenceSignal = RhiFenceWait;

/// Executes command lists of the queue's type after *Waits*, then sets *Signals*; also signals the queue's own
/// timeline, whose value Submit returns.
struct RhiSubmitDesc
{
    Span<RhiCommandList *const> CommandLists;
    Span<const RhiFenceWait> Waits;
    Span<const RhiFenceSignal> Signals;
};

enum class RhiPresentMode : uint8
{
    /// Waits for vertical blank; never tears.
    Fifo,
    /// Tears when late, for variable refresh rate displays and the lowest latency.
    Immediate,
    /// Replaces the queued image instead of waiting; never tears.
    Mailbox,
};

enum class RhiColorSpace : uint8
{
    Srgb,
    /// HDR10: Rec. 2020 primaries with the PQ curve.
    Hdr10,
    /// Linear Rec. 709 primaries, values past 1 for HDR.
    ScRgb,
};

struct RhiSwapchainDesc
{
    /// The OS window, such as an HWND.
    void *NativeWindow = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    RhiFormat Format = RhiFormat::BGRA8Unorm;
    RhiColorSpace ColorSpace = RhiColorSpace::Srgb;
    RhiPresentMode PresentMode = RhiPresentMode::Fifo;
    uint32 ImageCount = 3;
    /// Frames the CPU may queue ahead of the display; WaitForPresent blocks beyond it.
    uint32 MaxFrameLatency = 2;
    RhiTextureUsage Usage = RhiTextureUsage::RenderTarget;
    StringView DebugName;
};

/// The mastering display and content light levels of HDR output, in CIE 1931 xy and nits.
struct RhiHdrMetadata
{
    float32 RedPrimary[2] = {0.708f, 0.292f};
    float32 GreenPrimary[2] = {0.170f, 0.797f};
    float32 BluePrimary[2] = {0.131f, 0.046f};
    float32 WhitePoint[2] = {0.3127f, 0.3290f};
    float32 MaxLuminance = 1000;
    float32 MinLuminance = 0.001f;
    float32 MaxContentLightLevel = 1000;
    float32 MaxFrameAverageLightLevel = 400;
};

/// A texture as the graphics API sees it, for SDKs that take native resources, such as upscalers: a VkImage and a
/// VkImageView of the asked range on Vulkan, an ID3D12Resource on D3D12. Format is the API's own format value.
struct RhiNativeTexture
{
    void *Resource = nullptr;
    void *View = nullptr;
    uint32 Format = 0;
};

/// A VkBuffer or ID3D12Resource, with its GPU address.
struct RhiNativeBuffer
{
    void *Resource = nullptr;
    uint64 Size = 0;
    uint64 Address = 0;
};

/// What a capture or crash tool needs of a device: its native objects, as the backend defines them.
struct RhiNativeHandles
{
    Name Backend;
    void *Instance = nullptr;
    void *Adapter = nullptr;
    void *Device = nullptr;
    void *Queues[RhiQueueTypeCount] = {};
};

/// A GPU, and every object made on it. Creation and destruction are free-threaded; Submit, Present,
/// AcquireNextImage and ProcessCompletions belong to one submitter at a time, such as the frame's render job, on any
/// thread (07 §1.2). Destroyed objects are released once the
/// queues have finished the work submitted before the call.
class RHI_API RhiDevice
{
public:
    RhiDevice() = default;
    virtual ~RhiDevice() = default;
    MARLIN_NONCOPYABLE(RhiDevice);

    [[nodiscard]] virtual Name GetBackendName() const = 0;
    [[nodiscard]] virtual const RhiAdapterInfo &GetAdapterInfo() const = 0;
    [[nodiscard]] virtual const RhiDeviceLimits &GetLimits() const = 0;
    /// The optional features this device has (RHI/RhiFeatures.h); a backend may add names of its own.
    [[nodiscard]] virtual Span<const Name> GetFeatures() const = 0;
    [[nodiscard]] bool HasFeature(Name feature) const;

    [[nodiscard]] bool HasFeature(StringView feature) const
    {
        const Name name = Name::Find(feature);
        return !name.IsNone() && HasFeature(name);
    }
    [[nodiscard]] virtual RhiNativeHandles GetNativeHandles() const = 0;
    [[nodiscard]] virtual RhiMemoryBudget GetMemoryBudget(RhiMemoryLocation location) const = 0;
    /// Appends the allocator pools that hold memory.
    virtual void GetMemoryStats(List<RhiMemoryPoolStats> &pools) const = 0;

    [[nodiscard]] virtual Result<RhiHeap, RhiError> CreateHeap(const RhiHeapDesc &desc) = 0;
    virtual void Destroy(RhiHeap heap) = 0;

    [[nodiscard]] virtual RhiAllocationInfo GetAllocationInfo(const RhiBufferDesc &desc) const = 0;
    [[nodiscard]] virtual RhiAllocationInfo GetAllocationInfo(const RhiTextureDesc &desc) const = 0;

    [[nodiscard]] virtual Result<RhiBuffer, RhiError> CreateBuffer(const RhiBufferDesc &desc) = 0;
    virtual void Destroy(RhiBuffer buffer) = 0;
    /// The CPU address of an Upload or Readback buffer, valid until it is destroyed.
    [[nodiscard]] virtual uint8 *GetMappedData(RhiBuffer buffer) const = 0;
    [[nodiscard]] virtual uint64 GetBufferAddress(RhiBuffer buffer) const = 0;

    [[nodiscard]] virtual Result<RhiTexture, RhiError> CreateTexture(const RhiTextureDesc &desc) = 0;
    virtual void Destroy(RhiTexture texture) = 0;
    [[nodiscard]] virtual const RhiTextureDesc &GetTextureDesc(RhiTexture texture) const = 0;
    /// The native texture, with a view of *range* in *viewFormat* (Unknown for the texture's), kept with the
    /// texture; null fields if the handle is stale.
    [[nodiscard]] virtual RhiNativeTexture GetNativeTexture(RhiTexture texture, const RhiSubresourceRange &range = {},
                                                            RhiFormat viewFormat = RhiFormat::Unknown) = 0;
    [[nodiscard]] virtual RhiNativeBuffer GetNativeBuffer(RhiBuffer buffer) const = 0;
    [[nodiscard]] virtual RhiTileInfo GetTileInfo(RhiTexture texture) const = 0;

    [[nodiscard]] virtual Result<RhiResourceView, RhiError> CreateView(const RhiTextureViewDesc &desc) = 0;
    [[nodiscard]] virtual Result<RhiResourceView, RhiError> CreateView(const RhiBufferViewDesc &desc) = 0;
    [[nodiscard]] virtual Result<RhiResourceView, RhiError> CreateView(RhiAccelerationStructure structure) = 0;
    virtual void Destroy(RhiResourceView view) = 0;

    /// Equal descriptions share a sampler; each Create needs its own Destroy.
    [[nodiscard]] virtual Result<RhiSampler, RhiError> CreateSampler(const RhiSamplerDesc &desc) = 0;
    virtual void Destroy(RhiSampler sampler) = 0;

    [[nodiscard]] virtual RhiAccelerationStructureSizes GetAccelerationStructureSizes(
        const RhiAccelerationStructureInputs &inputs) const = 0;
    [[nodiscard]] virtual Result<RhiAccelerationStructure, RhiError> CreateAccelerationStructure(
        const RhiAccelerationStructureDesc &desc) = 0;
    virtual void Destroy(RhiAccelerationStructure structure) = 0;
    [[nodiscard]] virtual uint64 GetAccelerationStructureAddress(RhiAccelerationStructure structure) const = 0;

    /// Pipelines compile on jobs: the handle is valid at once and its state turns Ready or Failed later.
    [[nodiscard]] virtual RhiPipeline CreatePipeline(const RhiGraphicsPipelineDesc &desc) = 0;
    [[nodiscard]] virtual RhiPipeline CreatePipeline(const RhiComputePipelineDesc &desc) = 0;
    [[nodiscard]] virtual RhiPipeline CreatePipeline(const RhiRayTracingPipelineDesc &desc) = 0;
    virtual void Destroy(RhiPipeline pipeline) = 0;
    [[nodiscard]] virtual RhiPipelineState GetPipelineState(RhiPipeline pipeline) const = 0;
    /// Blocks until the pipeline is Ready or Failed, helping with jobs meanwhile.
    virtual RhiPipelineState WaitForPipeline(RhiPipeline pipeline) = 0;
    /// Writes *count* handles from group *first* of a Ready ray tracing pipeline, ShaderGroupHandleSize bytes each.
    virtual void GetShaderGroupHandles(RhiPipeline pipeline, uint32 first, uint32 count, Span<uint8> output) const = 0;
    /// The driver's pipeline cache, to be passed to the next run through RhiDeviceDesc::PipelineCache.
    [[nodiscard]] virtual List<uint8> GetPipelineCacheData() const = 0;

    [[nodiscard]] virtual Result<RhiIndirectPipelineSet, RhiError> CreateIndirectPipelineSet(
        const RhiIndirectPipelineSetDesc &desc) = 0;
    virtual void Destroy(RhiIndirectPipelineSet set) = 0;
    [[nodiscard]] virtual Result<RhiCommandSignature, RhiError> CreateCommandSignature(
        const RhiCommandSignatureDesc &desc) = 0;
    virtual void Destroy(RhiCommandSignature signature) = 0;
    /// The signature of a single plain command: Draw, DrawIndexed, DrawMesh, Dispatch or DispatchRays.
    [[nodiscard]] virtual RhiCommandSignature GetCommandSignature(RhiIndirectCommandKind kind) const = 0;

    [[nodiscard]] virtual Result<RhiQueryPool, RhiError> CreateQueryPool(const RhiQueryPoolDesc &desc) = 0;
    virtual void Destroy(RhiQueryPool pool) = 0;
    /// Reads *count* results from query *first* into *output* on the CPU, as uint64 values (nine per pipeline
    /// statistics query), then resets those queries; for queues whose lists cannot ResolveQueries. The work that wrote
    /// them must be done. False if a result is not available.
    virtual bool ReadQueries(RhiQueryPool pool, uint32 first, uint32 count, Span<uint64> output) = 0;
    /// A CPU time and the queue's timestamp taken at the same moment, to put GPU times on the CPU timeline.
    virtual void GetCalibratedTimestamps(RhiQueueType queue, uint64 &cpuTicks, uint64 &gpuTicks) const = 0;

    [[nodiscard]] virtual Result<RhiFence, RhiError> CreateFence(uint64 initialValue = 0,
                                                                 StringView debugName = {}) = 0;
    virtual void Destroy(RhiFence fence) = 0;
    [[nodiscard]] virtual uint64 GetCompletedValue(RhiFence fence) const = 0;
    /// False on timeout, in milliseconds.
    virtual bool Wait(RhiFence fence, uint64 value, uint32 timeout = 0xFFFFFFFFu) = 0;
    virtual void Signal(RhiFence fence, uint64 value) = 0;
    /// The fence each queue signals on every Submit.
    [[nodiscard]] virtual RhiFence GetQueueFence(RhiQueueType queue) const = 0;
    /// The values the queue fences reach once everything submitted so far is done.
    [[nodiscard]] virtual RhiQueueValues GetSubmittedValues() const = 0;
    [[nodiscard]] virtual RhiQueueValues GetCompletedValues() const = 0;

    /// A command list in the recording state; any thread.
    [[nodiscard]] virtual RhiCommandList *BeginCommandList(RhiQueueType queue) = 0;
    /// Abandons a command list without submitting it.
    virtual void DiscardCommandList(RhiCommandList *list) = 0;
    /// The value the queue's fence reaches when this submission is done.
    virtual Result<uint64, RhiError> Submit(RhiQueueType queue, const RhiSubmitDesc &desc) = 0;
    /// Maps and unmaps tiles of reserved resources on *queue*, in order with its submissions.
    virtual Result<void, RhiError> UpdateTileMappings(RhiQueueType queue, Span<const RhiTileMapping> mappings) = 0;
    /// Waits for the queues to finish everything submitted.
    virtual void WaitIdle() = 0;
    /// Releases what destroyed objects held once their queues are done, and runs pipeline callbacks. The submit
    /// thread calls it every frame.
    virtual void ProcessCompletions() = 0;

    [[nodiscard]] virtual Result<RhiSwapchain, RhiError> CreateSwapchain(const RhiSwapchainDesc &desc) = 0;
    virtual void Destroy(RhiSwapchain swapchain) = 0;
    /// Recreates the images at a new size or mode, after waiting for the device to be idle; the old textures become
    /// stale.
    virtual Result<void, RhiError> ResizeSwapchain(RhiSwapchain swapchain, uint32 width, uint32 height) = 0;
    virtual Result<void, RhiError> SetPresentMode(RhiSwapchain swapchain, RhiPresentMode mode) = 0;
    [[nodiscard]] virtual Span<const RhiColorSpace> GetSupportedColorSpaces(RhiSwapchain swapchain) const = 0;
    virtual Result<void, RhiError> SetColorSpace(RhiSwapchain swapchain, RhiColorSpace colorSpace,
                                                 RhiFormat format) = 0;
    /// Tells the display how bright the content is, for an HDR color space; kept across swapchain changes.
    /// Unsupported if the device or display has no HDR metadata.
    virtual Result<void, RhiError> SetHdrMetadata(RhiSwapchain swapchain, const RhiHdrMetadata &metadata) = 0;
    /// Blocks until the swapchain can take another frame without exceeding MaxFrameLatency. A present the compositor
    /// never shows, as while the window resizes, is waited for 100 ms at most.
    virtual void WaitForPresent(RhiSwapchain swapchain) = 0;
    /// The image to render this frame, in the Present layout. The next Graphics submission waits for it to be
    /// available before rendering or compute work; work on other queues must reach the image through a wait for a
    /// Graphics submission (RenderGraph starts imported resources on Graphics).
    virtual Result<RhiTexture, RhiError> AcquireNextImage(RhiSwapchain swapchain) = 0;
    /// Presents the acquired image after all work submitted to *queue* so far.
    virtual Result<void, RhiError> Present(RhiSwapchain swapchain, RhiQueueType queue = RhiQueueType::Graphics) = 0;

    /// After device loss: the adapter, the breadcrumbs of the unfinished submissions and the backend's own fault
    /// information.
    virtual void WriteCrashReport(StringWriter &output) const = 0;

    /// Sets names for debuggers and capture tools; nothing in Shipping.
    virtual void SetDebugName(RhiBuffer buffer, StringView name) = 0;
    virtual void SetDebugName(RhiTexture texture, StringView name) = 0;
};
} // namespace Marlin
