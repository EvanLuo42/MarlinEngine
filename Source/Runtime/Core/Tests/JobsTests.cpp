#include "Concurrency/Atomic.h"
#include "Concurrency/FiberLocal.h"
#include "Concurrency/Jobs.h"
#include "Concurrency/ThreadLocal.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "String/Format.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{

constexpr uint64 SumBits = 20;
Atomic<uint32> _executed{0};
Atomic<uint32> _localMismatches{0};
Atomic<uint32> _pinMismatches{0};
FiberLocal<uint64> _jobLocal;
ThreadLocal<uint32> _perThread;

void Increment(void * /*argument*/)
{
    _executed.FetchAdd(1);
}

/// Sums [0, n) by splitting in two jobs and waiting, to depth; checks the FiberLocal survives waits.
struct SumTask
{
    uint64 Begin;
    uint64 End;
    uint64 Result;
};

void Sum(void *argument)
{
    auto *task = static_cast<SumTask *>(argument);
    MARLIN_EXPECT(_jobLocal.Get() == 0);
    _jobLocal.Set(task->Begin + 1);
    if (task->End - task->Begin <= 64)
    {
        uint64 sum = 0;
        for (uint64 i = task->Begin; i < task->End; ++i)
        {
            sum += i;
        }
        task->Result = sum;
    }
    else
    {
        const uint64 middle = (task->Begin + task->End) / 2;
        SumTask halves[2] = {{task->Begin, middle, 0}, {middle, task->End, 0}};
        const JobDecl decls[2] = {{Sum, &halves[0]}, {Sum, &halves[1], JobPriority::High}};
        JobCounter counter;
        Jobs::Run(Span<const JobDecl>(decls), &counter);
        Jobs::Wait(counter);
        task->Result = halves[0].Result + halves[1].Result;
    }
    if (_jobLocal.Get() != task->Begin + 1)
    {
        _localMismatches.FetchAdd(1);
    }
}

void DeepStack(void *argument)
{
    volatile uint8 buffer[256 * 1024];
    buffer[0] = 1;
    buffer[sizeof(buffer) - 1] = 2;
    *static_cast<uint32 *>(argument) = buffer[0] + buffer[sizeof(buffer) - 1];
}

void Sleepy(void * /*argument*/)
{
    PlatformThread::Sleep(20);
    _executed.FetchAdd(1);
}

/// Waits on a small fiber for high-priority large-stack jobs, which only large fibers take.
void RunLargeHigh(void *argument)
{
    auto *deep = static_cast<uint32 *>(argument);
    JobDecl large[4];
    for (uint32 i = 0; i < 4; ++i)
    {
        large[i] = {DeepStack, &deep[i], JobPriority::High, true};
    }
    JobCounter counter;
    Jobs::Run(Span<const JobDecl>(large), &counter);
    Jobs::Wait(counter);
}

void Pinned(void * /*argument*/)
{
    const JobScope pin = JobScope::Pin();
    const int32 before = Jobs::GetWorkerIndex();
    for (int32 round = 0; round < 20; ++round)
    {
        JobCounter counter;
        Jobs::Run({Increment, nullptr}, &counter);
        Jobs::Wait(counter);
        if (Jobs::GetWorkerIndex() != before)
        {
            _pinMismatches.FetchAdd(1);
        }
    }
}
} // namespace

MARLIN_TEST(Jobs, System)
{
    {
        JobCounter counter;
        Jobs::Run({Increment, nullptr}, &counter);
        MARLIN_EXPECT(counter.IsDone() && _executed.Load() == 1);
        uint64 total = 0;
        Jobs::ParallelFor(1000, 7, [&](uint32 begin, uint32 end) { total += end - begin; });
        MARLIN_EXPECT(total == 1000);
    }

    JobSystemDesc desc;
    MARLIN_EXPECT(JobSystem::Start(desc));
    MARLIN_EXPECT(JobSystem::GetWorkerCount() >= 1);
    {
        _executed.Store(0);
        JobDecl decls[1000];
        for (JobDecl &decl : decls)
        {
            decl = {Increment, nullptr, JobPriority::Low};
        }
        JobCounter counter;
        for (int32 i = 0; i < 100; ++i)
        {
            Jobs::Run(Span<const JobDecl>(decls), &counter);
        }
        Jobs::Wait(counter);
        MARLIN_EXPECT(_executed.Load() == 100000);

        SumTask task{0, uint64{1} << SumBits, 0};
        JobCounter sumCounter;
        Jobs::Run({Sum, &task}, &sumCounter);
        Jobs::Wait(sumCounter);
        MARLIN_EXPECT(task.Result == (uint64{1} << SumBits) * ((uint64{1} << SumBits) - 1) / 2);
        MARLIN_EXPECT(_localMismatches.Load() == 0);

        uint32 deep[8] = {};
        JobDecl large[8];
        for (uint32 i = 0; i < 8; ++i)
        {
            large[i] = {DeepStack, &deep[i], JobPriority::Normal, true};
        }
        JobCounter largeCounter;
        Jobs::Run(Span<const JobDecl>(large), &largeCounter);
        Jobs::Wait(largeCounter);
        for (const uint32 value : deep)
        {
            MARLIN_EXPECT(value == 3);
        }

        uint32 deepHigh[4] = {};
        JobCounter largeHighCounter;
        Jobs::Run({RunLargeHigh, deepHigh}, &largeHighCounter);
        Jobs::Wait(largeHighCounter);
        for (const uint32 value : deepHigh)
        {
            MARLIN_EXPECT(value == 3);
        }

        // Idle workers sleep with no timeout, so work after a quiet spell must still wake them; the wait here, off
        // the workers, helps and then blocks until the last job wakes it.
        PlatformThread::Sleep(30);
        _executed.Store(0);
        const JobDecl sleepy[4] = {{Sleepy, nullptr}, {Sleepy, nullptr}, {Sleepy, nullptr}, {Sleepy, nullptr}};
        JobCounter sleepyCounter;
        Jobs::Run(Span<const JobDecl>(sleepy), &sleepyCounter);
        Jobs::Wait(sleepyCounter);
        MARLIN_EXPECT(_executed.Load() == 4 && sleepyCounter.IsDone());

        JobDecl pinned[16];
        for (JobDecl &decl : pinned)
        {
            decl = {Pinned, nullptr};
        }
        JobCounter pinnedCounter;
        Jobs::Run(Span<const JobDecl>(pinned), &pinnedCounter);
        Jobs::Wait(pinnedCounter);
        MARLIN_EXPECT(_pinMismatches.Load() == 0);

        Atomic<uint64> parallelSum{0};
        Jobs::ParallelFor(100000, 100, [&](uint32 begin, uint32 end) {
            uint64 sum = 0;
            for (uint32 i = begin; i < end; ++i)
            {
                sum += i;
            }
            parallelSum.FetchAdd(sum);
            ++_perThread.Get();
        });
        MARLIN_EXPECT(parallelSum.Load() == uint64{100000} * 99999 / 2);
        MARLIN_EXPECT(!Jobs::IsInJob() && Jobs::GetWorkerIndex() == -1);
    }
    JobSystem::Stop();
    MARLIN_EXPECT(!JobSystem::IsRunning());
}

MARLIN_TEST(Atomic, RefCompareExchange)
{
    uint32 value = 5;
    const AtomicRef<uint32> ref(value);
    uint32 expected = 4;
    MARLIN_EXPECT(!ref.CompareExchange(expected, 9) && expected == 5 && value == 5);
    MARLIN_EXPECT(ref.CompareExchange(expected, 9) && value == 9);

    // Many threads adding through a retry loop lose no update.
    const bool started = JobSystem::Start();
    uint32 total = 0;
    Jobs::ParallelFor(10000, 16, [&](uint32 begin, uint32 end) {
        const AtomicRef<uint32> shared(total);
        for (uint32 i = begin; i < end; ++i)
        {
            uint32 seen = shared.Load(MemoryOrder::Relaxed);
            while (!shared.CompareExchangeWeak(seen, seen + 1, MemoryOrder::Relaxed))
            {
            }
        }
    });
    if (started)
    {
        JobSystem::Stop();
    }
    MARLIN_EXPECT(total == 10000);
}
} // namespace Marlin::Tests
