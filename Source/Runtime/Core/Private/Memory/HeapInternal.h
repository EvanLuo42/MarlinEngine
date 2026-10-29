#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "CoreTypes.h"
#include "HAL/PlatformHeapConfig.h"
#include "Memory/Memory.h"
#include "Templates/Bit.h"

/// MarlinHeap internals. Memory comes in segments carved from an arena reserved at startup; a segment's header,
/// found by masking a pointer, tells how to free it. Small blocks live in slab pages and medium blocks in TLSF pools,
/// both owned by thread heaps; blocks freed by another thread go through atomic lists back to their owner. Large
/// blocks take whole segments.
namespace Marlin::Heap
{
inline constexpr usize SegmentSize = PlatformHeapConfig::SegmentSize;
inline constexpr usize SmallPageSize = PlatformHeapConfig::SmallPageSize;
inline constexpr uint32 SmallPagesPerSegment = SegmentSize / SmallPageSize;
inline constexpr usize SmallMaxSize = 1024;
inline constexpr uint32 SmallDirectCount = (SmallMaxSize / 16) + 1;
inline constexpr uint32 SmallClassCount = 32;
inline constexpr usize MediumMaxSize = usize{1024} * 1024;
inline constexpr usize MediumMaxAlignment = usize{64} * 1024;
inline constexpr usize OsPageSize = 4096;
inline constexpr uint32 MaxTagSlots = MARLIN_MEMORY_TAGS ? 128 : 1;
inline constexpr uint8 FreedFill = 0xDD;
/// Freed memory is overwritten to expose use after free, except where performance is measured or shipped.
inline constexpr bool FillFreed = MARLIN_BUILD_DEBUG || MARLIN_BUILD_DEVELOPMENT;
/// Tag statistics gather per thread and reach the shared counters in steps of this many bytes.
inline constexpr int64 StatsFlushBytes = int64{256} * 1024;

static_assert(IsPowerOfTwo(SegmentSize) && SegmentSize % SmallPageSize == 0 && SmallPageSize >= 4 * SmallMaxSize);
static_assert(MediumMaxSize + MediumMaxAlignment + 256 < SegmentSize);

enum class SegmentKind : uint32
{
    Small = 0x4C4C4D53,
    Medium = 0x4D44454D,
    Large = 0x4752414C,
    Guarded = 0x44524147,
};

enum class Tier : uint8
{
    Small,
    Medium,
    Large,
};

struct SegmentHeader
{
    SegmentKind Kind = SegmentKind::Small;
    uint16 TagSlot = 0;
    /// Reserved from the OS directly rather than from the arena.
    bool FromOs = false;
    uint32 SegmentCount = 0;
    usize ReservedSize = 0;
    usize CommittedSize = 0;
    usize UserSize = 0;
};

[[nodiscard]] inline SegmentHeader *SegmentOf(const void *pointer)
{
    return reinterpret_cast<SegmentHeader *>(reinterpret_cast<uintptr>(pointer) & ~(SegmentSize - 1));
}

extern Atomic<usize> _reservedBytes;
extern Atomic<usize> _committedBytes;

struct SegmentSpan
{
    void *Base = nullptr;
    /// The span's first bytes that are already committed, as when it comes from the retained cache; the rest is not.
    usize CommittedBytes = 0;
    bool FromOs = false;
};

/// Takes `count` contiguous segments; fatal when out of memory.
SegmentSpan AcquireSegments(uint32 count);
/// The span's first `committedBytes` are committed and the rest is not; the committed pages are kept for reuse.
void ReleaseSegments(void *base, uint32 count, usize committedBytes, bool fromOs);
/// Decommits the retained cache.
void TrimSegments();
bool CommitPages(void *address, usize size);
void DecommitPages(void *address, usize size);

struct FreeBlock
{
    FreeBlock *Next;
};

struct ThreadHeap;

struct SmallPage
{
    Atomic<ThreadHeap *> Owner{nullptr};
    /// Blocks other threads freed, or FullPageMark while the page is full and none have been.
    Atomic<FreeBlock *> ThreadFree{nullptr};
    FreeBlock *LocalFree = nullptr;
    uint8 *Start = nullptr;
    uint32 BlockSize = 0;
    uint32 Capacity = 0;
    uint32 Carved = 0;
    uint32 Used = 0;
    uint16 SizeClass = 0;
    uint16 TagSlot = 0;
    bool InUse = false;
    bool Committed = false;
    bool Full = false;
    SmallPage *Next = nullptr;
    SmallPage *Previous = nullptr;
};

struct PageList
{
    SmallPage *Head = nullptr;

    void PushFront(SmallPage *page);
    void Remove(SmallPage *page);
};

struct PageQueue
{
    PageList Available;
    PageList Full;
};

struct MediumBlock;
struct MediumSegment;

inline constexpr uint32 TlsfFirstLevelCount = 15;
inline constexpr uint32 TlsfSecondLevelCount = 16;

struct TlsfControl
{
    uint32 FirstBitmap = 0;
    uint32 SecondBitmaps[TlsfFirstLevelCount] = {};
    MediumBlock *Lists[TlsfFirstLevelCount][TlsfSecondLevelCount] = {};
    MediumSegment *Segments = nullptr;
    /// Segments with no allocation, kept for the next burst.
    uint32 EmptySegments = 0;
};

/// Per-thread heap state. Heaps outlive their threads: a heap left by an exiting thread is adopted by the next new
/// thread together with its pages and pools.
struct ThreadHeap
{
    /// The page serving each 16-byte size step, or _emptyPage; the allocation fast path reads only this.
    SmallPage *Direct[MaxTagSlots][SmallDirectCount] = {};
    PageQueue Queues[MaxTagSlots][SmallClassCount];
    TlsfControl Medium;
    /// Set when another thread defers frees to one of Medium's segments.
    Atomic<bool> MediumDeferred{false};
    /// Blocks other threads freed in full small pages; the owner frees them locally, which makes the pages available.
    Atomic<FreeBlock *> DelayedFree{nullptr};
    int64 PendingBytes[MaxTagSlots] = {};
    int64 PendingAllocations[MaxTagSlots] = {};
    int64 PendingTiers[3] = {};
    ThreadHeap *NextAbandoned = nullptr;
};

/// A page with no free blocks that every empty Direct entry points to, so the fast path needs no null check.
extern SmallPage _emptyPage;
extern thread_local ThreadHeap *_threadHeap;

/// The calling thread's heap, created or adopted on first use; null while the thread exits.
ThreadHeap *GetOrCreateThreadHeap();
/// For threads without a heap of their own; use under _sharedHeapLock.
ThreadHeap *GetSharedHeap();
extern SpinLock _sharedHeapLock;

/// Class index for a small request, or -1 when it needs the medium tier.
[[nodiscard]] int32 SmallClassFor(usize size, usize alignment);
[[nodiscard]] uint32 SmallClassSize(uint32 sizeClass);
[[nodiscard]] void *SmallAllocate(ThreadHeap *heap, uint32 sizeClass, uint16 tagSlot);
[[nodiscard]] SmallPage *SmallPageOf(SegmentHeader *segment, const void *pointer);
/// Frees a block of `page`; `heap` is the caller's heap, possibly null.
void SmallFree(ThreadHeap *heap, SmallPage *page, void *pointer);
void SmallTrim();

[[nodiscard]] void *MediumAllocate(ThreadHeap *heap, usize size, usize alignment, uint16 tagSlot);
/// Frees `pointer` into `heap` if it owns the block's segment, or defers the free to the owner.
void MediumFree(ThreadHeap *heap, void *pointer);
[[nodiscard]] usize MediumSize(const void *pointer);
[[nodiscard]] uint16 MediumTag(const void *pointer);
void MediumTrim(ThreadHeap *heap);

/// Adds a heap's pending counts for `tagSlot` to the shared tag counters.
void FlushStats(ThreadHeap *heap, uint16 tagSlot);
void FlushAllStats(ThreadHeap *heap);
/// For threads without a heap.
void CountDirect(uint16 tagSlot, int64 bytes, int64 allocations, Tier tier);
/// Per-tag flush step; zero means StatsFlushBytes. Tags with a budget flush sooner.
extern Atomic<int64> _flushSteps[MaxTagSlots];

inline void Count(ThreadHeap *heap, uint16 tagSlot, int64 bytes, int64 allocations, Tier tier)
{
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        if (heap == nullptr)
        {
            CountDirect(tagSlot, bytes, allocations, tier);
            return;
        }
        heap->PendingTiers[static_cast<uint32>(tier)] += bytes;
        heap->PendingAllocations[tagSlot] += allocations;
        int64 &pending = heap->PendingBytes[tagSlot];
        pending += bytes;
        const int64 step = _flushSteps[tagSlot].Load(MemoryOrder::Relaxed);
        const int64 limit = step != 0 ? step : StatsFlushBytes;
        if (pending >= limit || pending <= -limit)
        {
            FlushStats(heap, tagSlot);
        }
    }
}
} // namespace Marlin::Heap
