#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "RenderGraph/RenderGraphExport.gen.h"

namespace Marlin
{
/// One resource of an exported graph.
MSTRUCT()
struct RenderGraphExportResource
{
    MGENERATED_BODY()

    MPROPERTY()
    String Name;
    /// Texture or Buffer.
    MPROPERTY()
    String Kind;
    MPROPERTY()
    bool Imported = false;
    /// Accessed by no kept pass, so never allocated.
    MPROPERTY()
    bool Culled = false;
    MPROPERTY()
    String Format;
    MPROPERTY()
    uint32 Width = 0;
    MPROPERTY()
    uint32 Height = 0;
    MPROPERTY()
    uint32 DepthOrArraySize = 0;
    MPROPERTY()
    uint32 MipCount = 0;
    /// The buffer's size, or the memory the texture takes.
    MPROPERTY()
    uint64 Size = 0;
    /// The transient heap and offset it was placed at; -1 when it is imported or takes no memory.
    MPROPERTY()
    int32 Heap = -1;
    MPROPERTY()
    uint64 Offset = 0;
    /// The kept passes that access it first and last; -1 if none.
    MPROPERTY()
    int32 FirstPass = -1;
    MPROPERTY()
    int32 LastPass = -1;
};

MSTRUCT()
struct RenderGraphExportAccess
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Resource = 0;
    /// RhiAccess flags, joined by '|'.
    MPROPERTY()
    String Access;
    MPROPERTY()
    String Sync;
    MPROPERTY()
    uint32 FirstMip = 0;
    MPROPERTY()
    uint32 MipCount = 0;
    MPROPERTY()
    uint32 FirstSlice = 0;
    MPROPERTY()
    uint32 SliceCount = 0;
};

MSTRUCT()
struct RenderGraphExportBarrier
{
    MGENERATED_BODY()

    /// The resource, or -1 for a global barrier.
    MPROPERTY()
    int32 Resource = -1;
    MPROPERTY()
    String SyncBefore;
    MPROPERTY()
    String SyncAfter;
    MPROPERTY()
    String AccessBefore;
    MPROPERTY()
    String AccessAfter;
    MPROPERTY()
    String LayoutBefore;
    MPROPERTY()
    String LayoutAfter;
    MPROPERTY()
    uint32 FirstMip = 0;
    MPROPERTY()
    uint32 MipCount = 0;
    MPROPERTY()
    uint32 FirstSlice = 0;
    MPROPERTY()
    uint32 SliceCount = 0;
    MPROPERTY()
    bool Discard = false;
    /// "Graphics->Compute" for an ownership transfer, empty otherwise.
    MPROPERTY()
    String Transfer;
};

MSTRUCT()
struct RenderGraphExportPass
{
    MGENERATED_BODY()

    MPROPERTY()
    String Name;
    /// The queue it was declared on and the one it runs on.
    MPROPERTY()
    String DeclaredQueue;
    MPROPERTY()
    String Queue;
    MPROPERTY()
    bool Culled = false;
    MPROPERTY()
    bool Renders = false;
    /// Position among kept passes; -1 if culled.
    MPROPERTY()
    int32 Order = -1;
    MPROPERTY()
    int32 Submission = -1;
    MPROPERTY()
    List<RenderGraphExportAccess> Accesses;
    /// Passes of other queues it waits for, as indices into Passes.
    MPROPERTY()
    List<uint32> Waits;
    MPROPERTY()
    List<RenderGraphExportBarrier> BarriersBefore;
    MPROPERTY()
    List<RenderGraphExportBarrier> BarriersAfter;
};

MSTRUCT()
struct RenderGraphExportSubmission
{
    MGENERATED_BODY()

    MPROPERTY()
    String Queue;
    /// Indices into Passes.
    MPROPERTY()
    List<uint32> Passes;
    /// Submissions it waits for, as indices into Submissions.
    MPROPERTY()
    List<uint32> Waits;
    /// Closes the queue's frame with final transitions.
    MPROPERTY()
    bool Epilogue = false;
};

/// A compiled graph as Remote's Render.Graph returns it (Documentation/Architecture/10): passes in declaration order
/// with their accesses, barriers and waits, resources with their placement, and submissions in order.
MSTRUCT()
struct RenderGraphExport
{
    MGENERATED_BODY()

    /// How many graphs executed before this one.
    MPROPERTY()
    uint64 Graph = 0;
    MPROPERTY()
    List<RenderGraphExportPass> Passes;
    MPROPERTY()
    List<RenderGraphExportResource> Resources;
    MPROPERTY()
    List<RenderGraphExportSubmission> Submissions;
    /// Bytes of transient heaps the graph used, and what its transient resources would take without aliasing.
    MPROPERTY()
    uint64 TransientBytes = 0;
    MPROPERTY()
    uint64 UnaliasedBytes = 0;
    MPROPERTY()
    uint32 HeapCount = 0;
};
} // namespace Marlin
