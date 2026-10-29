#include "Asset/BulkStreamer.h"

#include "Asset/Assets.h"
#include "Concurrency/ScopeLock.h"
#include "Memory/Memory.h"

namespace Marlin
{
BulkBytes::~BulkBytes()
{
    Memory::Free(_data);
}

BulkStreamer::BulkStreamer(const BulkStreamerDesc &desc) : _desc(desc), _shared(new Shared())
{
}

BulkStreamer::~BulkStreamer()
{
    for (auto &entry : _inFlight)
    {
        entry.Value.Read.Cancel();
    }
}

BulkRequestId BulkStreamer::Request(const AssetSlot &slot, uint64 offset, uint64 size, float32 priority,
                                    BulkCallback callback, IoPriority io)
{
    return Request(Assets::GetBulk(slot), offset, size, priority, Move(callback), io);
}

BulkRequestId BulkStreamer::Request(const FileRegion &bulk, uint64 offset, uint64 size, float32 priority,
                                    BulkCallback callback, IoPriority io)
{
    Queued &queued = _queue.AddDefaulted();
    queued.Id = _nextId++;
    queued.Region = bulk;
    queued.Offset = offset;
    queued.Size = size;
    queued.Priority = priority;
    queued.Io = io;
    queued.Callback = Move(callback);
    const BulkRequestId id = queued.Id;
    _queueIndex.Add(id, _queue.Count() - 1);
    SiftUp(_queue.Count() - 1);
    return id;
}

void BulkStreamer::SetPriority(BulkRequestId request, float32 priority)
{
    if (const usize *index = _queueIndex.Find(request))
    {
        _queue[*index].Priority = priority;
        Place(*index);
    }
}

void BulkStreamer::Cancel(BulkRequestId request)
{
    if (const usize *index = _queueIndex.Find(request))
    {
        RemoveQueued(*index);
        return;
    }
    if (InFlight *inFlight = _inFlight.Find(request))
    {
        inFlight->Cancelled = true;
        inFlight->Callback = {};
        inFlight->Read.Cancel();
    }
}

void BulkStreamer::Swap(usize a, usize b)
{
    Queued moved = Move(_queue[a]);
    _queue[a] = Move(_queue[b]);
    _queue[b] = Move(moved);
    *_queueIndex.Find(_queue[a].Id) = a;
    *_queueIndex.Find(_queue[b].Id) = b;
}

void BulkStreamer::SiftUp(usize index)
{
    while (index > 0)
    {
        const usize parent = (index - 1) / 2;
        if (!Before(_queue[index], _queue[parent]))
        {
            return;
        }
        Swap(index, parent);
        index = parent;
    }
}

void BulkStreamer::SiftDown(usize index)
{
    while (true)
    {
        const usize left = index * 2 + 1;
        const usize right = left + 1;
        usize best = index;
        if (left < _queue.Count() && Before(_queue[left], _queue[best]))
        {
            best = left;
        }
        if (right < _queue.Count() && Before(_queue[right], _queue[best]))
        {
            best = right;
        }
        if (best == index)
        {
            return;
        }
        Swap(index, best);
        index = best;
    }
}

void BulkStreamer::Place(usize index)
{
    if (index > 0 && Before(_queue[index], _queue[(index - 1) / 2]))
    {
        SiftUp(index);
    }
    else
    {
        SiftDown(index);
    }
}

void BulkStreamer::RemoveQueued(usize index)
{
    _queueIndex.Remove(_queue[index].Id);
    const usize last = _queue.Count() - 1;
    if (index != last)
    {
        _queue[index] = Move(_queue[last]);
        *_queueIndex.Find(_queue[index].Id) = index;
    }
    _queue.Pop();
    if (index < _queue.Count())
    {
        Place(index);
    }
}

void BulkStreamer::ProcessFrame()
{
    List<Finished> finished;
    {
        const ScopeLock lock(_shared->Lock);
        finished = Move(_shared->Reads);
        _shared->Reads.Clear();
    }
    for (const Finished &read : finished)
    {
        InFlight *inFlight = _inFlight.Find(read.Id);
        if (inFlight == nullptr)
        {
            continue;
        }
        const bool cancelled = inFlight->Cancelled;
        BulkCallback callback = Move(inFlight->Callback);
        _inFlightBytes -= inFlight->Size;
        _inFlight.Remove(read.Id);
        if (cancelled)
        {
            continue;
        }
        usize size = 0;
        uint8 *data = read.Read.TakeData(size);
        callback(read.Id, data != nullptr ? SharedPtr<const BulkBytes>(new BulkBytes(data, size)) : nullptr);
    }

    while (!_queue.IsEmpty())
    {
        Queued &next = _queue[0];
        const bool fits =
            _inFlight.Count() < _desc.MaxInFlightRequests && _inFlightBytes + next.Size <= _desc.MaxInFlightBytes;
        if (!fits && !_inFlight.IsEmpty())
        {
            break;
        }
        const BulkRequestId id = next.Id;
        InFlight &inFlight = _inFlight.Add(id, {});
        inFlight.Size = next.Size;
        inFlight.Callback = Move(next.Callback);
        _inFlightBytes += next.Size;
        SharedPtr<Shared> shared = _shared;
        inFlight.Read =
            Vfs::ReadAsync(next.Region, next.Io, next.Offset, next.Size, [shared, id](const AsyncReadRequest &read) {
                const ScopeLock lock(shared->Lock);
                shared->Reads.Add({id, read});
            });
        RemoveQueued(0);
    }
}

void BulkStreamer::Flush()
{
    ProcessFrame();
    while (!_inFlight.IsEmpty())
    {
        for (auto &entry : _inFlight)
        {
            (void)entry.Value.Read.Wait();
        }
        ProcessFrame();
    }
}
} // namespace Marlin
