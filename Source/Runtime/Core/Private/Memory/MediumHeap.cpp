#include "Memory/HeapInternal.h"

#include "Diagnostics/Assert.h"
#include "Memory/MemoryOps.h"

namespace Marlin::Heap
{
namespace
{
constexpr uint32 SecondLevelLog2 = 4;
constexpr uint32 AlignmentLog2 = 4;
constexpr uint32 FirstLevelShift = SecondLevelLog2 + AlignmentLog2;
constexpr usize SmallBlockSize = usize{1} << FirstLevelShift;
constexpr usize BlockHeaderSize = 16;
constexpr usize MinPayload = 16;
constexpr uint32 FreeBit = 1;
constexpr uint32 PreviousFreeBit = 2;
constexpr uint32 FlagMask = 15;

static_assert(TlsfSecondLevelCount == 1u << SecondLevelLog2);
} // namespace

/// A TLSF block: the header, then the payload, which holds the free-list links while the block is free.
struct MediumBlock
{
    MediumBlock *PreviousPhysical;
    uint32 SizeAndFlags;
    uint16 TagSlot;
    uint16 Unused;
    MediumBlock *NextFree;
    MediumBlock *PreviousFree;

    [[nodiscard]] usize Size() const
    {
        return SizeAndFlags & ~FlagMask;
    }

    void SetSize(usize size)
    {
        SizeAndFlags = static_cast<uint32>(size) | (SizeAndFlags & FlagMask);
    }

    [[nodiscard]] bool IsFree() const
    {
        return (SizeAndFlags & FreeBit) != 0;
    }

    void SetFree(bool free)
    {
        SizeAndFlags = free ? SizeAndFlags | FreeBit : SizeAndFlags & ~FreeBit;
    }

    [[nodiscard]] bool IsPreviousFree() const
    {
        return (SizeAndFlags & PreviousFreeBit) != 0;
    }

    void SetPreviousFree(bool free)
    {
        SizeAndFlags = free ? SizeAndFlags | PreviousFreeBit : SizeAndFlags & ~PreviousFreeBit;
    }

    [[nodiscard]] uint8 *Payload()
    {
        return reinterpret_cast<uint8 *>(this) + BlockHeaderSize;
    }

    [[nodiscard]] MediumBlock *NextPhysical()
    {
        return reinterpret_cast<MediumBlock *>(Payload() + Size());
    }
};

/// A TLSF pool owned by one heap. Other threads push blocks they free onto Deferred.
struct MediumSegment
{
    SegmentHeader Header;
    Atomic<ThreadHeap *> Owner{nullptr};
    Atomic<FreeBlock *> Deferred{nullptr};
    MediumSegment *Next = nullptr;
    MediumSegment *Previous = nullptr;
    uint32 Allocations = 0;
};

namespace
{
constexpr usize FirstBlockOffset = AlignUp(sizeof(MediumSegment), usize{16});
constexpr usize PoolPayload = SegmentSize - FirstBlockOffset - (2 * BlockHeaderSize);
/// Empty pools a heap keeps instead of returning them to the arena.
constexpr uint32 KeptEmptySegments = 1;

MediumBlock *BlockFromPayload(void *payload)
{
    return reinterpret_cast<MediumBlock *>(static_cast<uint8 *>(payload) - BlockHeaderSize);
}

const MediumBlock *BlockFromPayload(const void *payload)
{
    return reinterpret_cast<const MediumBlock *>(static_cast<const uint8 *>(payload) - BlockHeaderSize);
}

MediumSegment *MediumSegmentOf(const void *pointer)
{
    return reinterpret_cast<MediumSegment *>(SegmentOf(pointer));
}

MediumBlock *FirstBlock(MediumSegment *segment)
{
    return reinterpret_cast<MediumBlock *>(reinterpret_cast<uint8 *>(segment) + FirstBlockOffset);
}

struct Mapping
{
    uint32 First;
    uint32 Second;
};

Mapping MapInsert(usize size)
{
    if (size < SmallBlockSize)
    {
        return {0, static_cast<uint32>(size / (SmallBlockSize / TlsfSecondLevelCount))};
    }
    const auto log2 = static_cast<uint32>(FloorLog2(size));
    const auto second = static_cast<uint32>(size >> (log2 - SecondLevelLog2)) ^ TlsfSecondLevelCount;
    return {log2 - (FirstLevelShift - 1), second};
}

Mapping MapSearch(usize size)
{
    if (size >= SmallBlockSize)
    {
        size += (usize{1} << (static_cast<uint32>(FloorLog2(size)) - SecondLevelLog2)) - 1;
    }
    return MapInsert(size);
}

void InsertFree(TlsfControl &control, MediumBlock *block)
{
    const Mapping mapping = MapInsert(block->Size());
    MediumBlock *&head = control.Lists[mapping.First][mapping.Second];
    block->NextFree = head;
    block->PreviousFree = nullptr;
    if (head != nullptr)
    {
        head->PreviousFree = block;
    }
    head = block;
    control.FirstBitmap |= 1u << mapping.First;
    control.SecondBitmaps[mapping.First] |= 1u << mapping.Second;
}

void RemoveFree(TlsfControl &control, const MediumBlock *block)
{
    const Mapping mapping = MapInsert(block->Size());
    if (block->PreviousFree != nullptr)
    {
        block->PreviousFree->NextFree = block->NextFree;
    }
    else
    {
        control.Lists[mapping.First][mapping.Second] = block->NextFree;
        if (block->NextFree == nullptr)
        {
            control.SecondBitmaps[mapping.First] &= ~(1u << mapping.Second);
            if (control.SecondBitmaps[mapping.First] == 0)
            {
                control.FirstBitmap &= ~(1u << mapping.First);
            }
        }
    }
    if (block->NextFree != nullptr)
    {
        block->NextFree->PreviousFree = block->PreviousFree;
    }
}

MediumBlock *FindFree(const TlsfControl &control, usize size)
{
    Mapping mapping = MapSearch(size);
    if (mapping.First >= TlsfFirstLevelCount)
    {
        return nullptr;
    }
    uint32 secondMap = control.SecondBitmaps[mapping.First] & (~0u << mapping.Second);
    if (secondMap == 0)
    {
        const uint32 firstMap = control.FirstBitmap & (~0u << (mapping.First + 1));
        if (firstMap == 0)
        {
            return nullptr;
        }
        mapping.First = static_cast<uint32>(CountTrailingZeros(firstMap));
        secondMap = control.SecondBitmaps[mapping.First];
    }
    mapping.Second = static_cast<uint32>(CountTrailingZeros(secondMap));
    return control.Lists[mapping.First][mapping.Second];
}

void AddSegment(ThreadHeap *heap)
{
    const SegmentSpan span = AcquireSegments(1);
    if (span.CommittedBytes < SegmentSize &&
        !CommitPages(static_cast<uint8 *>(span.Base) + span.CommittedBytes, SegmentSize - span.CommittedBytes))
    {
        MARLIN_FATAL("Out of memory");
    }
    auto *segment = ConstructAt(static_cast<MediumSegment *>(span.Base));
    segment->Header.Kind = SegmentKind::Medium;
    segment->Header.FromOs = span.FromOs;
    segment->Header.SegmentCount = 1;
    segment->Owner.Store(heap, MemoryOrder::Relaxed);
    TlsfControl &control = heap->Medium;
    segment->Next = control.Segments;
    if (segment->Next != nullptr)
    {
        segment->Next->Previous = segment;
    }
    control.Segments = segment;
    ++control.EmptySegments;

    MediumBlock *block = FirstBlock(segment);
    block->PreviousPhysical = nullptr;
    block->SizeAndFlags = 0;
    block->SetSize(PoolPayload);
    block->SetFree(true);
    MediumBlock *sentinel = block->NextPhysical();
    sentinel->PreviousPhysical = block;
    sentinel->SizeAndFlags = 0;
    sentinel->SetPreviousFree(true);
    InsertFree(control, block);
}

void ReleaseSegment(ThreadHeap *heap, MediumSegment *segment)
{
    TlsfControl &control = heap->Medium;
    RemoveFree(control, FirstBlock(segment));
    if (segment->Previous != nullptr)
    {
        segment->Previous->Next = segment->Next;
    }
    else
    {
        control.Segments = segment->Next;
    }
    if (segment->Next != nullptr)
    {
        segment->Next->Previous = segment->Previous;
    }
    --control.EmptySegments;
    ReleaseSegments(segment, 1, SegmentSize, segment->Header.FromOs);
}

/// Splits the tail past `size` off `block` into a free block.
void TrimTail(TlsfControl &control, MediumBlock *block, usize size)
{
    if (block->Size() < size + BlockHeaderSize + MinPayload)
    {
        return;
    }
    auto *rest = reinterpret_cast<MediumBlock *>(block->Payload() + size);
    rest->PreviousPhysical = block;
    rest->SizeAndFlags = 0;
    rest->SetSize(block->Size() - size - BlockHeaderSize);
    rest->SetFree(true);
    block->SetSize(size);
    MediumBlock *next = rest->NextPhysical();
    next->PreviousPhysical = rest;
    next->SetPreviousFree(true);
    InsertFree(control, rest);
}

/// Splits the head of `block` off into a free block so that the payload starts at `payload`.
MediumBlock *TrimHead(TlsfControl &control, MediumBlock *block, uint8 *payload)
{
    const auto gap = static_cast<usize>(payload - block->Payload());
    if (gap == 0)
    {
        return block;
    }
    MediumBlock *aligned = BlockFromPayload(payload);
    aligned->PreviousPhysical = block;
    aligned->SizeAndFlags = 0;
    aligned->SetSize(block->Size() - gap);
    aligned->SetPreviousFree(true);
    aligned->NextPhysical()->PreviousPhysical = aligned;
    block->SetSize(gap - BlockHeaderSize);
    InsertFree(control, block);
    return aligned;
}

void FreeLocal(ThreadHeap *heap, MediumBlock *block)
{
    TlsfControl &control = heap->Medium;
    MediumSegment *segment = MediumSegmentOf(block);
    block->SetFree(true);
    if (block->IsPreviousFree())
    {
        MediumBlock *previous = block->PreviousPhysical;
        RemoveFree(control, previous);
        previous->SetSize(previous->Size() + BlockHeaderSize + block->Size());
        block = previous;
    }
    MediumBlock *next = block->NextPhysical();
    if (next->IsFree())
    {
        RemoveFree(control, next);
        block->SetSize(block->Size() + BlockHeaderSize + next->Size());
    }
    next = block->NextPhysical();
    next->PreviousPhysical = block;
    next->SetPreviousFree(true);
    InsertFree(control, block);
    if (--segment->Allocations == 0)
    {
        ++control.EmptySegments;
        if (control.EmptySegments > KeptEmptySegments && segment->Deferred.Load(MemoryOrder::Acquire) == nullptr)
        {
            ReleaseSegment(heap, segment);
        }
    }
}

/// Frees the blocks other threads handed back to `heap`'s pools.
void DrainDeferred(ThreadHeap *heap)
{
    if (!heap->MediumDeferred.Load(MemoryOrder::Relaxed) || !heap->MediumDeferred.Exchange(false, MemoryOrder::Acquire))
    {
        return;
    }
    MediumSegment *segment = heap->Medium.Segments;
    while (segment != nullptr)
    {
        MediumSegment *next = segment->Next;
        FreeBlock *list = segment->Deferred.Exchange(nullptr, MemoryOrder::Acquire);
        while (list != nullptr)
        {
            FreeBlock *following = list->Next;
            FreeLocal(heap, BlockFromPayload(list));
            list = following;
        }
        segment = next;
    }
}
} // namespace

void *MediumAllocate(ThreadHeap *heap, usize size, usize alignment, uint16 tagSlot)
{
    DrainDeferred(heap);
    TlsfControl &control = heap->Medium;
    size = AlignUp(size < MinPayload ? MinPayload : size, usize{16});
    const usize gapReserve = alignment > 16 ? alignment + BlockHeaderSize + MinPayload : 0;
    MediumBlock *block = FindFree(control, size + gapReserve);
    if (block == nullptr)
    {
        AddSegment(heap);
        block = FindFree(control, size + gapReserve);
        MARLIN_ASSERT(block != nullptr);
    }
    RemoveFree(control, block);
    if (alignment > 16)
    {
        uint8 *payload = AlignPointerUp(block->Payload(), alignment);
        if (payload != block->Payload() &&
            static_cast<usize>(payload - block->Payload()) < BlockHeaderSize + MinPayload)
        {
            payload = AlignPointerUp(block->Payload() + BlockHeaderSize + MinPayload, alignment);
        }
        block = TrimHead(control, block, payload);
    }
    TrimTail(control, block, size);
    block->SetFree(false);
    block->NextPhysical()->SetPreviousFree(false);
    block->TagSlot = tagSlot;
    MediumSegment *segment = MediumSegmentOf(block);
    if (segment->Allocations++ == 0)
    {
        --control.EmptySegments;
    }
    return block->Payload();
}

void MediumFree(ThreadHeap *heap, void *pointer)
{
    MediumBlock *block = BlockFromPayload(pointer);
    if constexpr (FillFreed)
    {
        MemSet(pointer, FreedFill, block->Size());
    }
    MediumSegment *segment = MediumSegmentOf(pointer);
    ThreadHeap *owner = segment->Owner.Load(MemoryOrder::Relaxed);
    if (owner == heap)
    {
        FreeLocal(heap, block);
        return;
    }
    auto *deferred = static_cast<FreeBlock *>(pointer);
    FreeBlock *head = segment->Deferred.Load(MemoryOrder::Relaxed);
    do
    {
        deferred->Next = head;
    } while (!segment->Deferred.CompareExchangeWeak(head, deferred, MemoryOrder::Release));
    owner->MediumDeferred.Store(true, MemoryOrder::Release);
}

usize MediumSize(const void *pointer)
{
    return BlockFromPayload(pointer)->Size();
}

uint16 MediumTag(const void *pointer)
{
    return BlockFromPayload(pointer)->TagSlot;
}

void MediumTrim(ThreadHeap *heap)
{
    heap->MediumDeferred.Store(true, MemoryOrder::Relaxed);
    DrainDeferred(heap);
    MediumSegment *segment = heap->Medium.Segments;
    while (segment != nullptr)
    {
        MediumSegment *next = segment->Next;
        if (segment->Allocations == 0 && segment->Deferred.Load(MemoryOrder::Acquire) == nullptr)
        {
            ReleaseSegment(heap, segment);
        }
        segment = next;
    }
}
} // namespace Marlin::Heap
