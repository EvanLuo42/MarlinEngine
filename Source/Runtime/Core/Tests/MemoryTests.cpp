#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "Memory/Allocator.h"
#include "Memory/Arena.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"
#include "Templates/NumericLimits.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
struct Counter
{
    SpinLock Lock;
    uint64 Value = 0;
    Atomic<uint64> AtomicValue{0};
};

void IncrementCounter(void *argument)
{
    auto &counter = *static_cast<Counter *>(argument);
    for (int i = 0; i < 200000; ++i)
    {
        counter.Lock.Lock();
        ++counter.Value;
        counter.Lock.Unlock();
        counter.AtomicValue.FetchAdd(1, MemoryOrder::Relaxed);
    }
}

struct Tracked
{
    static inline int32 Alive = 0;
    int32 Value;
    explicit Tracked(int32 value) : Value(value)
    {
        ++Alive;
    }
    Tracked(Tracked &&other) noexcept : Value(other.Value)
    {
        ++Alive;
    }
    ~Tracked()
    {
        --Alive;
    }
    Tracked(const Tracked &) = delete;
    Tracked &operator=(const Tracked &) = delete;
    Tracked &operator=(Tracked &&) = delete;
};

bool IsFilled(const void *pointer, usize size, uint8 value)
{
    const auto *bytes = static_cast<const uint8 *>(pointer);
    for (usize i = 0; i < size; ++i)
    {
        if (bytes[i] != value)
        {
            return false;
        }
    }
    return true;
}

struct CrossThreadBatch
{
    void *Pointers[4096];
    uint32 Count = 0;
    Atomic<uint32> Ready{0};
};

void CrossThreadProducer(void *argument)
{
    auto &batch = *static_cast<CrossThreadBatch *>(argument);
    for (uint32 i = 0; i < batch.Count; ++i)
    {
        const usize size = 1 + (i * 37) % 3000;
        batch.Pointers[i] = Memory::Allocate(size);
        MemSet(batch.Pointers[i], static_cast<uint8>(i), size);
    }
    batch.Ready.Store(1, MemoryOrder::Release);
}

void ShortLivedThread(void * /*argument*/)
{
    for (int i = 0; i < 1000; ++i)
    {
        Memory::Free(Memory::Allocate(64));
    }
    void *leaked = Memory::Allocate(48);
    Memory::Free(leaked);
}

struct RemoteFrees
{
    void **Pointers;
    uint32 Count;
};

void FreeAll(void *argument)
{
    const auto &frees = *static_cast<RemoteFrees *>(argument);
    for (uint32 i = 0; i < frees.Count; ++i)
    {
        Memory::Free(frees.Pointers[i]);
    }
}

usize _siteBytes = 0;
uint32 _sites = 0;

void CountSite(const AllocationSite &site, void * /*context*/)
{
    ++_sites;
    _siteBytes += site.Bytes;
}
} // namespace

MARLIN_TEST(Memory, ThreadsAndAtomics)
{
    Counter counter;
    ThreadHandle threads[4];
    for (ThreadHandle &thread : threads)
    {
        ThreadDesc desc;
        desc.Entry = IncrementCounter;
        desc.Argument = &counter;
        thread = PlatformThread::Create(desc);
    }
    for (ThreadHandle thread : threads)
    {
        PlatformThread::Join(thread);
    }
    MARLIN_EXPECT(counter.Value == 800000 && counter.AtomicValue.Load() == 800000);

    Atomic<int32 *> pointer{nullptr};
    int32 values[4] = {};
    pointer.Store(values);
    MARLIN_EXPECT(pointer.FetchAdd(2) == values && pointer.Load() == values + 2);
    int32 expected = 5;
    Atomic<int32> number{3};
    MARLIN_EXPECT(!number.CompareExchange(expected, 9) && expected == 3 && number.CompareExchange(expected, 9));
    MARLIN_EXPECT(number.Load() == 9 && number.Exchange(1) == 9);

    alignas(Tracked) uint8 storage[sizeof(Tracked) * 4];
    auto *items = reinterpret_cast<Tracked *>(storage);
    for (int32 i = 0; i < 2; ++i)
    {
        ConstructAt(items + i, i * 10);
    }
    RelocateRange(items + 2, items, 2);
    MARLIN_EXPECT(Tracked::Alive == 2 && items[2].Value == 0 && items[3].Value == 10);
    DestroyRange(items + 2, 2);
    MARLIN_EXPECT(Tracked::Alive == 0);
    int32 a = 1;
    int32 b = 2;
    Swap(a, b);
    MARLIN_EXPECT(a == 2 && b == 1 && Exchange(a, 7) == 2 && a == 7);
    constexpr Pair Values{1, 2.5f};
    static_assert(Values.First == 1 && Clamp(5, 0, 3) == 3);
}

MARLIN_TEST(Memory, Allocators)
{
    const usize sizes[] = {0, 1, 15, 16, 17, 100, 255, 1024, 1025, 4000, 65536, 1000000, 1048576, 1048577, 5000000};
    const usize alignments[] = {1, 8, 16, 32, 64, 256, 4096, 65536};
    for (usize size : sizes)
    {
        for (usize alignment : alignments)
        {
            void *pointer = Memory::Allocate(size, alignment);
            MARLIN_EXPECT(pointer != nullptr && IsPointerAligned(pointer, alignment));
            MARLIN_EXPECT(Memory::GetAllocationSize(pointer) >= size);
            MemSet(pointer, 0xAB, size);
            Memory::Free(pointer);
        }
    }
    void *grown = Memory::Allocate(10);
    MemSet(grown, 7, 10);
    grown = Memory::Reallocate(grown, 5000);
    MARLIN_EXPECT(IsFilled(grown, 10, 7));
    grown = Memory::Reallocate(grown, 3000000);
    MARLIN_EXPECT(IsFilled(grown, 10, 7));
    Memory::Free(grown);
    Memory::Free(nullptr);

    void *many[20000];
    for (usize i = 0; i < 20000; ++i)
    {
        many[i] = Memory::Allocate(1 + (i % 2000));
        *static_cast<uint8 *>(many[i]) = static_cast<uint8>(i);
    }
    bool intact = true;
    for (usize i = 0; i < 20000; ++i)
    {
        intact = intact && *static_cast<uint8 *>(many[i]) == static_cast<uint8>(i);
        Memory::Free(many[i]);
    }
    MARLIN_EXPECT(intact);

    static CrossThreadBatch batches[8];
    ThreadHandle threads[8];
    for (uint32 t = 0; t < 8; ++t)
    {
        batches[t].Count = 4096;
        ThreadDesc desc;
        desc.Entry = CrossThreadProducer;
        desc.Argument = &batches[t];
        threads[t] = PlatformThread::Create(desc);
    }
    for (ThreadHandle thread : threads)
    {
        PlatformThread::Join(thread);
    }
    bool crossIntact = true;
    for (CrossThreadBatch &batch : batches)
    {
        for (uint32 i = 0; i < batch.Count; ++i)
        {
            crossIntact = crossIntact && *static_cast<uint8 *>(batch.Pointers[i]) == static_cast<uint8>(i);
            Memory::Free(batch.Pointers[i]);
        }
    }
    MARLIN_EXPECT(crossIntact);
    for (int round = 0; round < 3; ++round)
    {
        ThreadDesc desc;
        desc.Entry = ShortLivedThread;
        PlatformThread::Join(PlatformThread::Create(desc));
    }

    const MemoryTag physics = MemoryTag::Physics;
    void *tagged = nullptr;
    {
        MARLIN_MEMORY_SCOPE(physics);
        MARLIN_EXPECT(Memory::GetCurrentTag() == MemoryTag::Physics || !MARLIN_MEMORY_TAGS);
        tagged = Memory::Allocate(100000);
    }
    MARLIN_EXPECT(Memory::GetCurrentTag() == MemoryTag::Untagged);
    MemoryTagStats stats[64];
    uint32 count = Memory::GetTagStats(stats, 64);
    usize physicsBytes = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        if (stats[i].Id == MemoryTag::Physics.Id)
        {
            physicsBytes = stats[i].Current;
        }
    }
    MARLIN_EXPECT(physicsBytes >= 100000 || !MARLIN_MEMORY_TAGS);
    static int budgetCalls = 0;
    Memory::SetBudgetHandler([](const MemoryTagStats &) { ++budgetCalls; });
    Memory::SetBudget(MemoryTag("Budgeted"), 1000);
    {
        MARLIN_MEMORY_SCOPE(MemoryTag("Budgeted"));
        void *over = Memory::Allocate(2000);
        void *more = Memory::Allocate(2000);
        Memory::Free(over);
        Memory::Free(more);
    }
    MARLIN_EXPECT(budgetCalls == 1 || !MARLIN_MEMORY_TAGS);
    Memory::Free(tagged);
    count = Memory::GetTagStats(stats, 64);
    for (uint32 i = 0; i < count; ++i)
    {
        if (stats[i].Id == MemoryTag::Physics.Id)
        {
            MARLIN_EXPECT(stats[i].Current == 0 && stats[i].Peak >= 100000);
        }
    }

    auto *number = new int32(42);
    MARLIN_EXPECT(Memory::GetAllocationSize(number) >= sizeof(int32));
    delete number;
    struct alignas(128) Wide
    {
        uint8 Bytes[128];
    };
    auto *wide = new Wide[3];
    MARLIN_EXPECT(IsPointerAligned(wide, 128));
    delete[] wide;

    Arena arena(1024);
    auto *first = arena.AllocateArray<uint64>(10);
    const Arena::Marker marker = arena.GetMarker();
    for (int i = 0; i < 100; ++i)
    {
        (void)arena.Allocate(500, 64);
    }
    MARLIN_EXPECT(arena.GetUsedBytes() > 50000);
    arena.Rewind(marker);
    MARLIN_EXPECT(arena.Allocate(8, 8) == first + 10);
    arena.Reset();
    MARLIN_EXPECT(arena.AllocateArray<uint64>(1) == first);

    FrameArena frames(usize{64} << 20);
    void *frameA = frames.Allocate(usize{3} << 20, 256);
    MARLIN_EXPECT(IsPointerAligned(frameA, 256));
    MemSet(frameA, 1, usize{3} << 20);
    frames.BeginFrame();
    MARLIN_EXPECT(frames.GetUsedBytes() == 0);
    frames.BeginFrame();
    MARLIN_EXPECT(frames.Allocate(16) == frameA);

    PoolAllocator<Tracked> pool(4);
    Tracked *items[10];
    for (int32 i = 0; i < 10; ++i)
    {
        items[i] = pool.New(i);
    }
    MARLIN_EXPECT(pool.GetCount() == 10 && Tracked::Alive == 10 && items[9]->Value == 9);
    for (Tracked *item : items)
    {
        pool.Delete(item);
    }
    MARLIN_EXPECT(pool.GetCount() == 0 && Tracked::Alive == 0);

    InlineAllocator<4>::ForType<uint64> inlineStorage;
    uint64 *small = inlineStorage.Allocate(3);
    uint64 *large = inlineStorage.Allocate(10);
    MARLIN_EXPECT(inlineStorage.IsInline(small) && !inlineStorage.IsInline(large));
    inlineStorage.Free(large, 10);
    inlineStorage.Free(small, 3);
    PolymorphicAllocator::ForType<uint32> erased{PolymorphicAllocator{&arena}};
    MARLIN_EXPECT(erased.Allocate(4) != nullptr);

    Memory::EnableTracking();
    void *trackedA = Memory::Allocate(1234);
    void *trackedB = Memory::Allocate(1234);
    Memory::VisitAllocationSites(CountSite, nullptr);
    MARLIN_EXPECT((_sites >= 1 && _siteBytes >= 2468) || !MARLIN_MEMORY_TAGS);
    Memory::Free(trackedA);
    Memory::Free(trackedB);
    Memory::Trim();
    const HeapStats heap = Memory::GetHeapStats();
    MARLIN_EXPECT(heap.Committed > 0 && heap.Reserved >= heap.Committed);
}
MARLIN_TEST(Memory, LargeBlocksKeepTheirPages)
{
    // A block over a segment ends partway into its last one; freeing it keeps the committed pages, so the same
    // block every frame commits nothing new.
    constexpr usize Size = (usize{3} * 1024 * 1024) + 12345;
    Memory::Trim();
    const usize before = Memory::GetHeapStats().Committed;
    void *first = Memory::Allocate(Size);
    MemSet(first, 1, Size);
    Memory::Free(first);
    MARLIN_EXPECT(Memory::GetHeapStats().Committed >= before + Size);
    const usize retained = Memory::GetHeapStats().Committed;
    bool sameBlock = true;
    for (int32 frame = 0; frame < 20; ++frame)
    {
        void *block = Memory::Allocate(Size);
        MemSet(block, static_cast<uint8>(frame), Size);
        sameBlock = sameBlock && block == first;
        Memory::Free(block);
    }
    MARLIN_EXPECT(sameBlock && Memory::GetHeapStats().Committed <= retained);
    Memory::Trim();
}

MARLIN_TEST(Memory, RemoteFreesReturnFullPages)
{
    // Fills small pages here, frees every block on another thread, then allocates again: the full pages come
    // back through the blocks handed to this heap, without new pages.
    constexpr uint32 Count = 8192;
    constexpr usize Size = 256;
    static void *pointers[Count];
    for (int32 round = 0; round < 3; ++round)
    {
        for (void *&pointer : pointers)
        {
            pointer = Memory::Allocate(Size);
            MemSet(pointer, 0x5A, Size);
        }
        const usize committed = Memory::GetHeapStats().Committed;
        RemoteFrees frees{pointers, Count};
        ThreadDesc desc;
        desc.Entry = FreeAll;
        desc.Argument = &frees;
        PlatformThread::Join(PlatformThread::Create(desc));
        bool intact = true;
        for (void *&pointer : pointers)
        {
            pointer = Memory::Allocate(Size);
            MemSet(pointer, 0x3C, Size);
        }
        for (void *pointer : pointers)
        {
            intact = intact && IsFilled(pointer, Size, 0x3C);
        }
        MARLIN_EXPECT(intact && Memory::GetHeapStats().Committed <= committed);
        for (void *pointer : pointers)
        {
            Memory::Free(pointer);
        }
    }
}
} // namespace Marlin::Tests
