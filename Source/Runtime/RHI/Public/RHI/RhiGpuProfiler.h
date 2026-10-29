#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiDevice.h"

namespace Marlin
{
/// A timed GPU zone, on the CPU's PlatformTime::Ticks clock.
struct RhiGpuZone
{
    const char *Name = nullptr;
    uint32 Depth = 0;
    uint64 BeginTicks = 0;
    uint64 EndTicks = 0;
};

/// Times zones of one queue's command lists with timestamps, frame by frame, and hands finished frames to the
/// profiler hooks (a Tracy GPU track, say) on the CPU clock through calibrated timestamps. Lists may record zones
/// on any thread; the frame calls go on the submit thread. RenderGraph times its passes with one per queue. Where the
/// queue's lists cannot resolve queries, as on copy queues, results are read on the CPU once the frame is done; a
/// queue that cannot write timestamps times nothing.
class RHI_API RhiGpuProfiler
{
public:
    RhiGpuProfiler(RhiDevice &device, RhiQueueType queue, uint32 maxZonesPerFrame = 2048, uint32 framesInFlight = 4);
    ~RhiGpuProfiler();
    MARLIN_NONCOPYABLE(RhiGpuProfiler);

    /// Opens a zone in *list*, a list of this queue; *name* must stay valid. ~0u when the frame is full or the GPU
    /// is too far behind, in which case EndZone does nothing.
    uint32 BeginZone(RhiCommandList &list, const char *name, uint32 depth = 0);
    void EndZone(RhiCommandList &list, uint32 zone);

    /// Records the frame's resolve into *list*, the frame's last list on the queue, before it is submitted.
    void ResolveFrame(RhiCommandList &list);
    /// After that list is submitted: the queue value the submission completes at. Starts the next frame.
    void FrameSubmitted(uint64 queueValue);
    /// Reports the frames the GPU finished to the profiler hooks and keeps the newest.
    void Collect();

    /// The zones of the newest finished frame.
    [[nodiscard]] Span<const RhiGpuZone> GetLastFrame() const
    {
        return _lastFrame;
    }

private:
    struct Zone
    {
        const char *Name = nullptr;
        uint32 Depth = 0;
        uint32 Begin = 0;
        bool Ended = false;
    };

    struct Frame
    {
        RhiQueryPool Pool;
        RhiBuffer Readback;
        List<Zone> Zones;
        uint32 UsedQueries = 0;
        uint64 SubmitValue = 0;
        bool Submitted = false;
        bool Resolved = false;
    };

    RhiDevice &_device;
    RhiQueueType _queue;
    const char *_track;
    bool _readOnHost = false;
    List<uint64> _hostTicks;
    uint32 _maxZones;
    SpinLock _lock;
    List<Frame> _frames;
    uint32 _current = 0;
    List<RhiGpuZone> _lastFrame;
};
} // namespace Marlin
