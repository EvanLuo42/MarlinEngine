#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/SharedPtr.h"
#include "Templates/Function.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
class AssetSlot;

/// Bytes a bulk request read; freed when the last reference goes.
class ASSET_API BulkBytes final : public RefCounted
{
public:
    BulkBytes(uint8 *data, usize size) : _data(data), _size(size)
    {
    }
    ~BulkBytes();
    MARLIN_NONCOPYABLE(BulkBytes);

    [[nodiscard]] Span<const uint8> GetBytes() const
    {
        return {_data, _size};
    }

private:
    uint8 *_data;
    usize _size;
};

using BulkRequestId = uint64;
/// Gets the bytes of a request, or null if the read failed.
using BulkCallback = Function<void(BulkRequestId request, SharedPtr<const BulkBytes> bytes)>;

/// How much a BulkStreamer reads at once.
struct BulkStreamerDesc
{
    uint64 MaxInFlightBytes = 64 * 1024 * 1024; // RECOVERY-DECISION D1: default budget chosen in recovery.
    uint32 MaxInFlightRequests = 16;            // RECOVERY-DECISION D1: default budget chosen in recovery.
};

/// Streams parts of assets' bulk data (Documentation/Architecture/06 §4.1), such as texture mips and mesh pages:
/// requests wait in a queue and start in order of priority, highest first, as long as the reads in flight stay within
/// the budget; one request always runs when nothing else does, so a request larger than the budget still makes
/// progress. Reads finish on the I/O thread; ProcessFrame hands their bytes to the callbacks on the main thread. A
/// renderer raises or lowers a request's priority as the camera moves and cancels the ones it no longer needs.
/// Main thread only.
class ASSET_API BulkStreamer
{
public:
    explicit BulkStreamer(const BulkStreamerDesc &desc = {});
    ~BulkStreamer();
    MARLIN_NONCOPYABLE(BulkStreamer);

    /// Queues a read of *size* bytes at *offset* in the bulk data of loaded asset *slot*; *io* orders the read among
    /// other reads once it starts.
    BulkRequestId Request(const AssetSlot &slot, uint64 offset, uint64 size, float32 priority, BulkCallback callback,
                          IoPriority io = IoPriority::Streaming);
    /// The same within any region, such as one a store located.
    BulkRequestId Request(const FileRegion &bulk, uint64 offset, uint64 size, float32 priority, BulkCallback callback,
                          IoPriority io = IoPriority::Streaming);
    void SetPriority(BulkRequestId request, float32 priority);
    /// Drops a request; its callback does not run. A read in flight still finishes, and its bytes are discarded.
    void Cancel(BulkRequestId request);

    /// Runs the callbacks of finished reads and starts queued requests within the budget.
    void ProcessFrame();
    /// Runs ProcessFrame until every request, queued and in flight, has finished, such as before shutdown.
    void Flush();

    [[nodiscard]] usize GetQueuedCount() const
    {
        return _queue.Count();
    }
    [[nodiscard]] usize GetInFlightCount() const
    {
        return _inFlight.Count();
    }
    [[nodiscard]] uint64 GetInFlightBytes() const
    {
        return _inFlightBytes;
    }

private:
    struct Queued
    {
        BulkRequestId Id = 0;
        FileRegion Region;
        uint64 Offset = 0;
        uint64 Size = 0;
        float32 Priority = 0;
        IoPriority Io = IoPriority::Streaming;
        BulkCallback Callback;
    };
    struct InFlight
    {
        uint64 Size = 0;
        bool Cancelled = false;
        AsyncReadRequest Read;
        BulkCallback Callback;
    };
    struct Finished
    {
        BulkRequestId Id = 0;
        AsyncReadRequest Read;
    };

    /// True if *a* starts before *b*: higher priority first, then the older request.
    static bool Before(const Queued &a, const Queued &b)
    {
        return a.Priority > b.Priority || (a.Priority == b.Priority && a.Id < b.Id);
    }
    void Place(usize index);
    void SiftUp(usize index);
    void SiftDown(usize index);
    void Swap(usize a, usize b);
    void RemoveQueued(usize index);

    BulkStreamerDesc _desc;
    BulkRequestId _nextId = 1;
    /// A binary heap, the next request to start at the front; _queueIndex finds a request in it, so changing a
    /// priority or cancelling costs a logarithm of the queue, not a scan.
    List<Queued> _queue;
    HashMap<BulkRequestId, usize> _queueIndex;
    HashMap<BulkRequestId, InFlight> _inFlight;
    uint64 _inFlightBytes = 0;
    /// Where reads land on the I/O thread; shared with their completions, which may outlive the streamer.
    struct Shared final : RefCounted
    {
        SpinLock Lock;
        List<Finished> Reads;
    };
    SharedPtr<Shared> _shared;
};
} // namespace Marlin
