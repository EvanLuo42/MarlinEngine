#include "Memory/HeapInternal.h"

#include "Diagnostics/Assert.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"

namespace Marlin::Heap
{
Atomic<usize> _reservedBytes{0};
Atomic<usize> _committedBytes{0};

namespace
{
constexpr usize ArenaSegments = PlatformHeapConfig::ArenaReserve / SegmentSize;
constexpr usize BitmapWords = ArenaSegments == 0 ? 1 : (ArenaSegments + 63) / 64;
constexpr usize SegmentSlots = ArenaSegments == 0 ? 1 : ArenaSegments;
/// Spans longer than this go to the OS, so one huge allocation cannot fragment the arena.
constexpr usize MaxArenaSpan = ArenaSegments / 8;
/// Retained segments decommitted per pass; the system calls run outside the lock.
constexpr usize DecommitBatch = 64;

static_assert(SegmentSize % OsPageSize == 0 && SegmentSize / OsPageSize <= 0xFFFF);

SpinLock _arenaLock;
uint8 *_arenaBase = nullptr;
bool _arenaReserved = false;
/// Segments in use, including retained ones being decommitted outside the lock.
uint64 _usedSegments[BitmapWords] = {};
/// Free segments that still have committed pages.
uint64 _retainedSegments[BitmapWords] = {};
/// When each retained segment was freed, in milliseconds of PlatformTime.
uint32 _retainedSince[SegmentSlots] = {};
/// OS pages committed from the start of each retained segment; the rest of the segment is not committed.
uint16 _retainedPages[SegmentSlots] = {};
usize _retainedBytes = 0;
usize _retainedCount = 0;
usize _searchHint = 0;
uint32 _lastPurge = 0;

/// Retained segments taken out of the cache under the lock, to decommit after it.
struct DecommitList
{
    usize Indices[DecommitBatch];
    usize Bytes[DecommitBatch];
    usize Count = 0;
};

uint32 NowMilliseconds()
{
    return static_cast<uint32>(PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond());
}

bool TestBit(const uint64 *bits, usize index)
{
    return (bits[index / 64] & (uint64{1} << (index % 64))) != 0;
}

void SetBit(uint64 *bits, usize index)
{
    bits[index / 64] |= uint64{1} << (index % 64);
}

void ClearBit(uint64 *bits, usize index)
{
    bits[index / 64] &= ~(uint64{1} << (index % 64));
}

/// Removes a segment from the retained cache and returns its committed bytes; under _arenaLock.
usize TakeRetained(usize index)
{
    const usize bytes = usize{_retainedPages[index]} * OsPageSize;
    ClearBit(_retainedSegments, index);
    _retainedBytes -= bytes;
    --_retainedCount;
    return bytes;
}

/// Moves a retained segment to `list`, marked used so no one takes it while it is decommitted; under _arenaLock.
void TakeForDecommit(usize index, DecommitList &list)
{
    list.Bytes[list.Count] = TakeRetained(index);
    list.Indices[list.Count++] = index;
    SetBit(_usedSegments, index);
}

/// Decommits what TakeForDecommit collected, then frees the segments; without _arenaLock.
void FinishDecommit(const DecommitList &list)
{
    for (usize i = 0; i < list.Count; ++i)
    {
        DecommitPages(_arenaBase + (list.Indices[i] * SegmentSize), list.Bytes[i]);
    }
    const ScopeLock<SpinLock> lock(_arenaLock);
    for (usize i = 0; i < list.Count; ++i)
    {
        ClearBit(_usedSegments, list.Indices[i]);
        _searchHint = list.Indices[i] < _searchHint ? list.Indices[i] : _searchHint;
    }
}

/// Collects retained segments idle for longer than the purge delay while more than RetainedBytes are retained;
/// under _arenaLock.
void Purge(uint32 now, DecommitList &list)
{
    constexpr uint32 Delay = PlatformHeapConfig::PurgeDelayMilliseconds;
    if (_retainedBytes <= PlatformHeapConfig::RetainedBytes || now - _lastPurge < Delay / 4)
    {
        return;
    }
    _lastPurge = now;
    for (usize word = 0; word < BitmapWords && _retainedBytes > PlatformHeapConfig::RetainedBytes; ++word)
    {
        uint64 bits = _retainedSegments[word];
        while (bits != 0 && _retainedBytes > PlatformHeapConfig::RetainedBytes && list.Count < DecommitBatch)
        {
            const usize index = (word * 64) + static_cast<usize>(CountTrailingZeros(bits));
            bits &= bits - 1;
            if (now - _retainedSince[index] >= Delay)
            {
                TakeForDecommit(index, list);
            }
        }
    }
}

void ReserveArena()
{
    _arenaReserved = true;
    if constexpr (ArenaSegments > 0)
    {
        _arenaBase = static_cast<uint8 *>(PlatformMemory::Reserve(PlatformHeapConfig::ArenaReserve, SegmentSize));
        if (_arenaBase != nullptr)
        {
            _reservedBytes.FetchAdd(PlatformHeapConfig::ArenaReserve, MemoryOrder::Relaxed);
        }
    }
}

/// The free segments of `word`, without the bits past the end of the arena.
uint64 FreeBits(usize word)
{
    uint64 free = ~_usedSegments[word];
    if (ArenaSegments % 64 != 0 && word == BitmapWords - 1)
    {
        free &= (uint64{1} << (ArenaSegments % 64)) - 1;
    }
    return free;
}

/// Whether segments [start, start + count) are all free.
bool IsRunFree(usize start, usize count)
{
    if (start + count > ArenaSegments)
    {
        return false;
    }
    for (usize index = start; index < start + count;)
    {
        const usize bit = index % 64;
        const usize take = Min(64 - bit, start + count - index);
        const uint64 mask = (take == 64 ? ~uint64{0} : (uint64{1} << take) - 1) << bit;
        if ((FreeBits(index / 64) & mask) != mask)
        {
            return false;
        }
        index += take;
    }
    return true;
}

/// Finds `count` free segments in a row, preferring to start at a retained one; ArenaSegments if none.
usize FindRun(usize count)
{
    usize seen = 0;
    for (usize word = 0; word < BitmapWords && seen < _retainedCount; ++word)
    {
        for (uint64 bits = _retainedSegments[word]; bits != 0; bits &= bits - 1)
        {
            const usize index = (word * 64) + static_cast<usize>(CountTrailingZeros(bits));
            if (count == 1 || IsRunFree(index, count))
            {
                return index;
            }
            ++seen;
        }
    }
    if (count == 1)
    {
        for (usize i = 0; i < BitmapWords; ++i)
        {
            const usize word = (_searchHint / 64 + i) % BitmapWords;
            if (const uint64 free = FreeBits(word); free != 0)
            {
                return (word * 64) + static_cast<usize>(CountTrailingZeros(free));
            }
        }
        return ArenaSegments;
    }
    // A word of free bits at a time: a run carries into the next word while its free bits reach the top.
    usize run = 0;
    usize runStart = 0;
    for (usize word = 0; word < BitmapWords; ++word)
    {
        const uint64 free = FreeBits(word);
        usize bit = 0;
        while (bit < 64)
        {
            const uint64 rest = free >> bit;
            if ((rest & 1) == 0)
            {
                run = 0;
                if (rest == 0)
                {
                    break;
                }
                bit += static_cast<usize>(CountTrailingZeros(rest));
                continue;
            }
            const auto ones = static_cast<usize>(CountTrailingZeros(~rest));
            runStart = run == 0 ? (word * 64) + bit : runStart;
            run += ones;
            if (run >= count)
            {
                return runStart;
            }
            bit += ones;
        }
    }
    return ArenaSegments;
}
} // namespace

bool CommitPages(void *address, usize size)
{
    if (!PlatformMemory::Commit(address, size))
    {
        return false;
    }
    _committedBytes.FetchAdd(size, MemoryOrder::Relaxed);
    return true;
}

void DecommitPages(void *address, usize size)
{
    PlatformMemory::Decommit(address, size);
    _committedBytes.FetchSub(size, MemoryOrder::Relaxed);
}

SegmentSpan AcquireSegments(uint32 count)
{
    if (count <= MaxArenaSpan)
    {
        usize start = ArenaSegments;
        usize committed = 0;
        usize stale = 0;
        {
            const ScopeLock<SpinLock> lock(_arenaLock);
            if (!_arenaReserved)
            {
                ReserveArena();
            }
            if (_arenaBase != nullptr)
            {
                start = FindRun(count);
            }
            if (start < ArenaSegments)
            {
                // The span's committed pages must be a prefix; retained pages past the first gap are stale.
                bool prefix = true;
                for (usize index = start; index < start + count; ++index)
                {
                    SetBit(_usedSegments, index);
                    const usize bytes = TestBit(_retainedSegments, index) ? TakeRetained(index) : 0;
                    committed += prefix ? bytes : 0;
                    stale += prefix ? 0 : bytes;
                    prefix = prefix && bytes == SegmentSize;
                }
                _searchHint = start + count;
            }
        }
        if (start < ArenaSegments)
        {
            uint8 *base = _arenaBase + (start * SegmentSize);
            if (stale != 0)
            {
                // Decommitting pages that are not committed is allowed, so the rest of the span takes one call.
                const usize from = AlignUp(committed, SegmentSize);
                PlatformMemory::Decommit(base + from, (count * SegmentSize) - from);
                _committedBytes.FetchSub(stale, MemoryOrder::Relaxed);
            }
            return {.Base = base, .CommittedBytes = committed, .FromOs = false};
        }
    }
    void *base = PlatformMemory::Reserve(count * SegmentSize, SegmentSize);
    if (base == nullptr)
    {
        MARLIN_FATAL("Out of memory");
    }
    _reservedBytes.FetchAdd(count * SegmentSize, MemoryOrder::Relaxed);
    return {.Base = base, .CommittedBytes = 0, .FromOs = true};
}

void ReleaseSegments(void *base, uint32 count, usize committedBytes, bool fromOs)
{
    const usize size = count * SegmentSize;
    MARLIN_ASSERT(committedBytes <= size && committedBytes % OsPageSize == 0);
    if (fromOs)
    {
        PlatformMemory::Release(base, size);
        _reservedBytes.FetchSub(size, MemoryOrder::Relaxed);
        _committedBytes.FetchSub(committedBytes, MemoryOrder::Relaxed);
        return;
    }
    const uint32 now = NowMilliseconds();
    DecommitList purge;
    {
        const ScopeLock<SpinLock> lock(_arenaLock);
        const usize start = static_cast<usize>(static_cast<uint8 *>(base) - _arenaBase) / SegmentSize;
        for (usize i = 0; i < count; ++i)
        {
            const usize index = start + i;
            ClearBit(_usedSegments, index);
            const usize offset = i * SegmentSize;
            const usize bytes = committedBytes > offset ? Min(committedBytes - offset, SegmentSize) : 0;
            if (bytes != 0)
            {
                // Partly committed segments are kept too, so a large block freed every frame keeps its pages.
                SetBit(_retainedSegments, index);
                _retainedSince[index] = now;
                _retainedPages[index] = static_cast<uint16>(bytes / OsPageSize);
                ++_retainedCount;
            }
        }
        _retainedBytes += committedBytes;
        _searchHint = start < _searchHint ? start : _searchHint;
        Purge(now, purge);
    }
    if (purge.Count != 0)
    {
        FinishDecommit(purge);
    }
}

void TrimSegments()
{
    while (true)
    {
        DecommitList list;
        {
            const ScopeLock<SpinLock> lock(_arenaLock);
            for (usize word = 0; word < BitmapWords && _retainedCount != 0 && list.Count < DecommitBatch; ++word)
            {
                for (uint64 bits = _retainedSegments[word]; bits != 0 && list.Count < DecommitBatch; bits &= bits - 1)
                {
                    TakeForDecommit((word * 64) + static_cast<usize>(CountTrailingZeros(bits)), list);
                }
            }
        }
        if (list.Count == 0)
        {
            return;
        }
        FinishDecommit(list);
    }
}
} // namespace Marlin::Heap
