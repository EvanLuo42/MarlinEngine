#include "Memory/Memory.h"

#include "Concurrency/ExecutionContextInternal.h"
#include "Concurrency/SpinLock.h"
#include "Diagnostics/Assert.h"
#include "Diagnostics/Sanitizer.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformStackTrace.h"
#include "Hash/Hash.h"
#include "Memory/HeapInternal.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
namespace
{
using namespace Heap;

struct TagEntry
{
    Atomic<uint64> Id{0};
    Atomic<const char *> Name{nullptr};
    Atomic<int64> Current{0};
    Atomic<int64> Peak{0};
    Atomic<int64> Allocations{0};
    Atomic<usize> Budget{0};
    Atomic<bool> OverBudget{false};
};

TagEntry _tags[MaxTagSlots];
Atomic<int64> _tiers[3];

void DefaultBudgetHandler(const MemoryTagStats &stats)
{
    PlatformDebug::Output("Memory tag over budget: ");
    PlatformDebug::Output(stats.Name != nullptr ? stats.Name : "?");
    PlatformDebug::Output("\n");
}

Atomic<MemoryBudgetHandler> _budgetHandler{DefaultBudgetHandler};

Atomic<uint32> _initialized{0};
SpinLock _initializeLock;
bool _guardMode = false;
Atomic<bool> _tracking{false};

[[maybe_unused]] bool HasSwitch(const char *commandLine, const char *name)
{
    for (const char *start = commandLine; *start != '\0'; ++start)
    {
        if (*start != '-')
        {
            continue;
        }
        const char *a = start + 1;
        const char *b = name;
        while (*b != '\0' && (static_cast<uint8>(*a) | 0x20u) == (static_cast<uint8>(*b) | 0x20u))
        {
            ++a;
            ++b;
        }
        if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '"'))
        {
            return true;
        }
    }
    return false;
}

MemoryTagStats ReadStats(uint32 slot)
{
    const TagEntry &entry = _tags[slot];
    const int64 current = entry.Current.Load(MemoryOrder::Relaxed);
    return {
        .Name = entry.Name.Load(MemoryOrder::Acquire),
        .Id = entry.Id.Load(MemoryOrder::Acquire),
        .Current = current > 0 ? static_cast<usize>(current) : 0,
        .Peak = static_cast<usize>(entry.Peak.Load(MemoryOrder::Relaxed)),
        .Budget = entry.Budget.Load(MemoryOrder::Relaxed),
        .Allocations = static_cast<uint64>(entry.Allocations.Load(MemoryOrder::Relaxed)),
    };
}

void AddToShared(uint16 tagSlot, int64 bytes, int64 allocations)
{
    TagEntry &entry = _tags[tagSlot];
    const int64 current = entry.Current.FetchAdd(bytes, MemoryOrder::Relaxed) + bytes;
    entry.Allocations.FetchAdd(allocations, MemoryOrder::Relaxed);
    int64 peak = entry.Peak.Load(MemoryOrder::Relaxed);
    while (current > peak && !entry.Peak.CompareExchangeWeak(peak, current, MemoryOrder::Relaxed))
    {
    }
    const auto budget = static_cast<int64>(entry.Budget.Load(MemoryOrder::Relaxed));
    if (budget != 0 && current > budget)
    {
        if (!entry.OverBudget.Exchange(true, MemoryOrder::Relaxed))
        {
            _budgetHandler.Load(MemoryOrder::Acquire)(ReadStats(tagSlot));
        }
    }
    else if (entry.OverBudget.Load(MemoryOrder::Relaxed))
    {
        entry.OverBudget.Store(false, MemoryOrder::Relaxed);
    }
}

uint16 FindTagSlot(MemoryTag tag)
{
    if constexpr (MaxTagSlots == 1)
    {
        return 0;
    }
    else
    {
        if (tag == MemoryTag::Untagged)
        {
            return 0;
        }
        const auto start = static_cast<uint32>(tag.Id % (MaxTagSlots - 1));
        for (uint32 probe = 0; probe < MaxTagSlots - 1; ++probe)
        {
            const uint32 slot = 1 + ((start + probe) % (MaxTagSlots - 1));
            TagEntry &entry = _tags[slot];
            uint64 id = entry.Id.Load(MemoryOrder::Acquire);
            if (id == 0 && entry.Id.CompareExchange(id, tag.Id, MemoryOrder::AcquireRelease))
            {
                entry.Name.Store(tag.Name, MemoryOrder::Release);
                return static_cast<uint16>(slot);
            }
            if (id == tag.Id)
            {
                return static_cast<uint16>(slot);
            }
        }
        return 0;
    }
}

MARLIN_FORCEINLINE uint16 CurrentTagSlot()
{
    if constexpr (MaxTagSlots == 1)
    {
        return 0;
    }
    else
    {
        const ExecutionContext &context = Private::CurrentExecutionContext();
        return context.MemoryTagDepth == 0 ? 0 : context.MemoryTags[context.MemoryTagDepth - 1];
    }
}

/// Records the call stack of every live allocation while tracking is on; uses OS memory only, never the heap.
class AllocationTracker
{
public:
    void Enable()
    {
        const ScopeLock<SpinLock> lock(_lock);
        if (_entries != nullptr)
        {
            return;
        }
        _entries = static_cast<Entry *>(ReserveCommitted(sizeof(Entry) * EntryCapacity));
        _sites = static_cast<AllocationSite *>(ReserveCommitted(sizeof(AllocationSite) * SiteCapacity));
        _siteHashes = static_cast<uint64 *>(ReserveCommitted(sizeof(uint64) * SiteCapacity));
    }

    void Track(void *pointer, usize size, uint16 tagSlot)
    {
        AllocationSite site;
        site.FrameCount = PlatformStackTrace::Capture(site.Frames, AllocationSite::MaxFrames, 2);
        const uint64 hash =
            HashCombine(RapidHash(static_cast<const void *>(site.Frames), site.FrameCount * sizeof(void *)), tagSlot) |
            1;
        const ScopeLock<SpinLock> lock(_lock);
        uint32 siteIndex = SiteCapacity;
        for (uint32 probe = 0; probe < SiteCapacity; ++probe)
        {
            const auto index = static_cast<uint32>((hash + probe) & (SiteCapacity - 1));
            if (_siteHashes[index] == hash)
            {
                siteIndex = index;
                break;
            }
            if (_siteHashes[index] == 0)
            {
                _siteHashes[index] = hash;
                _sites[index] = site;
                _sites[index].TagSlot = tagSlot;
                siteIndex = index;
                break;
            }
        }
        if (siteIndex == SiteCapacity || _count * 4 >= EntryCapacity * 3)
        {
            return;
        }
        _sites[siteIndex].Bytes += size;
        ++_sites[siteIndex].Allocations;
        const uint64 key = HashInteger(reinterpret_cast<uintptr>(pointer));
        for (uint32 probe = 0;; ++probe)
        {
            Entry &entry = _entries[(key + probe) & (EntryCapacity - 1)];
            if (entry.Pointer == nullptr || entry.Pointer == Tombstone())
            {
                entry = {.Pointer = pointer, .Size = size, .Site = siteIndex};
                ++_count;
                return;
            }
        }
    }

    void Untrack(void *pointer)
    {
        const uint64 key = HashInteger(reinterpret_cast<uintptr>(pointer));
        const ScopeLock<SpinLock> lock(_lock);
        if (_entries == nullptr)
        {
            return;
        }
        for (uint32 probe = 0; probe < EntryCapacity; ++probe)
        {
            Entry &entry = _entries[(key + probe) & (EntryCapacity - 1)];
            if (entry.Pointer == nullptr)
            {
                return;
            }
            if (entry.Pointer == pointer)
            {
                AllocationSite &site = _sites[entry.Site];
                site.Bytes -= entry.Size;
                --site.Allocations;
                entry.Pointer = Tombstone();
                --_count;
                return;
            }
        }
    }

    void Visit(AllocationSiteVisitor visitor, void *context)
    {
        const ScopeLock<SpinLock> lock(_lock);
        if (_sites == nullptr)
        {
            return;
        }
        for (uint32 i = 0; i < SiteCapacity; ++i)
        {
            if (_siteHashes[i] != 0 && _sites[i].Allocations > 0)
            {
                visitor(_sites[i], context);
            }
        }
    }

private:
    struct Entry
    {
        void *Pointer;
        usize Size;
        uint32 Site;
    };

    static constexpr uint32 EntryCapacity = 1u << 22;
    static constexpr uint32 SiteCapacity = 1u << 16;

    static void *Tombstone()
    {
        return reinterpret_cast<void *>(uintptr{1});
    }

    static void *ReserveCommitted(usize size)
    {
        void *memory = PlatformMemory::Reserve(size);
        if (memory == nullptr || !PlatformMemory::Commit(memory, size))
        {
            MARLIN_FATAL("Out of memory for allocation tracking");
        }
        return memory;
    }

    SpinLock _lock;
    Entry *_entries = nullptr;
    AllocationSite *_sites = nullptr;
    uint64 *_siteHashes = nullptr;
    uint32 _count = 0;
};

AllocationTracker _tracker;

MARLIN_NOINLINE void InitializeSlow()
{
    const ScopeLock<SpinLock> lock(_initializeLock);
    if (_initialized.Load(MemoryOrder::Relaxed) != 0)
    {
        return;
    }
    _tags[0].Id.Store(MemoryTag::Untagged.Id, MemoryOrder::Relaxed);
    _tags[0].Name.Store(MemoryTag::Untagged.Name, MemoryOrder::Relaxed);
    if constexpr (!MARLIN_BUILD_SHIPPING)
    {
        constexpr usize Capacity = usize{64} * 1024;
        void *buffer = PlatformMemory::Reserve(Capacity);
        if (buffer != nullptr && PlatformMemory::Commit(buffer, Capacity))
        {
            auto *commandLine = static_cast<char *>(buffer);
            if (PlatformProcess::CommandLine(commandLine, Capacity) < Capacity)
            {
                _guardMode = HasSwitch(commandLine, "MallocGuard");
                if (MARLIN_MEMORY_TAGS && HasSwitch(commandLine, "MallocTrack"))
                {
                    _tracker.Enable();
                    _tracking.Store(true, MemoryOrder::Release);
                }
            }
        }
        PlatformMemory::Release(buffer, Capacity);
    }
    _initialized.Store(1, MemoryOrder::Release);
}

MARLIN_FORCEINLINE void EnsureInitialized()
{
    if (MARLIN_UNLIKELY(_initialized.Load(MemoryOrder::Acquire) == 0))
    {
        InitializeSlow();
    }
}

void *LargeAllocate(usize size, usize alignment, uint16 tagSlot)
{
    MARLIN_ASSERT(alignment < SegmentSize);
    const usize offset = alignment > OsPageSize ? alignment : OsPageSize;
    const usize needed = AlignUp(offset + size, OsPageSize);
    const auto count = static_cast<uint32>(DivideRoundUp(needed, SegmentSize));
    const SegmentSpan span = AcquireSegments(count);
    usize committed = span.CommittedBytes;
    if (committed < needed)
    {
        // Only the missing tail, so a block freed and allocated again every frame keeps its pages.
        if (!CommitPages(static_cast<uint8 *>(span.Base) + committed, needed - committed))
        {
            MARLIN_FATAL("Out of memory");
        }
        committed = needed;
    }
    auto *segment = static_cast<SegmentHeader *>(span.Base);
    segment->Kind = SegmentKind::Large;
    segment->TagSlot = tagSlot;
    segment->FromOs = span.FromOs;
    segment->SegmentCount = count;
    segment->ReservedSize = count * SegmentSize;
    segment->CommittedSize = committed;
    segment->UserSize = committed - offset;
    return static_cast<uint8 *>(span.Base) + offset;
}

/// Ends the allocation at a page that is never committed, so overruns fault at once; freed allocations stay
/// reserved and inaccessible, so later uses fault too.
void *GuardedAllocate(usize size, usize alignment, uint16 tagSlot)
{
    const usize data = AlignUp(size == 0 ? 1 : size, OsPageSize);
    const usize total = OsPageSize + data + OsPageSize;
    void *base = PlatformMemory::Reserve(total, SegmentSize);
    if (base == nullptr || !CommitPages(base, OsPageSize + data))
    {
        MARLIN_FATAL("Out of memory");
    }
    _reservedBytes.FetchAdd(total, MemoryOrder::Relaxed);
    auto *segment = static_cast<SegmentHeader *>(base);
    segment->Kind = SegmentKind::Guarded;
    segment->TagSlot = tagSlot;
    segment->FromOs = true;
    segment->ReservedSize = total;
    segment->CommittedSize = OsPageSize + data;
    segment->UserSize = size;
    const uint8 *end = static_cast<uint8 *>(base) + OsPageSize + data;
    return reinterpret_cast<void *>(AlignDown(reinterpret_cast<uintptr>(end - size), static_cast<uintptr>(alignment)));
}

usize SizeOf(SegmentHeader *segment, const void *pointer)
{
    switch (segment->Kind)
    {
    case SegmentKind::Small:
        return SmallPageOf(segment, pointer)->BlockSize;
    case SegmentKind::Medium:
        return MediumSize(pointer);
    case SegmentKind::Large:
    case SegmentKind::Guarded:
        return segment->UserSize;
    }
    MARLIN_FATAL("Pointer was not allocated by MarlinHeap");
}

[[maybe_unused]] uint16 TagOf(SegmentHeader *segment, const void *pointer)
{
    switch (segment->Kind)
    {
    case SegmentKind::Small:
        return SmallPageOf(segment, pointer)->TagSlot;
    case SegmentKind::Medium:
        return MediumTag(pointer);
    case SegmentKind::Large:
    case SegmentKind::Guarded:
        return segment->TagSlot;
    }
    MARLIN_FATAL("Pointer was not allocated by MarlinHeap");
}

[[maybe_unused]] Tier TierOf(SegmentKind kind)
{
    switch (kind)
    {
    case SegmentKind::Small:
        return Tier::Small;
    case SegmentKind::Medium:
        return Tier::Medium;
    case SegmentKind::Large:
    case SegmentKind::Guarded:
        return Tier::Large;
    }
    return Tier::Large;
}

MARLIN_NOINLINE void *AllocateSlow(usize size, usize alignment, uint16 tagSlot)
{
    EnsureInitialized();
    MARLIN_ASSERT(IsPowerOfTwo(alignment));
    alignment = alignment < DefaultAlignment ? DefaultAlignment : alignment;
    ThreadHeap *heap = GetOrCreateThreadHeap();
    void *pointer = nullptr;
    Tier tier = Tier::Small;
    if (_guardMode && size <= MediumMaxSize && alignment <= OsPageSize)
    {
        pointer = GuardedAllocate(size, alignment, tagSlot);
        tier = Tier::Large;
    }
    else if (heap != nullptr)
    {
        if (const int32 sizeClass = SmallClassFor(size, alignment); sizeClass >= 0)
        {
            pointer = SmallAllocate(heap, static_cast<uint32>(sizeClass), tagSlot);
        }
        else if (size <= MediumMaxSize && alignment <= MediumMaxAlignment)
        {
            pointer = MediumAllocate(heap, size, alignment, tagSlot);
            tier = Tier::Medium;
        }
    }
    else if (size <= MediumMaxSize && alignment <= MediumMaxAlignment)
    {
        const ScopeLock<SpinLock> lock(_sharedHeapLock);
        pointer = MediumAllocate(GetSharedHeap(), size, alignment, tagSlot);
        tier = Tier::Medium;
    }
    if (pointer == nullptr)
    {
        pointer = LargeAllocate(size, alignment, tagSlot);
        tier = Tier::Large;
    }
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        Count(heap, tagSlot, static_cast<int64>(SizeOf(SegmentOf(pointer), pointer)), 1, tier);
        if (_tracking.Load(MemoryOrder::Relaxed))
        {
            _tracker.Track(pointer, SizeOf(SegmentOf(pointer), pointer), tagSlot);
        }
    }
    return pointer;
}

MARLIN_NOINLINE void FreeSlow(SegmentHeader *segment, void *pointer)
{
    ThreadHeap *heap = _threadHeap;
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        Count(heap, TagOf(segment, pointer), -static_cast<int64>(SizeOf(segment, pointer)), 0, TierOf(segment->Kind));
        if (_tracking.Load(MemoryOrder::Relaxed))
        {
            _tracker.Untrack(pointer);
        }
    }
    switch (segment->Kind)
    {
    case SegmentKind::Small:
        SmallFree(heap, SmallPageOf(segment, pointer), pointer);
        return;
    case SegmentKind::Medium:
        if (heap != nullptr)
        {
            MediumFree(heap, pointer);
        }
        else
        {
            const ScopeLock<SpinLock> lock(_sharedHeapLock);
            MediumFree(GetSharedHeap(), pointer);
        }
        return;
    case SegmentKind::Large:
        ReleaseSegments(segment, segment->SegmentCount, segment->CommittedSize, segment->FromOs);
        return;
    case SegmentKind::Guarded:
        DecommitPages(segment, segment->CommittedSize);
        return;
    }
    MARLIN_FATAL("Free of a pointer that MarlinHeap did not allocate");
}

struct SanitizedHeader
{
    void *Base;
    usize Size;
    uint16 TagSlot;
};

MARLIN_NO_SANITIZE_ADDRESS SanitizedHeader ReadSanitizedHeader(const void *pointer)
{
    return static_cast<const SanitizedHeader *>(pointer)[-1];
}

[[maybe_unused]] Tier TierFor(usize size)
{
    return size <= SmallMaxSize ? Tier::Small : size <= MediumMaxSize ? Tier::Medium : Tier::Large;
}

[[maybe_unused]] void *SanitizedAllocate(usize size, usize alignment, uint16 tagSlot)
{
    EnsureInitialized();
    MARLIN_ASSERT(IsPowerOfTwo(alignment));
    alignment = alignment < DefaultAlignment ? DefaultAlignment : alignment;
    const usize offset = AlignUp(sizeof(SanitizedHeader), alignment);
    usize total = offset + size;
    auto *base = static_cast<uint8 *>(PlatformMemory::SystemAllocate(total));
    if (base != nullptr && !IsPointerAligned(base + offset, alignment))
    {
        PlatformMemory::SystemFree(base);
        total += alignment;
        base = static_cast<uint8 *>(PlatformMemory::SystemAllocate(total));
    }
    if (base == nullptr)
    {
        MARLIN_FATAL("Out of memory");
    }
    auto *user =
        reinterpret_cast<uint8 *>(AlignUp(reinterpret_cast<uintptr>(base + offset), static_cast<uintptr>(alignment)));
    reinterpret_cast<SanitizedHeader *>(user)[-1] = {.Base = base, .Size = size, .TagSlot = tagSlot};
    Sanitizer::PoisonMemory(base, static_cast<usize>(user - base));
    Sanitizer::PoisonMemory(user + size, static_cast<usize>(base + total - (user + size)));
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        CountDirect(tagSlot, static_cast<int64>(size), 1, TierFor(size));
        if (_tracking.Load(MemoryOrder::Relaxed))
        {
            _tracker.Track(user, size, tagSlot);
        }
    }
    return user;
}

[[maybe_unused]] void SanitizedFree(void *pointer)
{
    const SanitizedHeader header = ReadSanitizedHeader(pointer);
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        CountDirect(header.TagSlot, -static_cast<int64>(header.Size), 0, TierFor(header.Size));
        if (_tracking.Load(MemoryOrder::Relaxed))
        {
            _tracker.Untrack(pointer);
        }
    }
    PlatformMemory::SystemFree(header.Base);
}
} // namespace

namespace Heap
{
Atomic<int64> _flushSteps[MaxTagSlots];
} // namespace Heap

namespace Heap
{
void FlushStats(ThreadHeap *heap, uint16 tagSlot)
{
    AddToShared(tagSlot, heap->PendingBytes[tagSlot], heap->PendingAllocations[tagSlot]);
    heap->PendingBytes[tagSlot] = 0;
    heap->PendingAllocations[tagSlot] = 0;
    for (uint32 tier = 0; tier < 3; ++tier)
    {
        _tiers[tier].FetchAdd(heap->PendingTiers[tier], MemoryOrder::Relaxed);
        heap->PendingTiers[tier] = 0;
    }
}

void FlushAllStats(ThreadHeap *heap)
{
    for (uint16 slot = 0; slot < MaxTagSlots; ++slot)
    {
        if (heap->PendingBytes[slot] != 0 || heap->PendingAllocations[slot] != 0)
        {
            FlushStats(heap, slot);
        }
    }
}

void CountDirect(uint16 tagSlot, int64 bytes, int64 allocations, Tier tier)
{
    AddToShared(tagSlot, bytes, allocations);
    _tiers[static_cast<uint32>(tier)].FetchAdd(bytes, MemoryOrder::Relaxed);
}
} // namespace Heap

namespace Memory
{
void *Allocate(usize size, usize alignment)
{
    if constexpr (MARLIN_ASAN)
    {
        return SanitizedAllocate(size, alignment, CurrentTagSlot());
    }
    ThreadHeap *heap = _threadHeap;
    if (MARLIN_LIKELY(heap != nullptr && size <= SmallMaxSize && alignment <= DefaultAlignment))
    {
        const uint16 tagSlot = CurrentTagSlot();
        SmallPage *page = heap->Direct[tagSlot][(size + 15) >> 4];
        if (FreeBlock *block = page->LocalFree; MARLIN_LIKELY(block != nullptr))
        {
            page->LocalFree = block->Next;
            ++page->Used;
            if constexpr (MARLIN_MEMORY_TAGS)
            {
                Count(heap, tagSlot, page->BlockSize, 1, Tier::Small);
                if (MARLIN_UNLIKELY(_tracking.Load(MemoryOrder::Relaxed)))
                {
                    _tracker.Track(block, page->BlockSize, tagSlot);
                }
            }
            return block;
        }
    }
    return AllocateSlow(size, alignment, CurrentTagSlot());
}

void Free(void *pointer)
{
    if (pointer == nullptr)
    {
        return;
    }
    if constexpr (MARLIN_ASAN)
    {
        SanitizedFree(pointer);
        return;
    }
    SegmentHeader *segment = SegmentOf(pointer);
    if (MARLIN_LIKELY(segment->Kind == SegmentKind::Small))
    {
        SmallPage *page = SmallPageOf(segment, pointer);
        ThreadHeap *heap = _threadHeap;
        if (MARLIN_LIKELY(heap != nullptr && page->Owner.Load(MemoryOrder::Relaxed) == heap && !page->Full &&
                          !FillFreed &&
                          (page->Used > 1 || heap->Queues[page->TagSlot][page->SizeClass].Available.Head == page)))
        {
            if constexpr (MARLIN_MEMORY_TAGS)
            {
                Count(heap, page->TagSlot, -static_cast<int64>(page->BlockSize), 0, Tier::Small);
                if (MARLIN_UNLIKELY(_tracking.Load(MemoryOrder::Relaxed)))
                {
                    _tracker.Untrack(pointer);
                }
            }
            auto *block = static_cast<FreeBlock *>(pointer);
            block->Next = page->LocalFree;
            page->LocalFree = block;
            --page->Used;
            return;
        }
    }
    FreeSlow(segment, pointer);
}

void *Reallocate(void *pointer, usize size, usize alignment)
{
    if (pointer == nullptr)
    {
        return Allocate(size, alignment);
    }
    const usize current = GetAllocationSize(pointer);
    if (!MARLIN_ASAN && IsPointerAligned(pointer, alignment) && size <= current && size >= current / 2)
    {
        return pointer;
    }
    void *result = Allocate(size, alignment);
    MemCopy(result, pointer, size < current ? size : current);
    Free(pointer);
    return result;
}

usize GetAllocationSize(const void *pointer)
{
    if constexpr (MARLIN_ASAN)
    {
        return pointer != nullptr ? ReadSanitizedHeader(pointer).Size : 0;
    }
    return pointer != nullptr ? SizeOf(SegmentOf(pointer), pointer) : 0;
}

void Trim()
{
    if constexpr (MARLIN_ASAN)
    {
        return;
    }
    if (ThreadHeap *heap = _threadHeap)
    {
        MediumTrim(heap);
    }
    SmallTrim();
    TrimSegments();
}

HeapStats GetHeapStats()
{
    if (ThreadHeap *heap = _threadHeap)
    {
        FlushAllStats(heap);
    }
    const auto clamp = [](int64 value) { return value > 0 ? static_cast<usize>(value) : usize{0}; };
    return {
        .Reserved = _reservedBytes.Load(MemoryOrder::Relaxed),
        .Committed = _committedBytes.Load(MemoryOrder::Relaxed),
        .SmallInUse = clamp(_tiers[0].Load(MemoryOrder::Relaxed)),
        .MediumInUse = clamp(_tiers[1].Load(MemoryOrder::Relaxed)),
        .LargeInUse = clamp(_tiers[2].Load(MemoryOrder::Relaxed)),
    };
}

void PushTag(MemoryTag tag)
{
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        EnsureInitialized();
        ExecutionContext &context = Private::CurrentExecutionContext();
        MARLIN_ASSERT(context.MemoryTagDepth < ExecutionContext::MaxMemoryTagDepth);
        context.MemoryTags[context.MemoryTagDepth++] = FindTagSlot(tag);
    }
}

void PopTag()
{
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        ExecutionContext &context = Private::CurrentExecutionContext();
        MARLIN_ASSERT(context.MemoryTagDepth > 0);
        --context.MemoryTagDepth;
    }
}

MemoryTag GetCurrentTag()
{
    const char *name = _tags[CurrentTagSlot()].Name.Load(MemoryOrder::Acquire);
    return name != nullptr ? MemoryTag(name) : MemoryTag::Untagged;
}

uint32 GetTagStats(MemoryTagStats *stats, uint32 maxCount)
{
    EnsureInitialized();
    if (ThreadHeap *heap = _threadHeap)
    {
        FlushAllStats(heap);
    }
    uint32 count = 0;
    for (uint32 slot = 0; slot < MaxTagSlots; ++slot)
    {
        if (_tags[slot].Id.Load(MemoryOrder::Acquire) == 0)
        {
            continue;
        }
        if (count < maxCount)
        {
            stats[count] = ReadStats(slot);
        }
        ++count;
    }
    return count;
}

void SetBudget(MemoryTag tag, usize bytes)
{
    EnsureInitialized();
    const uint16 slot = FindTagSlot(tag);
    TagEntry &entry = _tags[slot];
    entry.Budget.Store(bytes, MemoryOrder::Relaxed);
    const int64 step = bytes == 0 ? 0 : static_cast<int64>(bytes / 16) + 1;
    _flushSteps[slot].Store(step < StatsFlushBytes ? step : 0, MemoryOrder::Relaxed);
    entry.OverBudget.Store(false, MemoryOrder::Relaxed);
}

void SetBudgetHandler(MemoryBudgetHandler handler)
{
    _budgetHandler.Store(handler != nullptr ? handler : DefaultBudgetHandler, MemoryOrder::Release);
}

void EnableTracking()
{
    if constexpr (MARLIN_MEMORY_TAGS)
    {
        EnsureInitialized();
        _tracker.Enable();
        _tracking.Store(true, MemoryOrder::Release);
    }
}

bool IsTracking()
{
    return _tracking.Load(MemoryOrder::Acquire);
}

void VisitAllocationSites(AllocationSiteVisitor visitor, void *context)
{
    _tracker.Visit(visitor, context);
}
} // namespace Memory
} // namespace Marlin
