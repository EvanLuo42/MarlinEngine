#include "ScriptHeap.h"

#include "Diagnostics/Assert.h"
#include "HAL/PlatformMemory.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
namespace
{
// RECOVERY-DECISION SC1: the old generation's size classes, spaced about 25% apart up to LargeObjectBytes.
constexpr uint32 SizeClasses[] = {16,   32,   48,   64,   80,   96,   128,  160,  192,  256,  320,  384, 512,
                                  640,  768,  1024, 1280, 1536, 2048, 2560, 3072, 4096, 5120, 6144, 8192};
constexpr uint32 SizeClassCount = sizeof(SizeClasses) / sizeof(SizeClasses[0]);

usize RoundUp(usize value, usize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

ScriptHeap::ScriptHeap(const ScriptDomainOptions &options)
{
    const usize nursery = RoundUp(options.NurseryBytes > 0 ? options.NurseryBytes : BlockBytes, BlockBytes);
    _survivorBytes = RoundUp(options.SurvivorBytes > 0 ? options.SurvivorBytes : BlockBytes, BlockBytes);
    const usize young = nursery + 2 * _survivorBytes;
    _reserved = RoundUp(options.HeapReserveBytes > young + BlockBytes ? options.HeapReserveBytes : young + BlockBytes,
                        BlockBytes);
    _base = static_cast<uint8 *>(PlatformMemory::Reserve(_reserved, BlockBytes));
    if (_base == nullptr || !PlatformMemory::Commit(_base, young))
    {
        MARLIN_FATAL("the script heap cannot reserve its address space");
    }
    _edenBegin = _base;
    _edenTop = _base;
    _edenEnd = _base + nursery;
    _survivor[0] = _edenEnd;
    _survivor[1] = _edenEnd + _survivorBytes;
    _survivorTop = _survivor[0];
    _oldBase = _base + young;
    _oldEnd = _base + _reserved;
    _freeCells.Resize(SizeClassCount, nullptr);
    _youngStarts.Resize(young / 16 / 64, 0);
}

void ScriptHeap::ClearYoungStarts(const uint8 *begin, const uint8 *end)
{
    // The spaces start on block boundaries, so the words of one hold no bits of another.
    const usize first = static_cast<usize>(begin - _edenBegin) / 16 / 64;
    const usize last = (static_cast<usize>(end - _edenBegin) / 16 + 63) / 64;
    if (last > first)
    {
        MemZero(&_youngStarts[first], (last - first) * sizeof(uint64));
    }
}

uint8 *ScriptHeap::FindYoungStart(const uint8 *address, const uint8 *begin) const
{
    const usize bit = static_cast<usize>(address - _edenBegin) / 16;
    const usize floor = static_cast<usize>(begin - _edenBegin) / 16 / 64;
    usize word = bit / 64;
    uint64 bits = _youngStarts[word] & (~uint64{0} >> (63 - bit % 64));
    while (bits == 0 && word > floor)
    {
        bits = _youngStarts[--word];
    }
    if (bits == 0)
    {
        return nullptr;
    }
    uint8 *start = _edenBegin + (word * 64 + 63 - static_cast<usize>(__builtin_clzll(bits))) * 16;
    return start >= begin ? start : nullptr;
}

ScriptHeap::~ScriptHeap()
{
    PlatformMemory::Release(_base, _reserved);
}

uint32 ScriptHeap::SizeClassOf(usize bytes)
{
    uint32 low = 0;
    uint32 high = SizeClassCount - 1;
    while (low < high)
    {
        const uint32 middle = (low + high) / 2;
        if (SizeClasses[middle] < bytes)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

void ScriptHeap::SetStart(const uint8 *object)
{
    const usize bit = static_cast<usize>(object - _oldBase) / 16;
    _starts[bit / 64] |= uint64{1} << (bit % 64);
}

uint8 *ScriptHeap::Allocate(usize bytes, bool &old)
{
    bytes = RoundUp(bytes, 16);
    if (bytes <= LargeObjectBytes && _edenTop + bytes <= _edenEnd)
    {
        uint8 *object = _edenTop;
        _edenTop += bytes;
        MemZero(object, bytes);
        SetYoungStart(object);
        old = false;
        return object;
    }
    old = true;
    return AllocateOld(bytes);
}

uint8 *ScriptHeap::AllocateBlocks(uint32 count)
{
    // First fit from the hint, below which no block is free.
    usize first = _blockHighWater;
    usize firstFree = _blockHighWater;
    usize run = 0;
    for (usize i = _freeBlockHint; i < _blockHighWater; ++i)
    {
        run = _blocks[i].Kind == 0 ? run + 1 : 0;
        firstFree = run == 1 && firstFree == _blockHighWater ? i : firstFree;
        if (run == count)
        {
            first = i + 1 - count;
            break;
        }
    }
    if (first == _blockHighWater)
    {
        const usize capacity = static_cast<usize>(_oldEnd - _oldBase) / BlockBytes;
        if (_blockHighWater + count > capacity ||
            !PlatformMemory::Commit(_oldBase + _blockHighWater * BlockBytes, count * BlockBytes))
        {
            return nullptr;
        }
        _blockHighWater += count;
        _blocks.Resize(_blockHighWater);
        _cards.Resize(_blockHighWater * (BlockBytes / CardBytes), 0);
        _starts.Resize(_blockHighWater * (BlockBytes / 16 / 64), 0);
    }
    _freeBlockHint = firstFree == first ? first + count : firstFree;
    return _oldBase + first * BlockBytes;
}

void ScriptHeap::FreeBlocks(usize first, uint32 count)
{
    for (usize i = first; i < first + count; ++i)
    {
        _blocks[i] = {};
    }
    MemZero(&_cards[first * (BlockBytes / CardBytes)], count * (BlockBytes / CardBytes));
    _oldBytes -= count * BlockBytes;
    _freeBlockHint = first < _freeBlockHint ? first : _freeBlockHint;
}

uint8 *ScriptHeap::AllocateOld(usize bytes)
{
    bytes = RoundUp(bytes, 16);
    if (bytes > LargeObjectBytes)
    {
        const uint32 count = static_cast<uint32>(RoundUp(bytes, BlockBytes) / BlockBytes);
        uint8 *object = AllocateBlocks(count);
        if (object == nullptr)
        {
            return nullptr;
        }
        const usize first = static_cast<usize>(object - _oldBase) / BlockBytes;
        for (usize i = first; i < first + count; ++i)
        {
            _blocks[i] = {static_cast<uint8>(i == first ? 2 : 3), 0, count};
        }
        MemZero(object, bytes);
        SetStart(object);
        _oldBytes += count * BlockBytes;
        return object;
    }
    const uint32 sizeClass = SizeClassOf(bytes);
    const usize cellBytes = SizeClasses[sizeClass];
    if (_freeCells[sizeClass] == nullptr)
    {
        uint8 *block = AllocateBlocks(1);
        if (block == nullptr)
        {
            return nullptr;
        }
        _blocks[static_cast<usize>(block - _oldBase) / BlockBytes] = {1, static_cast<uint8>(sizeClass), 1};
        // Free cells hold the next free cell; pushed from the end so the list runs in address order.
        const usize cells = BlockBytes / cellBytes;
        for (usize i = cells; i-- > 0;)
        {
            uint8 *cell = block + i * cellBytes;
            MemCopy(cell, &_freeCells[sizeClass], sizeof(uint8 *));
            _freeCells[sizeClass] = cell;
        }
    }
    uint8 *cell = _freeCells[sizeClass];
    MemCopy(&_freeCells[sizeClass], cell, sizeof(uint8 *));
    MemZero(cell, cellBytes);
    SetStart(cell);
    _oldBytes += cellBytes;
    return cell;
}

usize ScriptHeap::GetUsedBytes() const
{
    return static_cast<usize>(_edenTop - _edenBegin) +
           static_cast<usize>(_survivorTop - _survivor[_survivorIndex]) + _oldBytes;
}
} // namespace Marlin
