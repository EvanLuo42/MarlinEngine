#include "ValidationCommandList.h"

#include "HAL/PlatformThread.h"
#include "ValidationDevice.h"

namespace Marlin
{
namespace
{
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

uint32 MipSize(uint32 size, uint32 mip)
{
    const uint32 value = size >> mip;
    return value > 0 ? value : 1;
}

/// The work each queue type can wait for or block.
RhiSync QueueSync(RhiQueueType queue)
{
    switch (queue)
    {
    case RhiQueueType::Graphics:
        return RhiSync::All;
    case RhiQueueType::Compute:
        return RhiSync::ComputeShading | RhiSync::RayTracing | RhiSync::Copy | RhiSync::IndirectArguments |
               RhiSync::AccelerationStructureBuild | RhiSync::AccelerationStructureCopy | RhiSync::Host |
               RhiSync::IndirectPreprocess;
    case RhiQueueType::Copy:
        return RhiSync::Copy | RhiSync::Host;
    }
    return RhiSync::All;
}
} // namespace

ValidationCommandList::ValidationCommandList(ValidationDevice &device, RhiCommandList &inner)
    : _device(device), _inner(inner), _queue(inner.GetQueueType())
{
}

ValidationCommandList::Use::Use(ValidationCommandList &list, const char *call) : _list(list)
{
    const uint32 thread = PlatformThread::CurrentId();
    uint32 expected = 0;
    if (!_list._user.CompareExchange(expected, thread))
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} on thread {1} while thread {2} uses the same command list", call, thread,
                                    expected);
    }
    if (_list._submitted)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} on a command list that was already submitted or discarded", call);
    }
    if (_list._native)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} between BeginNativeCommands and EndNativeCommands", call);
    }
}

ValidationCommandList::Use::~Use()
{
    _list._user.Store(0);
}

bool ValidationCommandList::PrepareSubmit(const char *call)
{
    if (_submitted)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} of a command list that was already submitted or discarded", call);
        return false;
    }
    if (_user.Load() != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} of a command list another thread is recording", call);
    }
    if (_rendering)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} of a command list inside BeginRendering", call);
    }
    if (_markerDepth != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} of a command list with {1} markers open", call, _markerDepth);
    }
    if (_openQueries != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} of a command list with {1} queries open", call, _openQueries);
    }
    if (_native)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} of a command list still in native commands", call);
    }
    _submitted = true;
    return true;
}

void ValidationCommandList::RequireQueue(bool graphics, bool compute, bool copy, const char *call) const
{
    const bool allowed = (_queue == RhiQueueType::Graphics && graphics) ||
                         (_queue == RhiQueueType::Compute && compute) || (_queue == RhiQueueType::Copy && copy);
    if (!allowed)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} on a {1} command list", call, QueueName(_queue));
    }
}

void ValidationCommandList::RequireRendering(bool inside, const char *call) const
{
    if (_rendering != inside)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} {1} BeginRendering and EndRendering", call, inside ? "outside" : "between");
    }
}

void ValidationCommandList::RequirePipeline(RhiPipelineKind kind, const char *call) const
{
    if (!_hasPipeline || _pipelineKind != kind)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} without a {1} pipeline set", call,
                                    kind == RhiPipelineKind::Graphics  ? "graphics"
                                    : kind == RhiPipelineKind::Compute ? "compute"
                                                                       : "ray tracing");
    }
}

Optional<RhiBufferDesc> ValidationCommandList::RequireBuffer(RhiBuffer buffer, const char *call) const
{
    Optional<RhiBufferDesc> desc = _device.FindBuffer(buffer);
    if (!desc.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} with a stale buffer (slot {1})", call, buffer.Index);
    }
    return desc;
}

Optional<RhiTextureDesc> ValidationCommandList::RequireTexture(RhiTexture texture, const char *call) const
{
    Optional<RhiTextureDesc> desc = _device.FindTexture(texture);
    if (!desc.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("{0} with a stale texture (slot {1})", call, texture.Index);
    }
    return desc;
}

void ValidationCommandList::CheckBufferRange(const RhiBufferDesc &desc, uint64 offset, uint64 size,
                                             const char *call) const
{
    if (offset > desc.Size || size > desc.Size - offset)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0}: bytes {1}+{2} are outside the {3}-byte buffer", call, offset, size,
                                    desc.Size);
    }
}

void ValidationCommandList::CheckRegion(const RhiTextureDesc &desc, const RhiTextureRegion &region,
                                        const char *call) const
{
    const uint32 slices = desc.Dimension == RhiTextureDimension::Texture3D ? 1 : desc.DepthOrArraySize;
    if (region.Mip >= desc.MipCount || region.Slice >= slices)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0}: mip {1} or slice {2} is outside the texture", call, region.Mip,
                                    region.Slice);
        return;
    }
    const uint32 width = MipSize(desc.Width, region.Mip);
    const uint32 height = MipSize(desc.Height, region.Mip);
    const uint32 depth = desc.Dimension == RhiTextureDimension::Texture3D ? MipSize(desc.DepthOrArraySize, region.Mip)
                                                                          : 1;
    if (region.X + region.Width > width || region.Y + region.Height > height || region.Z + region.Depth > depth)
    {
        MARLIN_RHI_VALIDATION_ERROR("{0}: the region is outside the {1}x{2}x{3} mip", call, width, height, depth);
    }
}

RhiQueueType ValidationCommandList::GetQueueType() const
{
    return _queue;
}

void ValidationCommandList::Barrier(const RhiBarriers &barriers)
{
    const Use use(*this, "Barrier");
    RequireRendering(false, "Barrier");
    const RhiSync allowed = QueueSync(_queue);
    const auto checkSync = [&](RhiSync before, RhiSync after, RhiAccess accessBefore, RhiAccess accessAfter) {
        if (before != RhiSync::All && HasAnyFlags(before, ~allowed))
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: SyncBefore names work a {0} queue cannot do", QueueName(_queue));
        }
        if (after != RhiSync::All && HasAnyFlags(after, ~allowed))
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: SyncAfter names work a {0} queue cannot do", QueueName(_queue));
        }
        if ((before == RhiSync::None) != (accessBefore == RhiAccess::None) && accessBefore != RhiAccess::None)
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: AccessBefore without SyncBefore");
        }
        if (after == RhiSync::None && accessAfter != RhiAccess::None)
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: AccessAfter without SyncAfter");
        }
    };
    for (const RhiGlobalBarrier &barrier : barriers.Global)
    {
        checkSync(barrier.SyncBefore, barrier.SyncAfter, barrier.AccessBefore, barrier.AccessAfter);
    }
    for (const RhiBufferBarrier &barrier : barriers.Buffers)
    {
        checkSync(barrier.SyncBefore, barrier.SyncAfter, barrier.AccessBefore, barrier.AccessAfter);
        if (const Optional<RhiBufferDesc> desc = RequireBuffer(barrier.Buffer, "Barrier"); desc.HasValue())
        {
            CheckBufferRange(*desc, barrier.Offset, barrier.Size == 0 ? desc->Size - barrier.Offset : barrier.Size,
                             "Barrier");
        }
        if (barrier.TransferQueue && barrier.Transfer.From != _queue && barrier.Transfer.To != _queue)
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: a queue transfer recorded on neither of its queues");
        }
    }
    for (const RhiTextureBarrier &barrier : barriers.Textures)
    {
        checkSync(barrier.SyncBefore, barrier.SyncAfter, barrier.AccessBefore, barrier.AccessAfter);
        const Optional<RhiTextureDesc> desc = RequireTexture(barrier.Texture, "Barrier");
        if (!desc.HasValue())
        {
            continue;
        }
        if (barrier.LayoutAfter == RhiLayout::Undefined)
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: Undefined is only a layout before");
        }
        if ((barrier.LayoutAfter == RhiLayout::Present || barrier.LayoutBefore == RhiLayout::Present) &&
            !HasAnyFlags(desc->Usage, RhiTextureUsage::Present))
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: the Present layout on a texture that is not a swapchain image");
        }
        const struct
        {
            RhiLayout Layout;
            RhiTextureUsage Usage;
        } needs[] = {
            {RhiLayout::RenderTarget, RhiTextureUsage::RenderTarget},
            {RhiLayout::DepthStencilWrite, RhiTextureUsage::DepthStencil},
            {RhiLayout::DepthStencilRead, RhiTextureUsage::DepthStencil},
            {RhiLayout::UnorderedAccess, RhiTextureUsage::UnorderedAccess},
            {RhiLayout::ShaderResource, RhiTextureUsage::ShaderResource},
            {RhiLayout::CopySource, RhiTextureUsage::CopySource},
            {RhiLayout::CopyDest, RhiTextureUsage::CopyDest},
            {RhiLayout::ShadingRateSource, RhiTextureUsage::ShadingRate},
        };
        for (const auto &need : needs)
        {
            if (barrier.LayoutAfter == need.Layout && !HasAnyFlags(desc->Usage, need.Usage))
            {
                MARLIN_RHI_VALIDATION_ERROR("Barrier: a layout the texture's usage does not allow");
            }
        }
        const uint32 slices = desc->Dimension == RhiTextureDimension::Texture3D ? 1 : desc->DepthOrArraySize;
        const RhiSubresourceRange &range = barrier.Range;
        const uint32 mipCount = range.MipCount == 0 ? desc->MipCount - range.FirstMip : range.MipCount;
        const uint32 sliceCount = range.SliceCount == 0 ? slices - range.FirstSlice : range.SliceCount;
        if (range.FirstMip + mipCount > desc->MipCount || range.FirstSlice + sliceCount > slices)
        {
            MARLIN_RHI_VALIDATION_ERROR("Barrier: the subresource range is outside the texture");
        }
    }
    _inner.Barrier(barriers);
}

void *ValidationCommandList::BeginNativeCommands()
{
    const Use use(*this, "BeginNativeCommands");
    RequireRendering(false, "BeginNativeCommands");
    _native = true;
    return _inner.BeginNativeCommands();
}

void ValidationCommandList::EndNativeCommands()
{
    if (!_native)
    {
        MARLIN_RHI_VALIDATION_ERROR("EndNativeCommands without BeginNativeCommands");
        return;
    }
    _native = false;
    const Use use(*this, "EndNativeCommands");
    _hasPipeline = false;
    _inner.EndNativeCommands();
}

void ValidationCommandList::BeginMarker(Name name)
{
    const Use use(*this, "BeginMarker");
    ++_markerDepth;
    _inner.BeginMarker(name);
}

void ValidationCommandList::EndMarker()
{
    const Use use(*this, "EndMarker");
    if (_markerDepth == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("EndMarker without a BeginMarker");
        return;
    }
    --_markerDepth;
    _inner.EndMarker();
}

void ValidationCommandList::BeginRendering(const RhiRenderingDesc &desc)
{
    const Use use(*this, "BeginRendering");
    RequireQueue(true, false, false, "BeginRendering");
    RequireRendering(false, "BeginRendering");
    if (desc.RenderTargets.Count() > RhiMaxRenderTargets || desc.Width == 0 || desc.Height == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("BeginRendering: more than {0} render targets, or no size", RhiMaxRenderTargets);
    }
    for (const RhiRenderTarget &target : desc.RenderTargets)
    {
        const Optional<RhiTextureDesc> texture = RequireTexture(target.Texture, "BeginRendering");
        if (!texture.HasValue())
        {
            continue;
        }
        if (!HasAnyFlags(texture->Usage, RhiTextureUsage::RenderTarget))
        {
            MARLIN_RHI_VALIDATION_ERROR("BeginRendering: a render target without the RenderTarget usage");
        }
        if (MipSize(texture->Width, target.Mip) < desc.Width || MipSize(texture->Height, target.Mip) < desc.Height)
        {
            MARLIN_RHI_VALIDATION_ERROR("BeginRendering: the render area is larger than a render target");
        }
        if (target.Store == RhiStoreOp::Resolve)
        {
            const Optional<RhiTextureDesc> resolve = RequireTexture(target.ResolveTarget, "BeginRendering");
            if (texture->SampleCount == 1 || (resolve.HasValue() && resolve->SampleCount != 1))
            {
                MARLIN_RHI_VALIDATION_ERROR("BeginRendering: resolves go from a multisampled target to a single "
                                            "sampled one");
            }
        }
    }
    if (desc.Depth.Texture.IsValid())
    {
        const Optional<RhiTextureDesc> depth = RequireTexture(desc.Depth.Texture, "BeginRendering");
        if (depth.HasValue() && !HasAnyFlags(depth->Usage, RhiTextureUsage::DepthStencil))
        {
            MARLIN_RHI_VALIDATION_ERROR("BeginRendering: a depth target without the DepthStencil usage");
        }
    }
    if (desc.ShadingRate.IsValid())
    {
        const Optional<RhiTextureDesc> rate = RequireTexture(desc.ShadingRate, "BeginRendering");
        if (rate.HasValue() && !HasAnyFlags(rate->Usage, RhiTextureUsage::ShadingRate))
        {
            MARLIN_RHI_VALIDATION_ERROR("BeginRendering: a shading rate texture without the ShadingRate usage");
        }
    }
    _rendering = true;
    _inner.BeginRendering(desc);
}

void ValidationCommandList::EndRendering()
{
    const Use use(*this, "EndRendering");
    RequireRendering(true, "EndRendering");
    _rendering = false;
    _inner.EndRendering();
}

void ValidationCommandList::SetPipeline(RhiPipeline pipeline)
{
    const Use use(*this, "SetPipeline");
    RequireQueue(true, true, false, "SetPipeline");
    const Optional<RhiPipelineKind> kind = _device.FindPipelineKind(pipeline);
    if (!kind.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("SetPipeline with a stale pipeline (slot {0})", pipeline.Index);
        _hasPipeline = false;
        return;
    }
    _pipelineKind = kind.Value();
    _hasPipeline = true;
    if (_pipelineKind == RhiPipelineKind::Graphics && _queue != RhiQueueType::Graphics)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetPipeline of a graphics pipeline on a {0} command list", QueueName(_queue));
    }
    _inner.SetPipeline(pipeline);
}

void ValidationCommandList::SetShaderArguments(Span<const uint8> data, uint32 offset)
{
    const Use use(*this, "SetShaderArguments");
    RequireQueue(true, true, false, "SetShaderArguments");
    if (offset % 4 != 0 || data.Count() % 4 != 0 || offset + data.Count() > RhiShaderArgumentsSize)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetShaderArguments: bytes {0}+{1} are not in 4-byte words within {2}", offset,
                                    data.Count(), RhiShaderArgumentsSize);
        return;
    }
    _inner.SetShaderArguments(data, offset);
}

void ValidationCommandList::SetViewports(Span<const RhiViewport> viewports)
{
    const Use use(*this, "SetViewports");
    RequireQueue(true, false, false, "SetViewports");
    if (viewports.IsEmpty() || viewports.Count() > 16)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetViewports: between 1 and 16 viewports");
    }
    _inner.SetViewports(viewports);
}

void ValidationCommandList::SetScissors(Span<const RhiScissor> scissors)
{
    const Use use(*this, "SetScissors");
    RequireQueue(true, false, false, "SetScissors");
    if (scissors.IsEmpty() || scissors.Count() > 16)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetScissors: between 1 and 16 scissors");
    }
    _inner.SetScissors(scissors);
}

void ValidationCommandList::SetStencilReference(uint8 reference)
{
    const Use use(*this, "SetStencilReference");
    RequireQueue(true, false, false, "SetStencilReference");
    _inner.SetStencilReference(reference);
}

void ValidationCommandList::SetBlendConstants(const float32 (&constants)[4])
{
    const Use use(*this, "SetBlendConstants");
    RequireQueue(true, false, false, "SetBlendConstants");
    _inner.SetBlendConstants(constants);
}

void ValidationCommandList::SetDepthBounds(float32 minimum, float32 maximum)
{
    const Use use(*this, "SetDepthBounds");
    RequireQueue(true, false, false, "SetDepthBounds");
    if (minimum < 0 || maximum > 1 || minimum > maximum)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetDepthBounds: [{0}, {1}] is not within [0, 1]", minimum, maximum);
    }
    _inner.SetDepthBounds(minimum, maximum);
}

void ValidationCommandList::SetShadingRate(RhiShadingRate rate, RhiShadingRateCombiner primitive,
                                           RhiShadingRateCombiner attachment)
{
    const Use use(*this, "SetShadingRate");
    RequireQueue(true, false, false, "SetShadingRate");
    if (!_device.HasFeature(StringView("VariableRateShading")))
    {
        MARLIN_RHI_VALIDATION_ERROR("SetShadingRate on a device without VariableRateShading");
    }
    _inner.SetShadingRate(rate, primitive, attachment);
}

void ValidationCommandList::SetIndexBuffer(RhiBuffer buffer, uint64 offset, uint8 indexBits)
{
    const Use use(*this, "SetIndexBuffer");
    RequireQueue(true, false, false, "SetIndexBuffer");
    const Optional<RhiBufferDesc> desc = RequireBuffer(buffer, "SetIndexBuffer");
    if (desc.HasValue() && !HasAnyFlags(desc->Usage, RhiBufferUsage::IndexBuffer))
    {
        MARLIN_RHI_VALIDATION_ERROR("SetIndexBuffer: the buffer lacks the IndexBuffer usage");
    }
    if (indexBits != 16 && indexBits != 32)
    {
        MARLIN_RHI_VALIDATION_ERROR("SetIndexBuffer: {0}-bit indices; only 16 and 32", indexBits);
    }
    _inner.SetIndexBuffer(buffer, offset, indexBits);
}

void ValidationCommandList::Draw(const RhiDrawArguments &arguments)
{
    const Use use(*this, "Draw");
    RequireRendering(true, "Draw");
    RequirePipeline(RhiPipelineKind::Graphics, "Draw");
    _inner.Draw(arguments);
}

void ValidationCommandList::DrawIndexed(const RhiDrawIndexedArguments &arguments)
{
    const Use use(*this, "DrawIndexed");
    RequireRendering(true, "DrawIndexed");
    RequirePipeline(RhiPipelineKind::Graphics, "DrawIndexed");
    _inner.DrawIndexed(arguments);
}

void ValidationCommandList::DrawMesh(uint32 x, uint32 y, uint32 z)
{
    const Use use(*this, "DrawMesh");
    RequireRendering(true, "DrawMesh");
    RequirePipeline(RhiPipelineKind::Graphics, "DrawMesh");
    _inner.DrawMesh(x, y, z);
}

void ValidationCommandList::Dispatch(uint32 x, uint32 y, uint32 z)
{
    const Use use(*this, "Dispatch");
    RequireQueue(true, true, false, "Dispatch");
    RequireRendering(false, "Dispatch");
    RequirePipeline(RhiPipelineKind::Compute, "Dispatch");
    _inner.Dispatch(x, y, z);
}

void ValidationCommandList::DispatchRays(const RhiDispatchRaysDesc &desc)
{
    const Use use(*this, "DispatchRays");
    RequireQueue(true, true, false, "DispatchRays");
    RequireRendering(false, "DispatchRays");
    RequirePipeline(RhiPipelineKind::RayTracing, "DispatchRays");
    const uint32 alignment = _device.GetLimits().ShaderTableAlignment;
    for (const RhiShaderTable *table : {&desc.RayGeneration, &desc.Miss, &desc.HitGroups, &desc.Callable})
    {
        if (alignment != 0 && table->Address % alignment != 0)
        {
            MARLIN_RHI_VALIDATION_ERROR("DispatchRays: a shader table is not aligned to {0}", alignment);
        }
    }
    if (desc.RayGeneration.Address == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("DispatchRays without a ray generation table");
    }
    _inner.DispatchRays(desc);
}

void ValidationCommandList::ExecuteIndirect(RhiCommandSignature signature, RhiBuffer arguments,
                                            uint64 argumentsOffset, uint32 maxCount, RhiBuffer countBuffer,
                                            uint64 countOffset)
{
    const Use use(*this, "ExecuteIndirect");
    const Optional<RhiIndirectCommandKind> kind = _device.FindCommandSignature(signature);
    if (!kind.HasValue())
    {
        MARLIN_RHI_VALIDATION_ERROR("ExecuteIndirect with a stale command signature");
        return;
    }
    const bool draws = *kind == RhiIndirectCommandKind::Draw || *kind == RhiIndirectCommandKind::DrawIndexed ||
                       *kind == RhiIndirectCommandKind::DrawMesh;
    RequireRendering(draws, "ExecuteIndirect");
    RequirePipeline(draws ? RhiPipelineKind::Graphics
                    : *kind == RhiIndirectCommandKind::Dispatch ? RhiPipelineKind::Compute
                                                                : RhiPipelineKind::RayTracing,
                    "ExecuteIndirect");
    const Optional<RhiBufferDesc> buffer = RequireBuffer(arguments, "ExecuteIndirect");
    if (buffer.HasValue() && !HasAnyFlags(buffer->Usage, RhiBufferUsage::IndirectArguments))
    {
        MARLIN_RHI_VALIDATION_ERROR("ExecuteIndirect: the arguments buffer lacks the IndirectArguments usage");
    }
    if (countBuffer.IsValid())
    {
        const Optional<RhiBufferDesc> count = RequireBuffer(countBuffer, "ExecuteIndirect");
        if (count.HasValue())
        {
            CheckBufferRange(*count, countOffset, sizeof(uint32), "ExecuteIndirect");
        }
        if ((*kind == RhiIndirectCommandKind::Dispatch || *kind == RhiIndirectCommandKind::DispatchRays) &&
            signature == _device.GetCommandSignature(*kind))
        {
            MARLIN_RHI_VALIDATION_ERROR("ExecuteIndirect: a count buffer with the plain Dispatch or DispatchRays "
                                        "signature; make a generated command signature for it");
        }
    }
    if (argumentsOffset % 4 != 0 || maxCount == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("ExecuteIndirect: the arguments offset is unaligned, or maxCount is zero");
    }
    _inner.ExecuteIndirect(signature, arguments, argumentsOffset, maxCount, countBuffer, countOffset);
}

void ValidationCommandList::CopyBuffer(RhiBuffer destination, uint64 destinationOffset, RhiBuffer source,
                                       uint64 sourceOffset, uint64 size)
{
    const Use use(*this, "CopyBuffer");
    RequireRendering(false, "CopyBuffer");
    const Optional<RhiBufferDesc> target = RequireBuffer(destination, "CopyBuffer");
    const Optional<RhiBufferDesc> from = RequireBuffer(source, "CopyBuffer");
    if (target.HasValue())
    {
        CheckBufferRange(*target, destinationOffset, size, "CopyBuffer");
        if (!HasAnyFlags(target->Usage, RhiBufferUsage::CopyDest) && target->Location == RhiMemoryLocation::Device)
        {
            MARLIN_RHI_VALIDATION_ERROR("CopyBuffer: the destination lacks the CopyDest usage");
        }
    }
    if (from.HasValue())
    {
        CheckBufferRange(*from, sourceOffset, size, "CopyBuffer");
    }
    if (destination == source && destinationOffset < sourceOffset + size && sourceOffset < destinationOffset + size)
    {
        MARLIN_RHI_VALIDATION_ERROR("CopyBuffer: the source and destination ranges overlap");
    }
    _inner.CopyBuffer(destination, destinationOffset, source, sourceOffset, size);
}

void ValidationCommandList::CopyTexture(RhiTexture destination, const RhiTextureRegion &destinationRegion,
                                        RhiTexture source, const RhiTextureRegion &sourceRegion)
{
    const Use use(*this, "CopyTexture");
    RequireRendering(false, "CopyTexture");
    if (const Optional<RhiTextureDesc> target = RequireTexture(destination, "CopyTexture"); target.HasValue())
    {
        CheckRegion(*target, destinationRegion, "CopyTexture");
    }
    if (const Optional<RhiTextureDesc> from = RequireTexture(source, "CopyTexture"); from.HasValue())
    {
        CheckRegion(*from, sourceRegion, "CopyTexture");
    }
    _inner.CopyTexture(destination, destinationRegion, source, sourceRegion);
}

void ValidationCommandList::CopyBufferToTexture(RhiTexture destination, const RhiTextureRegion &region,
                                                RhiBuffer source, const RhiBufferTextureLayout &layout)
{
    const Use use(*this, "CopyBufferToTexture");
    RequireRendering(false, "CopyBufferToTexture");
    const Optional<RhiTextureDesc> target = RequireTexture(destination, "CopyBufferToTexture");
    if (target.HasValue())
    {
        CheckRegion(*target, region, "CopyBufferToTexture");
        if (!HasAnyFlags(target->Usage, RhiTextureUsage::CopyDest))
        {
            MARLIN_RHI_VALIDATION_ERROR("CopyBufferToTexture: the texture lacks the CopyDest usage");
        }
    }
    RequireBuffer(source, "CopyBufferToTexture");
    if (layout.Offset % GetFormatInfo(target.HasValue() ? target->Format : RhiFormat::R8Uint).BlockBytes != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("CopyBufferToTexture: the buffer offset is not a multiple of the block size");
    }
    _inner.CopyBufferToTexture(destination, region, source, layout);
}

void ValidationCommandList::CopyTextureToBuffer(RhiBuffer destination, const RhiBufferTextureLayout &layout,
                                                RhiTexture source, const RhiTextureRegion &region)
{
    const Use use(*this, "CopyTextureToBuffer");
    RequireRendering(false, "CopyTextureToBuffer");
    const Optional<RhiTextureDesc> from = RequireTexture(source, "CopyTextureToBuffer");
    if (from.HasValue())
    {
        CheckRegion(*from, region, "CopyTextureToBuffer");
        if (!HasAnyFlags(from->Usage, RhiTextureUsage::CopySource))
        {
            MARLIN_RHI_VALIDATION_ERROR("CopyTextureToBuffer: the texture lacks the CopySource usage");
        }
    }
    RequireBuffer(destination, "CopyTextureToBuffer");
    _inner.CopyTextureToBuffer(destination, layout, source, region);
}

void ValidationCommandList::FillBuffer(RhiBuffer buffer, uint64 offset, uint64 size, uint32 value)
{
    const Use use(*this, "FillBuffer");
    RequireRendering(false, "FillBuffer");
    const Optional<RhiBufferDesc> desc = RequireBuffer(buffer, "FillBuffer");
    if (offset % 4 != 0 || size % 4 != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("FillBuffer: offset {0} and size {1} are not multiples of four", offset, size);
    }
    if (desc.HasValue())
    {
        CheckBufferRange(*desc, offset, size == 0 ? desc->Size - offset : size, "FillBuffer");
        if (!HasAnyFlags(desc->Usage, RhiBufferUsage::CopyDest))
        {
            MARLIN_RHI_VALIDATION_ERROR("FillBuffer: the buffer lacks the CopyDest usage");
        }
    }
    _inner.FillBuffer(buffer, offset, size, value);
}

void ValidationCommandList::ClearTexture(RhiTexture texture, const RhiSubresourceRange &range,
                                         const RhiClearValue &value)
{
    const Use use(*this, "ClearTexture");
    RequireQueue(true, true, false, "ClearTexture");
    RequireRendering(false, "ClearTexture");
    const Optional<RhiTextureDesc> desc = RequireTexture(texture, "ClearTexture");
    if (desc.HasValue() && !HasAnyFlags(desc->Usage, RhiTextureUsage::CopyDest))
    {
        MARLIN_RHI_VALIDATION_ERROR("ClearTexture: the texture lacks the CopyDest usage");
    }
    if (desc.HasValue() && IsDepthFormat(desc->Format) && _queue != RhiQueueType::Graphics)
    {
        MARLIN_RHI_VALIDATION_ERROR("ClearTexture of a depth texture outside the Graphics queue");
    }
    _inner.ClearTexture(texture, range, value);
}

void ValidationCommandList::ResolveTexture(RhiTexture destination, uint32 destinationMip, uint32 destinationSlice,
                                           RhiTexture source, uint32 sourceMip, uint32 sourceSlice)
{
    const Use use(*this, "ResolveTexture");
    RequireQueue(true, false, false, "ResolveTexture");
    RequireRendering(false, "ResolveTexture");
    const Optional<RhiTextureDesc> target = RequireTexture(destination, "ResolveTexture");
    const Optional<RhiTextureDesc> from = RequireTexture(source, "ResolveTexture");
    if (target.HasValue() && from.HasValue() && (from->SampleCount == 1 || target->SampleCount != 1))
    {
        MARLIN_RHI_VALIDATION_ERROR("ResolveTexture goes from a multisampled texture to a single sampled one");
    }
    _inner.ResolveTexture(destination, destinationMip, destinationSlice, source, sourceMip, sourceSlice);
}

void ValidationCommandList::BuildAccelerationStructures(Span<const RhiAccelerationStructureBuild> builds)
{
    const Use use(*this, "BuildAccelerationStructures");
    RequireQueue(true, true, false, "BuildAccelerationStructures");
    RequireRendering(false, "BuildAccelerationStructures");
    for (const RhiAccelerationStructureBuild &build : builds)
    {
        if (!_device.IsLive(build.Destination) || (build.Update && !_device.IsLive(build.Source)))
        {
            MARLIN_RHI_VALIDATION_ERROR("BuildAccelerationStructures with a stale structure");
        }
        if (build.Update && !HasAnyFlags(build.Inputs.Flags, RhiBuildFlags::AllowUpdate))
        {
            MARLIN_RHI_VALIDATION_ERROR("BuildAccelerationStructures: an update of a build without AllowUpdate");
        }
        if (build.ScratchAddress == 0)
        {
            MARLIN_RHI_VALIDATION_ERROR("BuildAccelerationStructures without scratch memory");
        }
        if (build.Inputs.Kind == RhiAccelerationStructureKind::TopLevel && build.Inputs.InstanceCount > 0 &&
            build.Inputs.InstanceAddress % 16 != 0)
        {
            MARLIN_RHI_VALIDATION_ERROR("BuildAccelerationStructures: instances are not 16-byte aligned");
        }
    }
    _inner.BuildAccelerationStructures(builds);
}

void ValidationCommandList::CopyAccelerationStructure(RhiAccelerationStructure destination,
                                                      RhiAccelerationStructure source,
                                                      RhiAccelerationStructureCopyMode mode)
{
    const Use use(*this, "CopyAccelerationStructure");
    RequireQueue(true, true, false, "CopyAccelerationStructure");
    if (!_device.IsLive(destination) || !_device.IsLive(source))
    {
        MARLIN_RHI_VALIDATION_ERROR("CopyAccelerationStructure with a stale structure");
    }
    if (mode != RhiAccelerationStructureCopyMode::Clone && mode != RhiAccelerationStructureCopyMode::Compact)
    {
        MARLIN_RHI_VALIDATION_ERROR("CopyAccelerationStructure: Serialize and Deserialize go through memory");
    }
    _inner.CopyAccelerationStructure(destination, source, mode);
}

void ValidationCommandList::CopyAccelerationStructureMemory(uint64 destinationAddress,
                                                            RhiAccelerationStructure structure,
                                                            RhiAccelerationStructureCopyMode mode,
                                                            uint64 sourceAddress)
{
    const Use use(*this, "CopyAccelerationStructureMemory");
    RequireQueue(true, true, false, "CopyAccelerationStructureMemory");
    if (!_device.IsLive(structure))
    {
        MARLIN_RHI_VALIDATION_ERROR("CopyAccelerationStructureMemory with a stale structure");
    }
    if ((mode == RhiAccelerationStructureCopyMode::Serialize && destinationAddress == 0) ||
        (mode == RhiAccelerationStructureCopyMode::Deserialize && sourceAddress == 0) ||
        mode == RhiAccelerationStructureCopyMode::Clone || mode == RhiAccelerationStructureCopyMode::Compact)
    {
        MARLIN_RHI_VALIDATION_ERROR("CopyAccelerationStructureMemory: Serialize needs a destination address, "
                                    "Deserialize a source address");
    }
    _inner.CopyAccelerationStructureMemory(destinationAddress, structure, mode, sourceAddress);
}

void ValidationCommandList::WriteAccelerationStructureProperties(Span<const RhiAccelerationStructure> structures,
                                                                 RhiAccelerationStructureProperty property,
                                                                 RhiBuffer destination, uint64 offset)
{
    const Use use(*this, "WriteAccelerationStructureProperties");
    RequireQueue(true, true, false, "WriteAccelerationStructureProperties");
    RequireRendering(false, "WriteAccelerationStructureProperties");
    for (const RhiAccelerationStructure structure : structures)
    {
        if (!_device.IsLive(structure))
        {
            MARLIN_RHI_VALIDATION_ERROR("WriteAccelerationStructureProperties with a stale structure");
        }
    }
    if (const Optional<RhiBufferDesc> desc = RequireBuffer(destination, "WriteAccelerationStructureProperties");
        desc.HasValue())
    {
        CheckBufferRange(*desc, offset, structures.Count() * sizeof(uint64), "WriteAccelerationStructureProperties");
    }
    _inner.WriteAccelerationStructureProperties(structures, property, destination, offset);
}

void ValidationCommandList::WriteTimestamp(RhiQueryPool pool, uint32 index)
{
    const Use use(*this, "WriteTimestamp");
    const Optional<RhiQueryPoolDesc> desc = _device.FindQueryPool(pool);
    if (!desc.HasValue() || desc->Kind != RhiQueryKind::Timestamp || index >= desc->Count)
    {
        MARLIN_RHI_VALIDATION_ERROR("WriteTimestamp: the pool is stale, not for timestamps, or has no query {0}",
                                    index);
        return;
    }
    _inner.WriteTimestamp(pool, index);
}

void ValidationCommandList::BeginQuery(RhiQueryPool pool, uint32 index)
{
    const Use use(*this, "BeginQuery");
    RequireQueue(true, true, false, "BeginQuery");
    const Optional<RhiQueryPoolDesc> desc = _device.FindQueryPool(pool);
    if (!desc.HasValue() || desc->Kind == RhiQueryKind::Timestamp || index >= desc->Count)
    {
        MARLIN_RHI_VALIDATION_ERROR("BeginQuery: the pool is stale, for timestamps, or has no query {0}", index);
        return;
    }
    if (desc->Kind == RhiQueryKind::Occlusion)
    {
        RequireRendering(true, "BeginQuery of an occlusion query");
    }
    ++_openQueries;
    _inner.BeginQuery(pool, index);
}

void ValidationCommandList::EndQuery(RhiQueryPool pool, uint32 index)
{
    const Use use(*this, "EndQuery");
    const Optional<RhiQueryPoolDesc> desc = _device.FindQueryPool(pool);
    if (!desc.HasValue() || index >= desc->Count || _openQueries == 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("EndQuery: the pool is stale, has no query {0}, or no query is open", index);
        return;
    }
    --_openQueries;
    _inner.EndQuery(pool, index);
}

void ValidationCommandList::ResolveQueries(RhiQueryPool pool, uint32 first, uint32 count, RhiBuffer destination,
                                           uint64 offset)
{
    const Use use(*this, "ResolveQueries");
    RequireRendering(false, "ResolveQueries");
    const Optional<RhiQueryPoolDesc> desc = _device.FindQueryPool(pool);
    if (!desc.HasValue() || first + count > desc->Count)
    {
        MARLIN_RHI_VALIDATION_ERROR("ResolveQueries: the pool is stale, or queries {0}+{1} are outside it", first,
                                    count);
        return;
    }
    const uint64 stride =
        desc->Kind == RhiQueryKind::PipelineStatistics ? sizeof(RhiPipelineStatistics) : sizeof(uint64);
    if (const Optional<RhiBufferDesc> buffer = RequireBuffer(destination, "ResolveQueries"); buffer.HasValue())
    {
        CheckBufferRange(*buffer, offset, stride * count, "ResolveQueries");
    }
    if (offset % 8 != 0)
    {
        MARLIN_RHI_VALIDATION_ERROR("ResolveQueries: the destination offset is not 8-byte aligned");
    }
    _inner.ResolveQueries(pool, first, count, destination, offset);
}
} // namespace Marlin
