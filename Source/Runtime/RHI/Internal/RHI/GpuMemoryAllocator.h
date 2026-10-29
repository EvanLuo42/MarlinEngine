#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "RHI/RhiResources.h"
#include "Templates/Result.h"

namespace Marlin
{
/// Two-level segregated fit over a range of offsets: constant-time allocation and free with immediate coalescing,
/// for memory the CPU does not touch, so the bookkeeping lives outside it. Not thread-safe.
class RHI_API GpuTlsf
{
public:
    static constexpr uint32 InvalidNode = ~0u;

    struct Allocation
    {
        uint64 Offset = 0;
        uint64 Size = 0;
        uint32 Node = InvalidNode;
    };

    /// Manages [0, *size*); every piece is a multiple of *granularity*, a power of two.
    GpuTlsf(uint64 size, uint64 granularity);

    /// Null node if no free range fits.
    [[nodiscard]] Allocation Allocate(uint64 size, uint64 alignment);
    void Free(uint32 node);

    [[nodiscard]] uint64 GetSize() const
    {
        return _size;
    }

    [[nodiscard]] uint64 GetUsedBytes() const
    {
        return _used;
    }

    [[nodiscard]] bool IsEmpty() const
    {
        return _used == 0;
    }

    /// The largest piece Allocate could return now.
    [[nodiscard]] uint64 GetLargestFreeRange() const;

private:
    static constexpr uint32 SecondLevelLog2 = 5;
    static constexpr uint32 SecondLevelCount = 1u << SecondLevelLog2;
    static constexpr uint32 FirstLevelCount = 64 - SecondLevelLog2 + 1;

    struct Node
    {
        uint64 Offset = 0;
        uint64 Size = 0;
        uint32 PreviousPhysical = InvalidNode;
        uint32 NextPhysical = InvalidNode;
        uint32 PreviousFree = InvalidNode;
        uint32 NextFree = InvalidNode;
        bool Free = false;
    };

    static void Map(uint64 units, uint32 &firstLevel, uint32 &secondLevel);
    uint32 NewNode();
    void ReleaseNode(uint32 node);
    void InsertFree(uint32 node);
    void RemoveFree(uint32 node);
    uint32 FindFree(uint64 units) const;
    uint32 Split(uint32 node, uint64 size);

    List<Node> _nodes;
    uint32 _unusedNodes = InvalidNode;
    uint32 _freeHeads[FirstLevelCount][SecondLevelCount];
    uint32 _secondLevelBitmaps[FirstLevelCount] = {};
    uint64 _firstLevelBitmap = 0;
    uint64 _size;
    uint64 _granularity;
    uint32 _granularityShift;
    uint64 _used = 0;
};

/// A block of memory from the driver, which pieces of resources are placed in.
struct GpuMemoryBlock
{
    /// The backend's memory object, such as a VkDeviceMemory.
    uint64 Native = 0;
    uint64 Size = 0;
    uint32 Pool = 0;
    /// The CPU address of mapped memory, or null.
    uint8 *Mapped = nullptr;
    /// Null for a dedicated block, which holds one resource.
    GpuTlsf *Tlsf = nullptr;
};

struct GpuAllocation
{
    GpuMemoryBlock *Block = nullptr;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint32 Node = GpuTlsf::InvalidNode;

    [[nodiscard]] bool IsValid() const
    {
        return Block != nullptr;
    }

    [[nodiscard]] uint8 *GetMappedData() const
    {
        return Block->Mapped != nullptr ? Block->Mapped + Offset : nullptr;
    }
};

/// Where the backend gets driver memory from.
class IGpuMemorySource
{
public:
    IGpuMemorySource() = default;
    virtual ~IGpuMemorySource() = default;
    MARLIN_NONCOPYABLE(IGpuMemorySource);

    /// *dedicatedResource* is the backend's resource for a dedicated block, or null.
    virtual Result<GpuMemoryBlock, RhiError> AllocateBlock(uint32 pool, uint64 size, const void *dedicatedResource) = 0;
    virtual void FreeBlock(const GpuMemoryBlock &block) = 0;
};

struct GpuMemoryPoolDesc
{
    uint64 BlockSize = 0;
    RhiMemoryLocation Location = RhiMemoryLocation::Device;
};

struct GpuMemoryPoolStats
{
    uint32 BlockCount = 0;
    uint32 AllocationCount = 0;
    uint64 BlockBytes = 0;
    uint64 UsedBytes = 0;
};

struct GpuMemoryRequest
{
    uint32 Pool = 0;
    uint64 Size = 0;
    uint64 Alignment = 1;
    /// Asks for memory of its own, as drivers prefer for render targets of some sizes.
    bool PreferDedicated = false;
    const void *DedicatedResource = nullptr;
};

/// The engine's GPU memory allocator, shared by every backend: a set of pools the backend defines, such as one per
/// memory type and resource tiling, each suballocating blocks with TLSF. Sizes and policies come from
/// PlatformGpuMemoryConfig. Thread-safe.
class RHI_API GpuMemoryAllocator
{
public:
    GpuMemoryAllocator(IGpuMemorySource &source, Span<const GpuMemoryPoolDesc> pools);
    ~GpuMemoryAllocator();
    MARLIN_NONCOPYABLE(GpuMemoryAllocator);

    Result<GpuAllocation, RhiError> Allocate(const GpuMemoryRequest &request);
    void Free(const GpuAllocation &allocation);

    [[nodiscard]] GpuMemoryPoolStats GetStats(uint32 pool) const;
    [[nodiscard]] const GpuMemoryPoolDesc &GetPoolDesc(uint32 pool) const
    {
        return _pools[pool]->Desc;
    }
    [[nodiscard]] uint32 GetPoolCount() const
    {
        return static_cast<uint32>(_pools.Count());
    }

private:
    struct Pool
    {
        GpuMemoryPoolDesc Desc;
        List<GpuMemoryBlock *> Blocks;
        mutable SpinLock Lock;
        /// Held while a block is made, so threads that find the pool full make one between them.
        Mutex CreateLock;
        uint32 AllocationCount = 0;
        uint32 DedicatedCount = 0;
        uint64 DedicatedBytes = 0;
    };

    void FreeBlock(Pool &pool, GpuMemoryBlock *block);
    /// A piece of one of the pool's blocks; under its lock.
    static bool AllocateFromBlocks(Pool &pool, const GpuMemoryRequest &request, GpuAllocation &allocation);

    IGpuMemorySource &_source;
    List<Pool *> _pools;
};
} // namespace Marlin
