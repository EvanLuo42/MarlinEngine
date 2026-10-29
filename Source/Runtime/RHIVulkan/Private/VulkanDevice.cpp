#include "VulkanDevice.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformTime.h"
#include "RHI/PlatformGpuMemoryConfig.h"
#include "RHI/RhiFeatures.h"
#include "RHI/RhiLog.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "VulkanCommandList.h"
#include "VulkanConvert.h"
#include "VulkanFeatures.h"
#include "VulkanPlatform.h"

namespace Marlin
{
namespace
{
constexpr uint32 BreadcrumbSlots = 4096;
constexpr uint32 MaxResourceDescriptors = 1 << 20;
constexpr uint32 MaxSamplerDescriptors = 4096;

uint64 AlignUp(uint64 value, uint64 alignment)
{
    return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
}

const char *QueueName(RhiQueueType queue)
{
    switch (queue)
    {
    case RhiQueueType::Graphics:
        return "Graphics";
    case RhiQueueType::Compute:
        return "Compute";
    case RhiQueueType::Copy:
        return "Copy";
    }
    return "Unknown";
}
} // namespace

VulkanDevice::VulkanDevice(VulkanBackend &backend, const VulkanAdapter &adapter, const RhiDeviceDesc &desc)
    : _backend(backend), _adapter(adapter), _desc(desc)
{
}

VulkanDevice::~VulkanDevice()
{
    if (_device == VK_NULL_HANDLE)
    {
        return;
    }
    Vk.vkDeviceWaitIdle(_device);
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        for (const InFlightList &entry : _inFlight[queue])
        {
            Recycle(entry.List);
        }
        _inFlight[queue].Clear();
    }
    _releases.Flush();
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        for (VulkanCommandList *list : _freeLists[queue])
        {
            delete list;
        }
        _freeLists[queue].Clear();
    }
    _swapchains.ForEach([this](RhiSwapchain handle, VulkanSwapchain &) { Destroy(handle); });
    _releases.Flush();
    _pipelines.ForEach([this](RhiPipeline, VulkanPipeline &pipeline) {
        Jobs::Wait(pipeline.Compile);
        if (pipeline.Pipeline != VK_NULL_HANDLE)
        {
            Vk.vkDestroyPipeline(_device, pipeline.Pipeline, nullptr);
        }
    });
    _textures.ForEach([this](RhiTexture, VulkanTexture &texture) {
        for (const VulkanAttachmentView &view : texture.AttachmentViews)
        {
            Vk.vkDestroyImageView(_device, view.View, nullptr);
        }
        if (texture.OwnsImage)
        {
            Vk.vkDestroyImage(_device, texture.Image, nullptr);
            _allocator->Free(texture.Allocation);
        }
    });
    _buffers.ForEach([this](RhiBuffer, VulkanBuffer &buffer) {
        Vk.vkDestroyBuffer(_device, buffer.Buffer, nullptr);
        _allocator->Free(buffer.Allocation);
    });
    _accelerationStructures.ForEach([this](RhiAccelerationStructure, VulkanAccelerationStructure &structure) {
        Vk.vkDestroyAccelerationStructureKHR(_device, structure.Structure, nullptr);
    });
    _heaps.ForEach([this](RhiHeap, VulkanHeap &heap) { FreeBlock(heap.Block); });
    _queryPools.ForEach(
        [this](RhiQueryPool, VulkanQueryPool &pool) { Vk.vkDestroyQueryPool(_device, pool.Pool, nullptr); });
    _fences.ForEach([this](RhiFence, VulkanFence &fence) { Vk.vkDestroySemaphore(_device, fence.Semaphore, nullptr); });
    if (_pipelineCache != VK_NULL_HANDLE)
    {
        Vk.vkDestroyPipelineCache(_device, _pipelineCache, nullptr);
    }
    _breadcrumbs.Reset();
    delete _views;
    delete _samplers;
    _allocator.Reset();
    Vk.vkDestroyDevice(_device, nullptr);
}

Result<void, RhiError> VulkanDevice::Initialize(Span<const char *const> requestedExtensions,
                                                Span<VkBaseOutStructure *const> chained)
{
    if (!CreateDevice(requestedExtensions, chained))
    {
        return Failure(RhiError::Unsupported);
    }
    ReadLimits();
    CreateMemoryPools();

    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        Result<RhiFence, RhiError> fence = CreateFence(0, QueueName(static_cast<RhiQueueType>(queue)));
        if (fence.IsError())
        {
            return Failure(fence.Error());
        }
        _queues[queue].Fence = fence.Value();
        _queues[queue].Timeline = _fences.Get(fence.Value()).Semaphore;
    }

    Result<void, RhiError> heaps = CreateDescriptorHeaps();
    if (heaps.IsError())
    {
        return heaps;
    }
    if (_desc.Breadcrumbs && _breadcrumbMarkers)
    {
        Result<void, RhiError> breadcrumbs = CreateBreadcrumbBuffer();
        if (breadcrumbs.IsError())
        {
            return breadcrumbs;
        }
    }

    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = _desc.PipelineCache.Count();
    cacheInfo.pInitialData = _desc.PipelineCache.Data();
    VkResult result = Vk.vkCreatePipelineCache(_device, &cacheInfo, nullptr, &_pipelineCache);
    _desc.PipelineCache = {};
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }

    const struct
    {
        RhiIndirectCommandKind Kind;
        uint32 Stride;
    } plain[] = {
        {RhiIndirectCommandKind::Draw, sizeof(RhiDrawArguments)},
        {RhiIndirectCommandKind::DrawIndexed, sizeof(RhiDrawIndexedArguments)},
        {RhiIndirectCommandKind::DrawMesh, sizeof(RhiDispatchArguments)},
        {RhiIndirectCommandKind::Dispatch, sizeof(RhiDispatchArguments)},
        {RhiIndirectCommandKind::DispatchRays, sizeof(RhiDispatchRaysArguments)},
    };
    for (const auto &entry : plain)
    {
        _plainSignatures[static_cast<uint32>(entry.Kind)] =
            _commandSignatures.Allocate(VulkanCommandSignature{entry.Kind, entry.Stride});
    }
    return {};
}

bool VulkanDevice::CreateDevice(Span<const char *const> requestedExtensions, Span<VkBaseOutStructure *const> chained)
{
    const VkPhysicalDevice physicalDevice = _adapter.PhysicalDevice;
    uint32 familyCount = 0;
    Vk.vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &familyCount, nullptr);
    List<VkQueueFamilyProperties2> families;
    families.Resize(familyCount);
    for (VkQueueFamilyProperties2 &family : families)
    {
        family = {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2};
    }
    Vk.vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &familyCount, families.Data());

    const auto findFamily = [&](VkQueueFlags required, VkQueueFlags excluded) {
        for (uint32 i = 0; i < familyCount; ++i)
        {
            const VkQueueFlags flags = families[i].queueFamilyProperties.queueFlags;
            if ((flags & required) == required && (flags & excluded) == 0)
            {
                return i;
            }
        }
        return ~0u;
    };
    _families.Graphics = findFamily(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0);
    _families.Compute = findFamily(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
    _families.Copy = findFamily(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    if (_families.Graphics == ~0u)
    {
        MARLIN_LOG(LogRhi, Error, "{0} has no graphics queue", _adapter.Info.Name);
        return false;
    }
    if (_families.Compute == ~0u)
    {
        _families.Compute = _families.Graphics;
    }
    if (_families.Copy == ~0u)
    {
        _families.Copy = _families.Compute;
    }

    const uint32 queueFamilies[RhiQueueTypeCount] = {_families.Graphics, _families.Compute, _families.Copy};
    uint32 queueIndices[RhiQueueTypeCount] = {};
    uint32 used[64] = {};
    VulkanBackend::QueueLocation deviceQueues[RhiQueueTypeCount];
    VulkanBackend::QueueLocation additionalQueues[RhiQueueTypeCount];
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        const uint32 family = queueFamilies[queue];
        const uint32 available = families[family].queueFamilyProperties.queueCount;
        queueIndices[queue] = used[family] < available ? used[family]++ : available - 1;
        deviceQueues[queue] = {family, queueIndices[queue], 1};
    }
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        const uint32 family = queueFamilies[queue];
        const uint32 available = families[family].queueFamilyProperties.queueCount;
        const uint32 wanted = _backend.GetRequestedAdditionalQueues(static_cast<RhiQueueType>(queue));
        const uint32 count = wanted < available - used[family] ? wanted : available - used[family];
        if (count < wanted)
        {
            MARLIN_LOG(LogRhi, Warning, "Queue family {0} has room for {1} of {2} additional queues", family, count,
                       wanted);
        }
        additionalQueues[queue] = {family, used[family], count};
        used[family] += count;
    }
    _backend.SetQueueLocations(deviceQueues, additionalQueues);
    const float32 priorities[64] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    List<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32 family = 0; family < familyCount && family < 64; ++family)
    {
        if (used[family] == 0)
        {
            continue;
        }
        VkDeviceQueueCreateInfo &info = queueInfos.AddDefaulted();
        info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        info.queueFamilyIndex = family;
        info.queueCount = used[family];
        info.pQueuePriorities = priorities;
    }

    List<const char *> extensions;
    for (const char *extension : VulkanRequiredExtensions)
    {
        extensions.Add(extension);
    }
    for (const char *extension : VulkanOptionalExtensions)
    {
        if (_adapter.HasExtension(extension))
        {
            extensions.Add(extension);
        }
    }
    for (const char *extension : requestedExtensions)
    {
        if (!extensions.Contains(extension))
        {
            extensions.Add(extension);
        }
    }

    VulkanFeatureChain features;
    features.Query(_adapter);
    features.DisableUnwanted();
    auto *last = reinterpret_cast<VkBaseOutStructure *>(&features.Features);
    while (last->pNext != nullptr)
    {
        last = last->pNext;
    }
    for (VkBaseOutStructure *structure : chained)
    {
        structure->pNext = nullptr;
        last->pNext = structure;
        last = structure;
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features.Features;
    info.queueCreateInfoCount = static_cast<uint32>(queueInfos.Count());
    info.pQueueCreateInfos = queueInfos.Data();
    info.enabledExtensionCount = static_cast<uint32>(extensions.Count());
    info.ppEnabledExtensionNames = extensions.Data();
    const VkResult result = Vk.vkCreateDevice(physicalDevice, &info, nullptr, &_device);
    if (result != VK_SUCCESS)
    {
        MARLIN_LOG(LogRhi, Error, "vkCreateDevice failed on {0} ({1})", _adapter.Info.Name, static_cast<int32>(result));
        return false;
    }
    Vk.LoadDevice(_device);

    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        VulkanQueue &entry = _queues[queue];
        entry.Family = queueFamilies[queue];
        Vk.vkGetDeviceQueue(_device, entry.Family, queueIndices[queue], &entry.Queue);
        const VkQueueFlags flags = families[entry.Family].queueFamilyProperties.queueFlags;
        entry.SparseBinding = (flags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
        entry.ResolvesQueries = (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
        entry.TimestampValidBits = families[entry.Family].queueFamilyProperties.timestampValidBits;
    }

    _debugNames = _desc.DebugNames && _backend.HasDebugUtils();
    _breadcrumbMarkers = _adapter.HasExtension(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    _memoryBudget = _adapter.HasExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    _calibratedTimestamps = _adapter.HasExtension(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    _presentWait = features.PresentId.presentId == VK_TRUE && features.PresentWait.presentWait == VK_TRUE;
    _conservativeRasterization = _adapter.HasExtension(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
    _traceRaysIndirect2 = features.RayTracingMaintenance1.rayTracingPipelineTraceRaysIndirect2 == VK_TRUE;
    _generatedCommands = features.GeneratedCommands.deviceGeneratedCommands == VK_TRUE;
    _variableRateShading = features.ShadingRate.pipelineFragmentShadingRate == VK_TRUE &&
                           features.ShadingRate.primitiveFragmentShadingRate == VK_TRUE &&
                           features.ShadingRate.attachmentFragmentShadingRate == VK_TRUE;

    const VkPhysicalDeviceFeatures &core = features.Features.features;
    if (core.sparseBinding == VK_TRUE && core.sparseResidencyBuffer == VK_TRUE &&
        core.sparseResidencyImage2D == VK_TRUE && _queues[0].SparseBinding)
    {
        _features.Add(Name(RhiFeatures::SparseResources));
    }
    if (_variableRateShading)
    {
        _features.Add(Name(RhiFeatures::VariableRateShading));
    }
    if (features.ImageAtomic64.shaderImageInt64Atomics == VK_TRUE)
    {
        _features.Add(Name(RhiFeatures::ImageAtomic64));
    }
    if (features.InvocationReorder.rayTracingInvocationReorder == VK_TRUE)
    {
        _features.Add(Name(RhiFeatures::ShaderExecutionReorder));
    }
    if (features.CooperativeVector.cooperativeVector == VK_TRUE)
    {
        _features.Add(Name(RhiFeatures::CooperativeVector));
    }
    if (features.CooperativeVector.cooperativeVectorTraining == VK_TRUE)
    {
        _features.Add(Name(RhiFeatures::CooperativeVectorTraining));
    }
    if (features.CooperativeMatrix.cooperativeMatrix == VK_TRUE)
    {
        _features.Add(Name(RhiFeatures::CooperativeMatrix));
    }
    if (_backend.HasSwapchainColorSpaces())
    {
        _features.Add(Name(RhiFeatures::HdrOutput));
    }
    return true;
}

void VulkanDevice::ReadLimits()
{
    VulkanPropertyChain properties;
    properties.Query(_adapter);
    const VkPhysicalDeviceLimits &core = properties.Properties.properties.limits;
    _limits.MaxTextureSize2D = core.maxImageDimension2D;
    _limits.MaxTextureSize3D = core.maxImageDimension3D;
    _limits.MaxTextureSizeCube = core.maxImageDimensionCube;
    _limits.MaxTextureArraySize = core.maxImageArrayLayers;
    _limits.MinWaveSize = properties.Vulkan13.minSubgroupSize;
    _limits.MaxWaveSize = properties.Vulkan13.maxSubgroupSize;
    _limits.BufferPlacementAlignment = 256;
    _limits.TexturePlacementAlignment = static_cast<uint32>(
        core.bufferImageGranularity > 65536 ? core.bufferImageGranularity : 65536);
    _limits.ConstantDataAlignment = static_cast<uint32>(core.minUniformBufferOffsetAlignment);
    _limits.StructuredBufferOffsetAlignment = static_cast<uint32>(core.minStorageBufferOffsetAlignment);
    _limits.CopyRowPitchAlignment = static_cast<uint32>(core.optimalBufferCopyRowPitchAlignment);
    _limits.CopyOffsetAlignment = static_cast<uint32>(core.optimalBufferCopyOffsetAlignment);
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &rayTracing = properties.RayTracingPipeline;
    _limits.ShaderGroupHandleSize = rayTracing.shaderGroupHandleSize;
    _limits.ShaderGroupHandleAlignment = rayTracing.shaderGroupHandleAlignment;
    _limits.ShaderTableAlignment = rayTracing.shaderGroupBaseAlignment;
    _limits.MaxRayRecursionDepth = rayTracing.maxRayRecursionDepth;
    _limits.ShadingRateTileSize = properties.ShadingRate.maxFragmentShadingRateAttachmentTexelSize.width;
    _limits.MaxMeshOutputVertices = properties.MeshShader.maxMeshOutputVertices;
    _limits.MaxMeshOutputPrimitives = properties.MeshShader.maxMeshOutputPrimitives;
    _limits.MaxTaskPayloadBytes = properties.MeshShader.maxTaskPayloadSize;
    for (uint32 i = 0; i < 3; ++i)
    {
        _limits.MaxComputeGroupSize[i] = core.maxComputeWorkGroupSize[i];
    }
    _limits.MaxComputeGroupInvocations = core.maxComputeWorkGroupInvocations;
    _limits.MaxComputeSharedMemory = core.maxComputeSharedMemorySize;
    _generatedCommandStages = _generatedCommands
                                  ? properties.GeneratedCommands.supportedIndirectCommandsShaderStagesPipelineBinding
                                  : 0;
    if (_generatedCommandStages != 0)
    {
        _features.Add(Name(RhiFeatures::IndirectPipelineSwitch));
    }
    const uint64 frequency = static_cast<uint64>(1.0e9 / static_cast<float64>(core.timestampPeriod));
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        _limits.TimestampFrequency[queue] = _queues[queue].TimestampValidBits != 0 ? frequency : 0;
        _limits.ResolvesQueries[queue] = _queues[queue].ResolvesQueries;
    }

    const VkPhysicalDeviceDescriptorHeapPropertiesEXT &heap = properties.DescriptorHeap;
    uint64 stride = 0;
    for (const VkDescriptorType type :
         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
          VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR})
    {
        const uint64 size = Vk.vkGetPhysicalDeviceDescriptorSizeEXT(_adapter.PhysicalDevice, type);
        stride = size > stride ? size : stride;
    }
    const uint64 alignment = heap.imageDescriptorAlignment > heap.bufferDescriptorAlignment
                                 ? heap.imageDescriptorAlignment
                                 : heap.bufferDescriptorAlignment;
    _resourceStride = static_cast<uint32>(AlignUp(stride, alignment));
    _samplerStride = static_cast<uint32>(AlignUp(heap.samplerDescriptorSize, heap.samplerDescriptorAlignment));
    _resourceHeapReserved = heap.minResourceHeapReservedRange;
    _samplerHeapReserved = heap.minSamplerHeapReservedRange;

    uint64 resourceCapacity = (heap.maxResourceHeapSize - _resourceHeapReserved) / _resourceStride;
    resourceCapacity = resourceCapacity < MaxResourceDescriptors ? resourceCapacity : MaxResourceDescriptors;
    uint64 samplerCapacity = (heap.maxSamplerHeapSize - _samplerHeapReserved) / _samplerStride;
    samplerCapacity = samplerCapacity < MaxSamplerDescriptors ? samplerCapacity : MaxSamplerDescriptors;
    _views = new RhiObjectPool<VulkanView, RhiResourceView>(static_cast<uint32>(resourceCapacity) - 1024);
    _samplers = new RhiObjectPool<VulkanSampler, RhiSampler, 64>(static_cast<uint32>(samplerCapacity) - 64);
    _limits.MaxResourceDescriptors = _views->GetCapacity();
    _limits.MaxSamplerDescriptors = _samplers->GetCapacity();
    _limits.ResourceDescriptorStride = _resourceStride;
    _limits.SamplerDescriptorStride = _samplerStride;
}

void VulkanDevice::CreateMemoryPools()
{
    VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    Vk.vkGetPhysicalDeviceMemoryProperties2(_adapter.PhysicalDevice, &memory);
    _memoryProperties = memory.memoryProperties;
    _reBar = _adapter.Info.HostVisibleDeviceMemoryBytes > uint64{256} * 1024 * 1024;
    List<GpuMemoryPoolDesc> pools;
    for (uint32 type = 0; type < _memoryProperties.memoryTypeCount; ++type)
    {
        const VkMemoryPropertyFlags flags = _memoryProperties.memoryTypes[type].propertyFlags;
        GpuMemoryPoolDesc desc;
        const bool host = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        desc.BlockSize = host ? PlatformGpuMemoryConfig::HostBlockSize : PlatformGpuMemoryConfig::DeviceBlockSize;
        desc.Location = !host                                               ? RhiMemoryLocation::Device
                        : (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 ? RhiMemoryLocation::Readback
                                                                            : RhiMemoryLocation::Upload;
        pools.Add(desc);
        pools.Add(desc);
    }
    _allocator = MakeUnique<GpuMemoryAllocator>(*this, pools);
}

uint32 VulkanDevice::FindMemoryType(uint32 typeBits, RhiMemoryLocation location, bool staging) const
{
    struct Preference
    {
        VkMemoryPropertyFlags Required;
        VkMemoryPropertyFlags Avoided;
    };
    constexpr VkMemoryPropertyFlags DeviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags HostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr VkMemoryPropertyFlags HostCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    constexpr VkMemoryPropertyFlags HostCached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    Preference preferences[3] = {};
    uint32 count = 0;
    switch (location)
    {
    case RhiMemoryLocation::Device:
        preferences[count++] = {DeviceLocal, HostVisible};
        preferences[count++] = {DeviceLocal, 0};
        break;
    case RhiMemoryLocation::Upload:
        // Staging is only copied from: in GPU memory it would save nothing and take ReBAR space.
        if ((_reBar && !staging) || _adapter.Info.UnifiedMemory)
        {
            preferences[count++] = {DeviceLocal | HostVisible | HostCoherent, HostCached};
        }
        preferences[count++] = {HostVisible | HostCoherent, HostCached | DeviceLocal};
        preferences[count++] = {HostVisible | HostCoherent, 0};
        break;
    case RhiMemoryLocation::Readback:
        preferences[count++] = {HostVisible | HostCoherent | HostCached, 0};
        preferences[count++] = {HostVisible | HostCoherent, 0};
        break;
    }
    for (uint32 p = 0; p < count; ++p)
    {
        for (uint32 type = 0; type < _memoryProperties.memoryTypeCount; ++type)
        {
            const VkMemoryPropertyFlags flags = _memoryProperties.memoryTypes[type].propertyFlags;
            if ((typeBits & (1u << type)) != 0 && (flags & preferences[p].Required) == preferences[p].Required &&
                (flags & preferences[p].Avoided) == 0)
            {
                return type;
            }
        }
    }
    return ~0u;
}

Result<GpuMemoryBlock, RhiError> VulkanDevice::AllocateBlock(uint32 pool, uint64 size, const void *dedicatedResource)
{
    const uint32 type = pool / 2;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    if (dedicatedResource != nullptr)
    {
        dedicated = *static_cast<const VkMemoryDedicatedAllocateInfo *>(dedicatedResource);
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = nullptr;
        flags.pNext = &dedicated;
    }
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.pNext = &flags;
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = Vk.vkAllocateMemory(_device, &info, nullptr, &memory);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    GpuMemoryBlock block;
    block.Native = reinterpret_cast<uint64>(memory);
    block.Size = size;
    block.Pool = pool;
    if ((_memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
    {
        void *mapped = nullptr;
        if (Vk.vkMapMemory(_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
        {
            block.Mapped = static_cast<uint8 *>(mapped);
        }
    }
    return block;
}

void VulkanDevice::FreeBlock(const GpuMemoryBlock &block)
{
    Vk.vkFreeMemory(_device, reinterpret_cast<VkDeviceMemory>(block.Native), nullptr);
}

Result<void, RhiError> VulkanDevice::CreateDescriptorHeaps()
{
    VulkanPropertyChain properties;
    properties.Query(_adapter);
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT &heap = properties.DescriptorHeap;

    RhiBufferDesc desc;
    desc.Location = RhiMemoryLocation::Upload;
    desc.Size = AlignUp(uint64{_views->GetCapacity()} * _resourceStride, heap.resourceHeapAlignment) +
                _resourceHeapReserved;
    desc.DebugName = "ResourceHeap";
    Result<RhiBuffer, RhiError> resources = CreateRawBuffer(desc, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT);
    if (resources.IsError())
    {
        return Failure(resources.Error());
    }
    _resourceHeap = resources.Value();

    desc.Size = AlignUp(uint64{_samplers->GetCapacity()} * _samplerStride, heap.samplerHeapAlignment) +
                _samplerHeapReserved;
    desc.DebugName = "SamplerHeap";
    Result<RhiBuffer, RhiError> samplers = CreateRawBuffer(desc, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT);
    if (samplers.IsError())
    {
        return Failure(samplers.Error());
    }
    _samplerHeap = samplers.Value();
    if (_buffers.Get(_resourceHeap).Mapped == nullptr || _buffers.Get(_samplerHeap).Mapped == nullptr)
    {
        MARLIN_LOG(LogRhi, Error, "The descriptor heaps are not host-visible");
        return Failure(RhiError::Unsupported);
    }
    return {};
}

void VulkanDevice::BindHeaps(VkCommandBuffer commandBuffer) const
{
    const VulkanBuffer &resources = _buffers.Get(_resourceHeap);
    VkBindHeapInfoEXT info{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    info.heapRange = {resources.Address, resources.Desc.Size};
    info.reservedRangeOffset = resources.Desc.Size - _resourceHeapReserved;
    info.reservedRangeSize = _resourceHeapReserved;
    Vk.vkCmdBindResourceHeapEXT(commandBuffer, &info);
    const VulkanBuffer &samplers = _buffers.Get(_samplerHeap);
    info.heapRange = {samplers.Address, samplers.Desc.Size};
    info.reservedRangeOffset = samplers.Desc.Size - _samplerHeapReserved;
    info.reservedRangeSize = _samplerHeapReserved;
    Vk.vkCmdBindSamplerHeapEXT(commandBuffer, &info);
}

void VulkanDevice::WriteResourceDescriptor(uint32 slot, const VkResourceDescriptorInfoEXT &info)
{
    const VulkanBuffer &heap = _buffers.Get(_resourceHeap);
    VkHostAddressRangeEXT destination{heap.Mapped + uint64{slot} * _resourceStride, _resourceStride};
    Vk.vkWriteResourceDescriptorsEXT(_device, 1, &info, &destination);
}

Result<void, RhiError> VulkanDevice::CreateBreadcrumbBuffer()
{
    RhiBufferDesc desc;
    desc.Size = uint64{BreadcrumbSlots} * sizeof(uint32);
    desc.Location = RhiMemoryLocation::Readback;
    desc.Usage = RhiBufferUsage::Marker;
    desc.DebugName = "Breadcrumbs";
    Result<RhiBuffer, RhiError> buffer = CreateRawBuffer(desc, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (buffer.IsError())
    {
        return Failure(buffer.Error());
    }
    _breadcrumbBuffer = buffer.Value();
    const VulkanBuffer &entry = _buffers.Get(_breadcrumbBuffer);
    _breadcrumbs =
        MakeUnique<RhiBreadcrumbs>(reinterpret_cast<uint32 *>(entry.Mapped), entry.Address, BreadcrumbSlots);
    return {};
}

VkBuffer VulkanDevice::GetBreadcrumbBuffer() const
{
    return _buffers.Get(_breadcrumbBuffer).Buffer;
}

Name VulkanDevice::GetBackendName() const
{
    return _backend.GetName();
}

RhiNativeHandles VulkanDevice::GetNativeHandles() const
{
    RhiNativeHandles handles;
    handles.Backend = _backend.GetName();
    handles.Instance = _backend.GetInstance();
    handles.Adapter = _adapter.PhysicalDevice;
    handles.Device = _device;
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        handles.Queues[queue] = _queues[queue].Queue;
    }
    return handles;
}

RhiMemoryBudget VulkanDevice::GetMemoryBudget(RhiMemoryLocation location) const
{
    RhiMemoryBudget result;
    if (!_memoryBudget)
    {
        return result;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    memory.pNext = &budget;
    Vk.vkGetPhysicalDeviceMemoryProperties2(_adapter.PhysicalDevice, &memory);
    const bool device = location == RhiMemoryLocation::Device;
    for (uint32 heap = 0; heap < memory.memoryProperties.memoryHeapCount; ++heap)
    {
        const bool local = (memory.memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        if (local == device)
        {
            result.Budget += budget.heapBudget[heap];
            result.Usage += budget.heapUsage[heap];
        }
    }
    return result;
}

void VulkanDevice::GetMemoryStats(List<RhiMemoryPoolStats> &pools) const
{
    for (uint32 pool = 0; pool < _allocator->GetPoolCount(); ++pool)
    {
        const GpuMemoryPoolStats stats = _allocator->GetStats(pool);
        if (stats.BlockCount == 0)
        {
            continue;
        }
        RhiMemoryPoolStats &entry = pools.AddDefaulted();
        entry.Location = _allocator->GetPoolDesc(pool).Location;
        entry.Textures = pool % 2 == 1;
        entry.BlockCount = stats.BlockCount;
        entry.AllocationCount = stats.AllocationCount;
        entry.BlockBytes = stats.BlockBytes;
        entry.UsedBytes = stats.UsedBytes;
    }
}

bool VulkanDevice::CheckLost(VkResult result)
{
    if (result != VK_ERROR_DEVICE_LOST)
    {
        return false;
    }
    if (!_lost.Exchange(true) && _desc.OnDeviceLost != nullptr)
    {
        _desc.OnDeviceLost(_desc.DeviceLostContext);
    }
    return true;
}

void VulkanDevice::SetObjectName(VkObjectType type, uint64 handle, StringView name) const
{
    if (!_debugNames || name.IsEmpty() || handle == 0)
    {
        return;
    }
    StringBuilder<256> text;
    text.Append(name);
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = text.CStr();
    Vk.vkSetDebugUtilsObjectNameEXT(_device, &info);
}

void VulkanDevice::SetDebugName(RhiBuffer buffer, StringView name)
{
    if (const VulkanBuffer *entry = _buffers.Find(buffer))
    {
        SetObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(entry->Buffer), name);
    }
}

void VulkanDevice::SetDebugName(RhiTexture texture, StringView name)
{
    if (const VulkanTexture *entry = _textures.Find(texture))
    {
        SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64>(entry->Image), name);
    }
}

void VulkanDevice::WriteCrashReport(StringWriter &output) const
{
    FormatTo(output, "Adapter: {0} (vendor {1}, device {2}), driver {3}\n", _adapter.Info.Name,
             _adapter.Info.VendorId, _adapter.Info.DeviceId, _adapter.Info.DriverVersion);
    const RhiQueueValues completed = GetCompletedValues();
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        FormatTo(output, "{0} queue: submitted {1}, completed {2}\n", QueueName(static_cast<RhiQueueType>(queue)),
                 _queues[queue].Submitted.Load(MemoryOrder::Relaxed), completed.Values[queue]);
    }
    if (_breadcrumbs)
    {
        _breadcrumbs->WriteReport(output);
    }
    else
    {
        output.Append("Breadcrumbs are off.\n");
    }
}

bool VulkanDevice::ReadQueries(RhiQueryPool pool, uint32 first, uint32 count, Span<uint64> output)
{
    const VulkanQueryPool *entry = _queryPools.Find(pool);
    if (entry == nullptr || first + count > entry->Count)
    {
        return false;
    }
    const uint32 values = entry->Kind == RhiQueryKind::PipelineStatistics ? 9 : 1;
    if (output.Count() < usize{count} * values)
    {
        return false;
    }
    const VkResult result = Vk.vkGetQueryPoolResults(_device, entry->Pool, first, count, output.Count() * sizeof(uint64),
                                                     output.Data(), values * sizeof(uint64), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
    {
        CheckLost(result);
        return false;
    }
    Vk.vkResetQueryPool(_device, entry->Pool, first, count);
    return true;
}

void VulkanDevice::GetCalibratedTimestamps(RhiQueueType /*queue*/, uint64 &cpuTicks, uint64 &gpuTicks) const
{
    if (!_calibratedTimestamps)
    {
        cpuTicks = PlatformTime::Ticks();
        gpuTicks = 0;
        return;
    }
    VkCalibratedTimestampInfoKHR infos[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                             {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].timeDomain = VulkanPlatform::GetHostTimeDomain();
    uint64 timestamps[2] = {};
    uint64 deviation = 0;
    Vk.vkGetCalibratedTimestampsKHR(_device, 2, infos, timestamps, &deviation);
    gpuTicks = timestamps[0];
    cpuTicks = timestamps[1];
}

Result<RhiFence, RhiError> VulkanDevice::CreateFence(uint64 initialValue, StringView debugName)
{
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initialValue;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = Vk.vkCreateSemaphore(_device, &info, nullptr, &semaphore);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64>(semaphore), debugName);
    const RhiFence fence = _fences.Allocate(VulkanFence{semaphore});
    if (!fence.IsValid())
    {
        Vk.vkDestroySemaphore(_device, semaphore, nullptr);
        return Failure(RhiError::OutOfHostMemory);
    }
    return fence;
}

void VulkanDevice::Destroy(RhiFence fence)
{
    DeferRelease(fence, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiFence handle{static_cast<uint32>(payload), static_cast<uint32>(payload >> 32)};
        if (const VulkanFence *entry = device->_fences.Find(handle))
        {
            Vk.vkDestroySemaphore(device->_device, entry->Semaphore, nullptr);
            device->_fences.Free(handle);
        }
    });
}

uint64 VulkanDevice::GetCompletedValue(RhiFence fence) const
{
    uint64 value = 0;
    if (const VulkanFence *entry = _fences.Find(fence))
    {
        Vk.vkGetSemaphoreCounterValue(_device, entry->Semaphore, &value);
    }
    return value;
}

bool VulkanDevice::Wait(RhiFence fence, uint64 value, uint32 timeout)
{
    const VulkanFence *entry = _fences.Find(fence);
    if (entry == nullptr)
    {
        return false;
    }
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &entry->Semaphore;
    info.pValues = &value;
    const uint64 nanoseconds = timeout == 0xFFFFFFFFu ? ~uint64{0} : uint64{timeout} * 1000000;
    const VkResult result = Vk.vkWaitSemaphores(_device, &info, nanoseconds);
    CheckLost(result);
    return result == VK_SUCCESS;
}

void VulkanDevice::Signal(RhiFence fence, uint64 value)
{
    if (const VulkanFence *entry = _fences.Find(fence))
    {
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = entry->Semaphore;
        info.value = value;
        Vk.vkSignalSemaphore(_device, &info);
    }
}

RhiFence VulkanDevice::GetQueueFence(RhiQueueType queue) const
{
    return _queues[static_cast<uint32>(queue)].Fence;
}

RhiQueueValues VulkanDevice::GetSubmittedValues() const
{
    RhiQueueValues values;
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        values.Values[queue] = _queues[queue].Submitted.Load(MemoryOrder::Acquire);
    }
    return values;
}

RhiQueueValues VulkanDevice::GetCompletedValues() const
{
    RhiQueueValues values;
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        Vk.vkGetSemaphoreCounterValue(_device, _queues[queue].Timeline, &values.Values[queue]);
    }
    return values;
}

template <typename THandle>
void VulkanDevice::DeferRelease(THandle handle, RhiDeferredReleaseQueue::ReleaseFunction release)
{
    if (!handle.IsValid())
    {
        return;
    }
    const uint64 payload = (uint64{handle.Generation} << 32) | handle.Index;
    _releases.Enqueue(GetSubmittedValues(), release, this, payload);
}

RhiCommandList *VulkanDevice::BeginCommandList(RhiQueueType queue)
{
    const uint32 index = static_cast<uint32>(queue);
    VulkanCommandList *list = nullptr;
    {
        const ScopeLock lock(_listLock);
        if (!_freeLists[index].IsEmpty())
        {
            list = _freeLists[index].Pop();
        }
    }
    if (list == nullptr)
    {
        list = new VulkanCommandList(*this, queue, _queues[index].Family);
    }
    RhiBreadcrumbs::Trail *trail = _breadcrumbs ? _breadcrumbs->BeginTrail(queue) : nullptr;
    if (!list->Begin(trail))
    {
        if (trail != nullptr)
        {
            _breadcrumbs->Discard(trail);
        }
        delete list;
        return nullptr;
    }
    return list;
}

void VulkanDevice::Recycle(VulkanCommandList *list)
{
    if (list->GetTrail() != nullptr)
    {
        _breadcrumbs->Discard(list->GetTrail());
        list->TakeTrail();
    }
    for (const VulkanListRelease &release : list->GetReleases())
    {
        if (release.QueryPool != VK_NULL_HANDLE)
        {
            Vk.vkDestroyQueryPool(_device, release.QueryPool, nullptr);
        }
        if (release.Buffer.IsValid())
        {
            Destroy(release.Buffer);
        }
    }
    list->GetReleases().Clear();
    list->Reset();
    const ScopeLock lock(_listLock);
    _freeLists[static_cast<uint32>(list->GetQueueType())].Add(list);
}

void VulkanDevice::DiscardCommandList(RhiCommandList *list)
{
    auto *vulkanList = static_cast<VulkanCommandList *>(list);
    vulkanList->End();
    Recycle(vulkanList);
}

void VulkanDevice::AddPendingWait(VulkanQueue &queue, VkSemaphore semaphore, uint64 value,
                                  VkPipelineStageFlags2 stages)
{
    // A timeline wait for the larger value at the union of the stages covers both waits.
    for (VkSemaphoreSubmitInfo &wait : queue.PendingWaits)
    {
        if (wait.semaphore == semaphore)
        {
            wait.value = value > wait.value ? value : wait.value;
            wait.stageMask |= stages;
            return;
        }
    }
    VkSemaphoreSubmitInfo &wait = queue.PendingWaits.AddDefaulted();
    wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = semaphore;
    wait.value = value;
    wait.stageMask = stages;
}

void VulkanDevice::ResolvePendingAcquires(VulkanQueue &queue, uint64 value)
{
    for (const VulkanPendingAcquire &acquire : queue.PendingAcquires)
    {
        if (VulkanSwapchain *swapchain = _swapchains.Find(acquire.Swapchain))
        {
            swapchain->AcquireSemaphoreValues[acquire.Slot] = value;
        }
    }
    queue.PendingAcquires.Clear();
}

Result<void, RhiError> VulkanDevice::FlushPendingAcquires()
{
    VulkanQueue &graphics = _queues[static_cast<uint32>(RhiQueueType::Graphics)];
    if (graphics.PendingAcquires.IsEmpty())
    {
        return {};
    }
    uint64 value = 0;
    return SubmitEmpty(graphics, VK_NULL_HANDLE, value);
}

Result<uint64, RhiError> VulkanDevice::Submit(RhiQueueType queueType, const RhiSubmitDesc &desc)
{
    VulkanQueue &queue = _queues[static_cast<uint32>(queueType)];
    InlineList<VkCommandBufferSubmitInfo, 16> commandBuffers;
    for (RhiCommandList *list : desc.CommandLists)
    {
        auto *vulkanList = static_cast<VulkanCommandList *>(list);
        MARLIN_ASSERT(vulkanList->GetQueueType() == queueType);
        const VkResult result = vulkanList->End();
        if (result != VK_SUCCESS)
        {
            return Failure(VulkanConvert::Error(result));
        }
        VkCommandBufferSubmitInfo &info = commandBuffers.AddDefaulted();
        info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        info.commandBuffer = vulkanList->GetCommandBuffer();
    }
    InlineList<VkSemaphoreSubmitInfo, 8> waits;
    waits.AddRange(queue.PendingWaits);
    // The acquired images are first written by graphics work: rendering or compute. Lists on other queues reach them
    // through a wait for this submission (RenderGraph starts imported resources on Graphics).
    for (const VulkanPendingAcquire &acquire : queue.PendingAcquires)
    {
        VkSemaphoreSubmitInfo &info = waits.AddDefaulted();
        info = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        info.semaphore = acquire.Semaphore;
        info.stageMask = VulkanAcquireWaitStages;
    }
    for (const RhiFenceWait &wait : desc.Waits)
    {
        VkSemaphoreSubmitInfo &info = waits.AddDefaulted();
        info = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        info.semaphore = _fences.Get(wait.Fence).Semaphore;
        info.value = wait.Value;
        info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    const uint64 value = queue.Submitted.Load(MemoryOrder::Relaxed) + 1;
    InlineList<VkSemaphoreSubmitInfo, 4> signals;
    for (const RhiFenceSignal &signal : desc.Signals)
    {
        VkSemaphoreSubmitInfo &info = signals.AddDefaulted();
        info = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        info.semaphore = _fences.Get(signal.Fence).Semaphore;
        info.value = signal.Value;
        info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkSemaphoreSubmitInfo &timeline = signals.AddDefaulted();
    timeline = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    timeline.semaphore = queue.Timeline;
    timeline.value = value;
    timeline.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = static_cast<uint32>(waits.Count());
    submit.pWaitSemaphoreInfos = waits.Data();
    submit.commandBufferInfoCount = static_cast<uint32>(commandBuffers.Count());
    submit.pCommandBufferInfos = commandBuffers.Data();
    submit.signalSemaphoreInfoCount = static_cast<uint32>(signals.Count());
    submit.pSignalSemaphoreInfos = signals.Data();
    const VkResult result = Vk.vkQueueSubmit2(queue.Queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
    {
        CheckLost(result);
        return Failure(VulkanConvert::Error(result));
    }
    queue.Submitted.Store(value, MemoryOrder::Release);
    queue.PendingWaits.Clear();
    ResolvePendingAcquires(queue, value);
    for (RhiCommandList *list : desc.CommandLists)
    {
        auto *vulkanList = static_cast<VulkanCommandList *>(list);
        if (RhiBreadcrumbs::Trail *trail = vulkanList->GetTrail())
        {
            _breadcrumbs->Submitted(*trail, value);
            vulkanList->TakeTrail();
        }
        _inFlight[static_cast<uint32>(queueType)].Add({vulkanList, value});
    }
    return value;
}

Result<void, RhiError> VulkanDevice::SubmitEmpty(VulkanQueue &queue, VkSemaphore signalBinary, uint64 &signaledValue)
{
    InlineList<VkSemaphoreSubmitInfo, 8> waits;
    waits.AddRange(queue.PendingWaits);
    for (const VulkanPendingAcquire &acquire : queue.PendingAcquires)
    {
        VkSemaphoreSubmitInfo &info = waits.AddDefaulted();
        info = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        info.semaphore = acquire.Semaphore;
        info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    const uint64 value = queue.Submitted.Load(MemoryOrder::Relaxed) + 1;
    VkSemaphoreSubmitInfo signals[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
                                        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    signals[0].semaphore = queue.Timeline;
    signals[0].value = value;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].semaphore = signalBinary;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = static_cast<uint32>(waits.Count());
    submit.pWaitSemaphoreInfos = waits.Data();
    submit.signalSemaphoreInfoCount = signalBinary != VK_NULL_HANDLE ? 2 : 1;
    submit.pSignalSemaphoreInfos = signals;
    const VkResult result = Vk.vkQueueSubmit2(queue.Queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
    {
        CheckLost(result);
        return Failure(VulkanConvert::Error(result));
    }
    queue.Submitted.Store(value, MemoryOrder::Release);
    queue.PendingWaits.Clear();
    ResolvePendingAcquires(queue, value);
    signaledValue = value;
    return {};
}

Result<void, RhiError> VulkanDevice::UpdateTileMappings(RhiQueueType queueType, Span<const RhiTileMapping> mappings)
{
    VulkanQueue &queue = _queues[static_cast<uint32>(queueType)];
    if (!queue.SparseBinding)
    {
        return Failure(RhiError::Unsupported);
    }
    List<VkSparseImageMemoryBind> imageBinds;
    List<VkSparseMemoryBind> opaqueBinds;
    List<VkSparseImageMemoryBindInfo> imageInfos;
    List<VkSparseImageOpaqueMemoryBindInfo> opaqueInfos;
    List<VkSparseBufferMemoryBindInfo> bufferInfos;
    imageBinds.Reserve(mappings.Count());
    opaqueBinds.Reserve(mappings.Count());
    for (const RhiTileMapping &mapping : mappings)
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (const VulkanHeap *heap = _heaps.Find(mapping.Heap))
        {
            memory = reinterpret_cast<VkDeviceMemory>(heap->Block.Native);
        }
        if (const VulkanBuffer *buffer = _buffers.Find(mapping.Buffer))
        {
            VkSparseMemoryBind &bind = opaqueBinds.AddDefaulted();
            bind.resourceOffset = mapping.BufferOffset;
            bind.size = mapping.BufferSize;
            bind.memory = memory;
            bind.memoryOffset = mapping.HeapOffset;
            bufferInfos.Add({buffer->Buffer, 1, &bind});
            continue;
        }
        const VulkanTexture *texture = _textures.Find(mapping.Texture);
        if (texture == nullptr)
        {
            continue;
        }
        const RhiTextureRegion &region = mapping.Region;
        if (region.Mip >= texture->Tiles.FirstPackedMip)
        {
            VkSparseMemoryBind &bind = opaqueBinds.AddDefaulted();
            bind.resourceOffset =
                texture->MipTailOffset + (texture->SingleMipTail ? 0 : region.Slice * texture->MipTailStride);
            bind.size = texture->MipTailSize;
            bind.memory = memory;
            bind.memoryOffset = mapping.HeapOffset;
            opaqueInfos.Add({texture->Image, 1, &bind});
            continue;
        }
        const uint32 mipWidth = (texture->Desc.Width >> region.Mip) > 0 ? texture->Desc.Width >> region.Mip : 1;
        const uint32 mipHeight = (texture->Desc.Height >> region.Mip) > 0 ? texture->Desc.Height >> region.Mip : 1;
        const RhiTileInfo &tiles = texture->Tiles;
        VkSparseImageMemoryBind &bind = imageBinds.AddDefaulted();
        bind.subresource = {VulkanConvert::Aspect(texture->Desc.Format, region.Aspect), region.Mip, region.Slice};
        bind.offset = {static_cast<int32>(region.X * tiles.TileWidth), static_cast<int32>(region.Y * tiles.TileHeight),
                       static_cast<int32>(region.Z * tiles.TileDepth)};
        const uint32 width = region.Width * tiles.TileWidth;
        const uint32 height = region.Height * tiles.TileHeight;
        bind.extent = {width < mipWidth - static_cast<uint32>(bind.offset.x) ? width
                                                                              : mipWidth - static_cast<uint32>(bind.offset.x),
                       height < mipHeight - static_cast<uint32>(bind.offset.y)
                           ? height
                           : mipHeight - static_cast<uint32>(bind.offset.y),
                       region.Depth > 0 ? region.Depth * tiles.TileDepth : 1};
        bind.memory = memory;
        bind.memoryOffset = mapping.HeapOffset;
        imageInfos.Add({texture->Image, 1, &bind});
    }
    const uint64 waitValue = queue.Submitted.Load(MemoryOrder::Relaxed);
    const uint64 signalValue = waitValue + 1;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &waitValue;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &signalValue;
    VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    info.pNext = &timeline;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &queue.Timeline;
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores = &queue.Timeline;
    info.bufferBindCount = static_cast<uint32>(bufferInfos.Count());
    info.pBufferBinds = bufferInfos.Data();
    info.imageOpaqueBindCount = static_cast<uint32>(opaqueInfos.Count());
    info.pImageOpaqueBinds = opaqueInfos.Data();
    info.imageBindCount = static_cast<uint32>(imageInfos.Count());
    info.pImageBinds = imageInfos.Data();
    const VkResult result = Vk.vkQueueBindSparse(queue.Queue, 1, &info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
    {
        CheckLost(result);
        return Failure(VulkanConvert::Error(result));
    }
    queue.Submitted.Store(signalValue, MemoryOrder::Release);
    AddPendingWait(queue, queue.Timeline, signalValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    return {};
}

void VulkanDevice::WaitIdle()
{
    CheckLost(Vk.vkDeviceWaitIdle(_device));
}

void VulkanDevice::ProcessCompletions()
{
    const RhiQueueValues completed = GetCompletedValues();
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        List<InFlightList> &inFlight = _inFlight[queue];
        usize kept = 0;
        for (usize i = 0; i < inFlight.Count(); ++i)
        {
            if (inFlight[i].Value <= completed.Values[queue])
            {
                if (inFlight[i].List->GetSkippedCommands() > 0)
                {
                    MARLIN_LOG(LogRhi, Debug, "{0} commands skipped for pending pipelines",
                               inFlight[i].List->GetSkippedCommands());
                }
                Recycle(inFlight[i].List);
            }
            else
            {
                inFlight[kept++] = inFlight[i];
            }
        }
        inFlight.Resize(kept);
    }
    _releases.Process(completed);
    if (_breadcrumbs)
    {
        _breadcrumbs->Retire(completed);
    }
}
} // namespace Marlin
