#include "Concurrency/Atomic.h"
#include "HAL/PlatformThread.h"
#include "RHI/GpuMemoryAllocator.h"
#include "RHI/RhiBreadcrumbs.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
/// Hands out fake blocks slowly, so threads that find a pool full overlap while one is made.
class SlowMemorySource final : public IGpuMemorySource
{
public:
    Result<GpuMemoryBlock, RhiError> AllocateBlock(uint32 pool, uint64 size, const void * /*resource*/) override
    {
        PlatformThread::Sleep(20);
        GpuMemoryBlock block;
        block.Native = Blocks.FetchAdd(1) + 1;
        block.Size = size;
        block.Pool = pool;
        return block;
    }

    void FreeBlock(const GpuMemoryBlock & /*block*/) override
    {
    }

    Atomic<uint32> Blocks{0};
};

struct AllocatingThread
{
    GpuMemoryAllocator *Allocator = nullptr;
    Atomic<uint32> *Started = nullptr;
    GpuAllocation Allocation;

    static void Run(void *argument)
    {
        auto *self = static_cast<AllocatingThread *>(argument);
        self->Started->FetchAdd(1);
        while (self->Started->Load() < 4)
        {
        }
        GpuMemoryRequest request;
        request.Size = 4096;
        Result<GpuAllocation, RhiError> allocation = self->Allocator->Allocate(request);
        if (allocation.IsOk())
        {
            self->Allocation = allocation.Value();
        }
    }
};
} // namespace

MARLIN_TEST(GpuMemoryAllocator, ThreadsFindingAPoolFullMakeOneBlock)
{
    SlowMemorySource source;
    const GpuMemoryPoolDesc pool{uint64{1} << 20, RhiMemoryLocation::Device};
    GpuMemoryAllocator allocator(source, Span<const GpuMemoryPoolDesc>(&pool, 1));
    Atomic<uint32> started{0};
    AllocatingThread threads[4];
    ThreadHandle handles[4];
    for (uint32 i = 0; i < 4; ++i)
    {
        threads[i].Allocator = &allocator;
        threads[i].Started = &started;
        ThreadDesc desc;
        desc.Name = "GpuMemoryAllocatorTest";
        desc.Entry = &AllocatingThread::Run;
        desc.Argument = &threads[i];
        handles[i] = PlatformThread::Create(desc);
        MARLIN_REQUIRE(handles[i].IsValid());
    }
    for (const ThreadHandle handle : handles)
    {
        PlatformThread::Join(handle);
    }
    MARLIN_EXPECT(source.Blocks.Load() == 1);
    MARLIN_EXPECT(allocator.GetStats(0).BlockCount == 1 && allocator.GetStats(0).AllocationCount == 4);
    for (const AllocatingThread &thread : threads)
    {
        MARLIN_EXPECT(thread.Allocation.IsValid());
        allocator.Free(thread.Allocation);
    }
}

MARLIN_TEST(RhiBreadcrumbs, ReusesReleasedTrails)
{
    uint32 slots[4] = {};
    RhiBreadcrumbs breadcrumbs(slots, 0x1000, 4);
    RhiBreadcrumbs::Trail *first = breadcrumbs.BeginTrail(RhiQueueType::Compute);
    MARLIN_REQUIRE(first != nullptr);
    MARLIN_EXPECT(breadcrumbs.BeginMarker(*first, Name("Pass")) == 1);
    breadcrumbs.Submitted(*first, 7);
    RhiQueueValues completed;
    completed.Values[static_cast<uint32>(RhiQueueType::Compute)] = 7;
    breadcrumbs.Retire(completed);

    RhiBreadcrumbs::Trail *second = breadcrumbs.BeginTrail(RhiQueueType::Graphics);
    MARLIN_REQUIRE(second != nullptr);
    MARLIN_EXPECT(second == first);
    MARLIN_EXPECT(second->Events.IsEmpty() && second->Depth == 0 && !second->Submitted);
    MARLIN_EXPECT(second->Queue == RhiQueueType::Graphics);
    MARLIN_EXPECT(breadcrumbs.BeginMarker(*second, Name("Other")) == 1);
    breadcrumbs.Discard(second);
}
} // namespace Marlin::Tests
