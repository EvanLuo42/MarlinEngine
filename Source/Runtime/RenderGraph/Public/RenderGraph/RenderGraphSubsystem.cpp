#include "RenderGraph/RenderGraphSubsystem.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/AssertFormat.h"
#include "Diagnostics/Profiler.h"
#include "Engine/Engine.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiGpuProfiler.h"
#include "RHI/RhiSubsystem.h"
#include "RenderGraph/RenderGraphExport.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "RenderGraph/RenderGraphLog.h"
#include "RenderGraph/RenderGraphTransientPool.h"

MARLIN_DEFINE_LOG_CATEGORY(LogRenderGraph);

namespace Marlin
{
bool RenderGraphSubsystem::AsyncCompute = true;
bool RenderGraphSubsystem::AsyncCopy = true;
bool RenderGraphSubsystem::Culling = true;
bool RenderGraphSubsystem::Aliasing = true;
uint32 RenderGraphSubsystem::PassesPerList = 8;                    // RECOVERY-DECISION D1
uint64 RenderGraphSubsystem::TransientHeapSize = uint64{64} << 20; // RECOVERY-DECISION D1
uint32 RenderGraphSubsystem::RetainGraphs = 3;                     // RECOVERY-DECISION D1

RenderGraphSubsystem::RenderGraphSubsystem() = default;

RenderGraphSubsystem::~RenderGraphSubsystem() = default;

void RenderGraphSubsystem::Initialize(SubsystemContext &context)
{
    _rhi = context.Collection->Get<RhiSubsystem>();
    _device = _rhi != nullptr ? _rhi->GetDevice() : nullptr;
    if (_device == nullptr)
    {
        return;
    }
    _transient = MakeUnique<RenderGraphTransientPool>(*_device);
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        _profilers[queue] = MakeUnique<RhiGpuProfiler>(*_device, static_cast<RhiQueueType>(queue));
    }
    _lastGraphValue = 0;
    _graphCount = 0;
}

void RenderGraphSubsystem::Deinitialize()
{
    if (_device != nullptr)
    {
        _device->WaitIdle();
    }
    for (UniquePtr<RhiGpuProfiler> &profiler : _profilers)
    {
        profiler.Reset();
    }
    {
        const ScopeLock lock(_frameLock);
        _spareFrames.Clear();
    }
    _transient.Reset();
    {
        const ScopeLock lock(_requestLock);
        _exportRequests.Clear();
    }
    const ScopeLock lock(_zoneNameLock);
    for (const auto &entry : _zoneNames)
    {
        Memory::Free(const_cast<char *>(entry.Value));
    }
    _zoneNames.Clear();
    _device = nullptr;
    _rhi = nullptr;
}

void RenderGraphSubsystem::RequestExport(Function<void(const RenderGraphExport &)> callback)
{
    const ScopeLock lock(_requestLock);
    _exportRequests.Add(Move(callback));
}

void RenderGraphSubsystem::RequestPassCapture(Name first, Name last)
{
    const ScopeLock lock(_requestLock);
    _captureFirst = first;
    _captureLast = last;
}

void RenderGraphSubsystem::TakeRequests(List<Function<void(const RenderGraphExport &)>> &exports, Name &captureFirst,
                                        Name &captureLast)
{
    const ScopeLock lock(_requestLock);
    exports = Move(_exportRequests);
    _exportRequests.Clear();
    captureFirst = _captureFirst;
    captureLast = _captureLast;
    _captureFirst = Name();
    _captureLast = Name();
}

const char *RenderGraphSubsystem::GetZoneName(Name name)
{
    const ScopeLock lock(_zoneNameLock);
    if (const char **found = _zoneNames.Find(name))
    {
        return *found;
    }
    const StringView text = name.ToView();
    char *copy = static_cast<char *>(Memory::Allocate(text.Length() + 1));
    MemCopy(copy, text.Data(), text.Length());
    copy[text.Length()] = '\0';
    _zoneNames.Add(name, copy);
    return copy;
}

UniquePtr<RenderGraphFrame> RenderGraphSubsystem::AcquireFrame()
{
    {
        const ScopeLock lock(_frameLock);
        if (!_spareFrames.IsEmpty())
        {
            return _spareFrames.Pop();
        }
    }
    MARLIN_ASSERTF(_device != nullptr, "A render graph needs a device");
    return MakeUnique<RenderGraphFrame>(*this, *_device);
}

void RenderGraphSubsystem::ReleaseFrame(UniquePtr<RenderGraphFrame> frame)
{
    frame->Reset();
    const ScopeLock lock(_frameLock);
    _spareFrames.Add(Move(frame));
}

Span<const RhiGpuZone> RenderGraphSubsystem::GetLastGpuFrame(RhiQueueType queue) const
{
    const RhiGpuProfiler *profiler = _profilers[static_cast<uint32>(queue)].Get();
    return profiler != nullptr ? profiler->GetLastFrame() : Span<const RhiGpuZone>();
}
} // namespace Marlin
