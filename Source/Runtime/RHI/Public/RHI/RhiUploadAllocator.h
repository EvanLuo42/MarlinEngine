#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiResources.h"

namespace Marlin
{
class RhiDevice;

/// A piece of upload memory: write it through Data, read it on the GPU at Address or Buffer and Offset.
struct RhiUploadAllocation
{
    uint8 *Data = nullptr;
    RhiBuffer Buffer;
    uint64 Offset = 0;
    uint64 Address = 0;
    uint64 Size = 0;
};

/// Linear upload memory for a frame's constants and small uploads, carved from persistently mapped chunks in
/// Upload memory (GPU memory with ReBAR or UMA). Thread-safe: small allocations come from a block of the chunk
/// that the job worker's shard owns, so threads rarely share a lock. A chunk comes back once the queues have finished
/// the work submitted before the EndFrame that retired it.
class RHI_API RhiUploadAllocator
{
public:
    explicit RhiUploadAllocator(RhiDevice &device, uint64 chunkSize = uint64{4} * 1024 * 1024);
    ~RhiUploadAllocator();
    MARLIN_NONCOPYABLE(RhiUploadAllocator);

    /// Valid until the work of this frame is done; larger than a chunk gets a buffer of its own. Data is null if
    /// the device is out of memory.
    [[nodiscard]] RhiUploadAllocation Allocate(uint64 size, uint64 alignment = 256);

    /// After the frame's last submission: retires the memory handed out since the last EndFrame.
    void EndFrame();

private:
    struct Chunk
    {
        RhiBuffer Buffer;
        uint8 *Data = nullptr;
        uint64 Address = 0;
        uint64 Size = 0;
        RhiQueueValues RetiredAt;
        /// Larger than a chunk; destroyed instead of reused.
        bool Oversized = false;
    };

    /// A block of the current chunk that the workers of one shard allocate from.
    struct Shard
    {
        SpinLock Lock;
        RhiBuffer Buffer;
        uint8 *ChunkData = nullptr;
        uint64 ChunkAddress = 0;
        /// Offsets in the chunk: the next free byte and the end of the block.
        uint64 Cursor = 0;
        uint64 End = 0;
    };
    static constexpr uint32 ShardCount = 8;

    bool NewChunk(uint64 size);
    /// From the current chunk, or a buffer of its own when larger than a chunk; under _lock.
    RhiUploadAllocation AllocateShared(uint64 size, uint64 alignment);

    RhiDevice &_device;
    uint64 _chunkSize;
    uint64 _blockSize;
    Shard _shards[ShardCount];
    SpinLock _lock;
    Chunk _current;
    uint64 _currentOffset = 0;
    List<Chunk> _used;
    List<Chunk> _retired;
};
} // namespace Marlin
