//! The fiber job system: submitting jobs, waiting on counters, parallel loops and worker pinning.
//!
//! Jobs run on fibers owned by worker threads. A job that waits parks its fiber so the worker can run other work,
//! and the job may resume on a different worker afterwards. While the system is stopped, jobs run immediately on the
//! calling thread.

#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "Containers/Span.h"
#include "CoreTypes.h"

namespace Marlin
{
/// A job body: a function that receives the job's argument and returns nothing.
///
/// The argument is whatever `JobDecl::Argument` holds; it may be null.
using JobEntry = void (*)(void *argument);

/// The priority that workers use when choosing among queued jobs.
///
/// Workers take a `High` job before a `Normal` job and a `Normal` job before a `Low` job. Priority is a
/// preference, not a global order: a running job is never preempted, and concurrent workers do not coordinate.
enum class JobPriority : uint8
{
    /// The highest priority, taken before `Normal` and `Low` jobs.
    High,
    /// The default priority, taken after `High` jobs.
    Normal,
    /// The lowest priority, taken after `High` and `Normal` jobs.
    Low,
};

/// One job to run.
///
/// When the job system queues a job, it copies the declaration. The argument is copied as a pointer, so the object
/// it points to must stay alive and unchanged until the job finishes.
struct JobDecl
{
    /// The function to run. Must not be null.
    JobEntry Entry = nullptr;
    /// Passed to `Entry` unchanged. May be null; the object it points to must outlive the job.
    void *Argument = nullptr;
    /// The priority used when queueing the job.
    JobPriority Priority = JobPriority::Normal;
    /// Runs the job on a large fiber instead of a small one; the stacks are 512 KiB and 64 KiB by default.
    ///
    /// Use it for jobs that need a deeper stack. [JobSystemDesc::SmallStackSize] and [JobSystemDesc::LargeStackSize]
    /// configure the two sizes. Large-stack jobs have their own queues, so a small fiber never waits for one.
    bool LargeStack = false;
};

namespace Private
{
struct JobWaiter;
}

/// Counts unfinished jobs so that a submitter can wait for them.
///
/// `Jobs::Run` adds the number of jobs it submits, and each job subtracts one when it finishes. `Jobs::Wait`
/// waits for the count to reach zero or a target. The counter is non-copyable and non-movable.
///
/// # Thread safety
/// Safe to use from any thread. `Jobs::Wait` parks the calling job's fiber on a worker or blocks a thread that is
/// not a worker.
class CORE_API JobCounter
{
public:
    /// Creates a counter at zero.
    JobCounter() = default;
    /// Waits for jobs that are still finishing with the counter, then destroys it.
    ///
    /// The count can reach zero slightly before the last job leaves the scheduler, so destruction waits for those
    /// jobs. It does not wait for jobs that are still queued.
    ///
    /// # Preconditions
    /// All jobs that use the counter have been submitted and the count has reached zero.
    ~JobCounter();
    MARLIN_NONCOPYABLE(JobCounter);

    /// Returns the current count.
    ///
    /// Another thread can change the count immediately after it is read.
    [[nodiscard]] uint32 GetValue() const
    {
        return _value.Load(MemoryOrder::Acquire);
    }

    /// Returns whether the count is zero.
    [[nodiscard]] bool IsDone() const
    {
        return GetValue() == 0;
    }

private:
    friend struct JobSystemAccess;

    Atomic<uint32> _value{0};
    /// Threads inside the scheduler with this counter; it must not be destroyed until they leave.
    Atomic<uint32> _busy{0};
    Atomic<Private::JobWaiter *> _waiters{nullptr};
    SpinLock _lock;
};

/// The configuration passed to [JobSystem::Start].
struct JobSystemDesc
{
    /// The number of worker threads, or zero for one per physical core less the reserved cores and the thread that
    /// starts the system. The result is at least one worker.
    uint32 WorkerCount = 0;
    /// The number of physical cores left to the OS. Consoles set it from platform configuration.
    uint32 ReservedCores = 0;
    /// The number of small-stack fibers created at start. Defaults to 160.
    uint32 SmallFiberCount = 160;
    /// The number of large-stack fibers created at start. Defaults to 32.
    uint32 LargeFiberCount = 32;
    /// The stack size of a small fiber, in bytes. Defaults to 64 KiB.
    usize SmallStackSize = usize{64} * 1024;
    /// The stack size of a large fiber, in bytes. Defaults to 512 KiB.
    usize LargeStackSize = usize{512} * 1024;
    /// The number of jobs that each priority's normal-stack queue holds. Defaults to 16384.
    ///
    /// The queue rounds the capacity up to a power of two. When a queue is full, `Jobs::Run` runs queued jobs on
    /// the calling thread when it can, to make room.
    uint32 QueueCapacity = 16384;
    /// The number of jobs that each priority's large-stack queue holds. Defaults to 2048.
    ///
    /// Large-stack jobs queue apart so that small fibers never wait for a large one. The queue rounds the capacity
    /// up to a power of two.
    uint32 LargeQueueCapacity = 2048;
};

/// Starts, stops and reports the state of the worker threads.
///
/// The owning subsystem starts the system once at startup and stops it at shutdown. While the system is stopped,
/// `Jobs::Run` executes jobs immediately on the calling thread.
///
/// # Thread safety
/// `Start` and `Stop` must not run concurrently with any other call. The query functions may be called while the
/// system runs, but not while it starts or stops.
class CORE_API JobSystem
{
public:
    /// Starts the worker threads and the fiber pools.
    ///
    /// Returns true after the worker threads are created.
    ///
    /// # Preconditions
    /// The system is stopped; this is checked by an assertion in builds with assertions.
    ///
    /// # Errors
    /// The current implementation has no failure path and always returns true once its preconditions hold.
    static bool Start(const JobSystemDesc &desc = {});
    /// Stops the workers and destroys the fiber pools.
    ///
    /// # Preconditions
    /// Every job has finished. The caller must not be a worker thread; this is checked by an assertion in builds
    /// with assertions.
    static void Stop();
    /// Returns whether the job system is running.
    [[nodiscard]] static bool IsRunning();
    /// Returns the number of worker threads, or zero when the system is stopped.
    [[nodiscard]] static uint32 GetWorkerCount();
    /// Returns the number of overflow fibers created since the system started.
    ///
    /// An overflow fiber is created when every pooled fiber is parked. A nonzero count means the fiber pool was too
    /// small for the load; the fibers stay alive until `Stop`. Returns zero when the system is stopped.
    [[nodiscard]] static uint32 GetOverflowFiberCount();
};

/// Submits jobs and waits for them.
namespace Jobs
{
/// Queues `jobs` and adds their count to `counter`.
///
/// The declarations are copied into the queue; the objects their arguments point to are not. When the job system
/// is stopped, runs the jobs immediately on the calling thread. When a queue is full, it may run queued jobs on the
/// calling thread to make room, so this call can execute work before it returns.
///
/// `counter` may be null. Otherwise it must outlive every job in `jobs` and must reach zero before it is
/// destroyed.
///
/// # Preconditions
/// No `JobDecl::Entry` is null. The queueing path checks this with an assertion in builds with assertions; a null
/// entry is undefined otherwise. Every argument stays valid until its job finishes.
///
/// # Thread safety
/// Safe to call from any thread, including from inside a job.
///
/// # Examples
///
/// ```cpp
/// void Work(void *argument)
/// {
///     *static_cast<uint32 *>(argument) = 42;
/// }
///
/// uint32 value = 0;
/// JobCounter counter;
/// Jobs::Run({Work, &value}, &counter);
/// Jobs::Wait(counter);
/// const uint32 result = value; // -> 42
/// ```
CORE_API void Run(Span<const JobDecl> jobs, JobCounter *counter = nullptr);

/// Queues one job and adds one to `counter`.
///
/// Equivalent to `Run(Span<const JobDecl>(&job, 1), counter)`.
inline void Run(const JobDecl &job, JobCounter *counter = nullptr)
{
    Run(Span<const JobDecl>(&job, 1), counter);
}

/// Waits until `counter` is at most `target`.
///
/// On a worker, parks the calling job's fiber so the worker can run other work; the job resumes when the counter
/// reaches the target. Off the workers, runs queued jobs while it waits and then sleeps until a finished job wakes
/// it. There is no timeout.
///
/// `target` is usually zero. A larger value waits until only that many jobs remain; passing a value above the
/// current count returns immediately.
///
/// # Thread safety
/// Safe to call from any thread, including from inside a job.
CORE_API void Wait(JobCounter &counter, uint32 target = 0);

/// Returns true when the caller runs inside a job on a worker thread.
[[nodiscard]] CORE_API bool IsInJob();

/// Returns the index of the worker running the caller, or -1 off the workers.
///
/// A wait can resume the job on another worker, so the index can change after `Jobs::Wait` unless a [JobScope]
/// pin keeps the job in place.
[[nodiscard]] CORE_API int32 GetWorkerIndex();

/// Runs `body(begin, end)` over `[0, count)` in batches and waits for every batch to finish.
///
/// Each call receives a contiguous half-open range of at most `batchSize` elements; zero is treated as 1, and the
/// last call may be shorter. A `count` of zero makes no calls. The calls can run on several threads, including the
/// calling thread when it helps, so `body` must synchronize shared state.
///
/// When the job system is stopped, the whole range runs serially on the calling thread.
///
/// # Thread safety
/// Safe to call from any thread, including from inside a job. `body` is called concurrently from several threads.
///
/// # Examples
///
/// ```cpp
/// Atomic<uint64> total{0};
/// Jobs::ParallelFor(1000, 100, [&](uint32 begin, uint32 end) {
///     total.FetchAdd(static_cast<uint64>(end - begin), MemoryOrder::Relaxed);
/// });
/// const uint64 sum = total.Load(); // -> 1000
/// ```
template <typename TBody> void ParallelFor(uint32 count, uint32 batchSize, const TBody &body)
{
    if (count == 0)
    {
        return;
    }
    batchSize = batchSize == 0 ? 1 : batchSize;
    struct Context
    {
        const TBody *Body = nullptr;
        uint32 Count = 0;
        uint32 BatchSize = 0;
        Atomic<uint32> Next{0};
    };
    Context context{&body, count, batchSize};
    const uint32 batches = (count + batchSize - 1) / batchSize;
    const uint32 workers = JobSystem::GetWorkerCount() + 1;
    const uint32 jobCount = batches < workers ? batches : workers;
    const JobEntry entry = [](void *argument) {
        auto *shared = static_cast<Context *>(argument);
        while (true)
        {
            const uint32 begin = shared->Next.FetchAdd(shared->BatchSize, MemoryOrder::Relaxed);
            if (begin >= shared->Count)
            {
                return;
            }
            const uint32 end = shared->Count - begin < shared->BatchSize ? shared->Count : begin + shared->BatchSize;
            (*shared->Body)(begin, end);
        }
    };
    JobDecl decls[64];
    JobCounter counter;
    for (uint32 started = 0; started < jobCount;)
    {
        uint32 chunk = 0;
        for (; chunk < 64 && started + chunk < jobCount; ++chunk)
        {
            decls[chunk] = {entry, &context};
        }
        Run(Span<const JobDecl>(decls, chunk), &counter);
        started += chunk;
    }
    Wait(counter);
}
} // namespace Jobs

/// Keeps the running job on its current worker across waits.
///
/// A job can resume on another worker after a wait. Pinning is for code that holds an OS mutex or calls
/// thread-affine platform APIs, where moving threads would break the contract. Create one with `JobScope::Pin()`
/// and keep it alive for as long as the pin is needed; destroying it restores the previous pin. The scope is
/// move-only and is inactive when created off a worker or outside a job; destroying an inactive scope does nothing.
class CORE_API JobScope
{
public:
    /// Pins the calling job to the worker that runs it.
    ///
    /// Returns an inactive scope when the caller is not a job on a worker. Pins nest; destroying an inner scope
    /// restores the pin that was active before it.
    [[nodiscard]] static JobScope Pin();
    /// Restores the pin that was active before this scope; does nothing for an inactive scope.
    ~JobScope();
    /// Transfers the pin from `other`; the moved-from scope becomes inactive.
    JobScope(JobScope &&other) noexcept : _active(other._active), _previous(other._previous)
    {
        other._active = false;
    }
    JobScope(const JobScope &) = delete;
    JobScope &operator=(const JobScope &) = delete;
    JobScope &operator=(JobScope &&) = delete;

private:
    JobScope(bool active, int32 previous) : _active(active), _previous(previous)
    {
    }

    bool _active;
    int32 _previous;
};
} // namespace Marlin
