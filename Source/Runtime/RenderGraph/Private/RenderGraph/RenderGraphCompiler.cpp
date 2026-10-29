#include "RenderGraph/RenderGraphCompiler.h"

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Diagnostics/AssertFormat.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "RenderGraph/RenderGraphLog.h"

namespace Marlin
{
namespace
{
constexpr RhiAccess WriteAccesses = RhiAccess::UnorderedAccess | RhiAccess::RenderTarget |
                                    RhiAccess::DepthStencilWrite | RhiAccess::CopyDest | RhiAccess::ResolveDest |
                                    RhiAccess::AccelerationStructureWrite | RhiAccess::IndirectPreprocessWrite |
                                    RhiAccess::HostWrite | RhiAccess::MarkerWrite;
constexpr RhiAccess ReadAccesses = RhiAccess::IndexBuffer | RhiAccess::ShaderResource | RhiAccess::DepthStencilRead |
                                   RhiAccess::CopySource | RhiAccess::ResolveSource | RhiAccess::IndirectArguments |
                                   RhiAccess::AccelerationStructureRead | RhiAccess::ShadingRateSource |
                                   RhiAccess::IndirectPreprocessRead | RhiAccess::HostRead;
constexpr RhiAccess ComputeAccesses =
    RhiAccess::ShaderResource | RhiAccess::UnorderedAccess | RhiAccess::CopySource | RhiAccess::CopyDest |
    RhiAccess::IndirectArguments | RhiAccess::AccelerationStructureRead | RhiAccess::AccelerationStructureWrite |
    RhiAccess::IndirectPreprocessRead | RhiAccess::IndirectPreprocessWrite | RhiAccess::MarkerWrite;
constexpr RhiAccess CopyAccesses = RhiAccess::CopySource | RhiAccess::CopyDest | RhiAccess::MarkerWrite;
constexpr RhiSync RasterStages =
    RhiSync::VertexShading | RhiSync::TaskShading | RhiSync::MeshShading | RhiSync::PixelShading;
constexpr RhiSync ComputeStages = RhiSync::ComputeShading | RhiSync::RayTracing;
constexpr uint8 NoQueue = 0xFF;
constexpr RhiAccess HostAccesses = RhiAccess::HostRead | RhiAccess::HostWrite;

uint32 QueueIndex(RhiQueueType queue)
{
    return static_cast<uint32>(queue);
}

/// The later of two orders, either of which may be RenderGraphNone.
uint32 Later(uint32 a, uint32 b)
{
    if (a == RenderGraphNone)
    {
        return b;
    }
    if (b == RenderGraphNone)
    {
        return a;
    }
    return a > b ? a : b;
}

/// The stages a queue's barriers may name.
RhiSync QueueStages(RhiQueueType queue)
{
    switch (queue)
    {
    case RhiQueueType::Graphics:
        return RhiSync::All;
    case RhiQueueType::Compute:
        return ComputeStages | RhiSync::Copy | RhiSync::IndirectArguments | RhiSync::AccelerationStructureBuild |
               RhiSync::AccelerationStructureCopy | RhiSync::IndirectPreprocess | RhiSync::Host;
    case RhiQueueType::Copy:
        return RhiSync::Copy | RhiSync::Host;
    }
    return RhiSync::All;
}

/// The accesses a queue's passes may make.
[[maybe_unused]] RhiAccess QueueAccesses(RhiQueueType queue)
{
    switch (queue)
    {
    case RhiQueueType::Graphics:
        return WriteAccesses | ReadAccesses;
    case RhiQueueType::Compute:
        return ComputeAccesses | HostAccesses;
    case RhiQueueType::Copy:
        return CopyAccesses | HostAccesses;
    }
    return WriteAccesses | ReadAccesses;
}

/// The stages a pass's views are used in: its own, or the raster stages for a pass with targets, compute and ray
/// tracing otherwise.
RhiSync DefaultShaderStages(const RenderGraphPass &pass)
{
    if (pass.ShaderStages != RhiSync::None)
    {
        return pass.ShaderStages;
    }
    if (pass.Renders())
    {
        return RasterStages;
    }
    if (pass.Queue == RhiQueueType::Copy)
    {
        return RhiSync::None;
    }
    return ComputeStages;
}

/// The stages that make *access*; view accesses happen in *shaderStages*.
RhiSync StagesFor(RhiAccess access, RhiSync shaderStages)
{
    RhiSync sync = RhiSync::None;
    if (HasAnyFlags(access, RhiAccess::IndexBuffer))
    {
        sync |= RhiSync::IndexInput;
    }
    if (HasAnyFlags(access, RhiAccess::ShaderResource | RhiAccess::UnorderedAccess))
    {
        sync |= shaderStages;
    }
    if (HasAnyFlags(access, RhiAccess::RenderTarget))
    {
        sync |= RhiSync::RenderTarget;
    }
    if (HasAnyFlags(access, RhiAccess::DepthStencilRead | RhiAccess::DepthStencilWrite))
    {
        sync |= RhiSync::DepthStencil;
    }
    if (HasAnyFlags(access, RhiAccess::CopySource | RhiAccess::CopyDest | RhiAccess::MarkerWrite))
    {
        sync |= RhiSync::Copy;
    }
    if (HasAnyFlags(access, RhiAccess::ResolveSource | RhiAccess::ResolveDest))
    {
        sync |= RhiSync::Resolve;
    }
    if (HasAnyFlags(access, RhiAccess::IndirectArguments))
    {
        sync |= RhiSync::IndirectArguments;
    }
    if (HasAnyFlags(access, RhiAccess::AccelerationStructureRead))
    {
        sync |= shaderStages | RhiSync::AccelerationStructureBuild | RhiSync::AccelerationStructureCopy;
    }
    if (HasAnyFlags(access, RhiAccess::AccelerationStructureWrite))
    {
        sync |= RhiSync::AccelerationStructureBuild | RhiSync::AccelerationStructureCopy;
    }
    if (HasAnyFlags(access, RhiAccess::ShadingRateSource))
    {
        sync |= RhiSync::ShadingRate;
    }
    if (HasAnyFlags(access, RhiAccess::IndirectPreprocessRead | RhiAccess::IndirectPreprocessWrite))
    {
        sync |= RhiSync::IndirectPreprocess;
    }
    if (HasAnyFlags(access, HostAccesses))
    {
        sync |= RhiSync::Host;
    }
    return sync;
}

/// The layout a texture takes for *access*; reads of more than one kind share General.
RhiLayout LayoutFor(RhiAccess access)
{
    if (HasAnyFlags(access, RhiAccess::UnorderedAccess))
    {
        return RhiLayout::UnorderedAccess;
    }
    if (HasAnyFlags(access, RhiAccess::RenderTarget))
    {
        return RhiLayout::RenderTarget;
    }
    if (HasAnyFlags(access, RhiAccess::DepthStencilWrite))
    {
        return RhiLayout::DepthStencilWrite;
    }
    if (HasAnyFlags(access, RhiAccess::CopyDest))
    {
        return RhiLayout::CopyDest;
    }
    if (HasAnyFlags(access, RhiAccess::ResolveDest))
    {
        return RhiLayout::ResolveDest;
    }
    if (access == RhiAccess::ShaderResource)
    {
        return RhiLayout::ShaderResource;
    }
    if (HasAnyFlags(access, RhiAccess::DepthStencilRead) &&
        (access & ~(RhiAccess::DepthStencilRead | RhiAccess::ShaderResource)) == RhiAccess::None)
    {
        return RhiLayout::DepthStencilRead;
    }
    if (access == RhiAccess::CopySource)
    {
        return RhiLayout::CopySource;
    }
    if (access == RhiAccess::ResolveSource)
    {
        return RhiLayout::ResolveSource;
    }
    if (access == RhiAccess::ShadingRateSource)
    {
        return RhiLayout::ShadingRateSource;
    }
    return RhiLayout::General;
}

RhiTextureUsage TextureUsageFor(RhiAccess access)
{
    RhiTextureUsage usage = RhiTextureUsage::None;
    if (HasAnyFlags(access, RhiAccess::ShaderResource))
    {
        usage |= RhiTextureUsage::ShaderResource;
    }
    if (HasAnyFlags(access, RhiAccess::UnorderedAccess))
    {
        usage |= RhiTextureUsage::UnorderedAccess;
    }
    if (HasAnyFlags(access, RhiAccess::RenderTarget))
    {
        usage |= RhiTextureUsage::RenderTarget;
    }
    if (HasAnyFlags(access, RhiAccess::DepthStencilRead | RhiAccess::DepthStencilWrite))
    {
        usage |= RhiTextureUsage::DepthStencil;
    }
    if (HasAnyFlags(access, RhiAccess::CopySource | RhiAccess::ResolveSource))
    {
        usage |= RhiTextureUsage::CopySource;
    }
    if (HasAnyFlags(access, RhiAccess::CopyDest | RhiAccess::ResolveDest))
    {
        usage |= RhiTextureUsage::CopyDest;
    }
    if (HasAnyFlags(access, RhiAccess::ShadingRateSource))
    {
        usage |= RhiTextureUsage::ShadingRate;
    }
    return usage;
}

RhiBufferUsage BufferUsageFor(RhiAccess access)
{
    RhiBufferUsage usage = RhiBufferUsage::None;
    if (HasAnyFlags(access, RhiAccess::IndexBuffer))
    {
        usage |= RhiBufferUsage::IndexBuffer;
    }
    if (HasAnyFlags(access, RhiAccess::ShaderResource))
    {
        usage |= RhiBufferUsage::ShaderResource;
    }
    if (HasAnyFlags(access, RhiAccess::UnorderedAccess))
    {
        usage |= RhiBufferUsage::UnorderedAccess;
    }
    if (HasAnyFlags(access, RhiAccess::IndirectArguments))
    {
        usage |= RhiBufferUsage::IndirectArguments;
    }
    if (HasAnyFlags(access, RhiAccess::AccelerationStructureRead | RhiAccess::AccelerationStructureWrite))
    {
        usage |= RhiBufferUsage::AccelerationStructure;
    }
    if (HasAnyFlags(access, RhiAccess::CopySource))
    {
        usage |= RhiBufferUsage::CopySource;
    }
    if (HasAnyFlags(access, RhiAccess::CopyDest))
    {
        usage |= RhiBufferUsage::CopyDest;
    }
    if (HasAnyFlags(access, RhiAccess::IndirectPreprocessRead | RhiAccess::IndirectPreprocessWrite))
    {
        usage |= RhiBufferUsage::IndirectPreprocess;
    }
    if (HasAnyFlags(access, RhiAccess::MarkerWrite))
    {
        usage |= RhiBufferUsage::Marker;
    }
    return usage;
}

/// Neighbouring subresources whose barriers are equal but for the range share one barrier.
bool SameTransition(const RhiTextureBarrier &a, const RhiTextureBarrier &b)
{
    return a.SyncBefore == b.SyncBefore && a.SyncAfter == b.SyncAfter && a.AccessBefore == b.AccessBefore &&
           a.AccessAfter == b.AccessAfter && a.LayoutBefore == b.LayoutBefore && a.LayoutAfter == b.LayoutAfter &&
           a.Discard == b.Discard && a.TransferQueue == b.TransferQueue && a.Transfer.From == b.Transfer.From &&
           a.Transfer.To == b.Transfer.To;
}

/// The latest use of a subresource on one queue: the stages and accesses since its last barrier, and the pass.
struct QueueUse
{
    RhiSync Sync = RhiSync::None;
    RhiAccess Access = RhiAccess::None;
    uint32 Pass = RenderGraphNone;
};

struct SubresourceState
{
    QueueUse Queues[RhiQueueTypeCount];
    RhiLayout Layout = RhiLayout::Undefined;
    uint8 Owner = NoQueue;
    bool Touched = false;
    bool Initialized = false;

    void Reset(uint32 queue, const QueueUse &use)
    {
        for (QueueUse &other : Queues)
        {
            other = QueueUse();
        }
        Queues[queue] = use;
    }
};

/// What one pass does to one subresource, its accesses merged.
struct MergedAccess
{
    uint32 Resource = RenderGraphNone;
    uint32 Subresource = 0;
    RhiAccess Access = RhiAccess::None;
    RhiSync Sync = RhiSync::None;
    bool Reads = false;
};

struct PendingTexture
{
    uint32 Order = 0;
    bool After = false;
    bool Alias = false;
    uint32 Resource = RenderGraphNone;
    uint32 Subresource = 0;
    RhiTextureBarrier Barrier;
};

struct PendingBuffer
{
    uint32 Order = 0;
    bool After = false;
    uint32 Resource = RenderGraphNone;
    RhiBufferBarrier Barrier;
};

struct PendingGlobal
{
    uint32 Order = 0;
    bool After = false;
    uint32 AliasResource = RenderGraphNone;
    RhiGlobalBarrier Barrier;
};

class Compiler
{
public:
    Compiler(RenderGraphFrame &frame, const RenderGraphCompileOptions &options) : _frame(frame), _options(options)
    {
    }

    void Run()
    {
        ResolveQueues();
        Cull();
        AddQueuePasses();
        PrepareResources();
        FindCapture();
        TrackStates();
        ComputeDone();
        BuildSegments();
        EmitBarriers();
    }

private:
    /// Folds Compute and Copy passes onto Graphics when their async queue is off.
    void ResolveQueues()
    {
        for (RenderGraphPass &pass : _frame.Passes)
        {
            if (pass.Renders())
            {
                MARLIN_ASSERTF(pass.DeclaredQueue == RhiQueueType::Graphics, "Pass {0} renders off the Graphics queue",
                               pass.PassName);
            }
            const bool fold = (pass.DeclaredQueue == RhiQueueType::Compute && !_options.AsyncCompute) ||
                              (pass.DeclaredQueue == RhiQueueType::Copy && !_options.AsyncCopy);
            pass.Queue = fold ? RhiQueueType::Graphics : pass.DeclaredQueue;
        }
    }

    /// Keeps, from the last pass back, the passes that write what a kept pass reads or what outlives the graph.
    void Cull()
    {
        List<bool> needed;
        needed.Resize(_frame.Resources.Count(), false);
        for (usize index = 0; index < _frame.Resources.Count(); ++index)
        {
            needed[index] = _frame.Resources[index].IsExternal();
        }
        for (usize index = _frame.Passes.Count(); index-- > 0;)
        {
            RenderGraphPass &pass = _frame.Passes[index];
            bool keep = !_options.Culling || pass.NeverCull;
            for (const RenderGraphAccess &access : GetAccesses(pass))
            {
                keep = keep || (access.WritesContents && needed[access.Resource]);
            }
            pass.Kept = keep;
            if (!keep)
            {
                continue;
            }
            for (const RenderGraphAccess &access : GetAccesses(pass))
            {
                if (access.ReadsContents)
                {
                    needed[access.Resource] = true;
                }
            }
        }
    }

    uint32 AddQueuePass(RenderGraphPassRole role, RhiQueueType queue)
    {
        static const char *const Names[2][RhiQueueTypeCount] = {
            {"RenderGraph.Prologue.Graphics", "RenderGraph.Prologue.Compute", "RenderGraph.Prologue.Copy"},
            {"RenderGraph.Epilogue.Graphics", "RenderGraph.Epilogue.Compute", "RenderGraph.Epilogue.Copy"},
        };
        const uint32 index = static_cast<uint32>(_frame.Passes.Count());
        RenderGraphPass &pass = _frame.Passes.AddDefaulted();
        pass.PassName = Name(Names[role == RenderGraphPassRole::Prologue ? 0 : 1][QueueIndex(queue)]);
        pass.Role = role;
        pass.DeclaredQueue = queue;
        pass.Queue = queue;
        pass.NeverCull = true;
        pass.Kept = true;
        pass.FirstAccess = static_cast<uint32>(_frame.Accesses.Count());
        pass.FirstView = static_cast<uint32>(_frame.Views.Count());
        return index;
    }

    /// Orders the kept passes: a prologue per used queue, the declared passes, then an epilogue per used queue with
    /// Graphics' last, so every graph opens and closes on Graphics.
    void AddQueuePasses()
    {
        bool used[RhiQueueTypeCount] = {true, false, false};
        List<uint32> declared;
        for (uint32 index = 0; index < _frame.Passes.Count(); ++index)
        {
            if (_frame.Passes[index].Kept)
            {
                declared.Add(index);
                used[QueueIndex(_frame.Passes[index].Queue)] = true;
            }
        }
        _frame.Kept.Clear();
        for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
        {
            _prologue[queue] =
                used[queue] ? AddQueuePass(RenderGraphPassRole::Prologue, RhiQueueType(queue)) : RenderGraphNone;
            _epilogue[queue] = RenderGraphNone;
            if (_prologue[queue] != RenderGraphNone)
            {
                _frame.Kept.Add(_prologue[queue]);
            }
        }
        _frame.Kept.AddRange(declared.AsSpan());
        for (const RhiQueueType queue : {RhiQueueType::Compute, RhiQueueType::Copy, RhiQueueType::Graphics})
        {
            if (used[QueueIndex(queue)])
            {
                _epilogue[QueueIndex(queue)] = AddQueuePass(RenderGraphPassRole::Epilogue, queue);
                _frame.Kept.Add(_epilogue[QueueIndex(queue)]);
            }
        }
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            _frame.GetKept(order).Order = order;
        }
    }

    /// Finds what the kept passes use, where, and with which stages, and the usage transient resources need.
    void PrepareResources()
    {
        List<RhiTextureUsage> textureUsage;
        List<RhiBufferUsage> bufferUsage;
        textureUsage.Resize(_frame.Resources.Count(), RhiTextureUsage::None);
        bufferUsage.Resize(_frame.Resources.Count(), RhiBufferUsage::None);
        for (RenderGraphResource &resource : _frame.Resources)
        {
            if (resource.IsTexture())
            {
                const RhiTextureDesc &desc = resource.TextureDesc;
                resource.MipCount = desc.MipCount > 0 ? desc.MipCount : 1;
                resource.SliceCount = desc.Dimension == RhiTextureDimension::Texture3D || desc.DepthOrArraySize == 0
                                          ? 1
                                          : desc.DepthOrArraySize;
                resource.Concurrent = HasAnyFlags(desc.Flags, RhiResourceFlags::Concurrent);
                resource.Memoryless = !resource.IsExternal() && HasAnyFlags(desc.Flags, RhiResourceFlags::Transient);
            }
            else
            {
                resource.Concurrent = resource.ExternalBuffer != nullptr
                                          ? resource.ExternalBuffer->Concurrent
                                          : HasAnyFlags(resource.BufferDesc.Flags, RhiResourceFlags::Concurrent);
            }
        }
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            const RenderGraphPass &pass = _frame.GetKept(order);
            const uint32 queue = QueueIndex(pass.Queue);
            const RhiSync stages = DefaultShaderStages(pass);
            for (uint32 index = pass.FirstAccess; index < pass.FirstAccess + pass.AccessCount; ++index)
            {
                RenderGraphAccess &access = _frame.Accesses[index];
                RenderGraphResource &resource = _frame.Resources[access.Resource];
                MARLIN_ASSERTF((access.Access & ~QueueAccesses(pass.Queue)) == RhiAccess::None,
                               "Pass {0} accesses {1} in a way the {2} queue cannot", pass.PassName, resource.Name,
                               RenderGraphCompiler::GetQueueName(pass.Queue));
                access.Sync = StagesFor(access.Access, stages);
                resource.Used = true;
                resource.FirstUse[queue] =
                    resource.FirstUse[queue] == RenderGraphNone ? order : resource.FirstUse[queue];
                resource.LastUse[queue] = order;
                textureUsage[access.Resource] |= TextureUsageFor(access.Access);
                bufferUsage[access.Resource] |= BufferUsageFor(access.Access);
            }
        }
        const uint32 epilogue = _frame.Passes[_epilogue[QueueIndex(RhiQueueType::Graphics)]].Order;
        for (uint32 index = 0; index < _frame.Resources.Count(); ++index)
        {
            RenderGraphResource &resource = _frame.Resources[index];
            const RenderGraphExternalTexture *external = resource.ExternalTexture;
            // An imported texture that must end in another layout, such as a swapchain image nothing drew to, is
            // still transitioned by the Graphics epilogue.
            if (!resource.Used && external != nullptr && external->FinalLayout != RhiLayout::Undefined &&
                external->FinalLayout != external->Layout)
            {
                resource.Used = true;
                resource.FirstUse[QueueIndex(RhiQueueType::Graphics)] = epilogue;
                resource.LastUse[QueueIndex(RhiQueueType::Graphics)] = epilogue;
            }
            if (!resource.Used)
            {
                continue;
            }
            if (resource.IsTexture())
            {
                if (external != nullptr && (textureUsage[index] & ~resource.TextureDesc.Usage) != RhiTextureUsage::None)
                {
                    MARLIN_LOG(LogRenderGraph, Error, "Passes use {0} in ways its usage flags do not allow",
                               resource.Name);
                }
                if (external == nullptr)
                {
                    resource.TextureDesc.Usage |= textureUsage[index];
                }
            }
            else if (resource.ExternalBuffer == nullptr)
            {
                resource.BufferDesc.Usage |= bufferUsage[index];
            }
        }
    }

    /// Whether *pass* leaves *resource* undefined: a target it stores with Discard or resolves away.
    bool DiscardsAfterPass(const RenderGraphPass &pass, uint32 resource) const
    {
        for (uint32 index = pass.FirstTarget; index < pass.FirstTarget + pass.TargetCount; ++index)
        {
            const RenderGraphRenderTarget &target = _frame.Targets[index];
            if (target.Texture.Index == resource && target.Store != RhiStoreOp::Store)
            {
                return true;
            }
        }
        if (pass.HasDepth && pass.Depth.Texture.Index == resource && !pass.Depth.ReadOnly)
        {
            return pass.Depth.DepthStore == RhiStoreOp::Discard && pass.Depth.StencilStore == RhiStoreOp::Discard;
        }
        return false;
    }

    /// Finds the kept passes a requested capture starts and ends at.
    void FindCapture()
    {
        _frame.CaptureFirst = RenderGraphNone;
        _frame.CaptureLast = RenderGraphNone;
        if (_options.CaptureFirst.IsNone())
        {
            return;
        }
        uint32 first = RenderGraphNone;
        uint32 last = RenderGraphNone;
        for (uint32 order = 0; order < _frame.Kept.Count() && last == RenderGraphNone; ++order)
        {
            const Name name = _frame.GetKept(order).PassName;
            if (first == RenderGraphNone && name == _options.CaptureFirst)
            {
                first = order;
            }
            if (first != RenderGraphNone && name == _options.CaptureLast)
            {
                last = order;
            }
        }
        if (first == RenderGraphNone || last == RenderGraphNone)
        {
            MARLIN_LOG(LogRenderGraph, Warning, "No kept passes from {0} through {1} to capture", _options.CaptureFirst,
                       _options.CaptureLast);
            return;
        }
        _frame.CaptureFirst = first;
        _frame.CaptureLast = last;
    }

    /// Walks the kept passes in order with the state of every subresource, collecting barriers and waits.
    void TrackStates()
    {
        const uint32 count = static_cast<uint32>(_frame.Resources.Count());
        _stateOffsets.Resize(count, 0);
        _warned.Resize(count, 0);
        uint32 total = 0;
        for (uint32 index = 0; index < count; ++index)
        {
            _stateOffsets[index] = total;
            total += _frame.Resources[index].Used ? _frame.Resources[index].GetSubresourceCount() : 0;
        }
        _states.Resize(total);
        const uint32 prologue = _frame.Passes[_prologue[QueueIndex(RhiQueueType::Graphics)]].Order;
        for (uint32 index = 0; index < count; ++index)
        {
            const RenderGraphResource &resource = _frame.Resources[index];
            if (!resource.Used)
            {
                continue;
            }
            for (uint32 subresource = 0; subresource < resource.GetSubresourceCount(); ++subresource)
            {
                SubresourceState &state = _states[_stateOffsets[index] + subresource];
                state = SubresourceState();
                if (resource.IsExternal())
                {
                    // Imported resources start on Graphics, finished with by the previous graph's last submission.
                    state.Owner = static_cast<uint8>(QueueIndex(RhiQueueType::Graphics));
                    state.Queues[QueueIndex(RhiQueueType::Graphics)].Pass = prologue;
                    state.Layout =
                        resource.ExternalTexture != nullptr ? resource.ExternalTexture->Layout : RhiLayout::Undefined;
                    state.Initialized = !resource.IsTexture() || state.Layout != RhiLayout::Undefined;
                }
            }
        }
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            const RenderGraphPass &pass = _frame.GetKept(order);
            if (pass.Role != RenderGraphPassRole::Declared)
            {
                continue;
            }
            _merged.Clear();
            _mergedIndex.Clear();
            MergeAccesses(pass);
            for (const MergedAccess &access : _merged)
            {
                Transition(order, access);
            }
        }
        AddFinalTransitions();
    }

    void MergeAccesses(const RenderGraphPass &pass)
    {
        for (const RenderGraphAccess &access : GetAccesses(pass))
        {
            const RenderGraphResource &resource = _frame.Resources[access.Resource];
            if (!resource.IsTexture())
            {
                Merge(access, 0, access.Sync);
                continue;
            }
            const RhiSubresourceRange &range = access.Range;
            const uint32 mipEnd = range.MipCount == 0 ? resource.MipCount : range.FirstMip + range.MipCount;
            const uint32 sliceEnd = range.SliceCount == 0 ? resource.SliceCount : range.FirstSlice + range.SliceCount;
            for (uint32 slice = range.FirstSlice; slice < sliceEnd && slice < resource.SliceCount; ++slice)
            {
                for (uint32 mip = range.FirstMip; mip < mipEnd && mip < resource.MipCount; ++mip)
                {
                    Merge(access, slice * resource.MipCount + mip, access.Sync);
                }
            }
        }
    }

    void Merge(const RenderGraphAccess &access, uint32 subresource, RhiSync sync)
    {
        const uint64 key = (uint64{access.Resource} << 32) | subresource;
        if (const uint32 *found = _mergedIndex.Find(key))
        {
            MergedAccess &merged = _merged[*found];
            merged.Access = merged.Access | access.Access;
            merged.Sync = merged.Sync | sync;
            merged.Reads = merged.Reads || access.ReadsContents;
            return;
        }
        _mergedIndex.Add(key, static_cast<uint32>(_merged.Count()));
        MergedAccess &merged = _merged.AddDefaulted();
        merged.Resource = access.Resource;
        merged.Subresource = subresource;
        merged.Access = access.Access;
        merged.Sync = sync;
        merged.Reads = access.ReadsContents;
    }

    /// Moves one subresource from its state to what the pass at *order* needs.
    void Transition(uint32 order, const MergedAccess &access)
    {
        RenderGraphPass &pass = _frame.GetKept(order);
        const RenderGraphResource &resource = _frame.Resources[access.Resource];
        SubresourceState &state = _states[_stateOffsets[access.Resource] + access.Subresource];
        const uint32 queue = QueueIndex(pass.Queue);
        const bool texture = resource.IsTexture();
        const bool writes = HasAnyFlags(access.Access, WriteAccesses);
        // Concurrent textures stay in General, which every queue may use at once.
        const RhiLayout layout = !texture              ? RhiLayout::Undefined
                                 : resource.Concurrent ? RhiLayout::General
                                                       : LayoutFor(access.Access);
        const QueueUse next{access.Sync, access.Access, order};
        // Ranges written in full or render targets cleared need nothing of what was there.
        const bool discards =
            !state.Initialized || (!access.Reads && !HasAnyFlags(access.Access, ReadAccesses) &&
                                   HasAnyFlags(access.Access, RhiAccess::UnorderedAccess | RhiAccess::RenderTarget |
                                                                  RhiAccess::DepthStencilWrite));
        if (!state.Initialized && access.Reads && _warned[access.Resource] == 0)
        {
            _warned[access.Resource] = 1;
            MARLIN_LOG(LogRenderGraph, Warning, "Pass {0} reads {1} before anything writes it", pass.PassName,
                       resource.Name);
        }

        if (!state.Touched && !resource.IsExternal())
        {
            // The first use of transient memory, after whatever used that memory before: the pool fills in its
            // stages once resources are placed.
            if (texture)
            {
                RhiTextureBarrier barrier;
                barrier.SyncAfter = next.Sync;
                barrier.AccessAfter = next.Access;
                barrier.LayoutAfter = layout;
                barrier.Discard = true;
                AddTexture(order, false, access, barrier, true);
            }
            else
            {
                AddGlobal(order, QueueUse(), next, access.Resource);
            }
            state.Reset(queue, next);
            state.Owner = static_cast<uint8>(queue);
        }
        else if (state.Owner != NoQueue && state.Owner != queue && !resource.Concurrent)
        {
            // Ownership moves to this queue: the owner releases after its last use, and this queue acquires after
            // waiting for it. Contents nobody needs are left behind instead.
            const uint32 owner = state.Owner;
            const QueueUse &previous = state.Queues[owner];
            MARLIN_ASSERT(previous.Pass != RenderGraphNone);
            const RhiQueueType from = static_cast<RhiQueueType>(owner);
            const bool transfers = !discards;
            if (texture)
            {
                RhiTextureBarrier release;
                release.SyncBefore = previous.Sync & QueueStages(from);
                release.AccessBefore = previous.Access;
                release.LayoutBefore = state.Layout;
                release.LayoutAfter = layout;
                release.TransferQueue = true;
                release.Transfer = {from, pass.Queue};
                RhiTextureBarrier acquire = release;
                acquire.SyncBefore = RhiSync::None;
                acquire.AccessBefore = RhiAccess::None;
                acquire.SyncAfter = next.Sync & QueueStages(pass.Queue);
                acquire.AccessAfter = next.Access;
                if (transfers)
                {
                    AddTexture(previous.Pass, true, access, release, false);
                }
                else
                {
                    acquire.TransferQueue = false;
                    acquire.LayoutBefore = RhiLayout::Undefined;
                    acquire.Discard = true;
                }
                AddTexture(order, false, access, acquire, false);
            }
            else if (transfers)
            {
                RhiBufferBarrier release;
                release.SyncBefore = previous.Sync & QueueStages(from);
                release.AccessBefore = previous.Access;
                release.TransferQueue = true;
                release.Transfer = {from, pass.Queue};
                RhiBufferBarrier acquire = release;
                acquire.SyncBefore = RhiSync::None;
                acquire.AccessBefore = RhiAccess::None;
                acquire.SyncAfter = next.Sync & QueueStages(pass.Queue);
                acquire.AccessAfter = next.Access;
                AddBuffer(previous.Pass, true, access.Resource, release);
                AddBuffer(order, false, access.Resource, acquire);
            }
            AddWait(pass, owner, previous.Pass);
            state.Reset(queue, next);
            state.Owner = static_cast<uint8>(queue);
        }
        else
        {
            // Other queues finish what this access conflicts with first: their writes, and their reads if this
            // writes. Their queue values make what they wrote visible.
            for (uint32 other = 0; other < RhiQueueTypeCount; ++other)
            {
                const QueueUse &use = state.Queues[other];
                if (other != queue && use.Pass != RenderGraphNone &&
                    ((writes && use.Access != RhiAccess::None) || HasAnyFlags(use.Access, WriteAccesses)))
                {
                    AddWait(pass, other, use.Pass);
                }
            }
            const QueueUse &previous = state.Queues[queue];
            const bool relayout = texture && state.Layout != layout;
            const bool hazard =
                HasAnyFlags(previous.Access, WriteAccesses) || (writes && previous.Access != RhiAccess::None);
            if (relayout || hazard)
            {
                if (texture)
                {
                    RhiTextureBarrier barrier;
                    barrier.SyncBefore = previous.Sync;
                    barrier.AccessBefore = previous.Access;
                    barrier.SyncAfter = next.Sync;
                    barrier.AccessAfter = next.Access;
                    barrier.LayoutBefore = discards ? RhiLayout::Undefined : state.Layout;
                    barrier.LayoutAfter = layout;
                    barrier.Discard = discards;
                    AddTexture(order, false, access, barrier, false);
                }
                else
                {
                    AddGlobal(order, previous, next, RenderGraphNone);
                }
                if (writes || relayout)
                {
                    state.Reset(queue, next);
                }
                else
                {
                    state.Queues[queue] = next;
                }
            }
            else
            {
                // Reads after reads need no barrier; a later write waits for all of them.
                QueueUse &use = state.Queues[queue];
                use.Sync = use.Sync | next.Sync;
                use.Access = use.Access | next.Access;
                use.Pass = order;
            }
            state.Owner = state.Owner == NoQueue ? static_cast<uint8>(queue) : state.Owner;
        }
        state.Touched = true;
        state.Layout = layout;
        state.Initialized = (state.Initialized || writes) && !DiscardsAfterPass(pass, access.Resource);
    }

    /// Leaves imported resources owned by Graphics and in their final layouts, and notes for the pool what each
    /// queue last did to every resource.
    void AddFinalTransitions()
    {
        const uint32 graphics = QueueIndex(RhiQueueType::Graphics);
        RenderGraphPass &graphicsEpilogue = _frame.Passes[_epilogue[graphics]];
        for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
        {
            if (queue != graphics && _epilogue[queue] != RenderGraphNone)
            {
                AddWait(graphicsEpilogue, queue, _frame.Passes[_epilogue[queue]].Order);
            }
        }
        for (uint32 index = 0; index < _frame.Resources.Count(); ++index)
        {
            RenderGraphResource &resource = _frame.Resources[index];
            if (!resource.Used)
            {
                continue;
            }
            const RhiLayout finalLayout = FinalLayout(index);
            resource.EndLayout = finalLayout;
            for (uint32 subresource = 0; subresource < resource.GetSubresourceCount(); ++subresource)
            {
                SubresourceState &state = _states[_stateOffsets[index] + subresource];
                for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
                {
                    resource.FinalSync[queue] = resource.FinalSync[queue] | state.Queues[queue].Sync;
                    resource.FinalAccess[queue] = resource.FinalAccess[queue] | state.Queues[queue].Access;
                }
                if (!resource.IsExternal())
                {
                    continue;
                }
                const uint32 owner = state.Owner == NoQueue ? graphics : state.Owner;
                const bool moves = owner != graphics && !resource.Concurrent;
                const bool relayout = resource.IsTexture() && state.Layout != finalLayout;
                if (!moves && !relayout)
                {
                    continue;
                }
                MergedAccess access;
                access.Resource = index;
                access.Subresource = subresource;
                const QueueUse &previous = state.Queues[owner];
                if (moves)
                {
                    MARLIN_ASSERT(previous.Pass != RenderGraphNone);
                    const RhiQueueType from = static_cast<RhiQueueType>(owner);
                    if (resource.IsTexture())
                    {
                        RhiTextureBarrier release;
                        release.SyncBefore = previous.Sync & QueueStages(from);
                        release.AccessBefore = previous.Access;
                        release.LayoutBefore = state.Layout;
                        release.LayoutAfter = finalLayout;
                        release.TransferQueue = true;
                        release.Transfer = {from, RhiQueueType::Graphics};
                        RhiTextureBarrier acquire = release;
                        acquire.SyncBefore = RhiSync::None;
                        acquire.AccessBefore = RhiAccess::None;
                        AddTexture(previous.Pass, true, access, release, false);
                        AddTexture(graphicsEpilogue.Order, false, access, acquire, false);
                    }
                    else
                    {
                        RhiBufferBarrier release;
                        release.SyncBefore = previous.Sync & QueueStages(from);
                        release.AccessBefore = previous.Access;
                        release.TransferQueue = true;
                        release.Transfer = {from, RhiQueueType::Graphics};
                        RhiBufferBarrier acquire = release;
                        acquire.SyncBefore = RhiSync::None;
                        acquire.AccessBefore = RhiAccess::None;
                        AddBuffer(previous.Pass, true, index, release);
                        AddBuffer(graphicsEpilogue.Order, false, index, acquire);
                    }
                    AddWait(graphicsEpilogue, owner, previous.Pass);
                }
                else
                {
                    // The Graphics epilogue runs after every queue's work, so any of them may have used it.
                    RhiTextureBarrier barrier;
                    for (const QueueUse &use : state.Queues)
                    {
                        barrier.SyncBefore = barrier.SyncBefore | (use.Sync & QueueStages(RhiQueueType::Graphics));
                        barrier.AccessBefore = barrier.AccessBefore | use.Access;
                    }
                    barrier.LayoutBefore = state.Layout;
                    barrier.LayoutAfter = finalLayout;
                    AddTexture(graphicsEpilogue.Order, false, access, barrier, false);
                }
                state.Layout = finalLayout;
                state.Owner = static_cast<uint8>(graphics);
            }
        }
    }

    /// The layout an imported texture ends in: the one it asks for, or that of its latest access.
    RhiLayout FinalLayout(uint32 index) const
    {
        const RenderGraphResource &resource = _frame.Resources[index];
        if (!resource.IsTexture() || !resource.IsExternal())
        {
            return RhiLayout::Undefined;
        }
        if (resource.ExternalTexture->FinalLayout != RhiLayout::Undefined)
        {
            return resource.ExternalTexture->FinalLayout;
        }
        RhiLayout layout = resource.ExternalTexture->Layout;
        uint32 latest = RenderGraphNone;
        for (uint32 subresource = 0; subresource < resource.GetSubresourceCount(); ++subresource)
        {
            const SubresourceState &state = _states[_stateOffsets[index] + subresource];
            for (const QueueUse &use : state.Queues)
            {
                if (use.Pass != RenderGraphNone && Later(latest, use.Pass) != latest)
                {
                    latest = use.Pass;
                    layout = state.Layout;
                }
            }
        }
        return layout;
    }

    /// For every pass, the latest pass of each queue known to have finished when it starts: through its waits, and
    /// what the passes before it on its queue knew.
    void ComputeDone()
    {
        uint32 previous[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            RenderGraphPass &pass = _frame.GetKept(order);
            const uint32 queue = QueueIndex(pass.Queue);
            for (uint32 &done : pass.Done)
            {
                done = RenderGraphNone;
            }
            if (previous[queue] != RenderGraphNone)
            {
                const RenderGraphPass &before = _frame.GetKept(previous[queue]);
                for (uint32 other = 0; other < RhiQueueTypeCount; ++other)
                {
                    pass.Done[other] = Later(pass.Done[other], before.Done[other]);
                }
            }
            for (uint32 other = 0; other < RhiQueueTypeCount; ++other)
            {
                const uint32 waited = pass.Waits[other];
                if (waited == RenderGraphNone)
                {
                    continue;
                }
                RenderGraphPass &signaler = _frame.GetKept(waited);
                signaler.Signals = true;
                pass.Done[other] = Later(pass.Done[other], waited);
                for (uint32 known = 0; known < RhiQueueTypeCount; ++known)
                {
                    pass.Done[known] = Later(pass.Done[known], signaler.Done[known]);
                }
            }
            previous[queue] = order;
        }
    }

    /// Splits each queue's kept passes into submissions: before a pass that waits, after one that signals, around a
    /// capture, and before an epilogue.
    void BuildSegments()
    {
        _frame.Segments.Clear();
        _frame.SegmentPasses.Clear();
        _frame.SubmitOrder.Clear();
        uint32 current[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            RenderGraphPass &pass = _frame.GetKept(order);
            const uint32 queue = QueueIndex(pass.Queue);
            bool split = current[queue] == RenderGraphNone || pass.Role == RenderGraphPassRole::Epilogue ||
                         order == _frame.CaptureFirst;
            for (const uint32 wait : pass.Waits)
            {
                split = split || wait != RenderGraphNone;
            }
            if (!split)
            {
                const uint32 last = _frame.Segments[current[queue]].LastOrder;
                split = _frame.GetKept(last).Signals || last == _frame.CaptureLast;
            }
            if (split)
            {
                const bool first = current[queue] == RenderGraphNone;
                current[queue] = static_cast<uint32>(_frame.Segments.Count());
                RenderGraphSegment &segment = _frame.Segments.AddDefaulted();
                segment.Queue = pass.Queue;
                segment.First = first;
                segment.Epilogue = pass.Role == RenderGraphPassRole::Epilogue;
                for (uint32 other = 0; other < RhiQueueTypeCount; ++other)
                {
                    segment.Waits[other] = pass.Waits[other];
                }
            }
            RenderGraphSegment &segment = _frame.Segments[current[queue]];
            segment.LastOrder = order;
            ++segment.PassCount;
            pass.Segment = current[queue];
        }
        uint32 next = 0;
        for (RenderGraphSegment &segment : _frame.Segments)
        {
            segment.FirstPass = next;
            next += segment.PassCount;
            segment.PassCount = 0;
        }
        _frame.SegmentPasses.Resize(next, 0);
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            RenderGraphSegment &segment = _frame.Segments[_frame.GetKept(order).Segment];
            _frame.SegmentPasses[segment.FirstPass + segment.PassCount++] = order;
        }
        // A segment waits only for passes that end earlier segments, so submitting by last order keeps every wait
        // after its signal.
        for (uint32 index = 0; index < _frame.Segments.Count(); ++index)
        {
            _frame.SubmitOrder.Add(index);
        }
        _frame.SubmitOrder.StableSort(
            [&](uint32 a, uint32 b) { return _frame.Segments[a].LastOrder < _frame.Segments[b].LastOrder; });
    }

    /// Writes the pending barriers into the frame's lists, by pass, before and after it.
    void EmitBarriers()
    {
        const auto earlier = [](const auto &a, const auto &b) {
            return a.Order != b.Order ? a.Order < b.Order : !a.After && b.After;
        };
        _textures.StableSort([&](const PendingTexture &a, const PendingTexture &b) {
            if (a.Order != b.Order || a.After != b.After)
            {
                return earlier(a, b);
            }
            return a.Resource != b.Resource ? a.Resource < b.Resource : a.Subresource < b.Subresource;
        });
        _buffers.StableSort(earlier);
        _globals.StableSort(earlier);
        usize texture = 0;
        usize buffer = 0;
        usize global = 0;
        for (uint32 order = 0; order < _frame.Kept.Count(); ++order)
        {
            RenderGraphPass &pass = _frame.GetKept(order);
            for (const bool after : {false, true})
            {
                RenderGraphBarrierRange &range = after ? pass.After : pass.Before;
                range.FirstTexture = static_cast<uint32>(_frame.TextureBarriers.Count());
                usize end = texture;
                while (end < _textures.Count() && _textures[end].Order == order && _textures[end].After == after)
                {
                    ++end;
                }
                EmitTextures(pass.Queue, texture, end);
                texture = end;
                range.TextureCount = static_cast<uint32>(_frame.TextureBarriers.Count()) - range.FirstTexture;

                range.FirstBuffer = static_cast<uint32>(_frame.BufferBarriers.Count());
                for (; buffer < _buffers.Count() && _buffers[buffer].Order == order && _buffers[buffer].After == after;
                     ++buffer)
                {
                    _frame.BufferBarriers.Add(_buffers[buffer].Barrier);
                    _frame.BufferBarrierResources.Add(_buffers[buffer].Resource);
                }
                range.BufferCount = static_cast<uint32>(_frame.BufferBarriers.Count()) - range.FirstBuffer;

                // Buffers' own hazards share one global barrier; each alias barrier stays apart for the pool.
                range.FirstGlobal = static_cast<uint32>(_frame.GlobalBarriers.Count());
                RhiGlobalBarrier merged;
                bool hasMerged = false;
                for (; global < _globals.Count() && _globals[global].Order == order && _globals[global].After == after;
                     ++global)
                {
                    const PendingGlobal &pending = _globals[global];
                    if (pending.AliasResource != RenderGraphNone)
                    {
                        _frame.AliasBarriers.Add({pending.AliasResource, RenderGraphBarrierKind::Global, pass.Queue,
                                                  static_cast<uint32>(_frame.GlobalBarriers.Count())});
                        _frame.GlobalBarriers.Add(pending.Barrier);
                        continue;
                    }
                    merged.SyncBefore = merged.SyncBefore | pending.Barrier.SyncBefore;
                    merged.SyncAfter = merged.SyncAfter | pending.Barrier.SyncAfter;
                    merged.AccessBefore = merged.AccessBefore | pending.Barrier.AccessBefore;
                    merged.AccessAfter = merged.AccessAfter | pending.Barrier.AccessAfter;
                    hasMerged = true;
                }
                if (hasMerged)
                {
                    _frame.GlobalBarriers.Add(merged);
                }
                range.GlobalCount = static_cast<uint32>(_frame.GlobalBarriers.Count()) - range.FirstGlobal;
            }
        }
    }

    /// Emits the texture barriers of one side of one pass: neighbouring mips of a slice with the same transition
    /// become one barrier, then neighbouring slices with the same mips.
    void EmitTextures(RhiQueueType queue, usize begin, usize end)
    {
        _runs.Clear();
        for (usize index = begin; index < end; ++index)
        {
            const PendingTexture &pending = _textures[index];
            const uint32 mipCount = _frame.Resources[pending.Resource].MipCount;
            const uint32 mip = pending.Subresource % mipCount;
            const uint32 slice = pending.Subresource / mipCount;
            if (!_runs.IsEmpty())
            {
                PendingTexture &run = _runs.Last();
                RhiSubresourceRange &range = run.Barrier.Range;
                if (run.Resource == pending.Resource && run.Alias == pending.Alias &&
                    SameTransition(run.Barrier, pending.Barrier) && range.FirstSlice == slice &&
                    range.FirstMip + range.MipCount == mip)
                {
                    ++range.MipCount;
                    continue;
                }
            }
            PendingTexture &run = _runs.Add(pending);
            run.Barrier.Range.FirstMip = mip;
            run.Barrier.Range.MipCount = 1;
            run.Barrier.Range.FirstSlice = slice;
            run.Barrier.Range.SliceCount = 1;
        }
        usize kept = 0;
        for (usize index = 0; index < _runs.Count(); ++index)
        {
            const PendingTexture &run = _runs[index];
            if (kept > 0)
            {
                PendingTexture &previous = _runs[kept - 1];
                RhiSubresourceRange &a = previous.Barrier.Range;
                const RhiSubresourceRange &b = run.Barrier.Range;
                if (previous.Resource == run.Resource && previous.Alias == run.Alias &&
                    SameTransition(previous.Barrier, run.Barrier) && a.FirstMip == b.FirstMip &&
                    a.MipCount == b.MipCount && a.FirstSlice + a.SliceCount == b.FirstSlice)
                {
                    ++a.SliceCount;
                    continue;
                }
            }
            _runs[kept++] = run;
        }
        _runs.Resize(kept);
        for (const PendingTexture &run : _runs)
        {
            if (run.Alias)
            {
                _frame.AliasBarriers.Add({run.Resource, RenderGraphBarrierKind::Texture, queue,
                                          static_cast<uint32>(_frame.TextureBarriers.Count())});
            }
            _frame.TextureBarriers.Add(run.Barrier);
            _frame.TextureBarrierResources.Add(run.Resource);
        }
    }

    void AddWait(RenderGraphPass &pass, uint32 queue, uint32 order)
    {
        pass.Waits[queue] = Later(pass.Waits[queue], order);
    }

    void AddTexture(uint32 order, bool after, const MergedAccess &access, const RhiTextureBarrier &barrier, bool alias)
    {
        PendingTexture &pending = _textures.AddDefaulted();
        pending.Order = order;
        pending.After = after;
        pending.Alias = alias;
        pending.Resource = access.Resource;
        pending.Subresource = access.Subresource;
        pending.Barrier = barrier;
    }

    void AddBuffer(uint32 order, bool after, uint32 resource, const RhiBufferBarrier &barrier)
    {
        PendingBuffer &pending = _buffers.AddDefaulted();
        pending.Order = order;
        pending.After = after;
        pending.Resource = resource;
        pending.Barrier = barrier;
    }

    void AddGlobal(uint32 order, const QueueUse &previous, const QueueUse &next, uint32 aliasResource)
    {
        PendingGlobal &pending = _globals.AddDefaulted();
        pending.Order = order;
        pending.AliasResource = aliasResource;
        pending.Barrier.SyncBefore = previous.Sync;
        pending.Barrier.AccessBefore = previous.Access;
        pending.Barrier.SyncAfter = next.Sync;
        pending.Barrier.AccessAfter = next.Access;
    }

    [[nodiscard]] Span<const RenderGraphAccess> GetAccesses(const RenderGraphPass &pass) const
    {
        return _frame.Accesses.AsSpan().Slice(pass.FirstAccess, pass.AccessCount);
    }

    RenderGraphFrame &_frame;
    const RenderGraphCompileOptions &_options;
    uint32 _prologue[RhiQueueTypeCount] = {};
    uint32 _epilogue[RhiQueueTypeCount] = {};
    List<uint32> _stateOffsets;
    List<SubresourceState> _states;
    List<uint8> _warned;
    List<MergedAccess> _merged;
    HashMap<uint64, uint32> _mergedIndex;
    List<PendingTexture> _textures;
    List<PendingBuffer> _buffers;
    List<PendingGlobal> _globals;
    List<PendingTexture> _runs;
};
} // namespace

void RenderGraphCompiler::Compile(RenderGraphFrame &frame, const RenderGraphCompileOptions &options)
{
    Compiler compiler(frame, options);
    compiler.Run();
}

void RenderGraphCompiler::ResolveBarrierHandles(RenderGraphFrame &frame)
{
    for (usize index = 0; index < frame.TextureBarriers.Count(); ++index)
    {
        frame.TextureBarriers[index].Texture = frame.Resources[frame.TextureBarrierResources[index]].Texture;
    }
    for (usize index = 0; index < frame.BufferBarriers.Count(); ++index)
    {
        frame.BufferBarriers[index].Buffer = frame.Resources[frame.BufferBarrierResources[index]].Buffer;
    }
}

const char *RenderGraphCompiler::GetQueueName(RhiQueueType queue)
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

const char *RenderGraphCompiler::GetLayoutName(RhiLayout layout)
{
    switch (layout)
    {
    case RhiLayout::Undefined:
        return "Undefined";
    case RhiLayout::General:
        return "General";
    case RhiLayout::Present:
        return "Present";
    case RhiLayout::RenderTarget:
        return "RenderTarget";
    case RhiLayout::DepthStencilWrite:
        return "DepthStencilWrite";
    case RhiLayout::DepthStencilRead:
        return "DepthStencilRead";
    case RhiLayout::ShaderResource:
        return "ShaderResource";
    case RhiLayout::UnorderedAccess:
        return "UnorderedAccess";
    case RhiLayout::CopySource:
        return "CopySource";
    case RhiLayout::CopyDest:
        return "CopyDest";
    case RhiLayout::ResolveSource:
        return "ResolveSource";
    case RhiLayout::ResolveDest:
        return "ResolveDest";
    case RhiLayout::ShadingRateSource:
        return "ShadingRateSource";
    }
    return "Unknown";
}
} // namespace Marlin
