#include "Memory/HeapInternal.h"

#include "Diagnostics/Assert.h"
#include "HAL/PlatformMemory.h"
#include "Memory/MemoryOps.h"

namespace Marlin::Heap
{
namespace
{
constexpr uint32 SmallClassSizes[SmallClassCount] = {
    16,  32,  48,  64,  80,  96,  112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
    288, 320, 352, 384, 416, 448, 480, 512, 576, 640, 704, 768, 832, 896, 960, 1024,
};

/// Blocks carved from a fresh page into its free list at a time.
constexpr uint32 ExtendBatch = 64;

struct ClassTables
{
    /// Size class of each 16-byte size step.
    uint8 ClassOfStep[SmallDirectCount] = {};
    /// The size steps a class serves.
    uint8 FirstStep[SmallClassCount] = {};
    uint8 LastStep[SmallClassCount] = {};
};

constexpr ClassTables MakeClassTables()
{
    ClassTables tables;
    uint32 sizeClass = 0;
    for (uint32 step = 0; step < SmallDirectCount; ++step)
    {
        while (SmallClassSizes[sizeClass] < step * 16)
        {
            ++sizeClass;
        }
        tables.ClassOfStep[step] = static_cast<uint8>(sizeClass);
    }
    for (uint8 &first : tables.FirstStep)
    {
        first = 0xFF;
    }
    for (uint32 step = 0; step < SmallDirectCount; ++step)
    {
        const uint8 cls = tables.ClassOfStep[step];
        if (tables.FirstStep[cls] == 0xFF)
        {
            tables.FirstStep[cls] = static_cast<uint8>(step);
        }
        tables.LastStep[cls] = static_cast<uint8>(step);
    }
    return tables;
}

constexpr ClassTables Tables = MakeClassTables();

struct SmallSegment
{
    SegmentHeader Header;
    SmallSegment *NextSegment = nullptr;
    SmallSegment *PreviousSegment = nullptr;
    /// Links among the segments that have free pages.
    SmallSegment *NextAvailable = nullptr;
    SmallSegment *PreviousAvailable = nullptr;
    uint32 FreePages = 0;
    SmallPage Pages[SmallPagesPerSegment];
};

static_assert(sizeof(SmallSegment) <= SmallPageSize, "The segment header must fit in page 0");

SpinLock _segmentLock;
SmallSegment *_segments = nullptr;
/// The segments with free pages, so taking a page does not walk the full ones.
SmallSegment *_availableSegments = nullptr;
uint32 _segmentCount = 0;

/// ThreadFree of a full page that no other thread has freed a block in yet. The first such free takes the mark and
/// hands its block to the owner's DelayedFree, so the owner learns of the page without scanning its full pages.
FreeBlock *FullPageMark()
{
    return reinterpret_cast<FreeBlock *>(uintptr{1});
}

/// Under _segmentLock.
void LinkSegment(SmallSegment *segment)
{
    segment->PreviousSegment = nullptr;
    segment->NextSegment = _segments;
    if (_segments != nullptr)
    {
        _segments->PreviousSegment = segment;
    }
    _segments = segment;
    ++_segmentCount;
}

void UnlinkSegment(SmallSegment *segment)
{
    (segment->PreviousSegment != nullptr ? segment->PreviousSegment->NextSegment : _segments) = segment->NextSegment;
    if (segment->NextSegment != nullptr)
    {
        segment->NextSegment->PreviousSegment = segment->PreviousSegment;
    }
    --_segmentCount;
}

void LinkAvailable(SmallSegment *segment)
{
    segment->PreviousAvailable = nullptr;
    segment->NextAvailable = _availableSegments;
    if (_availableSegments != nullptr)
    {
        _availableSegments->PreviousAvailable = segment;
    }
    _availableSegments = segment;
}

void UnlinkAvailable(SmallSegment *segment)
{
    (segment->PreviousAvailable != nullptr ? segment->PreviousAvailable->NextAvailable : _availableSegments) =
        segment->NextAvailable;
    if (segment->NextAvailable != nullptr)
    {
        segment->NextAvailable->PreviousAvailable = segment->PreviousAvailable;
    }
    segment->NextAvailable = nullptr;
    segment->PreviousAvailable = nullptr;
}

/// Points the class's size steps in the direct table at the head of its available pages.
void RefreshDirect(ThreadHeap *heap, uint16 tagSlot, uint32 sizeClass)
{
    SmallPage *head = heap->Queues[tagSlot][sizeClass].Available.Head;
    SmallPage *target = head != nullptr ? head : &_emptyPage;
    for (uint32 step = Tables.FirstStep[sizeClass]; step <= Tables.LastStep[sizeClass]; ++step)
    {
        heap->Direct[tagSlot][step] = target;
    }
}

usize CommittedBytesOf(const SmallSegment *segment)
{
    usize bytes = SmallPageSize;
    for (uint32 i = 1; i < SmallPagesPerSegment; ++i)
    {
        bytes += segment->Pages[i].Committed ? SmallPageSize : 0;
    }
    return bytes;
}

/// A segment with every page but the header's free; outside _segmentLock, since it may commit memory.
SmallSegment *NewSegment()
{
    const SegmentSpan span = AcquireSegments(1);
    auto *base = static_cast<uint8 *>(span.Base);
    // Page 0 holds the header. Committing on demand still keeps whatever the retained span brought.
    usize end = SegmentSize;
    if constexpr (PlatformHeapConfig::CommitOnDemand)
    {
        end = AlignUp(span.CommittedBytes, SmallPageSize);
        end = end < SmallPageSize ? SmallPageSize : end;
    }
    if (span.CommittedBytes < end && !CommitPages(base + span.CommittedBytes, end - span.CommittedBytes))
    {
        MARLIN_FATAL("Out of memory");
    }
    auto *segment = ConstructAt(reinterpret_cast<SmallSegment *>(base));
    segment->Header.Kind = SegmentKind::Small;
    segment->Header.FromOs = span.FromOs;
    segment->Header.SegmentCount = 1;
    segment->FreePages = SmallPagesPerSegment - 1;
    for (uint32 i = 0; i < SmallPagesPerSegment; ++i)
    {
        segment->Pages[i].Committed = (static_cast<usize>(i) + 1) * SmallPageSize <= end;
    }
    segment->Pages[0].InUse = true;
    return segment;
}

/// Returns an unlinked segment to the arena; outside _segmentLock.
void ReleaseSegment(SmallSegment *segment)
{
    const usize committed = CommittedBytesOf(segment);
    if (segment->Header.FromOs)
    {
        ReleaseSegments(segment, 1, committed, true);
        return;
    }
    // The arena keeps a committed prefix; pages committed past the first gap are decommitted in one call.
    usize prefix = SmallPageSize;
    for (uint32 i = 1; i < SmallPagesPerSegment && segment->Pages[i].Committed; ++i)
    {
        prefix += SmallPageSize;
    }
    if (committed > prefix)
    {
        PlatformMemory::Decommit(reinterpret_cast<uint8 *>(segment) + prefix, SegmentSize - prefix);
        _committedBytes.FetchSub(committed - prefix, MemoryOrder::Relaxed);
    }
    ReleaseSegments(segment, 1, prefix, false);
}

SmallPage *AcquirePage(ThreadHeap *heap, uint32 sizeClass, uint16 tagSlot)
{
    SmallSegment *segment = nullptr;
    SmallPage *page = nullptr;
    SmallSegment *fresh = nullptr;
    while (page == nullptr)
    {
        {
            const ScopeLock<SpinLock> lock(_segmentLock);
            if (fresh != nullptr)
            {
                LinkSegment(fresh);
                LinkAvailable(fresh);
            }
            segment = _availableSegments;
            if (segment != nullptr)
            {
                for (uint32 i = 1; i < SmallPagesPerSegment; ++i)
                {
                    if (!segment->Pages[i].InUse)
                    {
                        page = segment->Pages + i;
                        break;
                    }
                }
                page->InUse = true;
                if (--segment->FreePages == 0)
                {
                    UnlinkAvailable(segment);
                }
            }
        }
        if (page == nullptr)
        {
            fresh = NewSegment();
        }
    }
    const auto index = static_cast<usize>(page - segment->Pages);
    page->Start = reinterpret_cast<uint8 *>(segment) + (index * SmallPageSize);
    if (!page->Committed)
    {
        if (!CommitPages(page->Start, SmallPageSize))
        {
            MARLIN_FATAL("Out of memory");
        }
        page->Committed = true;
    }
    page->Owner.Store(heap, MemoryOrder::Relaxed);
    page->ThreadFree.Store(nullptr, MemoryOrder::Relaxed);
    page->LocalFree = nullptr;
    page->BlockSize = SmallClassSizes[sizeClass];
    page->Capacity = static_cast<uint32>(SmallPageSize / page->BlockSize);
    page->Carved = 0;
    page->Used = 0;
    page->SizeClass = static_cast<uint16>(sizeClass);
    page->TagSlot = tagSlot;
    page->Full = false;
    return page;
}

void ReleasePage(SmallPage *page)
{
    auto *segment = reinterpret_cast<SmallSegment *>(SegmentOf(page->Start));
    page->Owner.Store(nullptr, MemoryOrder::Relaxed);
    bool release = false;
    {
        const ScopeLock<SpinLock> lock(_segmentLock);
        page->InUse = false;
        if (segment->FreePages++ == 0)
        {
            LinkAvailable(segment);
        }
        if (segment->FreePages == SmallPagesPerSegment - 1 && _segmentCount > 1)
        {
            UnlinkAvailable(segment);
            UnlinkSegment(segment);
            release = true;
        }
    }
    if (release)
    {
        ReleaseSegment(segment);
    }
}

/// Moves the blocks other threads freed into the local list; returns whether there were any. Not for full pages.
bool CollectThreadFree(SmallPage *page)
{
    FreeBlock *list = page->ThreadFree.Exchange(nullptr, MemoryOrder::Acquire);
    MARLIN_CHECK_SLOW(list != FullPageMark());
    if (list == nullptr)
    {
        return false;
    }
    uint32 count = 1;
    FreeBlock *tail = list;
    while (tail->Next != nullptr)
    {
        tail = tail->Next;
        ++count;
    }
    tail->Next = page->LocalFree;
    page->LocalFree = list;
    page->Used -= count;
    return true;
}

/// Carves up to ExtendBatch untouched blocks into the free list.
void ExtendFreeList(SmallPage *page)
{
    const uint32 count = Min(ExtendBatch, page->Capacity - page->Carved);
    uint8 *first = page->Start + (static_cast<usize>(page->Carved) * page->BlockSize);
    for (uint32 i = 0; i + 1 < count; ++i)
    {
        reinterpret_cast<FreeBlock *>(first + (static_cast<usize>(i) * page->BlockSize))->Next =
            reinterpret_cast<FreeBlock *>(first + (static_cast<usize>(i + 1) * page->BlockSize));
    }
    reinterpret_cast<FreeBlock *>(first + (static_cast<usize>(count - 1) * page->BlockSize))->Next = page->LocalFree;
    page->LocalFree = reinterpret_cast<FreeBlock *>(first);
    page->Carved += count;
}

/// Frees a block of a page `heap` owns.
void LocalFree(ThreadHeap *heap, SmallPage *page, FreeBlock *block)
{
    block->Next = page->LocalFree;
    page->LocalFree = block;
    --page->Used;
    PageQueue &queue = heap->Queues[page->TagSlot][page->SizeClass];
    if (page->Full)
    {
        // Clears the mark unless another thread already took it; its block is then on the way through DelayedFree.
        FreeBlock *mark = FullPageMark();
        page->ThreadFree.CompareExchange(mark, nullptr, MemoryOrder::Relaxed);
        queue.Full.Remove(page);
        page->Full = false;
        queue.Available.PushFront(page);
        RefreshDirect(heap, page->TagSlot, page->SizeClass);
    }
    else if (page->Used == 0 && queue.Available.Head != page && !CollectThreadFree(page))
    {
        queue.Available.Remove(page);
        ReleasePage(page);
    }
}

/// Frees the blocks other threads handed over from full pages, which moves those pages back to available.
void CollectDelayedFree(ThreadHeap *heap)
{
    if (heap->DelayedFree.Load(MemoryOrder::Relaxed) == nullptr)
    {
        return;
    }
    FreeBlock *block = heap->DelayedFree.Exchange(nullptr, MemoryOrder::Acquire);
    while (block != nullptr)
    {
        FreeBlock *next = block->Next;
        LocalFree(heap, SmallPageOf(SegmentOf(block), block), block);
        block = next;
    }
}
} // namespace

int32 SmallClassFor(usize size, usize alignment)
{
    if (size > SmallMaxSize || alignment > SmallMaxSize)
    {
        return -1;
    }
    const uint8 first = Tables.ClassOfStep[(size + 15) / 16];
    if (alignment <= 16)
    {
        return first;
    }
    for (uint32 sizeClass = first; sizeClass < SmallClassCount; ++sizeClass)
    {
        if (SmallClassSizes[sizeClass] % alignment == 0)
        {
            return static_cast<int32>(sizeClass);
        }
    }
    return -1;
}

uint32 SmallClassSize(uint32 sizeClass)
{
    return SmallClassSizes[sizeClass];
}

SmallPage *SmallPageOf(SegmentHeader *segment, const void *pointer)
{
    const usize index = (reinterpret_cast<uintptr>(pointer) - reinterpret_cast<uintptr>(segment)) / SmallPageSize;
    return reinterpret_cast<SmallSegment *>(segment)->Pages + index;
}

void *SmallAllocate(ThreadHeap *heap, uint32 sizeClass, uint16 tagSlot)
{
    PageQueue &queue = heap->Queues[tagSlot][sizeClass];
    while (true)
    {
        SmallPage *page = queue.Available.Head;
        if (page == nullptr)
        {
            CollectDelayedFree(heap);
            if (queue.Available.Head == nullptr)
            {
                queue.Available.PushFront(AcquirePage(heap, sizeClass, tagSlot));
            }
            continue;
        }
        if (FreeBlock *block = page->LocalFree)
        {
            page->LocalFree = block->Next;
            ++page->Used;
            RefreshDirect(heap, tagSlot, sizeClass);
            return block;
        }
        if (CollectThreadFree(page))
        {
            continue;
        }
        if (page->Carved < page->Capacity)
        {
            ExtendFreeList(page);
            continue;
        }
        // Marks the page full, unless another thread freed a block in it since the collect above.
        FreeBlock *empty = nullptr;
        if (!page->ThreadFree.CompareExchange(empty, FullPageMark(), MemoryOrder::Relaxed))
        {
            continue;
        }
        queue.Available.Remove(page);
        page->Full = true;
        queue.Full.PushFront(page);
    }
}

void SmallFree(ThreadHeap *heap, SmallPage *page, void *pointer)
{
    if constexpr (FillFreed)
    {
        MemSet(pointer, FreedFill, page->BlockSize);
    }
    auto *block = static_cast<FreeBlock *>(pointer);
    if (heap != nullptr && page->Owner.Load(MemoryOrder::Relaxed) == heap)
    {
        LocalFree(heap, page, block);
        return;
    }
    FreeBlock *head = page->ThreadFree.Load(MemoryOrder::Relaxed);
    while (true)
    {
        if (head == FullPageMark())
        {
            // The page cannot change owner while it is full, and heaps are never destroyed.
            if (page->ThreadFree.CompareExchangeWeak(head, nullptr, MemoryOrder::Relaxed))
            {
                ThreadHeap *owner = page->Owner.Load(MemoryOrder::Relaxed);
                FreeBlock *delayed = owner->DelayedFree.Load(MemoryOrder::Relaxed);
                do
                {
                    block->Next = delayed;
                } while (!owner->DelayedFree.CompareExchangeWeak(delayed, block, MemoryOrder::Release));
                return;
            }
            continue;
        }
        block->Next = head;
        if (page->ThreadFree.CompareExchangeWeak(head, block, MemoryOrder::Release))
        {
            return;
        }
    }
}

void SmallTrim()
{
    const ScopeLock<SpinLock> lock(_segmentLock);
    for (SmallSegment *segment = _segments; segment != nullptr; segment = segment->NextSegment)
    {
        for (uint32 i = 1; i < SmallPagesPerSegment; ++i)
        {
            SmallPage &page = segment->Pages[i];
            if (!page.InUse && page.Committed)
            {
                DecommitPages(reinterpret_cast<uint8 *>(segment) + (static_cast<usize>(i) * SmallPageSize),
                              SmallPageSize);
                page.Committed = false;
            }
        }
    }
}
} // namespace Marlin::Heap
