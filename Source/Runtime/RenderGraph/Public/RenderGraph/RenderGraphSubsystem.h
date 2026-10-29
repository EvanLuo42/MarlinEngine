#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "RHI/RhiResources.h"
#include "String/Name.h"
#include "Templates/Function.h"
#include "RenderGraph/RenderGraphSubsystem.gen.h"

namespace Marlin
{
class RenderGraphTransientPool;
class RhiDevice;
class RhiGpuProfiler;
struct RhiGpuZone;
class RhiSubsystem;
struct RenderGraphExport;
struct RenderGraphFrame;

/// What RenderGraphBuilders share across frames: the transient heaps and the resources placed in them, per-queue
/// GPU profilers that time every pass, the queue value that orders one graph after the last, and requests from tools
/// to export or capture the next graph.
MCLASS(Config = "Engine.RenderGraph", Dependencies = "RhiSubsystem")
class RENDERGRAPH_API RenderGraphSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Runs Compute passes on the compute queue; off folds them onto Graphics.
    MPROPERTY(ConsoleVariable = "r.RenderGraph.AsyncCompute")
    static bool AsyncCompute;
    /// Runs Copy passes on the copy queue; off folds them onto Graphics.
    MPROPERTY(ConsoleVariable = "r.RenderGraph.AsyncCopy")
    static bool AsyncCopy;
    /// Removes passes whose results nothing uses.
    MPROPERTY(ConsoleVariable = "r.RenderGraph.Culling")
    static bool Culling;
    /// Lets transient resources whose lifetimes do not overlap share memory.
    MPROPERTY(ConsoleVariable = "r.RenderGraph.Aliasing")
    static bool Aliasing;
    /// Passes one job records into one command list.
    MPROPERTY(ConsoleVariable = "r.RenderGraph.PassesPerList")
    static uint32 PassesPerList;
    /// Bytes of each transient heap; a bigger resource gets a heap of its own size.
    MPROPERTY()
    static uint64 TransientHeapSize;
    /// Graphs a transient heap or placed resource may go unused before it is released.
    MPROPERTY()
    static uint32 RetainGraphs;

    RenderGraphSubsystem();
    ~RenderGraphSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    /// Null if no backend made a device.
    [[nodiscard]] RhiDevice *GetDevice() const
    {
        return _device;
    }

    /// The GPU times of the passes on *queue* in the newest graph the GPU has finished, on the CPU clock; empty
    /// without a device.
    [[nodiscard]] Span<const RhiGpuZone> GetLastGpuFrame(RhiQueueType queue) const;

    /// Graphs executed so far.
    [[nodiscard]] uint64 GetGraphCount() const
    {
        return _graphCount;
    }

    /// Calls *callback* once with the next graph that executes, compiled, on the submit thread.
    void RequestExport(Function<void(const RenderGraphExport &)> callback);
    /// Captures the next graph with the injected capture tool, from the first pass named *first* through the next
    /// pass named *last*; submissions split at both ends.
    void RequestPassCapture(Name first, Name last);

private:
    friend class RenderGraphBuilder;
    friend struct RenderGraphExecutor;

    /// A frame for a new graph: one a finished graph left, emptied, or a new one.
    UniquePtr<RenderGraphFrame> AcquireFrame();
    /// Keeps *frame*, which its graph is done with, for a later graph.
    void ReleaseFrame(UniquePtr<RenderGraphFrame> frame);
    /// A null-terminated copy of *name* that lives as long as the subsystem, for profiler zones.
    const char *GetZoneName(Name name);
    void TakeRequests(List<Function<void(const RenderGraphExport &)>> &exports, Name &captureFirst, Name &captureLast);

    RhiSubsystem *_rhi = nullptr;
    RhiDevice *_device = nullptr;
    UniquePtr<RenderGraphTransientPool> _transient;
    UniquePtr<RhiGpuProfiler> _profilers[RhiQueueTypeCount];
    /// The Graphics value the last graph's final submission completes at; every queue waits for it first.
    uint64 _lastGraphValue = 0;
    uint64 _graphCount = 0;
    SpinLock _requestLock;
    List<Function<void(const RenderGraphExport &)>> _exportRequests;
    Name _captureFirst;
    Name _captureLast;
    SpinLock _zoneNameLock;
    HashMap<Name, const char *> _zoneNames;
    SpinLock _frameLock;
    List<UniquePtr<RenderGraphFrame>> _spareFrames;
};
} // namespace Marlin
