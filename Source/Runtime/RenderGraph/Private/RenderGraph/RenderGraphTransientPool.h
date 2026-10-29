#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiResources.h"
#include "Templates/Result.h"

namespace Marlin
{
class RhiDevice;
struct RenderGraphFrame;

/// The memory of transient resources: heaps kept across graphs, resources placed in them where their lifetimes
/// allow sharing, and the placed resources and their views, kept while later graphs place the same description at
/// the same spot.
class RENDERGRAPH_API RenderGraphTransientPool
{
public:
    explicit RenderGraphTransientPool(RhiDevice &device);
    ~RenderGraphTransientPool();
    MARLIN_NONCOPYABLE(RenderGraphTransientPool);

    /// Places the frame's used transient resources, creates them and every view, and patches the alias barriers.
    /// *graph* is the graph's number; heaps are *heapSize* bytes unless a resource needs more.
    Result<void, RhiError> Allocate(RenderGraphFrame &frame, uint64 graph, bool aliasing, uint64 heapSize);
    /// After the graph is submitted: destroys its own views and what *retain* graphs in a row have not used.
    void Release(RenderGraphFrame &frame, uint64 graph, uint32 retain);

private:
    struct Heap
    {
        RhiHeap Handle;
        uint64 Size = 0;
        uint64 LastUsed = 0;
    };

    struct CachedView
    {
        uint64 Key = 0;
        RhiResourceView View;
    };

    struct CachedResource
    {
        RhiTexture Texture;
        RhiBuffer Buffer;
        RhiHeap Heap;
        uint64 LastUsed = 0;
        uint64 NameHash = 0;
        List<CachedView> Views;
    };

    /// What the resources placed before one in its memory last did on one queue, which its alias barriers wait for.
    struct AliasWait
    {
        RhiSync Sync = RhiSync::None;
        RhiAccess Access = RhiAccess::None;
        bool Known = false;
    };

    struct Interval
    {
        uint64 Begin = 0;
        uint64 End = 0;
    };

    [[nodiscard]] bool CanAlias(const RenderGraphFrame &frame, uint32 before, uint32 after) const;
    [[nodiscard]] uint64 FindOffset(const RenderGraphFrame &frame, uint32 heap, uint32 resource, bool aliasing);
    Result<void, RhiError> CreateResource(RenderGraphFrame &frame, uint32 resource, uint64 graph);
    void PatchAliasBarriers(RenderGraphFrame &frame);
    Result<void, RhiError> CreateViews(RenderGraphFrame &frame);
    void DestroyCached(const CachedResource &resource);

    RhiDevice &_device;
    List<Heap> _heaps;
    HashMap<uint64, CachedResource> _resources;
    /// Per heap, the resources this graph placed in it.
    List<List<uint32>> _placed;
    /// Per resource of the graph, its key in _resources; zero for none.
    List<uint64> _keys;
    List<Interval> _blocked;
    /// Scratch kept between graphs: the used transient resources in order of first use, and alias waits per resource
    /// and queue.
    List<uint32> _order;
    List<AliasWait> _aliasWaits;
    /// GetAllocationInfo by description hash, which does not change for a description.
    HashMap<uint64, RhiAllocationInfo> _allocationInfos;
    bool _warnedPlacement = false;
};
} // namespace Marlin
