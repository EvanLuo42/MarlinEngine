#include "RenderGraph/RenderGraphBuilder.h"

#include "Diagnostics/AssertFormat.h"
#include "Diagnostics/Profiler.h"
#include "RHI/RhiDevice.h"
#include "RenderGraph/RenderGraphCompiler.h"
#include "RenderGraph/RenderGraphExecutor.h"
#include "RenderGraph/RenderGraphExport.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "RenderGraph/RenderGraphSubsystem.h"
#include "RenderGraph/RenderGraphTransientPool.h"

namespace Marlin
{
namespace
{
constexpr RhiAccess ContentReads = RhiAccess::IndexBuffer | RhiAccess::ShaderResource | RhiAccess::UnorderedAccess |
                                   RhiAccess::DepthStencilRead | RhiAccess::CopySource | RhiAccess::ResolveSource |
                                   RhiAccess::IndirectArguments | RhiAccess::AccelerationStructureRead |
                                   RhiAccess::ShadingRateSource | RhiAccess::IndirectPreprocessRead |
                                   RhiAccess::HostRead;
constexpr RhiAccess ContentWrites = RhiAccess::UnorderedAccess | RhiAccess::RenderTarget |
                                    RhiAccess::DepthStencilWrite | RhiAccess::CopyDest | RhiAccess::ResolveDest |
                                    RhiAccess::AccelerationStructureWrite | RhiAccess::IndirectPreprocessWrite |
                                    RhiAccess::HostWrite | RhiAccess::MarkerWrite;

StringView CopyText(Arena &arena, StringView text)
{
    char *copy = arena.AllocateArray<char>(text.Length() > 0 ? text.Length() : 1);
    MemCopy(copy, text.Data(), text.Length());
    return StringView(copy, text.Length());
}

RenderGraphResource &GetResource(RenderGraphFrame &frame, uint32 index, RenderGraphResourceKind kind)
{
    MARLIN_ASSERTF(index < frame.Resources.Count() && frame.Resources[index].Kind == kind,
                   "{0} is not a resource of this graph", index);
    return frame.Resources[index];
}

RenderGraphPass &GetOpenPass(RenderGraphFrame &frame, uint32 pass)
{
    MARLIN_ASSERTF(frame.OpenPass == pass, "Accesses are declared only in their pass's setup function");
    return frame.Passes[pass];
}

void AddAccess(RenderGraphFrame &frame, RenderGraphPass &pass, uint32 resource, RhiAccess access,
               const RhiSubresourceRange &range, bool reads, bool writes)
{
    RenderGraphAccess &entry = frame.Accesses.AddDefaulted();
    entry.Resource = resource;
    entry.Access = access;
    entry.Range = range;
    entry.ReadsContents = reads;
    entry.WritesContents = writes;
    ++pass.AccessCount;
}

RenderGraphView AddView(RenderGraphFrame &frame, RenderGraphPass &pass, RenderGraphViewRecord &&view)
{
    const uint32 index = static_cast<uint32>(frame.Views.Count());
    frame.Views.Add(Move(view));
    ++pass.ViewCount;
    return {index};
}

RhiSubresourceRange TargetRange(uint32 mip, uint32 firstSlice, uint32 sliceCount)
{
    RhiSubresourceRange range;
    range.FirstMip = mip;
    range.MipCount = 1;
    range.FirstSlice = firstSlice;
    range.SliceCount = sliceCount;
    return range;
}
} // namespace

RenderGraphView RenderGraphPassBuilder::Texture(RenderGraphTexture texture, RhiAccess access,
                                                const RenderGraphTextureViewDesc &view)
{
    return AddTextureAccess(texture, access, view, HasAnyFlags(access, ContentReads));
}

RenderGraphView RenderGraphPassBuilder::OverwriteTexture(RenderGraphTexture texture,
                                                         const RenderGraphTextureViewDesc &view)
{
    return AddTextureAccess(texture, RhiAccess::UnorderedAccess, view, false);
}

RenderGraphView RenderGraphPassBuilder::AddTextureAccess(RenderGraphTexture texture, RhiAccess access,
                                                         const RenderGraphTextureViewDesc &view, bool reads)
{
    RenderGraphPass &pass = GetOpenPass(_frame, _pass);
    GetResource(_frame, texture.Index, RenderGraphResourceKind::Texture);
    AddAccess(_frame, pass, texture.Index, access, view.Range, reads, HasAnyFlags(access, ContentWrites));
    if (!HasAnyFlags(access, RhiAccess::ShaderResource | RhiAccess::UnorderedAccess))
    {
        return {};
    }
    RenderGraphViewRecord record;
    record.Resource = texture.Index;
    record.Access = HasAnyFlags(access, RhiAccess::UnorderedAccess) ? RhiViewAccess::UnorderedAccess
                                                                    : RhiViewAccess::ShaderResource;
    record.AutoDimension = !view.Dimension.HasValue();
    record.Dimension = view.Dimension.ValueOr(RhiTextureViewDimension::Texture2D);
    record.Format = view.Format;
    record.Range = view.Range;
    return AddView(_frame, pass, Move(record));
}

RenderGraphView RenderGraphPassBuilder::Buffer(RenderGraphBuffer buffer, RhiAccess access,
                                               const RenderGraphBufferViewDesc &view)
{
    return AddBufferAccess(buffer, access, view, HasAnyFlags(access, ContentReads));
}

RenderGraphView RenderGraphPassBuilder::OverwriteBuffer(RenderGraphBuffer buffer, const RenderGraphBufferViewDesc &view)
{
    return AddBufferAccess(buffer, RhiAccess::UnorderedAccess, view, false);
}

RenderGraphView RenderGraphPassBuilder::AddBufferAccess(RenderGraphBuffer buffer, RhiAccess access,
                                                        const RenderGraphBufferViewDesc &view, bool reads)
{
    RenderGraphPass &pass = GetOpenPass(_frame, _pass);
    GetResource(_frame, buffer.Index, RenderGraphResourceKind::Buffer);
    AddAccess(_frame, pass, buffer.Index, access, {}, reads, HasAnyFlags(access, ContentWrites));
    if (!HasAnyFlags(access, RhiAccess::ShaderResource | RhiAccess::UnorderedAccess))
    {
        return {};
    }
    RenderGraphViewRecord record;
    record.Resource = buffer.Index;
    record.Access = HasAnyFlags(access, RhiAccess::UnorderedAccess) ? RhiViewAccess::UnorderedAccess
                                                                    : RhiViewAccess::ShaderResource;
    record.Offset = view.Offset;
    record.Size = view.Size;
    record.Stride = view.Stride;
    return AddView(_frame, pass, Move(record));
}

void RenderGraphPassBuilder::RenderTarget(const RenderGraphRenderTarget &target)
{
    RenderGraphPass &pass = GetOpenPass(_frame, _pass);
    GetResource(_frame, target.Texture.Index, RenderGraphResourceKind::Texture);
    MARLIN_ASSERTF(pass.TargetCount == 0 || pass.FirstTarget + pass.TargetCount == _frame.Targets.Count(),
                   "Render targets are declared only in their pass's setup function");
    MARLIN_ASSERTF(pass.TargetCount < RenderGraphMaxRenderTargets, "Pass {0} has too many render targets",
                   pass.PassName);
    if (pass.TargetCount == 0)
    {
        pass.FirstTarget = static_cast<uint32>(_frame.Targets.Count());
    }
    _frame.Targets.Add(target);
    ++pass.TargetCount;
    AddAccess(_frame, pass, target.Texture.Index, RhiAccess::RenderTarget,
              TargetRange(target.Mip, target.FirstSlice, target.SliceCount), target.Load == RhiLoadOp::Load, true);
    if (target.ResolveTarget.IsValid())
    {
        // Rendering resolves into an attachment of its own, which takes the RenderTarget layout.
        GetResource(_frame, target.ResolveTarget.Index, RenderGraphResourceKind::Texture);
        AddAccess(_frame, pass, target.ResolveTarget.Index, RhiAccess::RenderTarget,
                  TargetRange(target.ResolveMip, target.ResolveSlice, 1), false, true);
    }
}

void RenderGraphPassBuilder::RenderTarget(RenderGraphTexture texture, RhiLoadOp load, RhiStoreOp store)
{
    RenderGraphRenderTarget target;
    target.Texture = texture;
    target.Load = load;
    target.Store = store;
    RenderTarget(target);
}

void RenderGraphPassBuilder::DepthTarget(const RenderGraphDepthTarget &target)
{
    RenderGraphPass &pass = GetOpenPass(_frame, _pass);
    GetResource(_frame, target.Texture.Index, RenderGraphResourceKind::Texture);
    MARLIN_ASSERTF(!pass.HasDepth, "Pass {0} has two depth targets", pass.PassName);
    pass.HasDepth = true;
    pass.Depth = target;
    const bool loads = target.DepthLoad == RhiLoadOp::Load || target.StencilLoad == RhiLoadOp::Load;
    AddAccess(_frame, pass, target.Texture.Index,
              target.ReadOnly ? RhiAccess::DepthStencilRead : RhiAccess::DepthStencilWrite,
              TargetRange(target.Mip, target.FirstSlice, target.SliceCount), target.ReadOnly || loads,
              !target.ReadOnly);
}

void RenderGraphPassBuilder::DepthTarget(RenderGraphTexture texture, RhiLoadOp load, RhiStoreOp store)
{
    RenderGraphDepthTarget target;
    target.Texture = texture;
    target.DepthLoad = load;
    target.DepthStore = store;
    target.StencilLoad = load;
    target.StencilStore = store;
    DepthTarget(target);
}

void RenderGraphPassBuilder::ShadingRate(RenderGraphTexture texture)
{
    RenderGraphPass &pass = GetOpenPass(_frame, _pass);
    GetResource(_frame, texture.Index, RenderGraphResourceKind::Texture);
    pass.ShadingRate = texture;
    AddAccess(_frame, pass, texture.Index, RhiAccess::ShadingRateSource, TargetRange(0, 0, 1), true, false);
}

void RenderGraphPassBuilder::SetRenderArea(uint32 width, uint32 height)
{
    RenderGraphPass &pass = GetOpenPass(_frame, _pass);
    pass.Width = width;
    pass.Height = height;
}

void RenderGraphPassBuilder::SetViewMask(uint32 viewMask)
{
    GetOpenPass(_frame, _pass).ViewMask = viewMask;
}

void RenderGraphPassBuilder::SetShaderStages(RhiSync stages)
{
    GetOpenPass(_frame, _pass).ShaderStages = stages;
}

void RenderGraphPassBuilder::NeverCull()
{
    GetOpenPass(_frame, _pass).NeverCull = true;
}

RhiDevice &RenderGraphContext::GetDevice() const
{
    return _frame.Device;
}

Name RenderGraphContext::GetPassName() const
{
    return _frame.Passes[_pass].PassName;
}

RhiQueueType RenderGraphContext::GetQueue() const
{
    return _frame.Passes[_pass].Queue;
}

RhiTexture RenderGraphContext::GetTexture(RenderGraphTexture texture) const
{
    MARLIN_ASSERT(texture.Index < _frame.Resources.Count());
    return _frame.Resources[texture.Index].Texture;
}

RhiBuffer RenderGraphContext::GetBuffer(RenderGraphBuffer buffer) const
{
    MARLIN_ASSERT(buffer.Index < _frame.Resources.Count());
    return _frame.Resources[buffer.Index].Buffer;
}

uint64 RenderGraphContext::GetBufferAddress(RenderGraphBuffer buffer) const
{
    return _frame.Device.GetBufferAddress(GetBuffer(buffer));
}

RhiResourceView RenderGraphContext::GetView(RenderGraphView view) const
{
    MARLIN_ASSERT(view.Index < _frame.Views.Count());
    return _frame.Views[view.Index].View;
}

uint64 RenderGraphContext::GetAddress(RenderGraphView view) const
{
    MARLIN_ASSERT(view.Index < _frame.Views.Count());
    const RenderGraphViewRecord &record = _frame.Views[view.Index];
    return _frame.Device.GetBufferAddress(_frame.Resources[record.Resource].Buffer) + record.Offset;
}

uint32 RenderGraphContext::GetCount(RenderGraphView view) const
{
    MARLIN_ASSERT(view.Index < _frame.Views.Count());
    const RenderGraphViewRecord &record = _frame.Views[view.Index];
    const RenderGraphResource &resource = _frame.Resources[record.Resource];
    const uint64 size = record.Size != 0 ? record.Size : resource.BufferDesc.Size - record.Offset;
    return static_cast<uint32>(record.Stride != 0 ? size / record.Stride : size);
}

RenderGraphBuilder::RenderGraphBuilder(RenderGraphSubsystem &subsystem) : _subsystem(subsystem)
{
    _frame = subsystem.AcquireFrame();
}

RenderGraphBuilder::~RenderGraphBuilder()
{
    for (RenderGraphPass &pass : _frame->Passes)
    {
        if (pass.Function.Destroy != nullptr)
        {
            pass.Function.Destroy(pass.Function.Body, pass.Function.Data);
        }
    }
    _subsystem.ReleaseFrame(Move(_frame));
}

RenderGraphTexture RenderGraphBuilder::CreateTexture(StringView name, const RhiTextureDesc &desc)
{
    RenderGraphFrame &frame = *_frame;
    const uint32 index = static_cast<uint32>(frame.Resources.Count());
    RenderGraphResource &resource = frame.Resources.AddDefaulted();
    resource.Name = CopyText(frame.Memory, name);
    resource.Kind = RenderGraphResourceKind::Texture;
    resource.TextureDesc = desc;
    RhiFormat *formats =
        frame.Memory.AllocateArray<RhiFormat>(desc.ViewFormats.Count() > 0 ? desc.ViewFormats.Count() : 1);
    for (usize i = 0; i < desc.ViewFormats.Count(); ++i)
    {
        formats[i] = desc.ViewFormats[i];
    }
    resource.TextureDesc.ViewFormats = Span<const RhiFormat>(formats, desc.ViewFormats.Count());
    resource.TextureDesc.Placement = {};
    resource.TextureDesc.DebugName = resource.Name;
    return {index};
}

RenderGraphBuffer RenderGraphBuilder::CreateBuffer(StringView name, const RhiBufferDesc &desc)
{
    MARLIN_ASSERTF(desc.Location == RhiMemoryLocation::Device, "Transient buffer {0} is not in device memory", name);
    RenderGraphFrame &frame = *_frame;
    const uint32 index = static_cast<uint32>(frame.Resources.Count());
    RenderGraphResource &resource = frame.Resources.AddDefaulted();
    resource.Name = CopyText(frame.Memory, name);
    resource.Kind = RenderGraphResourceKind::Buffer;
    resource.BufferDesc = desc;
    resource.BufferDesc.Placement = {};
    resource.BufferDesc.DebugName = resource.Name;
    return {index};
}

RenderGraphTexture RenderGraphBuilder::ImportTexture(StringView name, RenderGraphExternalTexture &texture)
{
    RenderGraphFrame &frame = *_frame;
    for ([[maybe_unused]] const RenderGraphResource &existing : frame.Resources)
    {
        MARLIN_ASSERTF(existing.ExternalTexture == nullptr || existing.ExternalTexture->Texture != texture.Texture,
                       "{0} is imported twice", name);
    }
    const uint32 index = static_cast<uint32>(frame.Resources.Count());
    RenderGraphResource &resource = frame.Resources.AddDefaulted();
    resource.Name = CopyText(frame.Memory, name);
    resource.Kind = RenderGraphResourceKind::Texture;
    resource.ExternalTexture = &texture;
    resource.Texture = texture.Texture;
    resource.TextureDesc = frame.Device.GetTextureDesc(texture.Texture);
    return {index};
}

RenderGraphBuffer RenderGraphBuilder::ImportBuffer(StringView name, RenderGraphExternalBuffer &buffer)
{
    RenderGraphFrame &frame = *_frame;
    for ([[maybe_unused]] const RenderGraphResource &existing : frame.Resources)
    {
        MARLIN_ASSERTF(existing.ExternalBuffer == nullptr || existing.ExternalBuffer->Buffer != buffer.Buffer,
                       "{0} is imported twice", name);
    }
    const uint32 index = static_cast<uint32>(frame.Resources.Count());
    RenderGraphResource &resource = frame.Resources.AddDefaulted();
    resource.Name = CopyText(frame.Memory, name);
    resource.Kind = RenderGraphResourceKind::Buffer;
    resource.ExternalBuffer = &buffer;
    resource.Buffer = buffer.Buffer;
    resource.BufferDesc.Size = frame.Device.GetNativeBuffer(buffer.Buffer).Size;
    resource.BufferDesc.DebugName = resource.Name;
    return {index};
}

const RhiTextureDesc &RenderGraphBuilder::GetDesc(RenderGraphTexture texture) const
{
    return GetResource(*_frame, texture.Index, RenderGraphResourceKind::Texture).TextureDesc;
}

const RhiBufferDesc &RenderGraphBuilder::GetDesc(RenderGraphBuffer buffer) const
{
    return GetResource(*_frame, buffer.Index, RenderGraphResourceKind::Buffer).BufferDesc;
}

RenderGraphPassBuilder RenderGraphBuilder::BeginPass(StringView name, RhiQueueType queue)
{
    RenderGraphFrame &frame = *_frame;
    MARLIN_ASSERTF(!frame.Executed, "Pass {0} is added after the graph executed", name);
    MARLIN_ASSERTF(frame.OpenPass == RenderGraphNone, "Pass {0} is added inside another pass's setup", name);
    const uint32 index = static_cast<uint32>(frame.Passes.Count());
    RenderGraphPass &pass = frame.Passes.AddDefaulted();
    pass.PassName = Name(name);
    pass.DeclaredQueue = queue;
    pass.Queue = queue;
    pass.FirstAccess = static_cast<uint32>(frame.Accesses.Count());
    pass.FirstView = static_cast<uint32>(frame.Views.Count());
    frame.OpenPass = index;
    return RenderGraphPassBuilder(frame, index);
}

void RenderGraphBuilder::EndPass(const RenderGraphPassBuilder &builder, const RenderGraphPassFunction &function)
{
    RenderGraphFrame &frame = *_frame;
    frame.Passes[builder._pass].Function = function;
    frame.OpenPass = RenderGraphNone;
}

void *RenderGraphBuilder::Allocate(usize size, usize alignment)
{
    return _frame->Memory.Allocate(size, alignment);
}

Result<void, RhiError> RenderGraphBuilder::Execute()
{
    MARLIN_PROFILE_SCOPE("RenderGraph.Execute");
    RenderGraphFrame &frame = *_frame;
    MARLIN_ASSERTF(frame.OpenPass == RenderGraphNone, "The graph executes inside a pass's setup");
    MARLIN_ASSERTF(!frame.Executed, "A render graph executes once");
    frame.Executed = true;

    RenderGraphCompileOptions options;
    options.AsyncCompute = RenderGraphSubsystem::AsyncCompute;
    options.AsyncCopy = RenderGraphSubsystem::AsyncCopy;
    options.Culling = RenderGraphSubsystem::Culling;
    List<Function<void(const RenderGraphExport &)>> exports;
    _subsystem.TakeRequests(exports, options.CaptureFirst, options.CaptureLast);
    RenderGraphCompiler::Compile(frame, options);

    const uint64 graph = _subsystem._graphCount;
    RenderGraphTransientPool &pool = *_subsystem._transient;
    Result<void, RhiError> result =
        pool.Allocate(frame, graph, RenderGraphSubsystem::Aliasing, RenderGraphSubsystem::TransientHeapSize);
    if (result.IsOk())
    {
        RenderGraphCompiler::ResolveBarrierHandles(frame);
        result = RenderGraphExecutor::Run(frame, _subsystem);
    }
    if (!exports.IsEmpty())
    {
        RenderGraphExport graphExport;
        BuildRenderGraphExport(frame, graph, graphExport);
        for (Function<void(const RenderGraphExport &)> &callback : exports)
        {
            callback(graphExport);
        }
    }
    pool.Release(frame, graph, RenderGraphSubsystem::RetainGraphs);
    ++_subsystem._graphCount;
    return result;
}
} // namespace Marlin
