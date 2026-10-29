#include "Vfs/AsyncRead.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/Jobs.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/List.h"
#include "HAL/PlatformAsyncIo.h"
#include "HAL/PlatformThread.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "String/String.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
enum class AsyncReadStatus : uint8
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};
} // namespace

namespace Private
{
struct AsyncReadState : RefCounted
{
    AsyncReadState() = default;
    ~AsyncReadState() override
    {
        if (ReadBuffer != Buffer)
        {
            Memory::Free(ReadBuffer);
        }
        Memory::Free(Buffer);
    }
    MARLIN_NONCOPYABLE(AsyncReadState);

    FileRegion Region;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint8 *Buffer = nullptr;
    IoPriority Priority = IoPriority::Streaming;
    AsyncReadCallback Completion;
    Atomic<uint8> Status{static_cast<uint8>(AsyncReadStatus::Queued)};
    Event Done{EventReset::Manual};
    /// What the platform reads: Buffer itself for a plain region; for a compressed one, the stored bytes of blocks
    /// FirstBlock to LastBlock, which a job then decompresses into Buffer. A first or last block the read needs only
    /// part of may come from the compression's cache instead, and is then not read.
    uint64 ReadOffset = 0;
    uint64 ReadSize = 0;
    uint8 *ReadBuffer = nullptr;
    uint64 FirstBlock = 0;
    uint64 LastBlock = 0;
    SharedPtr<const CachedRegionBlock> FirstCached;
    SharedPtr<const CachedRegionBlock> LastCached;
    /// Bytes of ReadSize handed to the platform so far and the parts still in flight; touched by the I/O thread only.
    uint64 Issued = 0;
    uint32 PartsInFlight = 0;
    bool AnyFailed = false;
};
} // namespace Private

namespace
{
using Private::AsyncReadState;

/// One platform request reads at most this much; larger reads go in parts.
constexpr uint64 PartSize = uint64{16} << 20;
constexpr uint32 QueueCapacity = 64;
constexpr uint32 PriorityCount = 3;
/// How long the I/O thread waits for completions before it looks for new reads again; the platform queue cannot be
/// woken by ReadAsync, so this bounds how late a read queued behind running ones starts.
constexpr uint32 CompletionWait = 1;
/// Reads that decompress at least this many blocks spread them over the workers.
constexpr uint64 ParallelBlocks = 8;

/// Reads waiting to start at one priority, oldest first; taking one moves the head instead of shifting the rest.
struct ReadQueue
{
    List<SharedPtr<AsyncReadState>> Items;
    usize Head = 0;

    [[nodiscard]] bool IsEmpty() const
    {
        return Head == Items.Count();
    }

    [[nodiscard]] const SharedPtr<AsyncReadState> &Front() const
    {
        return Items[Head];
    }

    void PopFront()
    {
        Items[Head++].Reset();
        if (Head == Items.Count())
        {
            Clear();
        }
        else if (Head >= 64 && Head * 2 >= Items.Count())
        {
            Items.RemoveAt(0, Head);
            Head = 0;
        }
    }

    void Clear()
    {
        Items.Clear();
        Head = 0;
    }
};

struct IoService
{
    Mutex Lock;
    ReadQueue Queued[PriorityCount];
    Event Wake{EventReset::Auto};
    ThreadHandle Thread;
    bool Running = false;
    Atomic<bool> Stopping{false};

    /// Built on first use and never destroyed.
    static IoService &Get()
    {
        alignas(IoService) static uint8 storage[sizeof(IoService)];
        static IoService *service = ConstructAt(reinterpret_cast<IoService *>(storage));
        return *service;
    }
};

void Finish(AsyncReadState &state, AsyncReadStatus status)
{
    state.Region = {};
    state.Status.Store(static_cast<uint8>(status), MemoryOrder::Release);
    if (state.Completion.IsBound())
    {
        state.Completion(AsyncReadRequest(SharedPtr<AsyncReadState>(&state)));
    }
    state.Done.Set();
}

/// Writes the part of block *index* that *state* wants into its Buffer; false if the block cannot be read.
bool DecompressBlock(AsyncReadState &state, uint64 index)
{
    const IRegionCompression &compression = *state.Region.Compression;
    const uint64 blockSize = compression.GetBlockSize();
    const uint64 start = state.Region.Offset + state.Offset;
    const uint64 end = start + state.Size;
    uint64 offset = 0;
    uint32 stored = 0;
    uint32 logical = 0;
    if (!compression.GetBlock(index, offset, stored, logical))
    {
        return false;
    }
    const uint64 blockStart = index * blockSize;
    const uint64 from = start > blockStart ? start : blockStart;
    const uint64 to = end < blockStart + logical ? end : blockStart + logical;
    uint8 *target = state.Buffer + (from - start);
    const CachedRegionBlock *cached = index == state.FirstBlock  ? state.FirstCached.Get()
                                      : index == state.LastBlock ? state.LastCached.Get()
                                                                 : nullptr;
    if (cached != nullptr)
    {
        if (cached->Bytes.Count() != logical)
        {
            return false;
        }
        MemCopy(target, cached->Bytes.Data() + (from - blockStart), static_cast<usize>(to - from));
        return true;
    }
    if (offset < state.ReadOffset || offset + stored > state.ReadOffset + state.ReadSize)
    {
        return false;
    }
    const Span<uint8> bytes(state.ReadBuffer + (offset - state.ReadOffset), stored);
    const bool whole = from == blockStart && to == blockStart + logical;
    if (whole && stored == logical && compression.IsPlain(index))
    {
        MemCopy(target, bytes.Data(), logical);
        return true;
    }
    if (whole)
    {
        return compression.DecompressInPlace(index, bytes, Span<uint8>(target, logical));
    }
    // Only part of it is wanted, as for small objects that share a block: keep it for the reads of the others.
    const SharedPtr<CachedRegionBlock> block(new CachedRegionBlock());
    block->Bytes.ResizeUninitialized(logical);
    if (stored == logical && compression.IsPlain(index))
    {
        MemCopy(block->Bytes.Data(), bytes.Data(), logical);
    }
    else if (!compression.DecompressInPlace(index, bytes, block->Bytes.AsSpan()))
    {
        return false;
    }
    MemCopy(target, block->Bytes.Data() + (from - blockStart), static_cast<usize>(to - from));
    compression.CacheBlock(index, SharedPtr<const CachedRegionBlock>(block.Get()));
    return true;
}

void Decompress(void *argument)
{
    auto &state = *static_cast<AsyncReadState *>(argument);
    const uint64 count = state.LastBlock - state.FirstBlock + 1;
    bool ok = true;
    if (count < ParallelBlocks)
    {
        for (uint64 index = state.FirstBlock; ok && index <= state.LastBlock; ++index)
        {
            ok = DecompressBlock(state, index);
        }
    }
    else
    {
        // Blocks are independent, so a large read, such as a whole mip chain, decompresses on every worker.
        Atomic<bool> failed{false};
        Jobs::ParallelFor(static_cast<uint32>(count), 1, [&](uint32 begin, uint32 end) {
            for (uint32 i = begin; i < end && !failed.Load(MemoryOrder::Relaxed); ++i)
            {
                if (!DecompressBlock(state, state.FirstBlock + i))
                {
                    failed.Store(true, MemoryOrder::Relaxed);
                }
            }
        });
        ok = !failed.Load();
    }
    state.FirstCached.Reset();
    state.LastCached.Reset();
    Memory::Free(state.ReadBuffer);
    state.ReadBuffer = nullptr;
    Finish(state, ok ? AsyncReadStatus::Succeeded : AsyncReadStatus::Failed);
    if (state.ReleaseReference())
    {
        delete &state;
    }
}

/// Ends a read whose bytes have all arrived; a compressed region goes to a job to be decompressed first.
void Complete(AsyncReadState &state)
{
    if (state.AnyFailed || !state.Region.Compression.IsValid())
    {
        Finish(state, state.AnyFailed ? AsyncReadStatus::Failed : AsyncReadStatus::Succeeded);
        return;
    }
    state.AddReference();
    Jobs::Run(JobDecl{.Entry = &Decompress, .Argument = &state, .Priority = JobPriority::High});
}

/// Hands the parts of *state* not yet issued to *queue*; false when the queue is full.
bool Issue(AsyncIoQueue &queue, AsyncReadState &state, uint32 &inFlight)
{
    while (state.Issued < state.ReadSize)
    {
        if (inFlight == QueueCapacity)
        {
            return false;
        }
        const uint64 part = state.ReadSize - state.Issued < PartSize ? state.ReadSize - state.Issued : PartSize;
        AsyncIoRequest request;
        request.File = state.Region.File->File;
        request.Offset = state.ReadOffset + state.Issued;
        request.Buffer = state.ReadBuffer + state.Issued;
        request.Size = static_cast<uint32>(part);
        request.UserData = &state;
        if (!queue.Read(request))
        {
            return false;
        }
        state.Issued += part;
        ++state.PartsInFlight;
        ++inFlight;
    }
    return true;
}

void IoThread(void * /*argument*/)
{
    IoService &service = IoService::Get();
    AsyncIoQueue queue;
    if (!queue.Initialize(QueueCapacity))
    {
        return;
    }
    List<SharedPtr<AsyncReadState>> running;
    uint32 inFlight = 0;
    AsyncIoCompletion completions[QueueCapacity];
    while (true)
    {
        bool idle = true;
        {
            const ScopeLock lock(service.Lock);
            for (ReadQueue &queued : service.Queued)
            {
                while (!queued.IsEmpty() && inFlight < QueueCapacity)
                {
                    const SharedPtr<AsyncReadState> state = queued.Front();
                    if (state->Status.Load() == static_cast<uint8>(AsyncReadStatus::Cancelled))
                    {
                        queued.PopFront();
                        continue;
                    }
                    state->Status.Store(static_cast<uint8>(AsyncReadStatus::Running));
                    const bool complete = Issue(queue, *state, inFlight);
                    if (!running.Contains(state))
                    {
                        running.Add(state);
                    }
                    if (!complete)
                    {
                        break;
                    }
                    queued.PopFront();
                }
                idle = idle && queued.IsEmpty();
            }
        }
        queue.Submit();
        if (inFlight == 0)
        {
            if (idle && service.Stopping.Load())
            {
                break;
            }
            service.Wake.Wait(idle ? 100 : 1);
            continue;
        }
        const uint32 count = queue.Wait(completions, QueueCapacity, CompletionWait);
        for (uint32 i = 0; i < count; ++i)
        {
            auto &state = *static_cast<AsyncReadState *>(completions[i].UserData);
            --inFlight;
            --state.PartsInFlight;
            state.AnyFailed = state.AnyFailed || !completions[i].Succeeded;
            if (state.PartsInFlight == 0 && state.Issued == state.ReadSize)
            {
                Complete(state);
                running.RemoveAll([&](const SharedPtr<AsyncReadState> &entry) { return entry.Get() == &state; });
            }
        }
    }
    queue.Shutdown();
}

void EnsureRunning(IoService &service)
{
    if (service.Running)
    {
        return;
    }
    service.Stopping.Store(false);
    ThreadDesc desc;
    desc.Name = "Marlin I/O";
    desc.Entry = &IoThread;
    desc.Priority = ThreadPriority::AboveNormal;
    service.Thread = PlatformThread::Create(desc);
    service.Running = true;
}
} // namespace

AsyncReadRequest::AsyncReadRequest() = default;

AsyncReadRequest::AsyncReadRequest(SharedPtr<Private::AsyncReadState> state) : _state(Move(state))
{
}

AsyncReadRequest::~AsyncReadRequest() = default;
AsyncReadRequest::AsyncReadRequest(const AsyncReadRequest &other) = default;
AsyncReadRequest &AsyncReadRequest::operator=(const AsyncReadRequest &other) = default;
AsyncReadRequest::AsyncReadRequest(AsyncReadRequest &&other) noexcept = default;
AsyncReadRequest &AsyncReadRequest::operator=(AsyncReadRequest &&other) noexcept = default;

bool AsyncReadRequest::IsValid() const
{
    return _state.IsValid();
}

bool AsyncReadRequest::IsDone() const
{
    const auto status = static_cast<AsyncReadStatus>(_state->Status.Load(MemoryOrder::Acquire));
    return status != AsyncReadStatus::Queued && status != AsyncReadStatus::Running;
}

bool AsyncReadRequest::Succeeded() const
{
    return _state->Status.Load(MemoryOrder::Acquire) == static_cast<uint8>(AsyncReadStatus::Succeeded);
}

Span<const uint8> AsyncReadRequest::GetData() const
{
    return Succeeded() ? Span<const uint8>(_state->Buffer, static_cast<usize>(_state->Size)) : Span<const uint8>();
}

uint8 *AsyncReadRequest::TakeData(usize &size) const
{
    size = 0;
    if (!Succeeded())
    {
        return nullptr;
    }
    uint8 *data = _state->Buffer;
    size = static_cast<usize>(_state->Size);
    if (_state->ReadBuffer == data)
    {
        _state->ReadBuffer = nullptr;
    }
    _state->Buffer = nullptr;
    _state->Size = 0;
    return data;
}

bool AsyncReadRequest::Wait(uint32 timeout) const
{
    return _state->Done.Wait(timeout);
}

void AsyncReadRequest::Cancel() const
{
    auto expected = static_cast<uint8>(AsyncReadStatus::Queued);
    if (_state->Status.CompareExchange(expected, static_cast<uint8>(AsyncReadStatus::Cancelled)))
    {
        Finish(*_state, AsyncReadStatus::Cancelled);
    }
}

namespace Vfs
{
AsyncReadRequest ReadAsync(StringView path, IoPriority priority, uint64 offset, uint64 size,
                           AsyncReadCallback completion)
{
    FileRegion region;
    if (!OpenRegion(path, region))
    {
        const SharedPtr<AsyncReadState> state(new AsyncReadState());
        state->Completion = Move(completion);
        Finish(*state, AsyncReadStatus::Failed);
        return AsyncReadRequest(state);
    }
    return ReadAsync(region, priority, offset, size, Move(completion));
}

AsyncReadRequest ReadAsync(const FileRegion &region, IoPriority priority, uint64 offset, uint64 size,
                           AsyncReadCallback completion)
{
    const SharedPtr<AsyncReadState> state(new AsyncReadState());
    state->Priority = priority;
    state->Completion = Move(completion);
    state->Region = region;
    AsyncReadRequest request(state);
    if (!state->Region.IsValid() || offset > state->Region.Size)
    {
        Finish(*state, AsyncReadStatus::Failed);
        return request;
    }
    state->Offset = offset;
    state->Size = size == WholeFile || offset + size > state->Region.Size ? state->Region.Size - offset : size;
    state->Buffer = static_cast<uint8 *>(Memory::Allocate(state->Size != 0 ? state->Size : 1));
    if (state->Size == 0)
    {
        Finish(*state, AsyncReadStatus::Succeeded);
        return request;
    }
    const IRegionCompression *compression = state->Region.Compression.Get();
    if (compression == nullptr)
    {
        state->ReadOffset = state->Region.Offset + offset;
        state->ReadSize = state->Size;
        state->ReadBuffer = state->Buffer;
    }
    else
    {
        // The stored blocks that cover the logical range lie one after another, so one read fetches them all, but
        // for a first or last block that is wanted only in part and that an earlier read kept.
        const uint64 blockSize = compression->GetBlockSize();
        const uint64 start = state->Region.Offset + offset;
        const uint64 end = start + state->Size;
        uint64 firstOffset = 0;
        uint64 lastOffset = 0;
        uint32 storedSize = 0;
        uint32 logicalSize = 0;
        state->FirstBlock = blockSize != 0 ? start / blockSize : 0;
        state->LastBlock = blockSize != 0 ? (end - 1) / blockSize : 0;
        if (blockSize == 0)
        {
            Finish(*state, AsyncReadStatus::Failed);
            return request;
        }
        const auto partial = [&](uint64 index) { return start > index * blockSize || end < (index + 1) * blockSize; };
        if (partial(state->FirstBlock))
        {
            state->FirstCached = compression->FindCachedBlock(state->FirstBlock);
        }
        if (state->LastBlock != state->FirstBlock && partial(state->LastBlock))
        {
            state->LastCached = compression->FindCachedBlock(state->LastBlock);
        }
        const uint64 readFirst = state->FirstBlock + (state->FirstCached.IsValid() ? 1 : 0);
        const uint64 readLast = state->LastBlock - (state->LastCached.IsValid() ? 1 : 0);
        if (readFirst > readLast)
        {
            // Every block it needs is kept: nothing to read, only to copy.
            state->Status.Store(static_cast<uint8>(AsyncReadStatus::Running));
            state->AddReference();
            Jobs::Run(JobDecl{.Entry = &Decompress, .Argument = state.Get(), .Priority = JobPriority::High});
            return request;
        }
        if (!compression->GetBlock(readFirst, firstOffset, storedSize, logicalSize) ||
            !compression->GetBlock(readLast, lastOffset, storedSize, logicalSize) || lastOffset < firstOffset)
        {
            Finish(*state, AsyncReadStatus::Failed);
            return request;
        }
        state->ReadOffset = firstOffset;
        state->ReadSize = lastOffset + storedSize - firstOffset;
        state->ReadBuffer = static_cast<uint8 *>(Memory::Allocate(state->ReadSize != 0 ? state->ReadSize : 1));
    }
    if (const IRegionSource *source = state->Region.Source.Get())
    {
        // Not a file: the source reads, and its completion ends the read as the I/O thread's would.
        state->Status.Store(static_cast<uint8>(AsyncReadStatus::Running));
        state->AddReference();
        AsyncReadState *reading = state.Get();
        source->Read(state->ReadOffset, state->ReadSize, state->ReadBuffer, [reading](bool succeeded) {
            reading->AnyFailed = !succeeded;
            Complete(*reading);
            if (reading->ReleaseReference())
            {
                delete reading;
            }
        });
        return request;
    }
    IoService &service = IoService::Get();
    {
        const ScopeLock lock(service.Lock);
        EnsureRunning(service);
        service.Queued[static_cast<uint32>(priority)].Items.Add(state);
    }
    service.Wake.Set();
    return request;
}

void ShutdownAsync()
{
    IoService &service = IoService::Get();
    ThreadHandle thread;
    {
        const ScopeLock lock(service.Lock);
        if (!service.Running)
        {
            return;
        }
        for (ReadQueue &queued : service.Queued)
        {
            for (usize i = queued.Head; i < queued.Items.Count(); ++i)
            {
                AsyncReadRequest(queued.Items[i]).Cancel();
            }
            queued.Clear();
        }
        service.Stopping.Store(true);
        thread = service.Thread;
        service.Running = false;
    }
    service.Wake.Set();
    PlatformThread::Join(thread);
}
} // namespace Vfs
} // namespace Marlin
