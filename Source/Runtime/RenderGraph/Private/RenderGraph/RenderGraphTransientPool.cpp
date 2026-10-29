#include "RenderGraph/RenderGraphTransientPool.h"

#include "Hash/Hash.h"
#include "RHI/RhiDevice.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "RenderGraph/RenderGraphLog.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace
{
constexpr uint64 NoOffset = ~uint64{0};
constexpr uint64 HeapGranularity = uint64{64} * 1024;

uint64 HashFloat(uint64 seed, float32 value)
{
    return HashCombine(seed, GetHash(value));
}

uint64 HashDesc(const RhiTextureDesc &desc)
{
    uint64 hash = HashInteger(static_cast<uint64>(desc.Dimension));
    hash = HashCombine(hash, HashInteger(static_cast<uint64>(desc.Format)));
    hash = HashCombine(hash, HashInteger((uint64{desc.Width} << 32) | desc.Height));
    hash = HashCombine(hash, HashInteger((uint64{desc.DepthOrArraySize} << 32) | desc.MipCount));
    hash = HashCombine(hash, HashInteger((uint64{desc.SampleCount} << 32) | static_cast<uint32>(desc.Usage)));
    hash = HashCombine(hash, HashInteger(static_cast<uint64>(desc.Flags)));
    for (const RhiFormat format : desc.ViewFormats)
    {
        hash = HashCombine(hash, HashInteger(static_cast<uint64>(format)));
    }
    for (const float32 channel : desc.OptimizedClear.Color)
    {
        hash = HashFloat(hash, channel);
    }
    hash = HashFloat(hash, desc.OptimizedClear.Depth);
    return HashCombine(hash, HashInteger(desc.OptimizedClear.Stencil));
}

uint64 HashDesc(const RhiBufferDesc &desc)
{
    uint64 hash = HashInteger(desc.Size);
    hash = HashCombine(hash, HashInteger(static_cast<uint64>(desc.Usage)));
    hash = HashCombine(hash, HashInteger(static_cast<uint64>(desc.Location)));
    return HashCombine(hash, HashInteger(static_cast<uint64>(desc.Flags)));
}

uint64 HashView(const RenderGraphViewRecord &view)
{
    uint64 hash = HashInteger(static_cast<uint64>(view.Access));
    hash = HashCombine(hash, HashInteger((uint64{view.AutoDimension} << 8) | static_cast<uint64>(view.Dimension)));
    hash = HashCombine(hash, HashInteger(static_cast<uint64>(view.Format)));
    hash = HashCombine(hash, HashInteger((uint64{view.Range.FirstMip} << 32) | view.Range.MipCount));
    hash = HashCombine(hash, HashInteger((uint64{view.Range.FirstSlice} << 32) | view.Range.SliceCount));
    hash = HashCombine(hash, HashInteger(static_cast<uint64>(view.Range.Aspect)));
    hash = HashCombine(hash, HashInteger(view.Stride));
    hash = HashCombine(hash, HashInteger(view.Offset));
    return HashCombine(hash, HashInteger(view.Size));
}

/// The view dimension for *view* of *resource* when the pass named none: arrays for more than one slice, cubes for
/// shader reads of whole cubes of a texture that allows them.
RhiTextureViewDimension PickDimension(const RenderGraphResource &resource, const RenderGraphViewRecord &view)
{
    const RhiTextureDesc &desc = resource.TextureDesc;
    const uint32 first = view.Range.FirstSlice;
    const uint32 slices = view.Range.SliceCount != 0 ? view.Range.SliceCount
                                                     : (resource.SliceCount > first ? resource.SliceCount - first : 1);
    switch (desc.Dimension)
    {
    case RhiTextureDimension::Texture1D:
        return slices > 1 ? RhiTextureViewDimension::Texture1DArray : RhiTextureViewDimension::Texture1D;
    case RhiTextureDimension::Texture3D:
        return RhiTextureViewDimension::Texture3D;
    case RhiTextureDimension::Texture2D:
        break;
    }
    if (desc.SampleCount > 1)
    {
        return slices > 1 ? RhiTextureViewDimension::Texture2DMultisampleArray
                          : RhiTextureViewDimension::Texture2DMultisample;
    }
    if (HasAnyFlags(desc.Usage, RhiTextureUsage::Cube) && view.Access == RhiViewAccess::ShaderResource &&
        slices % 6 == 0)
    {
        return slices > 6 ? RhiTextureViewDimension::TextureCubeArray : RhiTextureViewDimension::TextureCube;
    }
    return slices > 1 ? RhiTextureViewDimension::Texture2DArray : RhiTextureViewDimension::Texture2D;
}
} // namespace

RenderGraphTransientPool::RenderGraphTransientPool(RhiDevice &device) : _device(device)
{
}

RenderGraphTransientPool::~RenderGraphTransientPool()
{
    for (const auto &entry : _resources)
    {
        DestroyCached(entry.Value);
    }
    for (const Heap &heap : _heaps)
    {
        _device.Destroy(heap.Handle);
    }
}

Result<void, RhiError> RenderGraphTransientPool::Allocate(RenderGraphFrame &frame, uint64 graph, bool aliasing,
                                                          uint64 heapSize)
{
    _keys.Clear();
    _keys.Resize(frame.Resources.Count(), 0);
    _placed.Resize(_heaps.Count());
    for (List<uint32> &placed : _placed)
    {
        placed.Clear();
    }
    // In order of first use, so a resource placed earlier is either still in use or done before the next starts.
    if (_allocationInfos.Count() > 4096)
    {
        _allocationInfos.Clear();
    }
    List<uint32> &order = _order;
    order.Clear();
    for (uint32 index = 0; index < frame.Resources.Count(); ++index)
    {
        if (frame.Resources[index].Used && !frame.Resources[index].IsExternal())
        {
            order.Add(index);
        }
    }
    order.StableSort(
        [&](uint32 a, uint32 b) { return frame.Resources[a].GetFirstUse() < frame.Resources[b].GetFirstUse(); });
    for (const uint32 index : order)
    {
        RenderGraphResource &resource = frame.Resources[index];
        const uint64 descHash = resource.IsTexture() ? HashDesc(resource.TextureDesc) : HashDesc(resource.BufferDesc);
        RhiAllocationInfo *known = _allocationInfos.Find(descHash);
        if (known == nullptr)
        {
            known = &_allocationInfos.FindOrAdd(descHash);
            *known = resource.IsTexture() ? _device.GetAllocationInfo(resource.TextureDesc)
                                          : _device.GetAllocationInfo(resource.BufferDesc);
        }
        const RhiAllocationInfo info = *known;
        resource.Size = info.Size;
        resource.Alignment = info.Alignment > 0 ? info.Alignment : 1;
        if (resource.Memoryless)
        {
            continue;
        }
        const RhiResourceFlags flags = resource.IsTexture() ? resource.TextureDesc.Flags : resource.BufferDesc.Flags;
        if (info.Size == 0 || HasAnyFlags(flags, RhiResourceFlags::Reserved))
        {
            if (!_warnedPlacement)
            {
                _warnedPlacement = true;
                MARLIN_LOG(LogRenderGraph, Warning,
                           "The device cannot place {0} in a transient heap; such resources get memory of their own",
                           resource.Name);
            }
            continue;
        }
        frame.UnaliasedBytes += info.Size;
        uint32 heap = 0;
        uint64 offset = NoOffset;
        for (; heap < _heaps.Count(); ++heap)
        {
            if (_heaps[heap].Size >= info.Size)
            {
                offset = FindOffset(frame, heap, index, aliasing);
                if (offset != NoOffset)
                {
                    break;
                }
            }
        }
        if (offset == NoOffset)
        {
            RhiHeapDesc desc;
            desc.Size = Max(heapSize, (info.Size + HeapGranularity - 1) / HeapGranularity * HeapGranularity);
            desc.DebugName = "RenderGraphTransient";
            Result<RhiHeap, RhiError> created = _device.CreateHeap(desc);
            if (created.IsError())
            {
                MARLIN_LOG(LogRenderGraph, Error, "No transient heap of {0} bytes: {1}", desc.Size,
                           GetErrorName(created.Error()));
                return Failure(created.Error());
            }
            heap = static_cast<uint32>(_heaps.Count());
            _heaps.Add({created.Value(), desc.Size, graph});
            _placed.AddDefaulted();
            offset = 0;
        }
        resource.Heap = heap;
        resource.Offset = offset;
        _placed[heap].Add(index);
        _heaps[heap].LastUsed = graph;
    }
    for (uint32 heap = 0; heap < _heaps.Count(); ++heap)
    {
        if (!_placed[heap].IsEmpty())
        {
            ++frame.HeapCount;
            frame.TransientBytes += _heaps[heap].Size;
        }
    }
    for (const uint32 index : order)
    {
        const Result<void, RhiError> created = CreateResource(frame, index, graph);
        if (created.IsError())
        {
            return created;
        }
    }
    PatchAliasBarriers(frame);
    return CreateViews(frame);
}

bool RenderGraphTransientPool::CanAlias(const RenderGraphFrame &frame, uint32 before, uint32 after) const
{
    // Every last use of *before* must be finished when any first use of *after* starts: earlier on the same queue,
    // where the alias barrier orders them, or waited for by the other queue.
    const RenderGraphResource &earlier = frame.Resources[before];
    const RenderGraphResource &later = frame.Resources[after];
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        const uint32 last = earlier.LastUse[queue];
        if (last == RenderGraphNone)
        {
            continue;
        }
        for (uint32 other = 0; other < RhiQueueTypeCount; ++other)
        {
            const uint32 first = later.FirstUse[other];
            if (first == RenderGraphNone)
            {
                continue;
            }
            if (other == queue)
            {
                if (last >= first)
                {
                    return false;
                }
                continue;
            }
            const uint32 done = frame.GetKept(first).Done[queue];
            if (done == RenderGraphNone || done < last)
            {
                return false;
            }
        }
    }
    return true;
}

uint64 RenderGraphTransientPool::FindOffset(const RenderGraphFrame &frame, uint32 heap, uint32 resource, bool aliasing)
{
    _blocked.Clear();
    const RenderGraphResource &placing = frame.Resources[resource];
    for (const uint32 other : _placed[heap])
    {
        if (!aliasing || !CanAlias(frame, other, resource))
        {
            const RenderGraphResource &placed = frame.Resources[other];
            _blocked.Add({placed.Offset, placed.Offset + placed.Size});
        }
    }
    _blocked.Sort([](const Interval &a, const Interval &b) { return a.Begin < b.Begin; });
    const auto align = [&](uint64 offset) {
        return (offset + placing.Alignment - 1) / placing.Alignment * placing.Alignment;
    };
    uint64 offset = 0;
    for (const Interval &blocked : _blocked)
    {
        if (align(offset) + placing.Size <= blocked.Begin)
        {
            break;
        }
        offset = Max(offset, blocked.End);
    }
    offset = align(offset);
    return offset + placing.Size <= _heaps[heap].Size ? offset : NoOffset;
}

Result<void, RhiError> RenderGraphTransientPool::CreateResource(RenderGraphFrame &frame, uint32 resource, uint64 graph)
{
    RenderGraphResource &entry = frame.Resources[resource];
    RhiPlacement placement;
    if (entry.Heap != RenderGraphNone)
    {
        placement.Heap = _heaps[entry.Heap].Handle;
        placement.Offset = entry.Offset;
    }
    // The same description at the same spot is the same resource as last graph; resources with memory of their
    // own and the same description in one graph each get one of their own.
    uint64 key = entry.IsTexture() ? HashDesc(entry.TextureDesc) : HashDesc(entry.BufferDesc);
    key = HashCombine(key, placement.Heap.GetHash());
    key = HashCombine(key, HashInteger(placement.Offset));
    CachedResource *cached = _resources.Find(key);
    while (cached != nullptr && cached->LastUsed == graph && !placement.Heap.IsValid())
    {
        key = HashCombine(key, HashInteger(1));
        cached = _resources.Find(key);
    }
    const uint64 nameHash = entry.Name.GetHash();
    if (cached == nullptr)
    {
        CachedResource created;
        created.Heap = placement.Heap;
        created.NameHash = nameHash;
        if (entry.IsTexture())
        {
            RhiTextureDesc desc = entry.TextureDesc;
            desc.Placement = placement;
            desc.DebugName = entry.Name;
            Result<RhiTexture, RhiError> texture = _device.CreateTexture(desc);
            if (texture.IsError())
            {
                MARLIN_LOG(LogRenderGraph, Error, "Cannot create {0}: {1}", entry.Name, GetErrorName(texture.Error()));
                return Failure(texture.Error());
            }
            created.Texture = texture.Value();
        }
        else
        {
            RhiBufferDesc desc = entry.BufferDesc;
            desc.Placement = placement;
            desc.DebugName = entry.Name;
            Result<RhiBuffer, RhiError> buffer = _device.CreateBuffer(desc);
            if (buffer.IsError())
            {
                MARLIN_LOG(LogRenderGraph, Error, "Cannot create {0}: {1}", entry.Name, GetErrorName(buffer.Error()));
                return Failure(buffer.Error());
            }
            created.Buffer = buffer.Value();
        }
        cached = &_resources.Add(key, Move(created));
    }
    else if (cached->NameHash != nameHash)
    {
        // A resource reused under another name, so captures show what it holds now.
        cached->NameHash = nameHash;
        if (cached->Texture.IsValid())
        {
            _device.SetDebugName(cached->Texture, entry.Name);
        }
        else
        {
            _device.SetDebugName(cached->Buffer, entry.Name);
        }
    }
    cached->LastUsed = graph;
    entry.Texture = cached->Texture;
    entry.Buffer = cached->Buffer;
    _keys[resource] = key;
    return {};
}

void RenderGraphTransientPool::PatchAliasBarriers(RenderGraphFrame &frame)
{
    // A resource's first barrier also waits for what the same queue last did to the resources placed before it in
    // the same memory.
    // A resource has an alias barrier per subresource, which all wait for the same, so it is found once per resource
    // and queue.
    _aliasWaits.Clear();
    if (!frame.AliasBarriers.IsEmpty())
    {
        _aliasWaits.Resize(frame.Resources.Count() * RhiQueueTypeCount);
    }
    for (const RenderGraphAliasBarrier &alias : frame.AliasBarriers)
    {
        const RenderGraphResource &resource = frame.Resources[alias.Resource];
        if (resource.Heap == RenderGraphNone)
        {
            continue;
        }
        const uint32 queue = static_cast<uint32>(alias.Queue);
        AliasWait &wait = _aliasWaits[static_cast<usize>(alias.Resource) * RhiQueueTypeCount + queue];
        if (!wait.Known)
        {
            wait.Known = true;
            for (const uint32 other : _placed[resource.Heap])
            {
                const RenderGraphResource &earlier = frame.Resources[other];
                const bool overlaps =
                    earlier.Offset < resource.Offset + resource.Size && resource.Offset < earlier.Offset + earlier.Size;
                if (other != alias.Resource && overlaps && earlier.GetFirstUse() < resource.GetFirstUse())
                {
                    wait.Sync = wait.Sync | earlier.FinalSync[queue];
                    wait.Access = wait.Access | earlier.FinalAccess[queue];
                }
            }
        }
        const RhiSync sync = wait.Sync;
        const RhiAccess access = wait.Access;
        switch (alias.Kind)
        {
        case RenderGraphBarrierKind::Texture:
            frame.TextureBarriers[alias.Index].SyncBefore = frame.TextureBarriers[alias.Index].SyncBefore | sync;
            frame.TextureBarriers[alias.Index].AccessBefore = frame.TextureBarriers[alias.Index].AccessBefore | access;
            break;
        case RenderGraphBarrierKind::Buffer:
            frame.BufferBarriers[alias.Index].SyncBefore = frame.BufferBarriers[alias.Index].SyncBefore | sync;
            frame.BufferBarriers[alias.Index].AccessBefore = frame.BufferBarriers[alias.Index].AccessBefore | access;
            break;
        case RenderGraphBarrierKind::Global:
            frame.GlobalBarriers[alias.Index].SyncBefore = frame.GlobalBarriers[alias.Index].SyncBefore | sync;
            frame.GlobalBarriers[alias.Index].AccessBefore = frame.GlobalBarriers[alias.Index].AccessBefore | access;
            break;
        }
    }
}

Result<void, RhiError> RenderGraphTransientPool::CreateViews(RenderGraphFrame &frame)
{
    for (const RenderGraphPass &pass : frame.Passes)
    {
        if (!pass.Kept)
        {
            continue;
        }
        for (uint32 index = pass.FirstView; index < pass.FirstView + pass.ViewCount; ++index)
        {
            RenderGraphViewRecord &view = frame.Views[index];
            const RenderGraphResource &resource = frame.Resources[view.Resource];
            if (!resource.IsTexture())
            {
                // Shaders read buffers through their device addresses (MarlinLanguage §4.5): no descriptor.
                continue;
            }
            const uint64 key = HashView(view);
            CachedResource *cached = _keys[view.Resource] != 0 ? _resources.Find(_keys[view.Resource]) : nullptr;
            if (cached != nullptr)
            {
                for (const CachedView &existing : cached->Views)
                {
                    view.View = existing.Key == key ? existing.View : view.View;
                }
                if (view.View.IsValid())
                {
                    continue;
                }
            }
            RhiTextureViewDesc desc;
            desc.Texture = resource.Texture;
            desc.Access = view.Access;
            desc.Dimension = view.AutoDimension ? PickDimension(resource, view) : view.Dimension;
            desc.Format = view.Format;
            desc.Range = view.Range;
            desc.DebugName = resource.Name;
            Result<RhiResourceView, RhiError> created = _device.CreateView(desc);
            if (created.IsError())
            {
                MARLIN_LOG(LogRenderGraph, Error, "Cannot create a view of {0}: {1}", resource.Name,
                           GetErrorName(created.Error()));
                return Failure(created.Error());
            }
            view.View = created.Value();
            if (cached != nullptr)
            {
                cached->Views.Add({key, view.View});
            }
            else
            {
                view.FrameOwned = true;
            }
        }
    }
    return {};
}

void RenderGraphTransientPool::Release(RenderGraphFrame &frame, uint64 graph, uint32 retain)
{
    for (RenderGraphViewRecord &view : frame.Views)
    {
        if (view.FrameOwned && view.View.IsValid())
        {
            _device.Destroy(view.View);
            view.View = {};
        }
    }
    // Destroying waits for the queues on the device's side, so what this graph used stays alive until it is done.
    _resources.RemoveIf([&](const KeyValuePair<uint64, CachedResource> &entry) {
        if (entry.Value.LastUsed + retain >= graph)
        {
            return false;
        }
        DestroyCached(entry.Value);
        return true;
    });
    for (usize index = _heaps.Count(); index-- > 0;)
    {
        if (_heaps[index].LastUsed + retain < graph)
        {
            _device.Destroy(_heaps[index].Handle);
            _heaps.RemoveAt(index);
        }
    }
    _placed.Resize(_heaps.Count());
}

void RenderGraphTransientPool::DestroyCached(const CachedResource &resource)
{
    for (const CachedView &view : resource.Views)
    {
        _device.Destroy(view.View);
    }
    if (resource.Texture.IsValid())
    {
        _device.Destroy(resource.Texture);
    }
    if (resource.Buffer.IsValid())
    {
        _device.Destroy(resource.Buffer);
    }
}
} // namespace Marlin
