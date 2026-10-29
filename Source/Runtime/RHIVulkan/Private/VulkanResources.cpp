#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Log.h"
#include "RHI/RhiLog.h"
#include "VulkanConvert.h"
#include "VulkanDevice.h"

namespace Marlin
{
namespace
{
template <typename THandle> THandle Unpack(uint64 payload)
{
    return {static_cast<uint32>(payload), static_cast<uint32>(payload >> 32)};
}

VkBufferUsageFlags BufferUsage(RhiBufferUsage usage)
{
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (HasAnyFlags(usage, RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess))
    {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::IndexBuffer))
    {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::IndirectArguments))
    {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::AccelerationStructure))
    {
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::AccelerationStructureBuildInput))
    {
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::ShaderBindingTable))
    {
        flags |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::CopySource))
    {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::CopyDest | RhiBufferUsage::Marker))
    {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (HasAnyFlags(usage, RhiBufferUsage::IndirectPreprocess))
    {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    return flags;
}

VkImageUsageFlags TextureUsage(RhiTextureUsage usage, bool transient)
{
    VkImageUsageFlags flags = 0;
    if (HasAnyFlags(usage, RhiTextureUsage::ShaderResource))
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::UnorderedAccess))
    {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::RenderTarget))
    {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::DepthStencil))
    {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::ShadingRate))
    {
        flags |= VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::CopySource))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (HasAnyFlags(usage, RhiTextureUsage::CopyDest))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    constexpr VkImageUsageFlags attachments =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (transient && (flags & ~attachments) == 0)
    {
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    return flags;
}

/// What a texture's VkImageCreateInfo points to.
struct ImageInfo
{
    VkImageCreateInfo Create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageFormatListCreateInfo FormatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    VkFormat Formats[8] = {};
};

void BuildImageInfo(const RhiTextureDesc &desc, Span<const uint32> families, ImageInfo &info)
{
    VkImageCreateInfo &create = info.Create;
    create.imageType = desc.Dimension == RhiTextureDimension::Texture1D   ? VK_IMAGE_TYPE_1D
                       : desc.Dimension == RhiTextureDimension::Texture3D ? VK_IMAGE_TYPE_3D
                                                                          : VK_IMAGE_TYPE_2D;
    create.format = VulkanConvert::Format(desc.Format);
    create.extent = {desc.Width, desc.Height,
                     desc.Dimension == RhiTextureDimension::Texture3D ? desc.DepthOrArraySize : 1};
    create.mipLevels = desc.MipCount;
    create.arrayLayers = desc.Dimension == RhiTextureDimension::Texture3D ? 1 : desc.DepthOrArraySize;
    create.samples = VulkanConvert::Samples(desc.SampleCount);
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = TextureUsage(desc.Usage, HasAnyFlags(desc.Flags, RhiResourceFlags::Transient));
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (HasAnyFlags(desc.Usage, RhiTextureUsage::Cube))
    {
        create.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Reserved))
    {
        create.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    }
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Concurrent) && families.Count() > 1)
    {
        create.sharingMode = VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = static_cast<uint32>(families.Count());
        create.pQueueFamilyIndices = families.Data();
    }
    if (!desc.ViewFormats.IsEmpty())
    {
        create.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        uint32 count = 0;
        info.Formats[count++] = create.format;
        for (const RhiFormat format : desc.ViewFormats)
        {
            if (count < 8)
            {
                info.Formats[count++] = VulkanConvert::Format(format);
            }
        }
        info.FormatList.viewFormatCount = count;
        info.FormatList.pViewFormats = info.Formats;
        create.pNext = &info.FormatList;
    }
}

void BuildBufferInfo(const RhiBufferDesc &desc, VkBufferUsageFlags extra, Span<const uint32> families,
                     VkBufferCreateInfo &create)
{
    create = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = desc.Size;
    create.usage = BufferUsage(desc.Usage) | extra;
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Reserved))
    {
        create.flags |= VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    }
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Concurrent) && families.Count() > 1)
    {
        create.sharingMode = VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = static_cast<uint32>(families.Count());
        create.pQueueFamilyIndices = families.Data();
    }
}
} // namespace

Result<RhiHeap, RhiError> VulkanDevice::CreateHeap(const RhiHeapDesc &desc)
{
    const uint32 type = FindMemoryType(~0u, desc.Location);
    if (type == ~0u)
    {
        return Failure(RhiError::Unsupported);
    }
    Result<GpuMemoryBlock, RhiError> block = AllocateBlock(GetPool(type, true), desc.Size, nullptr);
    if (block.IsError())
    {
        return Failure(block.Error());
    }
    SetObjectName(VK_OBJECT_TYPE_DEVICE_MEMORY, block.Value().Native, desc.DebugName);
    VulkanHeap heap;
    heap.Block = block.Value();
    heap.Desc = desc;
    heap.Desc.DebugName = {};
    const RhiHeap handle = _heaps.Allocate(heap);
    if (!handle.IsValid())
    {
        FreeBlock(block.Value());
        return Failure(RhiError::OutOfHostMemory);
    }
    return handle;
}

void VulkanDevice::Destroy(RhiHeap heap)
{
    DeferRelease(heap, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiHeap handle = Unpack<RhiHeap>(payload);
        if (const VulkanHeap *entry = device->_heaps.Find(handle))
        {
            device->FreeBlock(entry->Block);
            device->_heaps.Free(handle);
        }
    });
}

RhiAllocationInfo VulkanDevice::GetAllocationInfo(const RhiBufferDesc &desc) const
{
    VkBufferCreateInfo create;
    BuildBufferInfo(desc, 0, {}, create);
    VkDeviceBufferMemoryRequirements info{VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS};
    info.pCreateInfo = &create;
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    Vk.vkGetDeviceBufferMemoryRequirements(_device, &info, &requirements);
    return {requirements.memoryRequirements.size, requirements.memoryRequirements.alignment};
}

RhiAllocationInfo VulkanDevice::GetAllocationInfo(const RhiTextureDesc &desc) const
{
    ImageInfo image;
    BuildImageInfo(desc, {}, image);
    VkDeviceImageMemoryRequirements info{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
    info.pCreateInfo = &image.Create;
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    Vk.vkGetDeviceImageMemoryRequirements(_device, &info, &requirements);
    return {requirements.memoryRequirements.size, requirements.memoryRequirements.alignment};
}

Result<RhiBuffer, RhiError> VulkanDevice::CreateBuffer(const RhiBufferDesc &desc)
{
    return CreateRawBuffer(desc, 0);
}

Result<RhiBuffer, RhiError> VulkanDevice::CreateRawBuffer(const RhiBufferDesc &desc, VkBufferUsageFlags usage,
                                                          VkBufferUsageFlags2 usage2)
{
    const uint32 families[] = {_queues[0].Family, _queues[1].Family, _queues[2].Family};
    const uint32 familyCount = families[1] == families[0] ? 1 : (families[2] == families[1] ? 2 : 3);
    VkBufferCreateInfo create;
    BuildBufferInfo(desc, usage, Span<const uint32>(families, familyCount), create);
    VkBufferUsageFlags2CreateInfo usageFlags2{VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO};
    if (usage2 != 0)
    {
        usageFlags2.usage = usage2 | create.usage;
        create.pNext = &usageFlags2;
    }
    VulkanBuffer buffer;
    VkResult result = Vk.vkCreateBuffer(_device, &create, nullptr, &buffer.Buffer);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    buffer.Desc = desc;
    buffer.Desc.DebugName = {};
    if (!HasAnyFlags(desc.Flags, RhiResourceFlags::Reserved))
    {
        VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
        info.buffer = buffer.Buffer;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        Vk.vkGetBufferMemoryRequirements2(_device, &info, &requirements);
        const VkMemoryRequirements &memory = requirements.memoryRequirements;
        VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
        uint64 offset = 0;
        if (const VulkanHeap *heap = _heaps.Find(desc.Placement.Heap))
        {
            if ((memory.memoryTypeBits & (1u << (heap->Block.Pool / 2))) == 0 ||
                desc.Placement.Offset % memory.alignment != 0)
            {
                Vk.vkDestroyBuffer(_device, buffer.Buffer, nullptr);
                return Failure(RhiError::Unsupported);
            }
            deviceMemory = reinterpret_cast<VkDeviceMemory>(heap->Block.Native);
            offset = desc.Placement.Offset;
            buffer.Mapped = heap->Block.Mapped != nullptr ? heap->Block.Mapped + offset : nullptr;
        }
        else
        {
            const bool staging = desc.Location == RhiMemoryLocation::Upload && desc.Usage == RhiBufferUsage::CopySource;
            const uint32 type = FindMemoryType(memory.memoryTypeBits, desc.Location, staging);
            if (type == ~0u)
            {
                Vk.vkDestroyBuffer(_device, buffer.Buffer, nullptr);
                return Failure(RhiError::Unsupported);
            }
            VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicatedInfo.buffer = buffer.Buffer;
            GpuMemoryRequest request;
            request.Pool = GetPool(type, false);
            request.Size = memory.size;
            request.Alignment = memory.alignment > 256 ? memory.alignment : 256;
            request.PreferDedicated = dedicated.prefersDedicatedAllocation == VK_TRUE;
            request.DedicatedResource = &dedicatedInfo;
            Result<GpuAllocation, RhiError> allocation = _allocator->Allocate(request);
            if (allocation.IsError())
            {
                Vk.vkDestroyBuffer(_device, buffer.Buffer, nullptr);
                return Failure(allocation.Error());
            }
            buffer.Allocation = allocation.Value();
            deviceMemory = reinterpret_cast<VkDeviceMemory>(buffer.Allocation.Block->Native);
            offset = buffer.Allocation.Offset;
            buffer.Mapped = buffer.Allocation.GetMappedData();
        }
        result = Vk.vkBindBufferMemory(_device, buffer.Buffer, deviceMemory, offset);
        if (result != VK_SUCCESS)
        {
            Vk.vkDestroyBuffer(_device, buffer.Buffer, nullptr);
            _allocator->Free(buffer.Allocation);
            return Failure(VulkanConvert::Error(result));
        }
    }
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer.Buffer;
    buffer.Address = Vk.vkGetBufferDeviceAddress(_device, &addressInfo);
    SetObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64>(buffer.Buffer), desc.DebugName);
    const RhiBuffer handle = _buffers.Allocate(buffer);
    if (!handle.IsValid())
    {
        Vk.vkDestroyBuffer(_device, buffer.Buffer, nullptr);
        _allocator->Free(buffer.Allocation);
        return Failure(RhiError::OutOfHostMemory);
    }
    return handle;
}

void VulkanDevice::Destroy(RhiBuffer buffer)
{
    DeferRelease(buffer, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiBuffer handle = Unpack<RhiBuffer>(payload);
        if (const VulkanBuffer *entry = device->_buffers.Find(handle))
        {
            Vk.vkDestroyBuffer(device->_device, entry->Buffer, nullptr);
            device->_allocator->Free(entry->Allocation);
            device->_buffers.Free(handle);
        }
    });
}

uint8 *VulkanDevice::GetMappedData(RhiBuffer buffer) const
{
    const VulkanBuffer *entry = _buffers.Find(buffer);
    return entry != nullptr ? entry->Mapped : nullptr;
}

uint64 VulkanDevice::GetBufferAddress(RhiBuffer buffer) const
{
    const VulkanBuffer *entry = _buffers.Find(buffer);
    return entry != nullptr ? entry->Address : 0;
}

Result<RhiTexture, RhiError> VulkanDevice::CreateTexture(const RhiTextureDesc &desc)
{
    const uint32 families[] = {_queues[0].Family, _queues[1].Family, _queues[2].Family};
    const uint32 familyCount = families[1] == families[0] ? 1 : (families[2] == families[1] ? 2 : 3);
    ImageInfo image;
    BuildImageInfo(desc, Span<const uint32>(families, familyCount), image);
    VkImage vkImage = VK_NULL_HANDLE;
    VkResult result = Vk.vkCreateImage(_device, &image.Create, nullptr, &vkImage);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    GpuAllocation allocation;
    RhiTileInfo tiles;
    VkSparseImageMemoryRequirements2 sparse{VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2};
    VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    info.image = vkImage;
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    requirements.pNext = &dedicated;
    Vk.vkGetImageMemoryRequirements2(_device, &info, &requirements);
    const VkMemoryRequirements &memory = requirements.memoryRequirements;
    if (HasAnyFlags(desc.Flags, RhiResourceFlags::Reserved))
    {
        VkImageSparseMemoryRequirementsInfo2 sparseInfo{VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2};
        sparseInfo.image = vkImage;
        uint32 count = 1;
        Vk.vkGetImageSparseMemoryRequirements2(_device, &sparseInfo, &count, &sparse);
        const VkSparseImageMemoryRequirements &requirement = sparse.memoryRequirements;
        tiles.TileWidth = requirement.formatProperties.imageGranularity.width;
        tiles.TileHeight = requirement.formatProperties.imageGranularity.height;
        tiles.TileDepth = requirement.formatProperties.imageGranularity.depth;
        tiles.TileBytes = static_cast<uint32>(memory.alignment);
        tiles.FirstPackedMip = requirement.imageMipTailFirstLod;
        tiles.PackedMipTileCount =
            static_cast<uint32>((requirement.imageMipTailSize + memory.alignment - 1) / memory.alignment);
    }
    else
    {
        VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
        uint64 offset = 0;
        if (const VulkanHeap *heap = _heaps.Find(desc.Placement.Heap))
        {
            if ((memory.memoryTypeBits & (1u << (heap->Block.Pool / 2))) == 0 ||
                desc.Placement.Offset % memory.alignment != 0)
            {
                Vk.vkDestroyImage(_device, vkImage, nullptr);
                return Failure(RhiError::Unsupported);
            }
            deviceMemory = reinterpret_cast<VkDeviceMemory>(heap->Block.Native);
            offset = desc.Placement.Offset;
        }
        else
        {
            const uint32 type = FindMemoryType(memory.memoryTypeBits, RhiMemoryLocation::Device);
            if (type == ~0u)
            {
                Vk.vkDestroyImage(_device, vkImage, nullptr);
                return Failure(RhiError::Unsupported);
            }
            VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicatedInfo.image = vkImage;
            GpuMemoryRequest request;
            request.Pool = GetPool(type, true);
            request.Size = memory.size;
            request.Alignment = memory.alignment;
            request.PreferDedicated = dedicated.prefersDedicatedAllocation == VK_TRUE;
            request.DedicatedResource = &dedicatedInfo;
            Result<GpuAllocation, RhiError> allocated = _allocator->Allocate(request);
            if (allocated.IsError())
            {
                Vk.vkDestroyImage(_device, vkImage, nullptr);
                return Failure(allocated.Error());
            }
            allocation = allocated.Value();
            deviceMemory = reinterpret_cast<VkDeviceMemory>(allocation.Block->Native);
            offset = allocation.Offset;
        }
        result = Vk.vkBindImageMemory(_device, vkImage, deviceMemory, offset);
        if (result != VK_SUCCESS)
        {
            Vk.vkDestroyImage(_device, vkImage, nullptr);
            _allocator->Free(allocation);
            return Failure(VulkanConvert::Error(result));
        }
    }
    SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64>(vkImage), desc.DebugName);
    const RhiTexture handle = _textures.Allocate();
    if (!handle.IsValid())
    {
        Vk.vkDestroyImage(_device, vkImage, nullptr);
        _allocator->Free(allocation);
        return Failure(RhiError::OutOfHostMemory);
    }
    VulkanTexture &texture = _textures.Get(handle);
    texture.Image = vkImage;
    texture.Allocation = allocation;
    texture.Desc = desc;
    texture.Desc.ViewFormats = {};
    texture.Desc.DebugName = {};
    texture.Format = image.Create.format;
    texture.Aspect = VulkanConvert::Aspect(desc.Format);
    texture.Tiles = tiles;
    texture.MipTailOffset = sparse.memoryRequirements.imageMipTailOffset;
    texture.MipTailStride = sparse.memoryRequirements.imageMipTailStride;
    texture.MipTailSize = sparse.memoryRequirements.imageMipTailSize;
    texture.SingleMipTail =
        (sparse.memoryRequirements.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
    return handle;
}

void VulkanDevice::Destroy(RhiTexture texture)
{
    DeferRelease(texture, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiTexture handle = Unpack<RhiTexture>(payload);
        if (VulkanTexture *entry = device->_textures.Find(handle))
        {
            for (const VulkanAttachmentView &view : entry->AttachmentViews)
            {
                Vk.vkDestroyImageView(device->_device, view.View, nullptr);
            }
            if (entry->OwnsImage)
            {
                Vk.vkDestroyImage(device->_device, entry->Image, nullptr);
                device->_allocator->Free(entry->Allocation);
            }
            device->_textures.Free(handle);
        }
    });
}

const RhiTextureDesc &VulkanDevice::GetTextureDesc(RhiTexture texture) const
{
    return _textures.Get(texture).Desc;
}

RhiTileInfo VulkanDevice::GetTileInfo(RhiTexture texture) const
{
    const VulkanTexture *entry = _textures.Find(texture);
    return entry != nullptr ? entry->Tiles : RhiTileInfo{};
}

VkImageView VulkanDevice::GetAttachmentView(VulkanTexture &texture, uint32 mip, uint32 firstSlice, uint32 sliceCount,
                                            RhiFormat format)
{
    const VkFormat vkFormat = format == RhiFormat::Unknown ? texture.Format : VulkanConvert::Format(format);
    return GetTextureView(texture, mip, 1, firstSlice, sliceCount, vkFormat, texture.Aspect);
}

VkImageView VulkanDevice::GetTextureView(VulkanTexture &texture, uint32 mip, uint32 mipCount, uint32 firstSlice,
                                         uint32 sliceCount, VkFormat format, VkImageAspectFlags aspect)
{
    const ScopeLock lock(texture.ViewLock);
    for (const VulkanAttachmentView &view : texture.AttachmentViews)
    {
        if (view.Mip == mip && view.MipCount == mipCount && view.FirstSlice == firstSlice &&
            view.SliceCount == sliceCount && view.Format == format && view.Aspect == aspect)
        {
            return view.View;
        }
    }
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = texture.Image;
    if (texture.Desc.Dimension == RhiTextureDimension::Texture3D)
    {
        info.viewType = VK_IMAGE_VIEW_TYPE_3D;
    }
    else
    {
        info.viewType = sliceCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    info.format = format;
    info.subresourceRange = {aspect, mip, mipCount, firstSlice, sliceCount};
    VkImageView view = VK_NULL_HANDLE;
    if (Vk.vkCreateImageView(_device, &info, nullptr, &view) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    texture.AttachmentViews.Add({mip, mipCount, firstSlice, sliceCount, format, aspect, view});
    return view;
}

RhiNativeTexture VulkanDevice::GetNativeTexture(RhiTexture texture, const RhiSubresourceRange &range,
                                                RhiFormat viewFormat)
{
    VulkanTexture *entry = _textures.Find(texture);
    if (entry == nullptr)
    {
        return {};
    }
    const RhiTextureDesc &desc = entry->Desc;
    const uint32 slices = desc.Dimension == RhiTextureDimension::Texture3D ? 1 : desc.DepthOrArraySize;
    const uint32 mipCount = range.MipCount != 0 ? range.MipCount : desc.MipCount - range.FirstMip;
    const uint32 sliceCount = range.SliceCount != 0 ? range.SliceCount : slices - range.FirstSlice;
    RhiTextureAspect aspect = range.Aspect;
    if (aspect == RhiTextureAspect::All && IsDepthFormat(desc.Format))
    {
        aspect = RhiTextureAspect::Depth;
    }
    const VkFormat format = viewFormat == RhiFormat::Unknown ? entry->Format : VulkanConvert::Format(viewFormat);
    RhiNativeTexture native;
    native.Resource = entry->Image;
    native.View = GetTextureView(*entry, range.FirstMip, mipCount, range.FirstSlice, sliceCount, format,
                                 VulkanConvert::Aspect(desc.Format, aspect));
    native.Format = static_cast<uint32>(format);
    return native;
}

RhiNativeBuffer VulkanDevice::GetNativeBuffer(RhiBuffer buffer) const
{
    const VulkanBuffer *entry = _buffers.Find(buffer);
    return entry != nullptr ? RhiNativeBuffer{entry->Buffer, entry->Desc.Size, entry->Address} : RhiNativeBuffer{};
}

Result<RhiResourceView, RhiError> VulkanDevice::CreateView(const RhiTextureViewDesc &desc)
{
    const VulkanTexture *texture = _textures.Find(desc.Texture);
    if (texture == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    const bool storage = desc.Access == RhiViewAccess::UnorderedAccess;
    RhiTextureAspect aspect = desc.Range.Aspect;
    if (aspect == RhiTextureAspect::All && GetFormatInfo(texture->Desc.Format).HasDepth)
    {
        aspect = RhiTextureAspect::Depth;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture->Image;
    view.viewType = VulkanConvert::ViewType(desc.Dimension);
    view.format = desc.Format == RhiFormat::Unknown ? texture->Format : VulkanConvert::Format(desc.Format);
    view.subresourceRange = {VulkanConvert::Aspect(texture->Desc.Format, aspect), desc.Range.FirstMip,
                             desc.Range.MipCount == 0 ? VK_REMAINING_MIP_LEVELS : desc.Range.MipCount,
                             desc.Range.FirstSlice,
                             desc.Range.SliceCount == 0 ? VK_REMAINING_ARRAY_LAYERS : desc.Range.SliceCount};
    VkImageDescriptorInfoEXT image{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    image.pView = &view;
    image.layout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    VkResourceDescriptorInfoEXT info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    info.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    info.data.pImage = &image;
    const RhiResourceView handle = _views->Allocate(VulkanView{info.type});
    if (!handle.IsValid())
    {
        MARLIN_LOG(LogRhi, Error, "The resource heap is full ({0} views)", _views->GetCapacity());
        return Failure(RhiError::OutOfDeviceMemory);
    }
    WriteResourceDescriptor(handle.Index, info);
    return handle;
}

Result<RhiResourceView, RhiError> VulkanDevice::CreateView(const RhiBufferViewDesc &desc)
{
    const VulkanBuffer *buffer = _buffers.Find(desc.Buffer);
    if (buffer == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    const bool storage = desc.Access == RhiViewAccess::UnorderedAccess;
    VkDeviceAddressRangeEXT range{buffer->Address + desc.Offset,
                                  desc.Size == 0 ? buffer->Desc.Size - desc.Offset : desc.Size};
    VkTexelBufferDescriptorInfoEXT texel{VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT};
    VkResourceDescriptorInfoEXT info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    if (desc.Kind == RhiBufferViewKind::Typed)
    {
        texel.format = VulkanConvert::Format(desc.Format);
        texel.addressRange = range;
        info.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        info.data.pTexelBuffer = &texel;
    }
    else
    {
        info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        info.data.pAddressRange = &range;
    }
    const RhiResourceView handle = _views->Allocate(VulkanView{info.type});
    if (!handle.IsValid())
    {
        MARLIN_LOG(LogRhi, Error, "The resource heap is full ({0} views)", _views->GetCapacity());
        return Failure(RhiError::OutOfDeviceMemory);
    }
    WriteResourceDescriptor(handle.Index, info);
    return handle;
}

Result<RhiResourceView, RhiError> VulkanDevice::CreateView(RhiAccelerationStructure structure)
{
    const VulkanAccelerationStructure *entry = _accelerationStructures.Find(structure);
    if (entry == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    VkDeviceAddressRangeEXT range{entry->Address, entry->Size};
    VkResourceDescriptorInfoEXT info{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    info.data.pAddressRange = &range;
    const RhiResourceView handle = _views->Allocate(VulkanView{info.type});
    if (!handle.IsValid())
    {
        return Failure(RhiError::OutOfDeviceMemory);
    }
    WriteResourceDescriptor(handle.Index, info);
    return handle;
}

void VulkanDevice::Destroy(RhiResourceView view)
{
    DeferRelease(view, [](void *context, uint64 payload) {
        static_cast<VulkanDevice *>(context)->_views->Free(Unpack<RhiResourceView>(payload));
    });
}

Result<RhiSampler, RhiError> VulkanDevice::CreateSampler(const RhiSamplerDesc &desc)
{
    const ScopeLock lock(_samplerLock);
    RhiSampler found;
    _samplers->ForEach([&](RhiSampler handle, VulkanSampler &sampler) {
        if (!found.IsValid() && sampler.Desc == desc)
        {
            found = handle;
            ++sampler.References;
        }
    });
    if (found.IsValid())
    {
        return found;
    }
    const RhiSampler handle = _samplers->Allocate(VulkanSampler{desc, 1});
    if (!handle.IsValid())
    {
        MARLIN_LOG(LogRhi, Error, "The sampler heap is full ({0} samplers)", _samplers->GetCapacity());
        return Failure(RhiError::OutOfDeviceMemory);
    }
    VkSamplerReductionModeCreateInfo reduction;
    const VkSamplerCreateInfo info = VulkanConvert::Sampler(desc, reduction);
    const VulkanBuffer &heap = _buffers.Get(_samplerHeap);
    VkHostAddressRangeEXT destination{heap.Mapped + uint64{handle.Index} * _samplerStride, _samplerStride};
    const VkResult result = Vk.vkWriteSamplerDescriptorsEXT(_device, 1, &info, &destination);
    if (result != VK_SUCCESS)
    {
        _samplers->Free(handle);
        return Failure(VulkanConvert::Error(result));
    }
    return handle;
}

void VulkanDevice::Destroy(RhiSampler sampler)
{
    {
        const ScopeLock lock(_samplerLock);
        VulkanSampler *entry = _samplers->Find(sampler);
        if (entry == nullptr || --entry->References > 0)
        {
            return;
        }
    }
    DeferRelease(sampler, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const ScopeLock lock(device->_samplerLock);
        const RhiSampler handle = Unpack<RhiSampler>(payload);
        const VulkanSampler *entry = device->_samplers->Find(handle);
        if (entry != nullptr && entry->References == 0)
        {
            device->_samplers->Free(handle);
        }
    });
}

RhiAccelerationStructureSizes VulkanDevice::GetAccelerationStructureSizes(
    const RhiAccelerationStructureInputs &inputs) const
{
    VulkanConvert::AccelerationStructureGeometry geometry;
    VulkanConvert::Geometry(inputs, geometry);
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    Vk.vkGetAccelerationStructureBuildSizesKHR(_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &geometry.Info,
                                               geometry.PrimitiveCounts.Data(), &sizes);
    return {sizes.accelerationStructureSize, sizes.buildScratchSize, sizes.updateScratchSize};
}

Result<RhiAccelerationStructure, RhiError> VulkanDevice::CreateAccelerationStructure(
    const RhiAccelerationStructureDesc &desc)
{
    const VulkanBuffer *buffer = _buffers.Find(desc.Buffer);
    if (buffer == nullptr)
    {
        return Failure(RhiError::Unsupported);
    }
    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = buffer->Buffer;
    info.offset = desc.Offset;
    info.size = desc.Size;
    info.type = desc.Kind == RhiAccelerationStructureKind::TopLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                                                    : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VulkanAccelerationStructure structure;
    const VkResult result = Vk.vkCreateAccelerationStructureKHR(_device, &info, nullptr, &structure.Structure);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = structure.Structure;
    structure.Address = Vk.vkGetAccelerationStructureDeviceAddressKHR(_device, &addressInfo);
    structure.Kind = desc.Kind;
    structure.Size = desc.Size;
    SetObjectName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64>(structure.Structure),
                  desc.DebugName);
    const RhiAccelerationStructure handle = _accelerationStructures.Allocate(structure);
    if (!handle.IsValid())
    {
        Vk.vkDestroyAccelerationStructureKHR(_device, structure.Structure, nullptr);
        return Failure(RhiError::OutOfHostMemory);
    }
    return handle;
}

void VulkanDevice::Destroy(RhiAccelerationStructure structure)
{
    DeferRelease(structure, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiAccelerationStructure handle = Unpack<RhiAccelerationStructure>(payload);
        if (const VulkanAccelerationStructure *entry = device->_accelerationStructures.Find(handle))
        {
            Vk.vkDestroyAccelerationStructureKHR(device->_device, entry->Structure, nullptr);
            device->_accelerationStructures.Free(handle);
        }
    });
}

uint64 VulkanDevice::GetAccelerationStructureAddress(RhiAccelerationStructure structure) const
{
    const VulkanAccelerationStructure *entry = _accelerationStructures.Find(structure);
    return entry != nullptr ? entry->Address : 0;
}

Result<RhiQueryPool, RhiError> VulkanDevice::CreateQueryPool(const RhiQueryPoolDesc &desc)
{
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryCount = desc.Count;
    switch (desc.Kind)
    {
    case RhiQueryKind::Timestamp:
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        break;
    case RhiQueryKind::Occlusion:
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        break;
    case RhiQueryKind::PipelineStatistics:
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT |
                                  VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT |
                                  VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
        break;
    }
    VulkanQueryPool pool;
    pool.Kind = desc.Kind;
    pool.Count = desc.Count;
    const VkResult result = Vk.vkCreateQueryPool(_device, &info, nullptr, &pool.Pool);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    Vk.vkResetQueryPool(_device, pool.Pool, 0, desc.Count);
    SetObjectName(VK_OBJECT_TYPE_QUERY_POOL, reinterpret_cast<uint64>(pool.Pool), desc.DebugName);
    const RhiQueryPool handle = _queryPools.Allocate(pool);
    if (!handle.IsValid())
    {
        Vk.vkDestroyQueryPool(_device, pool.Pool, nullptr);
        return Failure(RhiError::OutOfHostMemory);
    }
    return handle;
}

void VulkanDevice::Destroy(RhiQueryPool pool)
{
    DeferRelease(pool, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiQueryPool handle = Unpack<RhiQueryPool>(payload);
        if (const VulkanQueryPool *entry = device->_queryPools.Find(handle))
        {
            Vk.vkDestroyQueryPool(device->_device, entry->Pool, nullptr);
            device->_queryPools.Free(handle);
        }
    });
}

VkShaderStageFlags VulkanDevice::GetGeneratedCommandStages(RhiPipelineKind kind) const
{
    switch (kind)
    {
    case RhiPipelineKind::Graphics:
        return _generatedCommandStages & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                          VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT);
    case RhiPipelineKind::Compute:
        return _generatedCommandStages & VK_SHADER_STAGE_COMPUTE_BIT;
    case RhiPipelineKind::RayTracing:
        return _generatedCommandStages & (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                          VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR);
    }
    return 0;
}

Result<RhiIndirectPipelineSet, RhiError> VulkanDevice::CreateIndirectPipelineSet(const RhiIndirectPipelineSetDesc &desc)
{
    if (!_generatedCommands || desc.Pipelines.IsEmpty())
    {
        return Failure(RhiError::Unsupported);
    }
    List<VkWriteIndirectExecutionSetPipelineEXT> writes;
    RhiPipelineKind kind = RhiPipelineKind::Graphics;
    for (uint32 i = 0; i < desc.Pipelines.Count(); ++i)
    {
        const VulkanPipeline *pipeline = _pipelines.Find(desc.Pipelines[i]);
        if (pipeline == nullptr || pipeline->Pipeline == VK_NULL_HANDLE)
        {
            return Failure(RhiError::Unsupported);
        }
        if (!pipeline->IndirectBindable)
        {
            MARLIN_LOG(LogRhi, Error, "Indirect pipeline set {0}: pipeline {1} is not IndirectBindable",
                       desc.DebugName, pipeline->DebugName);
            return Failure(RhiError::Unsupported);
        }
        kind = pipeline->Kind;
        VkWriteIndirectExecutionSetPipelineEXT &write = writes.AddDefaulted();
        write = {VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT};
        write.index = i;
        write.pipeline = pipeline->Pipeline;
    }
    if (GetGeneratedCommandStages(kind) == 0)
    {
        return Failure(RhiError::Unsupported);
    }
    VkIndirectExecutionSetPipelineInfoEXT pipelineInfo{VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT};
    pipelineInfo.initialPipeline = writes[0].pipeline;
    pipelineInfo.maxPipelineCount = static_cast<uint32>(writes.Count());
    VkIndirectExecutionSetCreateInfoEXT info{VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT};
    info.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
    info.info.pPipelineInfo = &pipelineInfo;
    VulkanIndirectPipelineSet set;
    set.Kind = kind;
    set.Count = pipelineInfo.maxPipelineCount;
    const VkResult result = Vk.vkCreateIndirectExecutionSetEXT(_device, &info, nullptr, &set.Set);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    Vk.vkUpdateIndirectExecutionSetPipelineEXT(_device, set.Set, static_cast<uint32>(writes.Count()), writes.Data());
    SetObjectName(VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT, reinterpret_cast<uint64>(set.Set), desc.DebugName);
    const RhiIndirectPipelineSet handle = _indirectPipelineSets.Allocate(set);
    if (!handle.IsValid())
    {
        Vk.vkDestroyIndirectExecutionSetEXT(_device, set.Set, nullptr);
        return Failure(RhiError::OutOfHostMemory);
    }
    return handle;
}

void VulkanDevice::Destroy(RhiIndirectPipelineSet set)
{
    DeferRelease(set, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiIndirectPipelineSet handle = Unpack<RhiIndirectPipelineSet>(payload);
        if (const VulkanIndirectPipelineSet *entry = device->_indirectPipelineSets.Find(handle))
        {
            Vk.vkDestroyIndirectExecutionSetEXT(device->_device, entry->Set, nullptr);
            device->_indirectPipelineSets.Free(handle);
        }
    });
}

Result<RhiCommandSignature, RhiError> VulkanDevice::CreateCommandSignature(const RhiCommandSignatureDesc &desc)
{
    if (desc.Commands.IsEmpty() || desc.Commands.Last().Kind < RhiIndirectCommandKind::Draw)
    {
        return Failure(RhiError::Unsupported);
    }
    const RhiIndirectCommandKind kind = desc.Commands.Last().Kind;
    const VulkanCommandSignature &plain = _commandSignatures.Get(GetCommandSignature(kind));
    if (desc.Commands.Count() == 1)
    {
        const RhiCommandSignature handle =
            _commandSignatures.Allocate(VulkanCommandSignature{kind, desc.Stride != 0 ? desc.Stride : plain.Stride});
        if (!handle.IsValid())
        {
            return Failure(RhiError::OutOfHostMemory);
        }
        return handle;
    }
    const VkShaderStageFlags stages = GetGeneratedCommandStages(desc.PipelineKind);
    if (!_generatedCommands || stages == 0)
    {
        return Failure(RhiError::Unsupported);
    }
    const VulkanIndirectPipelineSet *set = _indirectPipelineSets.Find(desc.PipelineSet);

    List<VkIndirectCommandsLayoutTokenEXT> tokens;
    List<VkIndirectCommandsPushConstantTokenEXT> pushData;
    pushData.Reserve(desc.Commands.Count());
    VkIndirectCommandsExecutionSetTokenEXT executionSet{VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT, stages};
    VkIndirectCommandsIndexBufferTokenEXT indexBuffer{VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT};
    uint32 offset = 0;
    for (const RhiIndirectCommand &command : desc.Commands)
    {
        VkIndirectCommandsLayoutTokenEXT &token = tokens.AddDefaulted();
        token = {VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT};
        uint32 size = 0;
        switch (command.Kind)
        {
        case RhiIndirectCommandKind::SetPipeline:
            if (set == nullptr)
            {
                return Failure(RhiError::Unsupported);
            }
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT;
            token.data.pExecutionSet = &executionSet;
            size = sizeof(uint32);
            break;
        case RhiIndirectCommandKind::ShaderArguments: {
            VkIndirectCommandsPushConstantTokenEXT &push = pushData.AddDefaulted();
            push.updateRange = {VK_SHADER_STAGE_ALL, command.Offset, command.Size};
            token.data.pPushConstant = &push;
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_DATA_EXT;
            size = command.Size;
            break;
        }
        case RhiIndirectCommandKind::IndexBuffer:
            offset = (offset + 7) & ~7u;
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT;
            token.data.pIndexBuffer = &indexBuffer;
            size = sizeof(VkBindIndexBufferIndirectCommandEXT);
            break;
        case RhiIndirectCommandKind::Draw:
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT;
            size = sizeof(RhiDrawArguments);
            break;
        case RhiIndirectCommandKind::DrawIndexed:
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
            size = sizeof(RhiDrawIndexedArguments);
            break;
        case RhiIndirectCommandKind::DrawMesh:
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT;
            size = sizeof(RhiDispatchArguments);
            break;
        case RhiIndirectCommandKind::Dispatch:
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT;
            size = sizeof(RhiDispatchArguments);
            break;
        case RhiIndirectCommandKind::DispatchRays:
            offset = (offset + 7) & ~7u;
            token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_TRACE_RAYS2_EXT;
            size = sizeof(VkTraceRaysIndirectCommand2KHR);
            break;
        }
        token.offset = offset;
        offset += (size + 3) & ~3u;
    }
    VkIndirectCommandsLayoutCreateInfoEXT info{VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT};
    info.shaderStages = stages;
    info.indirectStride = desc.Stride != 0 ? desc.Stride : offset;
    info.pipelineLayout = VK_NULL_HANDLE;
    info.tokenCount = static_cast<uint32>(tokens.Count());
    info.pTokens = tokens.Data();
    VulkanCommandSignature signature;
    signature.Kind = kind;
    signature.Stride = info.indirectStride;
    signature.Set = set != nullptr ? set->Set : VK_NULL_HANDLE;
    signature.Stages = stages;
    const VkResult result = Vk.vkCreateIndirectCommandsLayoutEXT(_device, &info, nullptr, &signature.Layout);
    if (result != VK_SUCCESS)
    {
        return Failure(VulkanConvert::Error(result));
    }
    SetObjectName(VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT, reinterpret_cast<uint64>(signature.Layout),
                  desc.DebugName);
    const RhiCommandSignature handle = _commandSignatures.Allocate(signature);
    if (!handle.IsValid())
    {
        Vk.vkDestroyIndirectCommandsLayoutEXT(_device, signature.Layout, nullptr);
        return Failure(RhiError::OutOfHostMemory);
    }
    return handle;
}

void VulkanDevice::Destroy(RhiCommandSignature signature)
{
    DeferRelease(signature, [](void *context, uint64 payload) {
        auto *device = static_cast<VulkanDevice *>(context);
        const RhiCommandSignature handle = Unpack<RhiCommandSignature>(payload);
        if (const VulkanCommandSignature *entry = device->_commandSignatures.Find(handle))
        {
            if (entry->Layout != VK_NULL_HANDLE)
            {
                Vk.vkDestroyIndirectCommandsLayoutEXT(device->_device, entry->Layout, nullptr);
            }
            device->_commandSignatures.Free(handle);
        }
    });
}

void VulkanDevice::GetPreprocessRequirements(const VulkanCommandSignature &signature, uint32 maxCount, uint64 &size,
                                             uint64 &alignment) const
{
    VkGeneratedCommandsMemoryRequirementsInfoEXT info{
        VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT};
    info.indirectExecutionSet = signature.Set;
    info.indirectCommandsLayout = signature.Layout;
    info.maxSequenceCount = maxCount;
    info.maxDrawCount = 1;
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    Vk.vkGetGeneratedCommandsMemoryRequirementsEXT(_device, &info, &requirements);
    size = requirements.memoryRequirements.size;
    alignment = requirements.memoryRequirements.alignment;
}

RhiBuffer VulkanDevice::CreatePreprocessBuffer(uint64 size)
{
    RhiBufferDesc desc;
    desc.Size = size;
    desc.DebugName = "IndirectPreprocess";
    Result<RhiBuffer, RhiError> buffer = CreateRawBuffer(
        desc, 0, VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT);
    return buffer.IsOk() ? buffer.Value() : RhiBuffer{};
}

RhiCommandSignature VulkanDevice::GetCommandSignature(RhiIndirectCommandKind kind) const
{
    const uint32 index = static_cast<uint32>(kind);
    return index < static_cast<uint32>(RhiIndirectCommandKind::Draw) ? RhiCommandSignature{} : _plainSignatures[index];
}
} // namespace Marlin
