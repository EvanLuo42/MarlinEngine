#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiBarriers.h"
#include "RHI/RhiDevice.h"

namespace Marlin
{
/// Done once the upload queue's fence reaches Value; null tickets are done.
struct RhiUploadTicket
{
    uint64 Value = 0;

    [[nodiscard]] bool IsValid() const
    {
        return Value != 0;
    }
};

/// Where an uploaded buffer range goes after the copy.
struct RhiBufferUpload
{
    RhiBuffer Buffer;
    uint64 Offset = 0;
    Span<const uint8> Data;
    /// The queue that reads it next, which records GetAcquireBarrier before it does.
    RhiQueueType Destination = RhiQueueType::Graphics;
};

/// An upload into one subresource region; the data is laid out as in CopyBufferToTexture.
struct RhiTextureUpload
{
    RhiTexture Texture;
    RhiTextureRegion Region;
    Span<const uint8> Data;
    uint32 RowPitch = 0;
    uint32 RowsPerSlice = 0;
    /// The texture's contents before the upload are thrown away, as for a new texture; otherwise it is in CopyDest.
    bool Discard = true;
    /// The layout it is left in and the queue that uses it next, which records GetAcquireBarrier first.
    RhiLayout FinalLayout = RhiLayout::ShaderResource;
    RhiQueueType Destination = RhiQueueType::Graphics;
};

/// Streams data into GPU resources on the Copy queue: uploads from any thread go through a staging ring and are
/// recorded into the current batch, which Flush submits on the submit thread. A ticket says when an upload is done;
/// the queue that uses the resource waits for it (GetWait) and records the matching acquire barrier. An upload copies
/// its data outside the lock; when the ring is full it waits for the oldest submitted batch.
class RHI_API RhiUploadQueue
{
public:
    explicit RhiUploadQueue(RhiDevice &device, uint64 stagingSize = uint64{64} * 1024 * 1024);
    ~RhiUploadQueue();
    MARLIN_NONCOPYABLE(RhiUploadQueue);

    /// Null if the device could not take the upload, such as when out of memory.
    RhiUploadTicket Upload(const RhiBufferUpload &upload);
    RhiUploadTicket Upload(const RhiTextureUpload &upload);

    /// Submits the batch recorded since the last Flush, on the submit thread; does nothing if it is empty.
    Result<void, RhiError> Flush();

    [[nodiscard]] bool IsComplete(RhiUploadTicket ticket) const;
    /// What a submission that uses the uploaded resource waits for.
    [[nodiscard]] RhiFenceWait GetWait(RhiUploadTicket ticket) const;
    /// The barrier the destination queue records before it uses the resource.
    [[nodiscard]] static RhiBufferBarrier GetAcquireBarrier(const RhiBufferUpload &upload);
    [[nodiscard]] static RhiTextureBarrier GetAcquireBarrier(const RhiTextureUpload &upload);

    /// Frees the staging space and the buffers of finished batches; the submit thread calls it every frame, and an
    /// upload that finds the ring full calls it after waiting.
    void ProcessCompletions();

private:
    struct Batch
    {
        uint64 Value = 0;
        /// The staging ring's head once this batch is done.
        uint64 StagingEnd = 0;
        List<RhiBuffer> Oversized;
    };

    /// A copy waiting to be recorded with the batch's other copies, between one barrier before and one after.
    struct PendingCopy
    {
        RhiBuffer Staging;
        uint64 StagingOffset = 0;
        uint64 Size = 0;
        RhiBuffer Buffer;
        uint64 Offset = 0;
        RhiTexture Texture;
        RhiTextureRegion Region;
        uint32 RowPitch = 0;
        uint32 RowsPerSlice = 0;
    };

    /// Space for an upload: in the ring, or in *oversized*, a buffer of its own made by the caller. Under the lock;
    /// false when the ring is full.
    bool AllocateStaging(uint64 size, uint64 alignment, RhiBuffer oversized, RhiBuffer &buffer, uint64 &offset,
                         uint8 *&data);
    /// Makes the buffer for an upload larger than half the ring, outside the lock; invalid if it is not larger.
    RhiBuffer CreateOversized(uint64 size);
    /// Notes that the pending copies touch *resource*, recording them first if a pending copy already does, so two
    /// uploads of one resource keep their own barriers. Under the lock.
    void TouchResource(uint64 resource);
    /// Records the pending copies and their barriers into the list; under the lock.
    void RecordPending();
    /// Waits for the oldest submitted batch and frees what it held; false if no batch is in flight.
    bool WaitForStaging();
    RhiCommandList *GetList();

    RhiDevice &_device;
    RhiFence _fence;
    RhiBuffer _staging;
    uint8 *_stagingData = nullptr;
    uint64 _stagingSize;
    /// Ring positions grow without wrapping; the offset is the position modulo the size.
    uint64 _stagingHead = 0;
    uint64 _stagingTail = 0;
    mutable SpinLock _lock;
    RhiCommandList *_list = nullptr;
    Batch _current;
    uint64 _nextValue = 1;
    List<Batch> _inFlight;
    List<RhiTextureBarrier> _toCopy;
    List<PendingCopy> _copies;
    List<RhiBufferBarrier> _bufferReleases;
    List<RhiTextureBarrier> _textureReleases;
    /// The buffers and texture subresources the pending copies touch, hashed; a collision only splits a batch.
    HashSet<uint64> _touched;
    /// Uploads copying into staging space outside the lock; Flush submits once none are.
    Atomic<uint32> _copying{0};
};
} // namespace Marlin
