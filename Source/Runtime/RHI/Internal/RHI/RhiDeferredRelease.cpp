#include "RHI/RhiDeferredRelease.h"

#include "Concurrency/ScopeLock.h"

namespace Marlin
{
void RhiDeferredReleaseQueue::Enqueue(const RhiQueueValues &after, ReleaseFunction release, void *context,
                                      uint64 payload)
{
    const ScopeLock lock(_lock);
    _pending.Add({after, release, context, payload});
}

void RhiDeferredReleaseQueue::Process(const RhiQueueValues &completed)
{
    {
        const ScopeLock lock(_lock);
        usize kept = 0;
        for (usize i = 0; i < _pending.Count(); ++i)
        {
            const Entry &entry = _pending[i];
            bool done = true;
            for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
            {
                done = done && entry.After.Values[queue] <= completed.Values[queue];
            }
            if (done)
            {
                _ready.Add(entry);
            }
            else
            {
                _pending[kept++] = entry;
            }
        }
        _pending.Resize(kept);
    }
    for (const Entry &entry : _ready)
    {
        entry.Release(entry.Context, entry.Payload);
    }
    _ready.Clear();
}

void RhiDeferredReleaseQueue::Flush()
{
    RhiQueueValues all;
    for (uint64 &value : all.Values)
    {
        value = ~uint64{0};
    }
    Process(all);
}

bool RhiDeferredReleaseQueue::IsEmpty() const
{
    const ScopeLock lock(_lock);
    return _pending.IsEmpty();
}
} // namespace Marlin
