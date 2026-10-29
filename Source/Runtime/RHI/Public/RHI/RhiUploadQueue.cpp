#include "RHI/RhiUploadQueue.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformThread.h"
#include "RHI/RhiLog.h"

namespace Marlin
{
namespace
{
RhiTextureBarrier Release(const RhiTextureUpload &upload)
{
    RhiTextureBarrier barrier;
    barrier.Texture = upload.Texture;
    barrier.SyncBefore = RhiSync::Copy;
    barrier.AccessBefore = RhiAccess::CopyDest;
    barrier.LayoutBefore = RhiLayout::CopyDest;
    barrier.LayoutAfter = upload.FinalLayout;
    barrier.Range.FirstMip = upload.Region.Mip;
    barrier.Range.MipCount = 1;
    barrier.Range.FirstSlice = upload.Region.Slice;
    barrier.Range.SliceCount = 1;
    barrier.Range.Aspect = upload.Region.Aspect;
    barrier.TransferQueue = upload.Destination != RhiQueueType::Copy;
    barrier.Transfer = {RhiQueueType::Copy, upload.Destination};
    return barrier;
}

uint64 Mix(uint64 value)
{
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    return value;
}

uint64 BufferKey(RhiBuffer buffer)
{
    return Mix((uint64{buffer.Generation} << 32) | buffer.Index);
}

uint64 SubresourceKey(const RhiTextureUpload &upload)
{
    const uint64 texture = (uint64{upload.Texture.Generation} << 32) | upload.Texture.Index;
    const uint64 subresource = (uint64{upload.Region.Slice} << 24) | (uint64{upload.Region.Mip} << 8) |
                               static_cast<uint64>(upload.Region.Aspect);
    return Mix(texture ^ Mix(subresource + 1));
}
} // namespace

RhiUploadQueue::RhiUploadQueue(RhiDevice &device, uint64 stagingSize) : _device(device), _stagingSize(stagingSize)
{
    Result<RhiFence, RhiError> fence = device.CreateFence(0, "UploadQueue");
    if (fence.IsOk())
    {
        _fence = fence.Value();
    }
    RhiBufferDesc desc;
    desc.Size = stagingSize;
    desc.Usage = RhiBufferUsage::CopySource;
    desc.Location = RhiMemoryLocation::Upload;
    desc.DebugName = "UploadQueueStaging";
    Result<RhiBuffer, RhiError> staging = device.CreateBuffer(desc);
    if (staging.IsOk())
    {
        _staging = staging.Value();
        _stagingData = device.GetMappedData(_staging);
    }
    else
    {
        MARLIN_LOG(LogRhi, Error, "The upload queue has no staging memory: {0}", GetErrorName(staging.Error()));
    }
}

RhiUploadQueue::~RhiUploadQueue()
{
    if (_list != nullptr)
    {
        _device.DiscardCommandList(_list);
    }
    if (_fence.IsValid() && _nextValue > 1)
    {
        _device.Wait(_fence, _nextValue - 1);
    }
    ProcessCompletions();
    for (const RhiBuffer buffer : _current.Oversized)
    {
        _device.Destroy(buffer);
    }
    if (_staging.IsValid())
    {
        _device.Destroy(_staging);
    }
    if (_fence.IsValid())
    {
        _device.Destroy(_fence);
    }
}

RhiBuffer RhiUploadQueue::CreateOversized(uint64 size)
{
    if (size <= _stagingSize / 2)
    {
        return {};
    }
    RhiBufferDesc desc;
    desc.Size = size;
    desc.Usage = RhiBufferUsage::CopySource;
    desc.Location = RhiMemoryLocation::Upload;
    desc.DebugName = "UploadQueueOversized";
    Result<RhiBuffer, RhiError> created = _device.CreateBuffer(desc);
    return created.IsOk() ? created.Value() : RhiBuffer{};
}

bool RhiUploadQueue::AllocateStaging(uint64 size, uint64 alignment, RhiBuffer oversized, RhiBuffer &buffer,
                                     uint64 &offset, uint8 *&data)
{
    if (oversized.IsValid())
    {
        buffer = oversized;
        offset = 0;
        data = _device.GetMappedData(buffer);
        _current.Oversized.Add(buffer);
        return true;
    }
    uint64 position = (_stagingHead + alignment - 1) & ~(alignment - 1);
    if (position % _stagingSize + size > _stagingSize)
    {
        position += _stagingSize - position % _stagingSize;
    }
    if (position + size - _stagingTail > _stagingSize)
    {
        return false;
    }
    _stagingHead = position + size;
    buffer = _staging;
    offset = position % _stagingSize;
    data = _stagingData + offset;
    return true;
}

RhiCommandList *RhiUploadQueue::GetList()
{
    if (_list == nullptr)
    {
        _list = _device.BeginCommandList(RhiQueueType::Copy);
        _current.Value = _nextValue;
    }
    return _list;
}

void RhiUploadQueue::TouchResource(uint64 resource)
{
    if (_touched.Contains(resource))
    {
        RecordPending();
    }
    _touched.Add(resource);
}

void RhiUploadQueue::RecordPending()
{
    if (_copies.IsEmpty())
    {
        return;
    }
    // One barrier into CopyDest for every texture, the copies, one barrier releasing everything to its queue.
    RhiBarriers barriers;
    if (!_toCopy.IsEmpty())
    {
        barriers.Textures = _toCopy.AsSpan();
        _list->Barrier(barriers);
    }
    for (const PendingCopy &copy : _copies)
    {
        if (copy.Texture.IsValid())
        {
            RhiBufferTextureLayout layout;
            layout.Offset = copy.StagingOffset;
            layout.RowPitch = copy.RowPitch;
            layout.RowsPerSlice = copy.RowsPerSlice;
            _list->CopyBufferToTexture(copy.Texture, copy.Region, copy.Staging, layout);
        }
        else
        {
            _list->CopyBuffer(copy.Buffer, copy.Offset, copy.Staging, copy.StagingOffset, copy.Size);
        }
    }
    if (!_bufferReleases.IsEmpty() || !_textureReleases.IsEmpty())
    {
        barriers.Buffers = _bufferReleases.AsSpan();
        barriers.Textures = _textureReleases.AsSpan();
        _list->Barrier(barriers);
    }
    _toCopy.Clear();
    _copies.Clear();
    _bufferReleases.Clear();
    _textureReleases.Clear();
    _touched.Clear();
}

bool RhiUploadQueue::WaitForStaging()
{
    uint64 oldest = 0;
    {
        const ScopeLock lock(_lock);
        if (_inFlight.IsEmpty())
        {
            return false;
        }
        oldest = _inFlight[0].Value;
    }
    _device.Wait(_fence, oldest);
    ProcessCompletions();
    return true;
}

RhiUploadTicket RhiUploadQueue::Upload(const RhiBufferUpload &upload)
{
    if (!_staging.IsValid() || upload.Data.IsEmpty())
    {
        return {};
    }
    const uint64 size = upload.Data.Count();
    const RhiBuffer oversized = CreateOversized(size);
    if (size > _stagingSize / 2 && !oversized.IsValid())
    {
        return {};
    }
    RhiBuffer staging;
    uint64 offset = 0;
    uint8 *data = nullptr;
    uint64 value = 0;
    while (true)
    {
        {
            const ScopeLock lock(_lock);
            if (GetList() == nullptr)
            {
                break;
            }
            if (AllocateStaging(size, 16, oversized, staging, offset, data))
            {
                TouchResource(BufferKey(upload.Buffer));
                PendingCopy &copy = _copies.AddDefaulted();
                copy.Staging = staging;
                copy.StagingOffset = offset;
                copy.Size = size;
                copy.Buffer = upload.Buffer;
                copy.Offset = upload.Offset;
                if (upload.Destination != RhiQueueType::Copy)
                {
                    RhiBufferBarrier &release = _bufferReleases.AddDefaulted();
                    release.Buffer = upload.Buffer;
                    release.SyncBefore = RhiSync::Copy;
                    release.AccessBefore = RhiAccess::CopyDest;
                    release.Offset = upload.Offset;
                    release.Size = size;
                    release.TransferQueue = true;
                    release.Transfer = {RhiQueueType::Copy, upload.Destination};
                }
                _copying.FetchAdd(1, MemoryOrder::Relaxed);
                value = _current.Value;
            }
        }
        if (value != 0)
        {
            __builtin_memcpy(data, upload.Data.Data(), size);
            _copying.FetchSub(1, MemoryOrder::Release);
            return {value};
        }
        if (!WaitForStaging())
        {
            MARLIN_LOG(LogRhi, Warning, "The upload queue's staging ring is full of unflushed uploads");
            break;
        }
    }
    if (oversized.IsValid())
    {
        _device.Destroy(oversized);
    }
    return {};
}

RhiUploadTicket RhiUploadQueue::Upload(const RhiTextureUpload &upload)
{
    if (!_staging.IsValid() || upload.Data.IsEmpty())
    {
        return {};
    }
    const uint64 alignment = _device.GetLimits().CopyOffsetAlignment > 16 ? _device.GetLimits().CopyOffsetAlignment : 16;
    const uint64 size = upload.Data.Count();
    const RhiBuffer oversized = CreateOversized(size);
    if (size > _stagingSize / 2 && !oversized.IsValid())
    {
        return {};
    }
    RhiBuffer staging;
    uint64 offset = 0;
    uint8 *data = nullptr;
    uint64 value = 0;
    while (true)
    {
        {
            const ScopeLock lock(_lock);
            if (GetList() == nullptr)
            {
                break;
            }
            if (AllocateStaging(size, alignment, oversized, staging, offset, data))
            {
                TouchResource(SubresourceKey(upload));
                const RhiTextureBarrier release = Release(upload);
                RhiTextureBarrier &toCopy = _toCopy.AddDefaulted();
                toCopy.Texture = upload.Texture;
                toCopy.SyncAfter = RhiSync::Copy;
                toCopy.AccessAfter = RhiAccess::CopyDest;
                toCopy.LayoutBefore = upload.Discard ? RhiLayout::Undefined : RhiLayout::CopyDest;
                toCopy.LayoutAfter = RhiLayout::CopyDest;
                toCopy.Discard = upload.Discard;
                toCopy.Range = release.Range;
                PendingCopy &copy = _copies.AddDefaulted();
                copy.Staging = staging;
                copy.StagingOffset = offset;
                copy.Size = size;
                copy.Texture = upload.Texture;
                copy.Region = upload.Region;
                copy.RowPitch = upload.RowPitch;
                copy.RowsPerSlice = upload.RowsPerSlice;
                _textureReleases.Add(release);
                _copying.FetchAdd(1, MemoryOrder::Relaxed);
                value = _current.Value;
            }
        }
        if (value != 0)
        {
            __builtin_memcpy(data, upload.Data.Data(), size);
            _copying.FetchSub(1, MemoryOrder::Release);
            return {value};
        }
        if (!WaitForStaging())
        {
            MARLIN_LOG(LogRhi, Warning, "The upload queue's staging ring is full of unflushed uploads");
            break;
        }
    }
    if (oversized.IsValid())
    {
        _device.Destroy(oversized);
    }
    return {};
}

Result<void, RhiError> RhiUploadQueue::Flush()
{
    RhiCommandList *list = nullptr;
    Batch batch;
    {
        const ScopeLock lock(_lock);
        if (_list == nullptr)
        {
            return {};
        }
        RecordPending();
        list = _list;
        _list = nullptr;
        batch = Move(_current);
        _current = {};
        batch.StagingEnd = _stagingHead;
        ++_nextValue;
    }
    // Uploads of this batch may still be copying their data into staging memory.
    while (_copying.Load(MemoryOrder::Acquire) != 0)
    {
        PlatformThread::YieldThread();
    }
    RhiCommandList *lists[] = {list};
    const RhiFenceSignal signal{_fence, batch.Value};
    RhiSubmitDesc submit;
    submit.CommandLists = lists;
    submit.Signals = Span<const RhiFenceSignal>(&signal, 1);
    Result<uint64, RhiError> result = _device.Submit(RhiQueueType::Copy, submit);
    const ScopeLock lock(_lock);
    _inFlight.Add(Move(batch));
    if (result.IsError())
    {
        return Failure(result.Error());
    }
    return {};
}

bool RhiUploadQueue::IsComplete(RhiUploadTicket ticket) const
{
    return !ticket.IsValid() || _device.GetCompletedValue(_fence) >= ticket.Value;
}

RhiFenceWait RhiUploadQueue::GetWait(RhiUploadTicket ticket) const
{
    return {_fence, ticket.Value};
}

RhiBufferBarrier RhiUploadQueue::GetAcquireBarrier(const RhiBufferUpload &upload)
{
    RhiBufferBarrier barrier;
    barrier.Buffer = upload.Buffer;
    barrier.Offset = upload.Offset;
    barrier.Size = upload.Data.Count();
    barrier.TransferQueue = true;
    barrier.Transfer = {RhiQueueType::Copy, upload.Destination};
    return barrier;
}

RhiTextureBarrier RhiUploadQueue::GetAcquireBarrier(const RhiTextureUpload &upload)
{
    RhiTextureBarrier barrier = Release(upload);
    barrier.SyncBefore = RhiSync::None;
    barrier.AccessBefore = RhiAccess::None;
    return barrier;
}

void RhiUploadQueue::ProcessCompletions()
{
    if (!_fence.IsValid())
    {
        return;
    }
    const uint64 completed = _device.GetCompletedValue(_fence);
    List<RhiBuffer> released;
    {
        const ScopeLock lock(_lock);
        usize done = 0;
        while (done < _inFlight.Count() && _inFlight[done].Value <= completed)
        {
            _stagingTail = _inFlight[done].StagingEnd;
            released.AddRange(_inFlight[done].Oversized);
            ++done;
        }
        _inFlight.RemoveAt(0, done);
    }
    for (const RhiBuffer buffer : released)
    {
        _device.Destroy(buffer);
    }
}
} // namespace Marlin
