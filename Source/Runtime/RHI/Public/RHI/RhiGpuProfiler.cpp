#include "RHI/RhiGpuProfiler.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Profiler.h"
#include "HAL/PlatformTime.h"

namespace Marlin
{
namespace
{
const char *TrackName(RhiQueueType queue)
{
    switch (queue)
    {
    case RhiQueueType::Graphics:
        return "GPU Graphics";
    case RhiQueueType::Compute:
        return "GPU Compute";
    case RhiQueueType::Copy:
        return "GPU Copy";
    }
    return "GPU";
}
} // namespace

RhiGpuProfiler::RhiGpuProfiler(RhiDevice &device, RhiQueueType queue, uint32 maxZonesPerFrame, uint32 framesInFlight)
    : _device(device), _queue(queue), _track(TrackName(queue)), _maxZones(maxZonesPerFrame)
{
    _frames.Resize(framesInFlight);
    const RhiDeviceLimits &limits = device.GetLimits();
    const auto index = static_cast<uint32>(queue);
    if (limits.TimestampFrequency[index] == 0)
    {
        return;
    }
    _readOnHost = !limits.ResolvesQueries[index];
    if (_readOnHost)
    {
        _hostTicks.Resize(usize{maxZonesPerFrame} * 2);
    }
    for (Frame &frame : _frames)
    {
        RhiQueryPoolDesc pool;
        pool.Kind = RhiQueryKind::Timestamp;
        pool.Count = maxZonesPerFrame * 2;
        pool.DebugName = "GpuProfiler";
        Result<RhiQueryPool, RhiError> created = device.CreateQueryPool(pool);
        if (created.IsError())
        {
            continue;
        }
        if (!_readOnHost)
        {
            RhiBufferDesc readback;
            readback.Size = uint64{pool.Count} * sizeof(uint64);
            readback.Usage = RhiBufferUsage::CopyDest;
            readback.Location = RhiMemoryLocation::Readback;
            readback.DebugName = "GpuProfilerReadback";
            Result<RhiBuffer, RhiError> buffer = device.CreateBuffer(readback);
            if (buffer.IsError())
            {
                device.Destroy(created.Value());
                continue;
            }
            frame.Readback = buffer.Value();
        }
        frame.Pool = created.Value();
    }
}

RhiGpuProfiler::~RhiGpuProfiler()
{
    for (const Frame &frame : _frames)
    {
        if (frame.Pool.IsValid())
        {
            _device.Destroy(frame.Pool);
        }
        if (frame.Readback.IsValid())
        {
            _device.Destroy(frame.Readback);
        }
    }
}

uint32 RhiGpuProfiler::BeginZone(RhiCommandList &list, const char *name, uint32 depth)
{
    uint32 index = ~0u;
    uint32 query = 0;
    RhiQueryPool pool;
    {
        const ScopeLock lock(_lock);
        Frame &frame = _frames[_current];
        if (!frame.Pool.IsValid() || frame.Submitted || frame.Zones.Count() >= _maxZones)
        {
            return ~0u;
        }
        index = static_cast<uint32>(frame.Zones.Count());
        query = frame.UsedQueries;
        frame.UsedQueries += 2;
        frame.Zones.Add({name, depth, query, false});
        pool = frame.Pool;
    }
    list.WriteTimestamp(pool, query);
    return index;
}

void RhiGpuProfiler::EndZone(RhiCommandList &list, uint32 zone)
{
    if (zone == ~0u)
    {
        return;
    }
    RhiQueryPool pool;
    uint32 query = 0;
    {
        const ScopeLock lock(_lock);
        Frame &frame = _frames[_current];
        if (zone >= frame.Zones.Count())
        {
            return;
        }
        frame.Zones[zone].Ended = true;
        query = frame.Zones[zone].Begin + 1;
        pool = frame.Pool;
    }
    list.WriteTimestamp(pool, query);
}

void RhiGpuProfiler::ResolveFrame(RhiCommandList &list)
{
    const ScopeLock lock(_lock);
    Frame &frame = _frames[_current];
    if (frame.UsedQueries == 0 || frame.Submitted)
    {
        return;
    }
    if (_readOnHost)
    {
        frame.Resolved = true;
        return;
    }
    list.ResolveQueries(frame.Pool, 0, frame.UsedQueries, frame.Readback, 0);
    RhiGlobalBarrier toHost;
    toHost.SyncBefore = RhiSync::Copy;
    toHost.AccessBefore = RhiAccess::CopyDest;
    toHost.SyncAfter = RhiSync::Host;
    toHost.AccessAfter = RhiAccess::HostRead;
    RhiBarriers barriers;
    barriers.Global = Span<const RhiGlobalBarrier>(&toHost, 1);
    list.Barrier(barriers);
    frame.Resolved = true;
}

void RhiGpuProfiler::FrameSubmitted(uint64 queueValue)
{
    const ScopeLock lock(_lock);
    Frame &frame = _frames[_current];
    if (frame.UsedQueries > 0)
    {
        frame.SubmitValue = queueValue;
        frame.Submitted = true;
    }
    _current = (_current + 1) % static_cast<uint32>(_frames.Count());
}

void RhiGpuProfiler::Collect()
{
    const uint64 completed = _device.GetCompletedValues().Values[static_cast<uint32>(_queue)];
    uint64 cpuNow = 0;
    uint64 gpuNow = 0;
    bool calibrated = false;
    const float64 gpuFrequency =
        static_cast<float64>(_device.GetLimits().TimestampFrequency[static_cast<uint32>(_queue)]);
    const float64 cpuFrequency = static_cast<float64>(PlatformTime::TicksPerSecond());
    const ScopeLock lock(_lock);
    for (uint32 step = 1; step <= _frames.Count(); ++step)
    {
        Frame &frame = _frames[(_current + step) % _frames.Count()];
        if (!frame.Submitted || frame.SubmitValue > completed)
        {
            continue;
        }
        if (frame.Resolved && gpuFrequency > 0)
        {
            if (!calibrated)
            {
                _device.GetCalibratedTimestamps(_queue, cpuNow, gpuNow);
                calibrated = true;
            }
            // Host-read frames come back through ReadQueries, which also resets the queries for the frame's reuse.
            const uint64 *ticks = nullptr;
            if (!_readOnHost)
            {
                ticks = reinterpret_cast<const uint64 *>(_device.GetMappedData(frame.Readback));
            }
            else if (_device.ReadQueries(frame.Pool, 0, frame.UsedQueries, _hostTicks))
            {
                ticks = _hostTicks.Data();
            }
            const auto toCpu = [&](uint64 gpu) {
                const float64 seconds = (static_cast<float64>(gpu) - static_cast<float64>(gpuNow)) / gpuFrequency;
                return static_cast<uint64>(static_cast<float64>(cpuNow) + seconds * cpuFrequency);
            };
            _lastFrame.Clear();
            for (const Zone &zone : frame.Zones)
            {
                if (!zone.Ended || ticks == nullptr)
                {
                    continue;
                }
                RhiGpuZone &timed = _lastFrame.AddDefaulted();
                timed.Name = zone.Name;
                timed.Depth = zone.Depth;
                timed.BeginTicks = toCpu(ticks[zone.Begin]);
                timed.EndTicks = toCpu(ticks[zone.Begin + 1]);
                Private::ProfileGpuZone(_track, timed.Name, timed.Depth, timed.BeginTicks, timed.EndTicks);
            }
        }
        frame.Zones.Clear();
        frame.UsedQueries = 0;
        frame.Submitted = false;
        frame.Resolved = false;
    }
}
} // namespace Marlin
