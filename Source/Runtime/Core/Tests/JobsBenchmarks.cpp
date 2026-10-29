#include "Testing/Testing.h"

#include "Concurrency/Jobs.h"
#include "HAL/PlatformTime.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
volatile uint64 _benchSink = 0;
Atomic<uint64> _benchCounter{0};

void Empty(void * /*argument*/)
{
}

void Touch(void * /*argument*/)
{
    _benchCounter.FetchAdd(1, MemoryOrder::Relaxed);
}

template <typename TFunction> float64 Best(int32 repeats, TFunction function)
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

/// Work that does not touch memory, so scaling measures the scheduler.
uint64 Spin(uint64 seed, uint32 rounds)
{
    for (uint32 i = 0; i < rounds; ++i)
    {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        seed ^= seed >> 29;
    }
    return seed;
}

struct ChainTask
{
    uint32 Rounds;
};

void WaitChain(void *argument)
{
    const auto *task = static_cast<ChainTask *>(argument);
    for (uint32 i = 0; i < task->Rounds; ++i)
    {
        JobCounter counter;
        Jobs::Run({Touch, nullptr, JobPriority::High}, &counter);
        Jobs::Wait(counter);
    }
}

struct FanTask
{
    uint32 Depth;
};

void Fan(void *argument)
{
    const auto *task = static_cast<FanTask *>(argument);
    if (task->Depth == 0)
    {
        _benchSink = _benchSink + Spin(task->Depth, 200);
        return;
    }
    FanTask children[4] = {{task->Depth - 1}, {task->Depth - 1}, {task->Depth - 1}, {task->Depth - 1}};
    JobDecl decls[4];
    for (uint32 i = 0; i < 4; ++i)
    {
        decls[i] = {Fan, &children[i]};
    }
    JobCounter counter;
    Jobs::Run(Span<const JobDecl>(decls), &counter);
    Jobs::Wait(counter);
}

struct RunTask
{
    JobDecl *Decls;
    uint32 Count;
};

void SubmitAndWait(void *argument)
{
    const auto *task = static_cast<RunTask *>(argument);
    JobCounter counter;
    for (uint32 i = 0; i < task->Count; i += 1000)
    {
        Jobs::Run(Span<const JobDecl>(task->Decls + i, 1000), &counter);
    }
    Jobs::Wait(counter);
}
} // namespace

MARLIN_TEST(Benchmark, Jobs, Benchmark)
{
    JobSystem::Start();
    const uint32 workers = JobSystem::GetWorkerCount();
    Testing::Print(Format("{0} workers\n", workers));

    constexpr uint32 JobCount = 1000000;
    static JobDecl decls[JobCount];
    for (JobDecl &decl : decls)
    {
        decl = {Empty, nullptr};
    }

    Report("1M empty jobs, submitted from main",
           Best(5,
                [] {
                    JobCounter counter;
                    for (uint32 i = 0; i < JobCount; i += 1000)
                    {
                        Jobs::Run(Span<const JobDecl>(decls + i, 1000), &counter);
                    }
                    Jobs::Wait(counter);
                }),
           "ns/job", JobCount);

    Report("1M empty jobs, submitted from a job",
           Best(5,
                [] {
                    RunTask task{decls, JobCount};
                    JobCounter counter;
                    Jobs::Run({SubmitAndWait, &task}, &counter);
                    Jobs::Wait(counter);
                }),
           "ns/job", JobCount);

    Report("100k run+wait in a job (park/resume)",
           Best(5,
                [] {
                    ChainTask task{100000};
                    JobCounter counter;
                    Jobs::Run({WaitChain, &task}, &counter);
                    Jobs::Wait(counter);
                }),
           "ns/wait", 100000);

    Report("100k run+wait from main (helps inline)",
           Best(5,
                [] {
                    for (uint32 i = 0; i < 100000; ++i)
                    {
                        JobCounter counter;
                        Jobs::Run({Touch, nullptr}, &counter);
                        Jobs::Wait(counter);
                    }
                }),
           "ns/wait", 100000);

    constexpr uint32 Items = 1 << 20;
    constexpr uint32 Rounds = 64;
    const float64 serial = Best(3, [] {
        uint64 sum = 0;
        for (uint32 i = 0; i < Items; ++i)
        {
            sum += Spin(i, Rounds);
        }
        _benchSink = _benchSink + sum;
    });
    Report("1M items x 64 rounds, serial", serial, "ns/item", Items);
    for (const uint32 batch : {64u, 1024u, 16384u})
    {
        const float64 parallel = Best(3, [batch] {
            Atomic<uint64> total{0};
            Jobs::ParallelFor(Items, batch, [&](uint32 begin, uint32 end) {
                uint64 sum = 0;
                for (uint32 i = begin; i < end; ++i)
                {
                    sum += Spin(i, Rounds);
                }
                total.FetchAdd(sum, MemoryOrder::Relaxed);
            });
            _benchSink = _benchSink + total.Load();
        });
        Report(Format("same, ParallelFor batch {0} ({1:F2}x)", batch, serial / parallel), parallel, "ns/item", Items);
    }

    const float64 fan = Best(5, [] {
        FanTask root{7};
        JobCounter counter;
        Jobs::Run({Fan, &root}, &counter);
        Jobs::Wait(counter);
    });
    Report(Format("4-way fan-out depth 7 (21845 jobs, {0} overflow)", JobSystem::GetOverflowFiberCount()), fan,
           "ns/job", 21845);

    JobSystem::Stop();
}
} // namespace Marlin::Tests
