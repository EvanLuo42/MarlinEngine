#include "VulkanCommandList.h"

#include "String/StringBuilder.h"
#include "VulkanConvert.h"
#include "VulkanDevice.h"

namespace Marlin
{
namespace
{
VkImageSubresourceRange Range(const VulkanTexture &texture, const RhiSubresourceRange &range)
{
    return {VulkanConvert::Aspect(texture.Desc.Format, range.Aspect), range.FirstMip,
            range.MipCount == 0 ? VK_REMAINING_MIP_LEVELS : range.MipCount, range.FirstSlice,
            range.SliceCount == 0 ? VK_REMAINING_ARRAY_LAYERS : range.SliceCount};
}

uint32 MipSize(uint32 size, uint32 mip)
{
    const uint32 value = size >> mip;
    return value > 0 ? value : 1;
}

VkImageSubresourceLayers Layers(const VulkanTexture &texture, const RhiTextureRegion &region)
{
    return {VulkanConvert::Aspect(texture.Desc.Format, region.Aspect), region.Mip, region.Slice, 1};
}

VkExtent3D Extent(const VulkanTexture &texture, const RhiTextureRegion &region)
{
    const RhiTextureDesc &desc = texture.Desc;
    const uint32 depth = desc.Dimension == RhiTextureDimension::Texture3D ? MipSize(desc.DepthOrArraySize, region.Mip) : 1;
    return {region.Width != 0 ? region.Width : MipSize(desc.Width, region.Mip) - region.X,
            region.Height != 0 ? region.Height : MipSize(desc.Height, region.Mip) - region.Y,
            region.Depth != 0 ? region.Depth : depth - region.Z};
}

VkBufferImageCopy2 BufferImageCopy(const VulkanTexture &texture, const RhiTextureRegion &region,
                                   const RhiBufferTextureLayout &layout)
{
    VkBufferImageCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    copy.bufferOffset = layout.Offset;
    const RhiFormatInfo &format = GetFormatInfo(texture.Desc.Format);
    copy.bufferRowLength =
        layout.RowPitch != 0 && format.BlockBytes != 0 ? layout.RowPitch / format.BlockBytes * format.BlockWidth : 0;
    copy.bufferImageHeight = layout.RowsPerSlice != 0 ? layout.RowsPerSlice * format.BlockHeight : 0;
    copy.imageSubresource = Layers(texture, region);
    copy.imageOffset = {static_cast<int32>(region.X), static_cast<int32>(region.Y), static_cast<int32>(region.Z)};
    copy.imageExtent = Extent(texture, region);
    return copy;
}
} // namespace

VulkanCommandList::VulkanCommandList(VulkanDevice &device, RhiQueueType queue, uint32 family)
    : _device(device), _queue(queue)
{
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = family;
    Vk.vkCreateCommandPool(device.GetDevice(), &info, nullptr, &_pool);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = _pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    Vk.vkAllocateCommandBuffers(device.GetDevice(), &allocate, &_commandBuffer);
}

VulkanCommandList::~VulkanCommandList()
{
    for (const PropertyQueries &queries : _propertyQueries)
    {
        if (queries.Pool != VK_NULL_HANDLE)
        {
            Vk.vkDestroyQueryPool(_device.GetDevice(), queries.Pool, nullptr);
        }
    }
    if (_preprocess.IsValid())
    {
        _device.Destroy(_preprocess);
    }
    Vk.vkDestroyCommandPool(_device.GetDevice(), _pool, nullptr);
}

bool VulkanCommandList::Begin(RhiBreadcrumbs::Trail *trail)
{
    _trail = trail;
    _pipelineReady = false;
    _inRendering = false;
    _skipped = 0;
    _markerDepth = 0;
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (Vk.vkBeginCommandBuffer(_commandBuffer, &info) != VK_SUCCESS)
    {
        return false;
    }
    if (_queue != RhiQueueType::Copy)
    {
        _device.BindHeaps(_commandBuffer);
    }
    return true;
}

VkResult VulkanCommandList::End()
{
    return Vk.vkEndCommandBuffer(_commandBuffer);
}

void VulkanCommandList::Reset()
{
    // The submission is done, so the next recording reuses the preprocess memory and queries from the start.
    _preprocessUsed = 0;
    for (PropertyQueries &queries : _propertyQueries)
    {
        queries.Used = 0;
    }
    Vk.vkResetCommandPool(_device.GetDevice(), _pool, 0);
}

bool VulkanCommandList::CanExecute()
{
    if (!_pipelineReady)
    {
        ++_skipped;
    }
    return _pipelineReady;
}

void VulkanCommandList::AllocatePreprocess(RhiCommandSignature handle, const VulkanCommandSignature &signature,
                                           uint32 maxCount, uint64 &address, uint64 &size)
{
    address = 0;
    size = 0;
    const PreprocessRequirements *requirements = nullptr;
    for (const PreprocessRequirements &cached : _requirements)
    {
        if (cached.Signature == handle && cached.MaxCount == maxCount)
        {
            requirements = &cached;
            break;
        }
    }
    if (requirements == nullptr)
    {
        PreprocessRequirements &added = _requirements[_nextRequirements];
        _nextRequirements =
            (_nextRequirements + 1) % static_cast<uint32>(sizeof(_requirements) / sizeof(*_requirements));
        added.Signature = handle;
        added.MaxCount = maxCount;
        _device.GetPreprocessRequirements(signature, maxCount, added.Size, added.Alignment);
        requirements = &added;
    }
    if (requirements->Size == 0)
    {
        return;
    }
    const uint64 alignment = requirements->Alignment > 1 ? requirements->Alignment : 1;
    const auto offsetIn = [&](uint64 base, uint64 used) {
        return (base + used + alignment - 1) / alignment * alignment - base;
    };
    uint64 offset = offsetIn(_preprocessAddress, _preprocessUsed);
    if (!_preprocess.IsValid() || offset + requirements->Size > _preprocessCapacity)
    {
        // Grow: the old memory may still be read by this list's earlier executes, so it goes when the list is done.
        if (_preprocess.IsValid())
        {
            _releases.Add({VK_NULL_HANDLE, _preprocess});
        }
        const uint64 needed = requirements->Size + alignment;
        const uint64 capacity = _preprocessCapacity * 2 > needed ? _preprocessCapacity * 2 : needed;
        _preprocess = _device.CreatePreprocessBuffer(capacity);
        const VulkanBuffer *buffer = _device.FindBuffer(_preprocess);
        _preprocessAddress = buffer != nullptr ? buffer->Address : 0;
        _preprocessCapacity = buffer != nullptr ? capacity : 0;
        _preprocessUsed = 0;
        if (buffer == nullptr)
        {
            _preprocess = {};
            return;
        }
        offset = offsetIn(_preprocessAddress, 0);
    }
    address = _preprocessAddress + offset;
    size = requirements->Size;
    _preprocessUsed = offset + requirements->Size;
}

VkQueryPool VulkanCommandList::AllocatePropertyQueries(VkQueryType type, uint32 count, uint32 &first)
{
    PropertyQueries &queries =
        _propertyQueries[type == VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR ? 0 : 1];
    if (queries.Pool == VK_NULL_HANDLE || queries.Used + count > queries.Capacity)
    {
        // Grow: the old pool may still be written by this list's earlier commands, so it goes when the list is done.
        if (queries.Pool != VK_NULL_HANDLE)
        {
            _releases.Add({queries.Pool});
        }
        const uint32 capacity = queries.Capacity * 2 > count ? queries.Capacity * 2 : (count > 64 ? count : 64);
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = type;
        info.queryCount = capacity;
        queries.Pool = VK_NULL_HANDLE;
        queries.Capacity = 0;
        queries.Used = 0;
        if (Vk.vkCreateQueryPool(_device.GetDevice(), &info, nullptr, &queries.Pool) != VK_SUCCESS)
        {
            queries.Pool = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        queries.Capacity = capacity;
    }
    first = queries.Used;
    queries.Used += count;
    // No earlier submission still uses these queries: the list is reused only once its submission is done.
    Vk.vkResetQueryPool(_device.GetDevice(), queries.Pool, first, count);
    return queries.Pool;
}

void VulkanCommandList::WriteBreadcrumb(uint32 value)
{
    Vk.vkCmdWriteBufferMarker2AMD(_commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _device.GetBreadcrumbBuffer(),
                                  uint64{_trail->Slot} * sizeof(uint32), value);
}

void VulkanCommandList::Barrier(const RhiBarriers &barriers)
{
    List<VkMemoryBarrier2, InlineAllocator<8>> memory;
    for (const RhiGlobalBarrier &barrier : barriers.Global)
    {
        VkMemoryBarrier2 &entry = memory.AddDefaulted();
        entry = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        entry.srcStageMask = VulkanConvert::Stages(barrier.SyncBefore);
        entry.dstStageMask = VulkanConvert::Stages(barrier.SyncAfter);
        entry.srcAccessMask = VulkanConvert::Access(barrier.AccessBefore);
        entry.dstAccessMask = VulkanConvert::Access(barrier.AccessAfter);
    }
    const uint32 families[RhiQueueTypeCount] = {_device._queues[0].Family, _device._queues[1].Family,
                                                _device._queues[2].Family};
    // RECOVERY-DECISION D1: within one family the acquire half is skipped (the release has the before-sync).
    // Within one family nothing changes owner: the release does the whole barrier, layout change included, and the
    // submission's signal carries it to the other queue. The acquire would repeat the layout change, so it records
    // nothing.
    const auto acquireInFamily = [&](bool transferQueue, const RhiQueueTransfer &transfer) {
        return transferQueue && _queue == transfer.To && transfer.From != transfer.To &&
               families[static_cast<uint32>(transfer.From)] == families[static_cast<uint32>(transfer.To)];
    };
    List<VkBufferMemoryBarrier2, InlineAllocator<16>> buffers;
    for (const RhiBufferBarrier &barrier : barriers.Buffers)
    {
        const VulkanBuffer *buffer = _device.FindBuffer(barrier.Buffer);
        if (buffer == nullptr || acquireInFamily(barrier.TransferQueue, barrier.Transfer))
        {
            continue;
        }
        VkBufferMemoryBarrier2 &entry = buffers.AddDefaulted();
        entry = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        entry.srcStageMask = VulkanConvert::Stages(barrier.SyncBefore);
        entry.dstStageMask = VulkanConvert::Stages(barrier.SyncAfter);
        entry.srcAccessMask = VulkanConvert::Access(barrier.AccessBefore);
        entry.dstAccessMask = VulkanConvert::Access(barrier.AccessAfter);
        entry.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        entry.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (barrier.TransferQueue && families[static_cast<uint32>(barrier.Transfer.From)] !=
                                         families[static_cast<uint32>(barrier.Transfer.To)])
        {
            entry.srcQueueFamilyIndex = families[static_cast<uint32>(barrier.Transfer.From)];
            entry.dstQueueFamilyIndex = families[static_cast<uint32>(barrier.Transfer.To)];
        }
        entry.buffer = buffer->Buffer;
        entry.offset = barrier.Offset;
        entry.size = barrier.Size == 0 ? VK_WHOLE_SIZE : barrier.Size;
    }
    List<VkImageMemoryBarrier2, InlineAllocator<16>> images;
    for (const RhiTextureBarrier &barrier : barriers.Textures)
    {
        const VulkanTexture *texture = _device.FindTexture(barrier.Texture);
        if (texture == nullptr || acquireInFamily(barrier.TransferQueue, barrier.Transfer))
        {
            continue;
        }
        VkImageMemoryBarrier2 &entry = images.AddDefaulted();
        entry = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        entry.srcStageMask = VulkanConvert::Stages(barrier.SyncBefore);
        if (!texture->OwnsImage && _queue == RhiQueueType::Graphics)
        {
            // A swapchain image: chain after the acquire wait, which is only at these stages.
            entry.srcStageMask |= VulkanAcquireWaitStages;
        }
        entry.dstStageMask = VulkanConvert::Stages(barrier.SyncAfter);
        entry.srcAccessMask = VulkanConvert::Access(barrier.AccessBefore);
        entry.dstAccessMask = VulkanConvert::Access(barrier.AccessAfter);
        entry.oldLayout =
            barrier.Discard ? VK_IMAGE_LAYOUT_UNDEFINED : VulkanConvert::Layout(barrier.LayoutBefore);
        entry.newLayout = VulkanConvert::Layout(barrier.LayoutAfter);
        entry.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        entry.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (barrier.TransferQueue && families[static_cast<uint32>(barrier.Transfer.From)] !=
                                         families[static_cast<uint32>(barrier.Transfer.To)])
        {
            entry.srcQueueFamilyIndex = families[static_cast<uint32>(barrier.Transfer.From)];
            entry.dstQueueFamilyIndex = families[static_cast<uint32>(barrier.Transfer.To)];
        }
        entry.image = texture->Image;
        entry.subresourceRange = Range(*texture, barrier.Range);
    }
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.memoryBarrierCount = static_cast<uint32>(memory.Count());
    info.pMemoryBarriers = memory.Data();
    info.bufferMemoryBarrierCount = static_cast<uint32>(buffers.Count());
    info.pBufferMemoryBarriers = buffers.Data();
    info.imageMemoryBarrierCount = static_cast<uint32>(images.Count());
    info.pImageMemoryBarriers = images.Data();
    Vk.vkCmdPipelineBarrier2(_commandBuffer, &info);
}

void *VulkanCommandList::BeginNativeCommands()
{
    return _commandBuffer;
}

void VulkanCommandList::EndNativeCommands()
{
    if (_queue != RhiQueueType::Copy)
    {
        _device.BindHeaps(_commandBuffer);
    }
    _pipelineReady = false;
}

void VulkanCommandList::BeginMarker(Name name)
{
    ++_markerDepth;
    if (_device.HasDebugNames())
    {
        StringBuilder<128> text;
        text.Append(name.ToView());
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = text.CStr();
        Vk.vkCmdBeginDebugUtilsLabelEXT(_commandBuffer, &label);
    }
    if (_trail != nullptr)
    {
        WriteBreadcrumb(_device.GetBreadcrumbs()->BeginMarker(*_trail, name));
    }
}

void VulkanCommandList::EndMarker()
{
    if (_markerDepth == 0)
    {
        return;
    }
    --_markerDepth;
    if (_trail != nullptr)
    {
        WriteBreadcrumb(_device.GetBreadcrumbs()->EndMarker(*_trail));
    }
    if (_device.HasDebugNames())
    {
        Vk.vkCmdEndDebugUtilsLabelEXT(_commandBuffer);
    }
}

void VulkanCommandList::BeginRendering(const RhiRenderingDesc &desc)
{
    VkRenderingAttachmentInfo colors[RhiMaxRenderTargets] = {};
    uint32 colorCount = 0;
    uint32 layerCount = 1;
    for (const RhiRenderTarget &target : desc.RenderTargets)
    {
        VkRenderingAttachmentInfo &attachment = colors[colorCount++];
        attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VulkanTexture *texture = _device.FindTexture(target.Texture);
        if (texture == nullptr)
        {
            continue;
        }
        attachment.imageView =
            _device.GetAttachmentView(*texture, target.Mip, target.FirstSlice, target.SliceCount, target.Format);
        attachment.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VulkanConvert::Load(target.Load);
        attachment.storeOp = VulkanConvert::Store(target.Store);
        for (uint32 i = 0; i < 4; ++i)
        {
            attachment.clearValue.color.float32[i] = target.Clear.Color[i];
        }
        layerCount = target.SliceCount > layerCount ? target.SliceCount : layerCount;
        if (target.Store == RhiStoreOp::Resolve)
        {
            if (VulkanTexture *resolve = _device.FindTexture(target.ResolveTarget))
            {
                const RhiFormatKind kind = GetFormatInfo(texture->Desc.Format).Kind;
                attachment.resolveMode = kind == RhiFormatKind::Uint || kind == RhiFormatKind::Sint
                                             ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                             : VK_RESOLVE_MODE_AVERAGE_BIT;
                attachment.resolveImageView = _device.GetAttachmentView(*resolve, target.ResolveMip, target.ResolveSlice,
                                                                        target.SliceCount, target.Format);
                attachment.resolveImageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
            }
        }
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool hasDepth = false;
    bool hasStencil = false;
    if (VulkanTexture *texture = _device.FindTexture(desc.Depth.Texture))
    {
        hasDepth = true;
        hasStencil = IsStencilFormat(texture->Desc.Format);
        const VkImageView view = _device.GetAttachmentView(*texture, desc.Depth.Mip, desc.Depth.FirstSlice,
                                                           desc.Depth.SliceCount, RhiFormat::Unknown);
        const VkImageLayout layout =
            desc.Depth.ReadOnly ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        depth.imageView = view;
        depth.imageLayout = layout;
        depth.loadOp = VulkanConvert::Load(desc.Depth.DepthLoad);
        depth.storeOp = VulkanConvert::Store(desc.Depth.DepthStore);
        depth.clearValue.depthStencil = {desc.Depth.Clear.Depth, desc.Depth.Clear.Stencil};
        stencil = depth;
        stencil.loadOp = VulkanConvert::Load(desc.Depth.StencilLoad);
        stencil.storeOp = VulkanConvert::Store(desc.Depth.StencilStore);
        layerCount = desc.Depth.SliceCount > layerCount ? desc.Depth.SliceCount : layerCount;
    }
    VkRenderingFragmentShadingRateAttachmentInfoKHR shadingRate{
        VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    if (VulkanTexture *texture = _device.FindTexture(desc.ShadingRate))
    {
        const uint32 tile = _device.GetLimits().ShadingRateTileSize;
        shadingRate.imageView = _device.GetAttachmentView(*texture, 0, 0, 1, RhiFormat::Unknown);
        shadingRate.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        shadingRate.shadingRateAttachmentTexelSize = {tile, tile};
        info.pNext = &shadingRate;
    }
    info.renderArea = {{0, 0}, {desc.Width, desc.Height}};
    info.layerCount = desc.ViewMask != 0 ? 1 : layerCount;
    info.viewMask = desc.ViewMask;
    info.colorAttachmentCount = colorCount;
    info.pColorAttachments = colors;
    info.pDepthAttachment = hasDepth ? &depth : nullptr;
    info.pStencilAttachment = hasStencil ? &stencil : nullptr;
    Vk.vkCmdBeginRendering(_commandBuffer, &info);
    _inRendering = true;
    // Every graphics pipeline takes its shading rate as dynamic state, which must be set before the first draw.
    SetShadingRate(RhiShadingRate::Rate1x1, RhiShadingRateCombiner::Passthrough,
                   info.pNext != nullptr ? RhiShadingRateCombiner::Override : RhiShadingRateCombiner::Passthrough);
}

void VulkanCommandList::EndRendering()
{
    Vk.vkCmdEndRendering(_commandBuffer);
    _inRendering = false;
}

void VulkanCommandList::SetPipeline(RhiPipeline pipeline)
{
    const VulkanPipeline *entry = _device.FindPipeline(pipeline);
    _pipelineReady = entry != nullptr &&
                     static_cast<RhiPipelineState>(entry->State.Load(MemoryOrder::Acquire)) == RhiPipelineState::Ready;
    if (_pipelineReady)
    {
        _bindPoint = entry->BindPoint;
        Vk.vkCmdBindPipeline(_commandBuffer, entry->BindPoint, entry->Pipeline);
    }
}

void VulkanCommandList::SetShaderArguments(Span<const uint8> data, uint32 offset)
{
    MARLIN_ASSERT(offset + data.Count() <= RhiShaderArgumentsSize);
    VkPushDataInfoEXT info{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
    info.offset = offset;
    info.data = {data.Data(), data.Count()};
    Vk.vkCmdPushDataEXT(_commandBuffer, &info);
}

void VulkanCommandList::SetViewports(Span<const RhiViewport> viewports)
{
    VkViewport converted[16];
    const uint32 count = viewports.Count() < 16 ? static_cast<uint32>(viewports.Count()) : 16;
    for (uint32 i = 0; i < count; ++i)
    {
        const RhiViewport &viewport = viewports[i];
        converted[i] = {viewport.X, viewport.Y + viewport.Height, viewport.Width, -viewport.Height, viewport.MinDepth,
                        viewport.MaxDepth};
    }
    Vk.vkCmdSetViewportWithCount(_commandBuffer, count, converted);
}

void VulkanCommandList::SetScissors(Span<const RhiScissor> scissors)
{
    VkRect2D converted[16];
    const uint32 count = scissors.Count() < 16 ? static_cast<uint32>(scissors.Count()) : 16;
    for (uint32 i = 0; i < count; ++i)
    {
        converted[i] = {{scissors[i].X, scissors[i].Y}, {scissors[i].Width, scissors[i].Height}};
    }
    Vk.vkCmdSetScissorWithCount(_commandBuffer, count, converted);
}

void VulkanCommandList::SetStencilReference(uint8 reference)
{
    Vk.vkCmdSetStencilReference(_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void VulkanCommandList::SetBlendConstants(const float32 (&constants)[4])
{
    Vk.vkCmdSetBlendConstants(_commandBuffer, constants);
}

void VulkanCommandList::SetDepthBounds(float32 minimum, float32 maximum)
{
    Vk.vkCmdSetDepthBounds(_commandBuffer, minimum, maximum);
}

void VulkanCommandList::SetShadingRate(RhiShadingRate rate, RhiShadingRateCombiner primitive,
                                       RhiShadingRateCombiner attachment)
{
    if (!_device.HasVariableRateShading())
    {
        return;
    }
    const VkExtent2D extent = VulkanConvert::ShadingRate(rate);
    const VkFragmentShadingRateCombinerOpKHR combiners[2] = {VulkanConvert::Combiner(primitive),
                                                             VulkanConvert::Combiner(attachment)};
    Vk.vkCmdSetFragmentShadingRateKHR(_commandBuffer, &extent, combiners);
}

void VulkanCommandList::SetIndexBuffer(RhiBuffer buffer, uint64 offset, uint8 indexBits)
{
    if (const VulkanBuffer *entry = _device.FindBuffer(buffer))
    {
        Vk.vkCmdBindIndexBuffer2(_commandBuffer, entry->Buffer, offset, VK_WHOLE_SIZE,
                                 indexBits == 16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
}

void VulkanCommandList::Draw(const RhiDrawArguments &arguments)
{
    if (CanExecute())
    {
        Vk.vkCmdDraw(_commandBuffer, arguments.VertexCount, arguments.InstanceCount, arguments.FirstVertex,
                     arguments.FirstInstance);
    }
}

void VulkanCommandList::DrawIndexed(const RhiDrawIndexedArguments &arguments)
{
    if (CanExecute())
    {
        Vk.vkCmdDrawIndexed(_commandBuffer, arguments.IndexCount, arguments.InstanceCount, arguments.FirstIndex,
                            arguments.VertexOffset, arguments.FirstInstance);
    }
}

void VulkanCommandList::DrawMesh(uint32 x, uint32 y, uint32 z)
{
    if (CanExecute())
    {
        Vk.vkCmdDrawMeshTasksEXT(_commandBuffer, x, y, z);
    }
}

void VulkanCommandList::Dispatch(uint32 x, uint32 y, uint32 z)
{
    if (CanExecute())
    {
        Vk.vkCmdDispatch(_commandBuffer, x, y, z);
    }
}

void VulkanCommandList::DispatchRays(const RhiDispatchRaysDesc &desc)
{
    if (!CanExecute())
    {
        return;
    }
    const auto region = [](const RhiShaderTable &table) {
        return VkStridedDeviceAddressRegionKHR{table.Address, table.Stride, table.Size};
    };
    const VkStridedDeviceAddressRegionKHR rayGeneration = region(desc.RayGeneration);
    const VkStridedDeviceAddressRegionKHR miss = region(desc.Miss);
    const VkStridedDeviceAddressRegionKHR hit = region(desc.HitGroups);
    const VkStridedDeviceAddressRegionKHR callable = region(desc.Callable);
    Vk.vkCmdTraceRaysKHR(_commandBuffer, &rayGeneration, &miss, &hit, &callable, desc.Width, desc.Height, desc.Depth);
}

void VulkanCommandList::ExecuteIndirect(RhiCommandSignature signature, RhiBuffer arguments, uint64 argumentsOffset,
                                        uint32 maxCount, RhiBuffer countBuffer, uint64 countOffset)
{
    const VulkanCommandSignature *entry = _device.FindCommandSignature(signature);
    const VulkanBuffer *argumentBuffer = _device.FindBuffer(arguments);
    if (entry == nullptr || argumentBuffer == nullptr || !CanExecute())
    {
        return;
    }
    const VulkanBuffer *count = _device.FindBuffer(countBuffer);
    if (entry->Layout != VK_NULL_HANDLE)
    {
        uint64 preprocessAddress = 0;
        uint64 preprocessSize = 0;
        AllocatePreprocess(signature, *entry, maxCount, preprocessAddress, preprocessSize);
        VkGeneratedCommandsInfoEXT info{VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT};
        info.shaderStages = entry->Stages;
        info.indirectExecutionSet = entry->Set;
        info.indirectCommandsLayout = entry->Layout;
        info.indirectAddress = argumentBuffer->Address + argumentsOffset;
        info.indirectAddressSize = uint64{entry->Stride} * maxCount;
        info.preprocessAddress = preprocessAddress;
        info.preprocessSize = preprocessSize;
        info.maxSequenceCount = maxCount;
        info.sequenceCountAddress = count != nullptr ? count->Address + countOffset : 0;
        info.maxDrawCount = 1;
        Vk.vkCmdExecuteGeneratedCommandsEXT(_commandBuffer, VK_FALSE, &info);
        return;
    }
    const VkBuffer buffer = argumentBuffer->Buffer;
    switch (entry->Kind)
    {
    case RhiIndirectCommandKind::Draw:
        if (count != nullptr)
        {
            Vk.vkCmdDrawIndirectCount(_commandBuffer, buffer, argumentsOffset, count->Buffer, countOffset, maxCount,
                                      entry->Stride);
        }
        else
        {
            Vk.vkCmdDrawIndirect(_commandBuffer, buffer, argumentsOffset, maxCount, entry->Stride);
        }
        break;
    case RhiIndirectCommandKind::DrawIndexed:
        if (count != nullptr)
        {
            Vk.vkCmdDrawIndexedIndirectCount(_commandBuffer, buffer, argumentsOffset, count->Buffer, countOffset,
                                             maxCount, entry->Stride);
        }
        else
        {
            Vk.vkCmdDrawIndexedIndirect(_commandBuffer, buffer, argumentsOffset, maxCount, entry->Stride);
        }
        break;
    case RhiIndirectCommandKind::DrawMesh:
        if (count != nullptr)
        {
            Vk.vkCmdDrawMeshTasksIndirectCountEXT(_commandBuffer, buffer, argumentsOffset, count->Buffer, countOffset,
                                                  maxCount, entry->Stride);
        }
        else
        {
            Vk.vkCmdDrawMeshTasksIndirectEXT(_commandBuffer, buffer, argumentsOffset, maxCount, entry->Stride);
        }
        break;
    case RhiIndirectCommandKind::Dispatch:
        MARLIN_ASSERT(count == nullptr);
        for (uint32 i = 0; i < maxCount; ++i)
        {
            Vk.vkCmdDispatchIndirect(_commandBuffer, buffer, argumentsOffset + uint64{i} * entry->Stride);
        }
        break;
    case RhiIndirectCommandKind::DispatchRays:
        MARLIN_ASSERT(count == nullptr && Vk.vkCmdTraceRaysIndirect2KHR != nullptr);
        for (uint32 i = 0; i < maxCount; ++i)
        {
            Vk.vkCmdTraceRaysIndirect2KHR(_commandBuffer,
                                          argumentBuffer->Address + argumentsOffset + uint64{i} * entry->Stride);
        }
        break;
    default:
        break;
    }
}

void VulkanCommandList::CopyBuffer(RhiBuffer destination, uint64 destinationOffset, RhiBuffer source,
                                   uint64 sourceOffset, uint64 size)
{
    const VulkanBuffer *target = _device.FindBuffer(destination);
    const VulkanBuffer *from = _device.FindBuffer(source);
    if (target == nullptr || from == nullptr)
    {
        return;
    }
    VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
    region.srcOffset = sourceOffset;
    region.dstOffset = destinationOffset;
    region.size = size;
    VkCopyBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
    info.srcBuffer = from->Buffer;
    info.dstBuffer = target->Buffer;
    info.regionCount = 1;
    info.pRegions = &region;
    Vk.vkCmdCopyBuffer2(_commandBuffer, &info);
}

void VulkanCommandList::CopyTexture(RhiTexture destination, const RhiTextureRegion &destinationRegion,
                                    RhiTexture source, const RhiTextureRegion &sourceRegion)
{
    const VulkanTexture *target = _device.FindTexture(destination);
    const VulkanTexture *from = _device.FindTexture(source);
    if (target == nullptr || from == nullptr)
    {
        return;
    }
    VkImageCopy2 region{VK_STRUCTURE_TYPE_IMAGE_COPY_2};
    region.srcSubresource = Layers(*from, sourceRegion);
    region.srcOffset = {static_cast<int32>(sourceRegion.X), static_cast<int32>(sourceRegion.Y),
                        static_cast<int32>(sourceRegion.Z)};
    region.dstSubresource = Layers(*target, destinationRegion);
    region.dstOffset = {static_cast<int32>(destinationRegion.X), static_cast<int32>(destinationRegion.Y),
                        static_cast<int32>(destinationRegion.Z)};
    region.extent = Extent(*from, sourceRegion);
    VkCopyImageInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
    info.srcImage = from->Image;
    info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dstImage = target->Image;
    info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    info.regionCount = 1;
    info.pRegions = &region;
    Vk.vkCmdCopyImage2(_commandBuffer, &info);
}

void VulkanCommandList::CopyBufferToTexture(RhiTexture destination, const RhiTextureRegion &region, RhiBuffer source,
                                            const RhiBufferTextureLayout &layout)
{
    const VulkanTexture *target = _device.FindTexture(destination);
    const VulkanBuffer *from = _device.FindBuffer(source);
    if (target == nullptr || from == nullptr)
    {
        return;
    }
    const VkBufferImageCopy2 copy = BufferImageCopy(*target, region, layout);
    VkCopyBufferToImageInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    info.srcBuffer = from->Buffer;
    info.dstImage = target->Image;
    info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    info.regionCount = 1;
    info.pRegions = &copy;
    Vk.vkCmdCopyBufferToImage2(_commandBuffer, &info);
}

void VulkanCommandList::CopyTextureToBuffer(RhiBuffer destination, const RhiBufferTextureLayout &layout,
                                            RhiTexture source, const RhiTextureRegion &region)
{
    const VulkanBuffer *target = _device.FindBuffer(destination);
    const VulkanTexture *from = _device.FindTexture(source);
    if (target == nullptr || from == nullptr)
    {
        return;
    }
    const VkBufferImageCopy2 copy = BufferImageCopy(*from, region, layout);
    VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
    info.srcImage = from->Image;
    info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dstBuffer = target->Buffer;
    info.regionCount = 1;
    info.pRegions = &copy;
    Vk.vkCmdCopyImageToBuffer2(_commandBuffer, &info);
}

void VulkanCommandList::FillBuffer(RhiBuffer buffer, uint64 offset, uint64 size, uint32 value)
{
    if (const VulkanBuffer *entry = _device.FindBuffer(buffer))
    {
        Vk.vkCmdFillBuffer(_commandBuffer, entry->Buffer, offset, size == 0 ? VK_WHOLE_SIZE : size, value);
    }
}

void VulkanCommandList::ClearTexture(RhiTexture texture, const RhiSubresourceRange &range, const RhiClearValue &value)
{
    const VulkanTexture *entry = _device.FindTexture(texture);
    if (entry == nullptr)
    {
        return;
    }
    const VkImageSubresourceRange subresources = Range(*entry, range);
    if (IsDepthFormat(entry->Desc.Format))
    {
        const VkClearDepthStencilValue clear{value.Depth, value.Stencil};
        Vk.vkCmdClearDepthStencilImage(_commandBuffer, entry->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                       &subresources);
        return;
    }
    VkClearColorValue clear{};
    const RhiFormatKind kind = GetFormatInfo(entry->Desc.Format).Kind;
    for (uint32 i = 0; i < 4; ++i)
    {
        if (kind == RhiFormatKind::Uint)
        {
            clear.uint32[i] = static_cast<uint32>(value.Color[i]);
        }
        else if (kind == RhiFormatKind::Sint)
        {
            clear.int32[i] = static_cast<int32>(value.Color[i]);
        }
        else
        {
            clear.float32[i] = value.Color[i];
        }
    }
    Vk.vkCmdClearColorImage(_commandBuffer, entry->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                            &subresources);
}

void VulkanCommandList::ResolveTexture(RhiTexture destination, uint32 destinationMip, uint32 destinationSlice,
                                       RhiTexture source, uint32 sourceMip, uint32 sourceSlice)
{
    const VulkanTexture *target = _device.FindTexture(destination);
    const VulkanTexture *from = _device.FindTexture(source);
    if (target == nullptr || from == nullptr)
    {
        return;
    }
    VkImageResolve2 region{VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2};
    region.srcSubresource = {from->Aspect, sourceMip, sourceSlice, 1};
    region.dstSubresource = {target->Aspect, destinationMip, destinationSlice, 1};
    region.extent = {MipSize(from->Desc.Width, sourceMip), MipSize(from->Desc.Height, sourceMip), 1};
    VkResolveImageInfo2 info{VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2};
    info.srcImage = from->Image;
    info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dstImage = target->Image;
    info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    info.regionCount = 1;
    info.pRegions = &region;
    Vk.vkCmdResolveImage2(_commandBuffer, &info);
}

void VulkanCommandList::BuildAccelerationStructures(Span<const RhiAccelerationStructureBuild> builds)
{
    List<VulkanConvert::AccelerationStructureGeometry> geometries;
    geometries.Resize(builds.Count());
    List<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    List<const VkAccelerationStructureBuildRangeInfoKHR *> ranges;
    for (usize i = 0; i < builds.Count(); ++i)
    {
        const RhiAccelerationStructureBuild &build = builds[i];
        VulkanConvert::Geometry(build.Inputs, geometries[i]);
        VkAccelerationStructureBuildGeometryInfoKHR info = geometries[i].Info;
        const VulkanAccelerationStructure *destination = _device.FindAccelerationStructure(build.Destination);
        const VulkanAccelerationStructure *source = _device.FindAccelerationStructure(build.Source);
        info.dstAccelerationStructure = destination != nullptr ? destination->Structure : VK_NULL_HANDLE;
        if (build.Update && source != nullptr)
        {
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            info.srcAccelerationStructure = source->Structure;
        }
        info.scratchData.deviceAddress = build.ScratchAddress;
        infos.Add(info);
        ranges.Add(geometries[i].Ranges.Data());
    }
    Vk.vkCmdBuildAccelerationStructuresKHR(_commandBuffer, static_cast<uint32>(infos.Count()), infos.Data(),
                                           ranges.Data());
}

void VulkanCommandList::CopyAccelerationStructure(RhiAccelerationStructure destination,
                                                  RhiAccelerationStructure source,
                                                  RhiAccelerationStructureCopyMode mode)
{
    const VulkanAccelerationStructure *target = _device.FindAccelerationStructure(destination);
    const VulkanAccelerationStructure *from = _device.FindAccelerationStructure(source);
    if (target == nullptr || from == nullptr)
    {
        return;
    }
    VkCopyAccelerationStructureInfoKHR info{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
    info.src = from->Structure;
    info.dst = target->Structure;
    info.mode = mode == RhiAccelerationStructureCopyMode::Compact ? VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR
                                                                   : VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
    Vk.vkCmdCopyAccelerationStructureKHR(_commandBuffer, &info);
}

void VulkanCommandList::CopyAccelerationStructureMemory(uint64 destinationAddress,
                                                        RhiAccelerationStructure structure,
                                                        RhiAccelerationStructureCopyMode mode, uint64 sourceAddress)
{
    const VulkanAccelerationStructure *entry = _device.FindAccelerationStructure(structure);
    if (entry == nullptr)
    {
        return;
    }
    if (mode == RhiAccelerationStructureCopyMode::Serialize)
    {
        VkCopyAccelerationStructureToMemoryInfoKHR info{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR};
        info.src = entry->Structure;
        info.dst.deviceAddress = destinationAddress;
        info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
        Vk.vkCmdCopyAccelerationStructureToMemoryKHR(_commandBuffer, &info);
    }
    else if (mode == RhiAccelerationStructureCopyMode::Deserialize)
    {
        VkCopyMemoryToAccelerationStructureInfoKHR info{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR};
        info.src.deviceAddress = sourceAddress;
        info.dst = entry->Structure;
        info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
        Vk.vkCmdCopyMemoryToAccelerationStructureKHR(_commandBuffer, &info);
    }
}

void VulkanCommandList::WriteAccelerationStructureProperties(Span<const RhiAccelerationStructure> structures,
                                                             RhiAccelerationStructureProperty property,
                                                             RhiBuffer destination, uint64 offset)
{
    const VulkanBuffer *target = _device.FindBuffer(destination);
    if (target == nullptr || structures.IsEmpty())
    {
        return;
    }
    const VkQueryType type = property == RhiAccelerationStructureProperty::CompactedSize
                                 ? VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR
                                 : VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR;
    const uint32 count = static_cast<uint32>(structures.Count());
    uint32 first = 0;
    const VkQueryPool pool = AllocatePropertyQueries(type, count, first);
    if (pool == VK_NULL_HANDLE)
    {
        return;
    }
    InlineList<VkAccelerationStructureKHR, 16> handles;
    for (const RhiAccelerationStructure structure : structures)
    {
        const VulkanAccelerationStructure *entry = _device.FindAccelerationStructure(structure);
        handles.Add(entry != nullptr ? entry->Structure : VK_NULL_HANDLE);
    }
    Vk.vkCmdWriteAccelerationStructuresPropertiesKHR(_commandBuffer, static_cast<uint32>(handles.Count()),
                                                     handles.Data(), type, pool, first);
    Vk.vkCmdCopyQueryPoolResults(_commandBuffer, pool, first, count, target->Buffer, offset,
                                 sizeof(uint64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

void VulkanCommandList::WriteTimestamp(RhiQueryPool pool, uint32 index)
{
    if (const VulkanQueryPool *entry = _device.FindQueryPool(pool))
    {
        Vk.vkCmdWriteTimestamp2(_commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, entry->Pool, index);
    }
}

void VulkanCommandList::BeginQuery(RhiQueryPool pool, uint32 index)
{
    if (const VulkanQueryPool *entry = _device.FindQueryPool(pool))
    {
        Vk.vkCmdBeginQuery(_commandBuffer, entry->Pool, index, 0);
    }
}

void VulkanCommandList::EndQuery(RhiQueryPool pool, uint32 index)
{
    if (const VulkanQueryPool *entry = _device.FindQueryPool(pool))
    {
        Vk.vkCmdEndQuery(_commandBuffer, entry->Pool, index);
    }
}

void VulkanCommandList::ResolveQueries(RhiQueryPool pool, uint32 first, uint32 count, RhiBuffer destination,
                                       uint64 offset)
{
    const VulkanQueryPool *entry = _device.FindQueryPool(pool);
    const VulkanBuffer *target = _device.FindBuffer(destination);
    if (entry == nullptr || target == nullptr)
    {
        return;
    }
    const uint64 stride = entry->Kind == RhiQueryKind::PipelineStatistics ? sizeof(RhiPipelineStatistics) : sizeof(uint64);
    Vk.vkCmdCopyQueryPoolResults(_commandBuffer, entry->Pool, first, count, target->Buffer, offset, stride,
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    Vk.vkCmdResetQueryPool(_commandBuffer, entry->Pool, first, count);
}
} // namespace Marlin
