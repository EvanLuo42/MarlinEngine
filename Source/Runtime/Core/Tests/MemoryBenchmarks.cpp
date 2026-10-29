#include "Testing/Testing.h"

#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
template <typename TFunction> float64 BestOf(int32 repeats, TFunction function)
{
    float64 best = 1e30;
    for (int32 i = 0; i < repeats; ++i)
    {
        const float64 start = PlatformTime::Seconds();
        function();
        const float64 elapsed = PlatformTime::Seconds() - start;
        best = elapsed < best ? elapsed : best;
    }
    return best;
}

void Report(StringView name, float64 seconds, StringView unit, float64 count)
{
    Testing::Print(Format("{0,-46} {1,10:F2} ms {2,10:F1} {3}\n", name, seconds * 1000, seconds * 1e9 / count, unit));
}

struct Blocks
{
    void **Pointers;
    uint32 Count;
};

void FreeBlocks(void *argument)
{
    const auto &blocks = *static_cast<Blocks *>(argument);
    for (uint32 i = 0; i < blocks.Count; ++i)
    {
        Memory::Free(blocks.Pointers[i]);
    }
}
} // namespace

MARLIN_TEST(Benchmark, Memory, Benchmark)
{
    // A large block allocated, touched and freed every frame, as for a per-frame staging buffer.
    constexpr usize LargeSize = (usize{3} * 1024 * 1024) + 4096;
    constexpr int32 Frames = 200;
    const float64 large = BestOf(5, [] {
        for (int32 frame = 0; frame < Frames; ++frame)
        {
            void *block = Memory::Allocate(LargeSize);
            MemSet(block, 1, LargeSize);
            Memory::Free(block);
        }
    });
    Report("3 MiB block, allocate + touch + free", large, "ns/frame", Frames);

    // Many long-lived small blocks fill pages; short-lived ones freed on another thread must come back cheaply.
    constexpr uint32 LongLived = 256 * 1024;
    constexpr uint32 Transient = 64 * 1024;
    constexpr usize Size = 64;
    auto **kept = static_cast<void **>(Memory::Allocate(LongLived * sizeof(void *)));
    auto **transient = static_cast<void **>(Memory::Allocate(Transient * sizeof(void *)));
    for (uint32 i = 0; i < LongLived; ++i)
    {
        kept[i] = Memory::Allocate(Size);
    }
    for (uint32 i = 0; i < Transient; ++i)
    {
        transient[i] = Memory::Allocate(Size);
    }
    const float64 remote = BestOf(5, [&] {
        Blocks blocks{transient, Transient};
        ThreadDesc desc;
        desc.Entry = FreeBlocks;
        desc.Argument = &blocks;
        PlatformThread::Join(PlatformThread::Create(desc));
        for (uint32 i = 0; i < Transient; ++i)
        {
            transient[i] = Memory::Allocate(Size);
        }
    });
    Report("64k small blocks freed remotely, then reused", remote, "ns/block", Transient);
    for (uint32 i = 0; i < Transient; ++i)
    {
        Memory::Free(transient[i]);
    }
    for (uint32 i = 0; i < LongLived; ++i)
    {
        Memory::Free(kept[i]);
    }
    Memory::Free(transient);
    Memory::Free(kept);
}
} // namespace Marlin::Tests
