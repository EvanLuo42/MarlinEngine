#include "RHI/GpuMemoryAllocator.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"
#include "RHI/PlatformGpuMemoryConfig.h"

namespace Marlin
{
GpuTlsf::GpuTlsf(uint64 size, uint64 granularity)
    : _size(size & ~(granularity - 1)), _granularity(granularity),
      _granularityShift(static_cast<uint32>(__builtin_ctzll(granularity)))
{
    MARLIN_ASSERT(granularity != 0 && (granularity & (granularity - 1)) == 0);
    for (auto &heads : _freeHeads)
    {
        for (uint32 &head : heads)
        {
            head = InvalidNode;
        }
    }
    const uint32 node = NewNode();
    _nodes[node].Size = _size;
    InsertFree(node);
}

void GpuTlsf::Map(uint64 units, uint32 &firstLevel, uint32 &secondLevel)
{
    if (units < SecondLevelCount)
    {
        firstLevel = 0;
        secondLevel = static_cast<uint32>(units);
        return;
    }
    const uint32 log = 63 - static_cast<uint32>(__builtin_clzll(units));
    firstLevel = log - SecondLevelLog2 + 1;
    secondLevel = static_cast<uint32>(units >> (log - SecondLevelLog2)) ^ SecondLevelCount;
}

uint32 GpuTlsf::NewNode()
{
    if (_unusedNodes != InvalidNode)
    {
        const uint32 node = _unusedNodes;
        _unusedNodes = _nodes[node].NextFree;
        _nodes[node] = Node{};
        return node;
    }
    _nodes.AddDefaulted();
    return static_cast<uint32>(_nodes.Count() - 1);
}

void GpuTlsf::ReleaseNode(uint32 node)
{
    _nodes[node].NextFree = _unusedNodes;
    _unusedNodes = node;
}

void GpuTlsf::InsertFree(uint32 node)
{
    Node &entry = _nodes[node];
    uint32 firstLevel = 0;
    uint32 secondLevel = 0;
    Map(entry.Size >> _granularityShift, firstLevel, secondLevel);
    entry.Free = true;
    entry.PreviousFree = InvalidNode;
    entry.NextFree = _freeHeads[firstLevel][secondLevel];
    if (entry.NextFree != InvalidNode)
    {
        _nodes[entry.NextFree].PreviousFree = node;
    }
    _freeHeads[firstLevel][secondLevel] = node;
    _secondLevelBitmaps[firstLevel] |= 1u << secondLevel;
    _firstLevelBitmap |= uint64{1} << firstLevel;
}

void GpuTlsf::RemoveFree(uint32 node)
{
    Node &entry = _nodes[node];
    if (entry.PreviousFree != InvalidNode)
    {
        _nodes[entry.PreviousFree].NextFree = entry.NextFree;
    }
    else
    {
        uint32 firstLevel = 0;
        uint32 secondLevel = 0;
        Map(entry.Size >> _granularityShift, firstLevel, secondLevel);
        _freeHeads[firstLevel][secondLevel] = entry.NextFree;
        if (entry.NextFree == InvalidNode)
        {
            _secondLevelBitmaps[firstLevel] &= ~(1u << secondLevel);
            if (_secondLevelBitmaps[firstLevel] == 0)
            {
                _firstLevelBitmap &= ~(uint64{1} << firstLevel);
            }
        }
    }
    if (entry.NextFree != InvalidNode)
    {
        _nodes[entry.NextFree].PreviousFree = entry.PreviousFree;
    }
    entry.Free = false;
    entry.PreviousFree = InvalidNode;
    entry.NextFree = InvalidNode;
}

uint32 GpuTlsf::FindFree(uint64 units) const
{
    if (units >= SecondLevelCount)
    {
        const uint32 log = 63 - static_cast<uint32>(__builtin_clzll(units));
        units += (uint64{1} << (log - SecondLevelLog2)) - 1;
    }
    uint32 firstLevel = 0;
    uint32 secondLevel = 0;
    Map(units, firstLevel, secondLevel);
    if (firstLevel >= FirstLevelCount)
    {
        return InvalidNode;
    }
    uint32 secondMap = secondLevel < SecondLevelCount ? _secondLevelBitmaps[firstLevel] & (~0u << secondLevel) : 0;
    if (secondMap == 0)
    {
        const uint64 firstMap =
            firstLevel + 1 < FirstLevelCount ? _firstLevelBitmap & (~uint64{0} << (firstLevel + 1)) : 0;
        if (firstMap == 0)
        {
            return InvalidNode;
        }
        firstLevel = static_cast<uint32>(__builtin_ctzll(firstMap));
        secondMap = _secondLevelBitmaps[firstLevel];
    }
    return _freeHeads[firstLevel][static_cast<uint32>(__builtin_ctz(secondMap))];
}

uint32 GpuTlsf::Split(uint32 node, uint64 size)
{
    const uint32 rest = NewNode();
    Node &entry = _nodes[node];
    Node &tail = _nodes[rest];
    tail.Offset = entry.Offset + size;
    tail.Size = entry.Size - size;
    tail.PreviousPhysical = node;
    tail.NextPhysical = entry.NextPhysical;
    if (tail.NextPhysical != InvalidNode)
    {
        _nodes[tail.NextPhysical].PreviousPhysical = rest;
    }
    entry.Size = size;
    entry.NextPhysical = rest;
    return rest;
}

GpuTlsf::Allocation GpuTlsf::Allocate(uint64 size, uint64 alignment)
{
    if (size == 0)
    {
        size = 1;
    }
    size = (size + _granularity - 1) & ~(_granularity - 1);
    alignment = alignment > _granularity ? alignment : _granularity;
    const uint64 padded = size + alignment - _granularity;
    const uint32 node = FindFree(padded >> _granularityShift);
    if (node == InvalidNode)
    {
        return {};
    }
    RemoveFree(node);
    uint32 result = node;
    const uint64 aligned = (_nodes[node].Offset + alignment - 1) & ~(alignment - 1);
    if (aligned != _nodes[node].Offset)
    {
        result = Split(node, aligned - _nodes[node].Offset);
        InsertFree(node);
    }
    if (_nodes[result].Size > size)
    {
        InsertFree(Split(result, size));
    }
    _used += _nodes[result].Size;
    return {_nodes[result].Offset, _nodes[result].Size, result};
}

void GpuTlsf::Free(uint32 node)
{
    MARLIN_ASSERT(node < _nodes.Count() && !_nodes[node].Free);
    _used -= _nodes[node].Size;
    const uint32 next = _nodes[node].NextPhysical;
    if (next != InvalidNode && _nodes[next].Free)
    {
        RemoveFree(next);
        _nodes[node].Size += _nodes[next].Size;
        _nodes[node].NextPhysical = _nodes[next].NextPhysical;
        if (_nodes[node].NextPhysical != InvalidNode)
        {
            _nodes[_nodes[node].NextPhysical].PreviousPhysical = node;
        }
        ReleaseNode(next);
    }
    const uint32 previous = _nodes[node].PreviousPhysical;
    if (previous != InvalidNode && _nodes[previous].Free)
    {
        RemoveFree(previous);
        _nodes[previous].Size += _nodes[node].Size;
        _nodes[previous].NextPhysical = _nodes[node].NextPhysical;
        if (_nodes[previous].NextPhysical != InvalidNode)
        {
            _nodes[_nodes[previous].NextPhysical].PreviousPhysical = previous;
        }
        ReleaseNode(node);
        InsertFree(previous);
        return;
    }
    InsertFree(node);
}

uint64 GpuTlsf::GetLargestFreeRange() const
{
    if (_firstLevelBitmap == 0)
    {
        return 0;
    }
    const uint32 firstLevel = 63 - static_cast<uint32>(__builtin_clzll(_firstLevelBitmap));
    const uint32 secondLevel = 31 - static_cast<uint32>(__builtin_clz(_secondLevelBitmaps[firstLevel]));
    uint64 largest = 0;
    for (uint32 node = _freeHeads[firstLevel][secondLevel]; node != InvalidNode; node = _nodes[node].NextFree)
    {
        largest = _nodes[node].Size > largest ? _nodes[node].Size : largest;
    }
    return largest;
}

GpuMemoryAllocator::GpuMemoryAllocator(IGpuMemorySource &source, Span<const GpuMemoryPoolDesc> pools)
    : _source(source)
{
    for (const GpuMemoryPoolDesc &desc : pools)
    {
        Pool *pool = new Pool;
        pool->Desc = desc;
        _pools.Add(pool);
    }
}

GpuMemoryAllocator::~GpuMemoryAllocator()
{
    for (Pool *pool : _pools)
    {
        MARLIN_ASSERT(pool->AllocationCount == 0);
        for (GpuMemoryBlock *block : pool->Blocks)
        {
            _source.FreeBlock(*block);
            delete block->Tlsf;
            delete block;
        }
        delete pool;
    }
}

Result<GpuAllocation, RhiError> GpuMemoryAllocator::Allocate(const GpuMemoryRequest &request)
{
    MARLIN_ASSERT(request.Pool < _pools.Count());
    Pool &pool = *_pools[request.Pool];
    if (request.PreferDedicated || request.Size >= PlatformGpuMemoryConfig::DedicatedThreshold ||
        request.Size > pool.Desc.BlockSize)
    {
        Result<GpuMemoryBlock, RhiError> block = _source.AllocateBlock(request.Pool, request.Size,
                                                                       request.DedicatedResource);
        if (block.IsError())
        {
            return Failure(block.Error());
        }
        auto *dedicated = new GpuMemoryBlock(block.Value());
        const ScopeLock lock(pool.Lock);
        ++pool.AllocationCount;
        ++pool.DedicatedCount;
        pool.DedicatedBytes += dedicated->Size;
        return GpuAllocation{dedicated, 0, dedicated->Size, GpuTlsf::InvalidNode};
    }

    GpuAllocation allocation;
    {
        const ScopeLock lock(pool.Lock);
        if (AllocateFromBlocks(pool, request, allocation))
        {
            return allocation;
        }
    }

    const ScopeLock creating(pool.CreateLock);
    {
        // Another thread may have made a block while this one waited.
        const ScopeLock lock(pool.Lock);
        if (AllocateFromBlocks(pool, request, allocation))
        {
            return allocation;
        }
    }
    Result<GpuMemoryBlock, RhiError> created = _source.AllocateBlock(request.Pool, pool.Desc.BlockSize, nullptr);
    if (created.IsError())
    {
        return Failure(created.Error());
    }
    auto *block = new GpuMemoryBlock(created.Value());
    block->Tlsf = new GpuTlsf(block->Size, PlatformGpuMemoryConfig::Granularity);
    const GpuTlsf::Allocation piece = block->Tlsf->Allocate(request.Size, request.Alignment);
    MARLIN_ASSERT(piece.Node != GpuTlsf::InvalidNode);
    const ScopeLock lock(pool.Lock);
    pool.Blocks.Add(block);
    ++pool.AllocationCount;
    return GpuAllocation{block, piece.Offset, piece.Size, piece.Node};
}

bool GpuMemoryAllocator::AllocateFromBlocks(Pool &pool, const GpuMemoryRequest &request, GpuAllocation &allocation)
{
    for (GpuMemoryBlock *block : pool.Blocks)
    {
        const GpuTlsf::Allocation piece = block->Tlsf->Allocate(request.Size, request.Alignment);
        if (piece.Node != GpuTlsf::InvalidNode)
        {
            ++pool.AllocationCount;
            allocation = GpuAllocation{block, piece.Offset, piece.Size, piece.Node};
            return true;
        }
    }
    return false;
}

void GpuMemoryAllocator::Free(const GpuAllocation &allocation)
{
    if (!allocation.IsValid())
    {
        return;
    }
    Pool &pool = *_pools[allocation.Block->Pool];
    if (allocation.Block->Tlsf == nullptr)
    {
        {
            const ScopeLock lock(pool.Lock);
            --pool.AllocationCount;
            --pool.DedicatedCount;
            pool.DedicatedBytes -= allocation.Block->Size;
        }
        _source.FreeBlock(*allocation.Block);
        delete allocation.Block;
        return;
    }
    GpuMemoryBlock *release = nullptr;
    {
        const ScopeLock lock(pool.Lock);
        --pool.AllocationCount;
        allocation.Block->Tlsf->Free(allocation.Node);
        if (allocation.Block->Tlsf->IsEmpty())
        {
            uint32 emptyBlocks = 0;
            for (const GpuMemoryBlock *block : pool.Blocks)
            {
                emptyBlocks += block->Tlsf->IsEmpty() ? 1 : 0;
            }
            if (emptyBlocks > PlatformGpuMemoryConfig::RetainedEmptyBlocks)
            {
                pool.Blocks.Remove(allocation.Block);
                release = allocation.Block;
            }
        }
    }
    if (release != nullptr)
    {
        FreeBlock(pool, release);
    }
}

void GpuMemoryAllocator::FreeBlock(Pool & /*pool*/, GpuMemoryBlock *block)
{
    _source.FreeBlock(*block);
    delete block->Tlsf;
    delete block;
}

GpuMemoryPoolStats GpuMemoryAllocator::GetStats(uint32 pool) const
{
    const Pool &entry = *_pools[pool];
    const ScopeLock lock(entry.Lock);
    GpuMemoryPoolStats stats;
    stats.BlockCount = static_cast<uint32>(entry.Blocks.Count()) + entry.DedicatedCount;
    stats.AllocationCount = entry.AllocationCount;
    stats.BlockBytes = entry.DedicatedBytes;
    stats.UsedBytes = entry.DedicatedBytes;
    for (const GpuMemoryBlock *block : entry.Blocks)
    {
        stats.BlockBytes += block->Size;
        stats.UsedBytes += block->Tlsf->GetUsedBytes();
    }
    return stats;
}
} // namespace Marlin
