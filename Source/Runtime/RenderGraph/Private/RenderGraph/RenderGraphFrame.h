#pragma once

#include "Concurrency/Jobs.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/Arena.h"
#include "RHI/RhiBarriers.h"
#include "RHI/RhiDevice.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "String/Name.h"
#include "String/StringView.h"

namespace Marlin
{
class RenderGraphSubsystem;

inline constexpr uint32 RenderGraphNone = ~0u;

enum class RenderGraphResourceKind : uint8
{
    Texture,
    Buffer,
};

struct RenderGraphResource
{
    StringView Name;
    RenderGraphResourceKind Kind = RenderGraphResourceKind::Texture;
    RhiTextureDesc TextureDesc;
    RhiBufferDesc BufferDesc;
    RenderGraphExternalTexture *ExternalTexture = nullptr;
    RenderGraphExternalBuffer *ExternalBuffer = nullptr;
    /// The RHI object: the imported one, or the transient one once the pool has placed it.
    RhiTexture Texture;
    RhiBuffer Buffer;
    uint32 MipCount = 1;
    uint32 SliceCount = 1;
    /// Queues share it without ownership transfers.
    bool Concurrent = false;
    /// A Transient texture that lives only within render passes and takes no memory of its own.
    bool Memoryless = false;
    /// Accessed by a kept pass.
    bool Used = false;
    /// The orders of the kept passes on each queue that access it first and last; RenderGraphNone if none.
    uint32 FirstUse[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
    uint32 LastUse[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
    /// The work of each queue that last touches it, which a resource aliasing its memory later waits for.
    RhiSync FinalSync[RhiQueueTypeCount] = {};
    RhiAccess FinalAccess[RhiQueueTypeCount] = {};
    /// The layout an imported texture is left in.
    RhiLayout EndLayout = RhiLayout::Undefined;
    /// The transient heap and offset it is placed at; RenderGraphNone when it has no placement.
    uint32 Heap = RenderGraphNone;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint64 Alignment = 0;

    [[nodiscard]] bool IsExternal() const
    {
        return ExternalTexture != nullptr || ExternalBuffer != nullptr;
    }

    [[nodiscard]] bool IsTexture() const
    {
        return Kind == RenderGraphResourceKind::Texture;
    }

    [[nodiscard]] uint32 GetSubresourceCount() const
    {
        return MipCount * SliceCount;
    }

    [[nodiscard]] uint32 GetFirstUse() const
    {
        uint32 first = RenderGraphNone;
        for (const uint32 use : FirstUse)
        {
            first = use < first ? use : first;
        }
        return first;
    }

    [[nodiscard]] uint32 GetLastUse() const
    {
        uint32 last = RenderGraphNone;
        for (const uint32 use : LastUse)
        {
            if (use != RenderGraphNone && (last == RenderGraphNone || use > last))
            {
                last = use;
            }
        }
        return last;
    }
};

struct RenderGraphAccess
{
    uint32 Resource = RenderGraphNone;
    RhiAccess Access = RhiAccess::None;
    /// Filled by the compiler from Access and the pass's shader stages.
    RhiSync Sync = RhiSync::None;
    RhiSubresourceRange Range;
    /// The pass needs what the range held before; false for render targets it clears and ranges it overwrites.
    bool ReadsContents = false;
    bool WritesContents = false;
};

struct RenderGraphViewRecord
{
    uint32 Resource = RenderGraphNone;
    RhiViewAccess Access = RhiViewAccess::ShaderResource;
    bool AutoDimension = true;
    RhiTextureViewDimension Dimension = RhiTextureViewDimension::Texture2D;
    RhiFormat Format = RhiFormat::Unknown;
    RhiSubresourceRange Range;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint32 Stride = 0;
    /// Created by the pool once the resource exists.
    RhiResourceView View;
    /// Made for this graph alone and destroyed after it, as views of imported resources are.
    bool FrameOwned = false;
};

/// The barriers recorded before or after one pass: ranges of the frame's barrier lists.
struct RenderGraphBarrierRange
{
    uint32 FirstTexture = 0;
    uint32 TextureCount = 0;
    uint32 FirstBuffer = 0;
    uint32 BufferCount = 0;
    uint32 FirstGlobal = 0;
    uint32 GlobalCount = 0;

    [[nodiscard]] bool IsEmpty() const
    {
        return TextureCount == 0 && BufferCount == 0 && GlobalCount == 0;
    }
};

/// Why a pass is in the graph.
enum class RenderGraphPassRole : uint8
{
    /// Added with AddPass.
    Declared,
    /// Opens a queue's frame: imported resources leave the Graphics queue here.
    Prologue,
    /// Closes the queue's frame with final transitions.
    Epilogue,
};

struct RenderGraphPass
{
    Name PassName;
    RenderGraphPassRole Role = RenderGraphPassRole::Declared;
    RhiQueueType DeclaredQueue = RhiQueueType::Graphics;
    RhiQueueType Queue = RhiQueueType::Graphics;
    RhiSync ShaderStages = RhiSync::None;
    bool NeverCull = false;
    uint32 FirstAccess = 0;
    uint32 AccessCount = 0;
    uint32 FirstView = 0;
    uint32 ViewCount = 0;
    uint32 FirstTarget = 0;
    uint32 TargetCount = 0;
    bool HasDepth = false;
    RenderGraphDepthTarget Depth;
    RenderGraphTexture ShadingRate;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 ViewMask = 0;
    RenderGraphPassFunction Function;
    /// What the compiler made of it.
    bool Kept = false;
    /// Position among kept passes; RenderGraphNone if culled.
    uint32 Order = RenderGraphNone;
    uint32 Segment = RenderGraphNone;
    /// Per queue, the order of the pass on that queue this one waits for; RenderGraphNone for none.
    uint32 Waits[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
    /// Per queue, the latest order on that queue that is known to have finished when this pass starts.
    uint32 Done[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
    /// Another queue waits for it, so its submission ends after it.
    bool Signals = false;
    RenderGraphBarrierRange Before;
    RenderGraphBarrierRange After;

    /// It executes inside BeginRendering: it has targets, or a render area to rasterize without attachments.
    [[nodiscard]] bool Renders() const
    {
        return TargetCount > 0 || HasDepth || (Width > 0 && Height > 0);
    }
};

/// Kept passes of one queue submitted together: they wait for other queues only before the first and signal only
/// after the last.
struct RenderGraphSegment
{
    RhiQueueType Queue = RhiQueueType::Graphics;
    uint32 FirstPass = 0;
    uint32 PassCount = 0;
    /// Per queue, the order of the pass this segment waits for; RenderGraphNone for none.
    uint32 Waits[RhiQueueTypeCount] = {RenderGraphNone, RenderGraphNone, RenderGraphNone};
    /// The queue's first segment, which waits for the previous graph.
    bool First = false;
    /// The queue's epilogue, into whose last command list the queue's GPU profiler resolves.
    bool Epilogue = false;
    uint32 LastOrder = RenderGraphNone;
    uint32 FirstList = 0;
    uint32 ListCount = 0;
    uint64 SubmitValue = 0;
};

enum class RenderGraphBarrierKind : uint8
{
    Texture,
    Buffer,
    Global,
};

/// A barrier at a transient resource's first use, which must also wait for what used its memory before.
struct RenderGraphAliasBarrier
{
    uint32 Resource = RenderGraphNone;
    RenderGraphBarrierKind Kind = RenderGraphBarrierKind::Texture;
    RhiQueueType Queue = RhiQueueType::Graphics;
    uint32 Index = 0;
};

struct RenderGraphFrame;

/// A run of a segment's passes that one executor job records into one command list.
struct RenderGraphRecordJob
{
    RenderGraphFrame *Frame = nullptr;
    RenderGraphSubsystem *Subsystem = nullptr;
    uint32 Segment = 0;
    uint32 FirstPass = 0;
    uint32 PassCount = 0;
    uint32 List = 0;
};

/// Everything one graph declares and what compiling it makes, in the frame's arena and lists. RenderGraphSubsystem
/// keeps frames between graphs, so that Reset leaves the next graph their memory.
struct RenderGraphFrame
{
    RenderGraphFrame(RenderGraphSubsystem &subsystem, RhiDevice &device) : Subsystem(subsystem), Device(device)
    {
    }

    RenderGraphSubsystem &Subsystem;
    RhiDevice &Device;
    Arena Memory;
    List<RenderGraphResource> Resources;
    List<RenderGraphPass> Passes;
    List<RenderGraphAccess> Accesses;
    List<RenderGraphViewRecord> Views;
    List<RenderGraphRenderTarget> Targets;
    uint32 OpenPass = RenderGraphNone;
    bool Executed = false;

    /// Passes by order.
    List<uint32> Kept;
    List<RhiTextureBarrier> TextureBarriers;
    List<RhiBufferBarrier> BufferBarriers;
    List<RhiGlobalBarrier> GlobalBarriers;
    /// The resource of each texture and buffer barrier, whose handle is filled in once it exists.
    List<uint32> TextureBarrierResources;
    List<uint32> BufferBarrierResources;
    List<RenderGraphAliasBarrier> AliasBarriers;
    List<RenderGraphSegment> Segments;
    /// The orders of each segment's passes, at the segment's FirstPass.
    List<uint32> SegmentPasses;
    /// Segments in the order they are submitted.
    List<uint32> SubmitOrder;
    List<RhiCommandList *> CommandLists;
    /// The orders a requested pass capture starts and ends at; RenderGraphNone for none.
    uint32 CaptureFirst = RenderGraphNone;
    uint32 CaptureLast = RenderGraphNone;
    /// Bytes of the heaps used, of the transient resources without aliasing, and the heaps used.
    uint64 TransientBytes = 0;
    uint64 UnaliasedBytes = 0;
    uint32 HeapCount = 0;
    /// The executor's jobs, and the profiler zone name of each kept pass, by order.
    List<RenderGraphRecordJob> RecordJobs;
    List<JobDecl> RecordDecls;
    List<const char *> ZoneNames;

    /// Empties the frame for another graph, keeping the memory of its lists and arena.
    void Reset()
    {
        Memory.Reset();
        Resources.Clear();
        Passes.Clear();
        Accesses.Clear();
        Views.Clear();
        Targets.Clear();
        OpenPass = RenderGraphNone;
        Executed = false;
        Kept.Clear();
        TextureBarriers.Clear();
        BufferBarriers.Clear();
        GlobalBarriers.Clear();
        TextureBarrierResources.Clear();
        BufferBarrierResources.Clear();
        AliasBarriers.Clear();
        Segments.Clear();
        SegmentPasses.Clear();
        SubmitOrder.Clear();
        CommandLists.Clear();
        CaptureFirst = RenderGraphNone;
        CaptureLast = RenderGraphNone;
        TransientBytes = 0;
        UnaliasedBytes = 0;
        HeapCount = 0;
        RecordJobs.Clear();
        RecordDecls.Clear();
        ZoneNames.Clear();
    }

    [[nodiscard]] RenderGraphPass &GetKept(uint32 order)
    {
        return Passes[Kept[order]];
    }

    [[nodiscard]] const RenderGraphPass &GetKept(uint32 order) const
    {
        return Passes[Kept[order]];
    }
};
} // namespace Marlin
