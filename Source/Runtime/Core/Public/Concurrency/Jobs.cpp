#include "Concurrency/Jobs.h"

#include "Concurrency/ExecutionContext.h"
#include "Concurrency/FiberLocal.h"
#include "Concurrency/MpmcQueue.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/List.h"
#include "Diagnostics/Profiler.h"
#include "HAL/PlatformCpu.h"
#include "HAL/PlatformFiber.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "Memory/Memory.h"
#include "String/Format.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace Private
{
struct JobFiber;

/// A waiter linked into its counter's list; lives on the waiting fiber's or thread's stack.
struct JobWaiter
{
    /// The parked fiber, or null for a thread off the workers, which sleeps on Woken.
    JobFiber *Fiber;
    uint32 Target;
    JobWaiter *Next;
    Atomic<uint32> Woken{0};
};

struct QueuedJob // NOLINT(misc-use-internal-linkage)
{
    JobEntry Entry;
    void *Argument;
    JobCounter *Counter;
    uint16 Tag;
};

struct JobFiber
{
    FiberHandle Handle;
    ExecutionContext Context;
    bool Large = false;
    int32 PinnedWorker = -1;
    /// The top of the stack, taken when the fiber first runs, to bound how deep jobs run inline on it.
    uintptr StackTop = 0;
    usize StackSize = 0;
    /// What profilers show for this fiber.
    char Name[24] = "Overflow fiber";
    /// A job handed over by the fiber that switched here, for a job that needs this fiber's stack.
    bool HasJob = false;
    QueuedJob Job{};
};
} // namespace Private

/// Reaches JobCounter's internals for the scheduler.
struct JobSystemAccess
{
    static Atomic<uint32> &Value(JobCounter &counter)
    {
        return counter._value;
    }

    static Atomic<uint32> &Busy(JobCounter &counter)
    {
        return counter._busy;
    }

    static Atomic<Private::JobWaiter *> &Waiters(JobCounter &counter)
    {
        return counter._waiters;
    }

    static SpinLock &Lock(JobCounter &counter)
    {
        return counter._lock;
    }
};

namespace
{
using Private::JobFiber;
using Private::JobWaiter;
using Private::QueuedJob;

/// What the fiber that gets control next does with the one that just switched away, once it is off the CPU.
enum class AfterSwitch : uint8
{
    None,
    Free,
    Park,
};

struct Worker
{
    uint32 Index = 0;
    ThreadHandle Thread;
    FiberHandle ThreadFiber;
    uint64 AffinityMask = 0;
    MpmcQueue<JobFiber *> *Pinned = nullptr;
    /// Released once for each time a waker clears this worker's sleep bit.
    Semaphore *Wake = nullptr;
    JobFiber *Current = nullptr;

    AfterSwitch Pending = AfterSwitch::None;
    JobFiber *PendingFiber = nullptr;
    JobCounter *PendingCounter = nullptr;
    JobWaiter *PendingWaiter = nullptr;
};

constexpr uint32 PriorityCount = 3;

struct JobSystemState
{
    JobSystemDesc Desc;
    Worker *Workers = nullptr;
    uint32 WorkerCount = 0;
    JobFiber *Fibers = nullptr;
    uint32 FiberCount = 0;
    MpmcQueue<QueuedJob> *Queues[PriorityCount] = {};
    /// Large-stack jobs by priority, which only large fibers run.
    MpmcQueue<QueuedJob> *LargeQueues[PriorityCount] = {};
    MpmcQueue<JobFiber *> *Ready = nullptr;
    MpmcQueue<JobFiber *> *FreeSmall = nullptr;
    MpmcQueue<JobFiber *> *FreeLarge = nullptr;
    /// One bit per sleeping worker; see Idle for the protocol.
    Atomic<uint64> *SleepMasks = nullptr;
    uint32 SleepMaskCount = 0;
    /// Fibers made when the pool ran out; they join the small pool and live until Stop.
    SpinLock OverflowLock;
    List<JobFiber *> Overflow;
    Atomic<bool> Stopping{false};
};

/// Room in the ready and free queues for overflow fibers.
constexpr usize FiberQueueCapacity = usize{1} << 16;
/// Pause iterations an idle worker polls for work before it sleeps; waking a sleeper costs a system call.
constexpr uint32 IdleSpinCount = 4096;
/// Jobs a thread off the workers runs nested inside its waits.
constexpr uint32 MaxInlineDepth = 8;
/// Polls a thread off the workers makes for its counter or a job to help with, pausing and then yielding, before
/// it sleeps until the counter's jobs wake it.
constexpr uint32 HelpPauseCount = 64;
constexpr uint32 HelpYieldCount = 16;
/// Jobs Run claims queue cells for at once; workers wake after each batch, so a long submission starts early.
constexpr usize RunBatch = 256;

JobSystemState *_jobs = nullptr;
thread_local Worker *_worker = nullptr;
thread_local uint32 _inlineDepth = 0;

/// Reads the thread-local again after every call, since a fiber can resume on another thread.
MARLIN_NOINLINE Worker *CurrentWorker()
{
    return _worker;
}

uint16 CurrentTagSlot()
{
    const ExecutionContext &context = GetExecutionContext();
    return context.MemoryTagDepth == 0 ? 0 : context.MemoryTags[context.MemoryTagDepth - 1];
}

/// The `count` lowest set bits of `bits`.
uint64 LowestBits(uint64 bits, uint32 count)
{
    if (static_cast<uint32>(PopCount(bits)) <= count)
    {
        return bits;
    }
    uint64 lowest = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        const uint64 low = bits & (~bits + 1);
        lowest |= low;
        bits ^= low;
    }
    return lowest;
}

/// Wakes up to `count` sleeping workers. Call after publishing the work they should find.
void WakeWorkers(uint32 count)
{
    // Pairs with the fence in Idle: either a sleeper sees the work published before this, or this sees its bit.
    AtomicThreadFence();
    for (uint32 word = 0; word < _jobs->SleepMaskCount && count != 0; ++word)
    {
        Atomic<uint64> &mask = _jobs->SleepMasks[word];
        uint64 sleeping = mask.Load(MemoryOrder::Relaxed);
        uint64 claimed = 0;
        // One exchange claims every sleeper to wake in this word.
        while (sleeping != 0)
        {
            claimed = LowestBits(sleeping, count);
            if (mask.CompareExchangeWeak(sleeping, sleeping & ~claimed, MemoryOrder::AcquireRelease))
            {
                break;
            }
            claimed = 0;
        }
        count -= static_cast<uint32>(PopCount(claimed));
        for (; claimed != 0; claimed &= claimed - 1)
        {
            _jobs->Workers[(word * 64) + static_cast<uint32>(CountTrailingZeros(claimed))].Wake->Release(1);
        }
    }
}

/// A snapshot, without the fence that WakeWorkers needs to be sure.
bool IsAnyWorkerSleeping()
{
    for (uint32 word = 0; word < _jobs->SleepMaskCount; ++word)
    {
        if (_jobs->SleepMasks[word].Load(MemoryOrder::Relaxed) != 0)
        {
            return true;
        }
    }
    return false;
}

/// Wakes `index` if it sleeps, for work only it can run. Call after publishing that work.
void WakeWorker(uint32 index)
{
    AtomicThreadFence();
    Atomic<uint64> &mask = _jobs->SleepMasks[index / 64];
    const uint64 bit = uint64{1} << (index % 64);
    if ((mask.Load(MemoryOrder::Relaxed) & bit) != 0 && (mask.FetchAnd(~bit, MemoryOrder::AcquireRelease) & bit) != 0)
    {
        _jobs->Workers[index].Wake->Release(1);
    }
}

void MakeReady(JobFiber *fiber)
{
    const int32 pinned = fiber->PinnedWorker;
    MpmcQueue<JobFiber *> *queue = pinned >= 0 ? _jobs->Workers[pinned].Pinned : _jobs->Ready;
    while (!queue->Push(fiber))
    {
        PlatformThread::YieldThread();
    }
    if (pinned >= 0)
    {
        WakeWorker(static_cast<uint32>(pinned));
    }
    else
    {
        WakeWorkers(1);
    }
}

bool HasLargeJobs()
{
    for (const MpmcQueue<QueuedJob> *queue : _jobs->LargeQueues)
    {
        if (!queue->IsEmpty())
        {
            return true;
        }
    }
    return false;
}

/// Returns a large fiber to the pool. A small fiber may have passed over a large job for want of one, and its
/// worker gone to sleep, so a waiting large job wakes a worker.
void FreeLargeFiber(JobFiber *fiber)
{
    MARLIN_VERIFY(_jobs->FreeLarge->Push(fiber));
    AtomicThreadFence();
    if (HasLargeJobs())
    {
        WakeWorkers(1);
    }
}

/// Wakes a thread off the workers that sleeps in BlockUntil, or readies a parked fiber.
void Resume(JobWaiter *waiter)
{
    if (JobFiber *fiber = waiter->Fiber)
    {
        MakeReady(fiber);
        return;
    }
    // The waiter can return as soon as Woken is set; waking its old address then is a spurious wake at worst.
    waiter->Woken.Store(1, MemoryOrder::Release);
    PlatformSync::WakeOneOnAddress(&waiter->Woken);
}

/// Wakes the waiters on `counter` whose target it has reached; they are resumed after the lock is released.
void WakeWaiters(JobCounter &counter)
{
    JobWaiter *woken = nullptr;
    {
        const ScopeLock lock(JobSystemAccess::Lock(counter));
        const uint32 value = JobSystemAccess::Value(counter).Load();
        JobWaiter *previous = nullptr;
        JobWaiter *waiter = JobSystemAccess::Waiters(counter).Load(MemoryOrder::Relaxed);
        while (waiter != nullptr)
        {
            JobWaiter *next = waiter->Next;
            if (value <= waiter->Target)
            {
                if (previous == nullptr)
                {
                    JobSystemAccess::Waiters(counter).Store(next);
                }
                else
                {
                    previous->Next = next;
                }
                waiter->Next = woken;
                woken = waiter;
            }
            else
            {
                previous = waiter;
            }
            waiter = next;
        }
    }
    while (woken != nullptr)
    {
        JobWaiter *next = woken->Next;
        Resume(woken);
        woken = next;
    }
}

void CompleteJob(JobCounter *counter)
{
    if (counter == nullptr)
    {
        return;
    }
    // Relaxed suffices for Busy: the decrement below releases it to whoever sees the count drop. The decrement and
    // the read of Waiters stay sequentially consistent, pairing with a waiter that links itself and then reads the
    // count.
    JobSystemAccess::Busy(*counter).FetchAdd(1, MemoryOrder::Relaxed);
    JobSystemAccess::Value(*counter).FetchSub(1);
    if (JobSystemAccess::Waiters(*counter).Load() != nullptr)
    {
        WakeWaiters(*counter);
    }
    JobSystemAccess::Busy(*counter).FetchSub(1, MemoryOrder::Release);
}

/// Links `waiter` into `counter`'s list unless the counter has already reached its target; returns whether it did.
bool LinkWaiter(JobCounter &counter, JobWaiter &waiter)
{
    // Linking before reading the count pairs with CompleteJob, which decrements before reading the list.
    const ScopeLock lock(JobSystemAccess::Lock(counter));
    waiter.Next = JobSystemAccess::Waiters(counter).Load(MemoryOrder::Relaxed);
    JobSystemAccess::Waiters(counter).Store(&waiter);
    if (JobSystemAccess::Value(counter).Load() <= waiter.Target)
    {
        JobSystemAccess::Waiters(counter).Store(waiter.Next);
        return false;
    }
    return true;
}

void ProcessAfterSwitch(Worker *worker)
{
    const AfterSwitch pending = worker->Pending;
    worker->Pending = AfterSwitch::None;
    if (pending == AfterSwitch::Free)
    {
        JobFiber *fiber = worker->PendingFiber;
        if (fiber->Large)
        {
            FreeLargeFiber(fiber);
        }
        else
        {
            MARLIN_VERIFY(_jobs->FreeSmall->Push(fiber));
        }
    }
    else if (pending == AfterSwitch::Park)
    {
        JobCounter &counter = *worker->PendingCounter;
        JobWaiter *waiter = worker->PendingWaiter;
        JobSystemAccess::Busy(counter).FetchAdd(1);
        if (!LinkWaiter(counter, *waiter))
        {
            MakeReady(waiter->Fiber);
        }
        JobSystemAccess::Busy(counter).FetchSub(1, MemoryOrder::Release);
    }
}

void SwitchFiber(Worker *worker, JobFiber *from, JobFiber *to, AfterSwitch action)
{
    worker->Pending = action;
    worker->PendingFiber = from;
    worker->Current = to;
    SetExecutionContext(&to->Context);
    Private::ProfileFiberSwitch(to, to->Name);
    PlatformFiber::SwitchTo(to->Handle);
}

/// Takes the most urgent job this stack can run; large-stack jobs only on a large stack.
bool PopJob(QueuedJob &job, bool largeStack)
{
    for (uint32 priority = 0; priority < PriorityCount; ++priority)
    {
        if (_jobs->Queues[priority]->Pop(job) || (largeStack && _jobs->LargeQueues[priority]->Pop(job)))
        {
            return true;
        }
    }
    return false;
}

/// Pops a job for a fiber's main loop.
///
/// On a small fiber, a large-stack job comes with a free large fiber to run it, returned through `large`, so small
/// fibers never wait for one.
bool TakeJob(bool largeStack, QueuedJob &job, JobFiber *&large)
{
    large = nullptr;
    for (uint32 priority = 0; priority < PriorityCount; ++priority)
    {
        if (_jobs->Queues[priority]->Pop(job))
        {
            return true;
        }
        MpmcQueue<QueuedJob> *queue = _jobs->LargeQueues[priority];
        if (queue->IsEmpty())
        {
            continue;
        }
        if (largeStack)
        {
            if (queue->Pop(job))
            {
                return true;
            }
        }
        else if (_jobs->FreeLarge->Pop(large))
        {
            if (queue->Pop(job))
            {
                return true;
            }
            FreeLargeFiber(large);
            large = nullptr;
        }
    }
    return false;
}

/// Whether a worker on a fiber of this size may find something to run.
bool HasWork(const Worker *worker, bool largeStack)
{
    if (!worker->Pinned->IsEmpty() || !_jobs->Ready->IsEmpty())
    {
        return true;
    }
    for (const MpmcQueue<QueuedJob> *queue : _jobs->Queues)
    {
        if (!queue->IsEmpty())
        {
            return true;
        }
    }
    return (largeStack || !_jobs->FreeLarge->IsEmpty()) && HasLargeJobs();
}

/// Whether a queued job may run nested on this stack: within half of a fiber's stack, or a few levels deep
/// elsewhere.
bool CanRunInline(const Worker *worker)
{
    if (worker == nullptr || worker->Current == nullptr)
    {
        return _inlineDepth < MaxInlineDepth;
    }
    const JobFiber *fiber = worker->Current;
    const auto here = reinterpret_cast<uintptr>(__builtin_frame_address(0));
    return fiber->StackTop - here < fiber->StackSize / 2;
}

/// Whether the caller's stack may run large-stack jobs: a large fiber or a thread's own stack.
bool OnLargeStack(const Worker *worker)
{
    return worker == nullptr || worker->Current == nullptr || worker->Current->Large;
}

void FiberMain(void *argument);

/// A small fiber beyond the pool, for a wait that has no fiber to switch to and no stack room to help inline.
JobFiber *CreateOverflowFiber()
{
    auto *fiber = ConstructAt(static_cast<JobFiber *>(Memory::Allocate(sizeof(JobFiber))));
    fiber->StackSize = _jobs->Desc.SmallStackSize;
    fiber->Handle = PlatformFiber::Create(fiber->StackSize, FiberMain, fiber);
    MARLIN_VERIFY(fiber->Handle.IsValid());
    const ScopeLock lock(_jobs->OverflowLock);
    _jobs->Overflow.Add(fiber);
    return fiber;
}

/// Runs `job` on the current stack and context, inside the job's memory tag and with its own fiber locals.
void ExecuteInline(const QueuedJob &job)
{
    ExecutionContext &context = GetExecutionContext();
    // Only the slots handed out so far can hold values, so only they are saved and cleared.
    const uint32 slots = Private::GetFiberLocalSlotCount();
    void *locals[ExecutionContext::MaxFiberLocals];
    MemCopy(locals, context.FiberLocals, slots * sizeof(void *));
    MemZero(context.FiberLocals, slots * sizeof(void *));
    const bool tagged = job.Tag != 0 && context.MemoryTagDepth < ExecutionContext::MaxMemoryTagDepth;
    if (tagged)
    {
        context.MemoryTags[context.MemoryTagDepth++] = job.Tag;
    }
    ++_inlineDepth;
    job.Entry(job.Argument);
    --_inlineDepth;
    if (tagged)
    {
        --context.MemoryTagDepth;
    }
    MemCopy(context.FiberLocals, locals, slots * sizeof(void *));
    // Slots the job handed out did not exist for the caller, which must still see them zero.
    const uint32 after = Private::GetFiberLocalSlotCount();
    MemZero(context.FiberLocals + slots, (after - slots) * sizeof(void *));
    CompleteJob(job.Counter);
}

void Execute(JobFiber *fiber, const QueuedJob &job)
{
    // A job starts with zeroed fiber locals and no memory tag but its own; the rest of the context is unused.
    ExecutionContext &context = fiber->Context;
    MemZero(context.FiberLocals, Private::GetFiberLocalSlotCount() * sizeof(void *));
    context.MemoryTags[0] = job.Tag;
    context.MemoryTagDepth = job.Tag != 0 ? 1 : 0;
    fiber->PinnedWorker = -1;
    job.Entry(job.Argument);
    CompleteJob(job.Counter);
}

/// Parks the worker until there may be work for it. A sleeper sets its bit in SleepMasks, then looks for work; a
/// waker publishes work, then claims bits and releases each claimed worker's semaphore once. Both fence between
/// their write and their read, so either the sleeper finds the work or the waker finds the bit: no wakeup is lost
/// and the wait needs no timeout.
void Idle(const Worker *worker, bool largeStack)
{
    Atomic<uint64> &mask = _jobs->SleepMasks[worker->Index / 64];
    const uint64 bit = uint64{1} << (worker->Index % 64);
    mask.FetchOr(bit, MemoryOrder::AcquireRelease);
    AtomicThreadFence();
    if (!HasWork(worker, largeStack) && !_jobs->Stopping.Load(MemoryOrder::Relaxed))
    {
        worker->Wake->Wait();
        return;
    }
    if ((mask.FetchAnd(~bit, MemoryOrder::AcquireRelease) & bit) == 0)
    {
        // A waker cleared the bit first and releases a token for it; take it, so the semaphore stays balanced.
        worker->Wake->Wait();
    }
}

void FiberMain(void *argument)
{
    auto *self = static_cast<JobFiber *>(argument);
    self->StackTop = reinterpret_cast<uintptr>(__builtin_frame_address(0));
    uint32 idleSpins = 0;
    while (true)
    {
        Worker *worker = CurrentWorker();
        ProcessAfterSwitch(worker);
        if (self->HasJob)
        {
            self->HasJob = false;
            Execute(self, self->Job);
            idleSpins = 0;
            continue;
        }
        JobFiber *ready = nullptr;
        if (worker->Pinned->Pop(ready) || _jobs->Ready->Pop(ready))
        {
            SwitchFiber(worker, self, ready, AfterSwitch::Free);
            idleSpins = 0;
            continue;
        }
        QueuedJob job{};
        JobFiber *large = nullptr;
        if (TakeJob(self->Large, job, large))
        {
            if (large != nullptr)
            {
                large->Job = job;
                large->HasJob = true;
                SwitchFiber(worker, self, large, AfterSwitch::Free);
            }
            else
            {
                Execute(self, job);
            }
            idleSpins = 0;
            continue;
        }
        if (_jobs->Stopping.Load())
        {
            worker->Pending = AfterSwitch::Free;
            worker->PendingFiber = self;
            worker->Current = nullptr;
            SetExecutionContext(nullptr);
            Private::ProfileFiberSwitch(nullptr, nullptr);
            PlatformFiber::SwitchTo(worker->ThreadFiber);
            continue;
        }
        if (++idleSpins < IdleSpinCount)
        {
            PlatformArch::Pause();
            continue;
        }
        idleSpins = 0;
        Idle(worker, self->Large);
    }
}

void WorkerMain(void *argument)
{
    auto *worker = static_cast<Worker *>(argument);
    _worker = worker;
    if (worker->AffinityMask != 0)
    {
        PlatformThread::SetCurrentAffinity(worker->AffinityMask);
    }
    worker->ThreadFiber = PlatformFiber::ConvertCurrentThread();
    MARLIN_VERIFY(worker->ThreadFiber.IsValid());
    JobFiber *first = nullptr;
    if (!_jobs->FreeSmall->Pop(first))
    {
        // Jobs that ran before this thread started may have parked every pooled fiber.
        first = CreateOverflowFiber();
    }
    worker->Current = first;
    SetExecutionContext(&first->Context);
    PlatformFiber::SwitchTo(first->Handle);
    ProcessAfterSwitch(worker);
    PlatformFiber::RevertCurrentThread();
    _worker = nullptr;
}

/// Sleeps off the workers until `counter` reaches `target`: the thread links itself as a waiter that CompleteJob
/// wakes through its Woken word.
void BlockUntil(JobCounter &counter, uint32 target)
{
    JobWaiter waiter{nullptr, target, nullptr};
    if (!LinkWaiter(counter, waiter))
    {
        return;
    }
    uint32 woken = 0;
    while ((woken = waiter.Woken.Load(MemoryOrder::Acquire)) == 0)
    {
        PlatformSync::WaitOnAddress(&waiter.Woken, &woken, sizeof(woken));
    }
}

/// Waits off the fibers: runs queued jobs on this stack until `counter` reaches `target`, and sleeps when there is
/// nothing to help with.
void HelpUntil(JobCounter &counter, uint32 target)
{
    uint32 polls = 0;
    while (JobSystemAccess::Value(counter).Load() > target)
    {
        QueuedJob job{};
        if (_jobs != nullptr && CanRunInline(CurrentWorker()) && PopJob(job, true))
        {
            ExecuteInline(job);
            polls = 0;
            continue;
        }
        ++polls;
        if (polls < HelpPauseCount)
        {
            PlatformArch::Pause();
        }
        else if (polls < HelpPauseCount + HelpYieldCount)
        {
            PlatformThread::YieldThread();
        }
        else
        {
            BlockUntil(counter, target);
            return;
        }
    }
}

/// The queue `decl` goes to.
MpmcQueue<QueuedJob> *QueueFor(const JobDecl &decl)
{
    const auto priority = static_cast<uint32>(decl.Priority);
    return decl.LargeStack ? _jobs->LargeQueues[priority] : _jobs->Queues[priority];
}
} // namespace

JobCounter::~JobCounter()
{
    while (_busy.Load(MemoryOrder::Acquire) != 0)
    {
        PlatformArch::Pause();
    }
}

bool JobSystem::Start(const JobSystemDesc &desc)
{
    MARLIN_ASSERT(_jobs == nullptr);
    const CpuTopology topology = PlatformCpu::GetTopology();
    const uint32 physical = topology.PhysicalCount != 0 ? topology.PhysicalCount : 1;
    uint32 workerCount = desc.WorkerCount;
    if (workerCount == 0)
    {
        const uint32 available = physical > desc.ReservedCores + 1 ? physical - desc.ReservedCores - 1 : 1;
        workerCount = available;
    }

    auto *state = ConstructAt(static_cast<JobSystemState *>(Memory::Allocate(sizeof(JobSystemState))));
    state->Desc = desc;
    state->WorkerCount = workerCount;
    const auto makeJobQueue = [](usize capacity) {
        return ConstructAt(static_cast<MpmcQueue<QueuedJob> *>(
                               Memory::Allocate(sizeof(MpmcQueue<QueuedJob>), alignof(MpmcQueue<QueuedJob>))),
                           capacity);
    };
    for (uint32 priority = 0; priority < PriorityCount; ++priority)
    {
        state->Queues[priority] = makeJobQueue(desc.QueueCapacity);
        state->LargeQueues[priority] = makeJobQueue(desc.LargeQueueCapacity);
    }
    state->FiberCount = desc.SmallFiberCount + desc.LargeFiberCount;
    const auto makeFiberQueue = [](usize capacity) {
        return ConstructAt(static_cast<MpmcQueue<JobFiber *> *>(
                               Memory::Allocate(sizeof(MpmcQueue<JobFiber *>), alignof(MpmcQueue<JobFiber *>))),
                           capacity);
    };
    state->Ready = makeFiberQueue(FiberQueueCapacity);
    state->FreeSmall = makeFiberQueue(FiberQueueCapacity);
    state->FreeLarge = makeFiberQueue(desc.LargeFiberCount);
    state->SleepMaskCount = (workerCount + 63) / 64;
    state->SleepMasks = static_cast<Atomic<uint64> *>(
        Memory::Allocate(state->SleepMaskCount * sizeof(Atomic<uint64>), alignof(Atomic<uint64>)));
    for (uint32 i = 0; i < state->SleepMaskCount; ++i)
    {
        ConstructAt(&state->SleepMasks[i], uint64{0});
    }

    state->Fibers = static_cast<JobFiber *>(Memory::Allocate(state->FiberCount * sizeof(JobFiber)));
    for (uint32 i = 0; i < state->FiberCount; ++i)
    {
        JobFiber *fiber = ConstructAt(&state->Fibers[i]);
        fiber->Large = i >= desc.SmallFiberCount;
        fiber->StackSize = fiber->Large ? desc.LargeStackSize : desc.SmallStackSize;
        StringBuilder<24> name;
        FormatTo(name, "{0} fiber {1}", fiber->Large ? "Large" : "Small", i);
        MemCopy(fiber->Name, name.CStr(), name.Length() + 1);
        fiber->Handle =
            PlatformFiber::Create(fiber->Large ? desc.LargeStackSize : desc.SmallStackSize, FiberMain, fiber);
        MARLIN_VERIFY(fiber->Handle.IsValid());
        MARLIN_VERIFY((fiber->Large ? state->FreeLarge : state->FreeSmall)->Push(fiber));
    }

    state->Workers = static_cast<Worker *>(Memory::Allocate(workerCount * sizeof(Worker)));
    for (uint32 i = 0; i < workerCount; ++i)
    {
        Worker *worker = ConstructAt(&state->Workers[i]);
        worker->Index = i;
        worker->Pinned = makeFiberQueue(state->FiberCount);
        worker->Wake = ConstructAt(static_cast<Semaphore *>(Memory::Allocate(sizeof(Semaphore))), 0u, 0x7FFFFFFFu);
        const uint32 core = workerCount < physical ? (desc.ReservedCores + 1 + i) % physical : i % physical;
        worker->AffinityMask = core < CpuTopology::MaxCores ? topology.CoreMasks[core] : 0;
    }
    _jobs = state;

    for (uint32 i = 0; i < workerCount; ++i)
    {
        Worker &worker = state->Workers[i];
        char name[32];
        StringBuilder<32> builder;
        FormatTo(builder, "Worker {0}", i);
        MemCopy(name, builder.CStr(), builder.Length() + 1);
        ThreadDesc thread;
        thread.Name = name;
        thread.Entry = WorkerMain;
        thread.Argument = &worker;
        thread.StackSize = usize{64} * 1024;
        worker.Thread = PlatformThread::Create(thread);
        MARLIN_VERIFY(worker.Thread.IsValid());
    }
    return true;
}

void JobSystem::Stop()
{
    JobSystemState *state = _jobs;
    MARLIN_ASSERT(state != nullptr && CurrentWorker() == nullptr);
    state->Stopping.Store(true);
    WakeWorkers(state->WorkerCount);
    for (uint32 i = 0; i < state->WorkerCount; ++i)
    {
        PlatformThread::Join(state->Workers[i].Thread);
    }
    for (uint32 i = 0; i < state->FiberCount; ++i)
    {
        PlatformFiber::Delete(state->Fibers[i].Handle);
        DestroyAt(&state->Fibers[i]);
    }
    for (JobFiber *fiber : state->Overflow)
    {
        PlatformFiber::Delete(fiber->Handle);
        DestroyAt(fiber);
        Memory::Free(fiber);
    }
    for (uint32 i = 0; i < state->WorkerCount; ++i)
    {
        DestroyAt(state->Workers[i].Pinned);
        Memory::Free(state->Workers[i].Pinned);
        DestroyAt(state->Workers[i].Wake);
        Memory::Free(state->Workers[i].Wake);
    }
    for (uint32 priority = 0; priority < PriorityCount; ++priority)
    {
        for (MpmcQueue<QueuedJob> *queue : {state->Queues[priority], state->LargeQueues[priority]})
        {
            DestroyAt(queue);
            Memory::Free(queue);
        }
    }
    for (MpmcQueue<JobFiber *> *queue : {state->Ready, state->FreeSmall, state->FreeLarge})
    {
        DestroyAt(queue);
        Memory::Free(queue);
    }
    Memory::Free(state->SleepMasks);
    Memory::Free(state->Fibers);
    Memory::Free(state->Workers);
    _jobs = nullptr;
    DestroyAt(state);
    Memory::Free(state);
}

bool JobSystem::IsRunning()
{
    return _jobs != nullptr;
}

uint32 JobSystem::GetWorkerCount()
{
    return _jobs != nullptr ? _jobs->WorkerCount : 0;
}

uint32 JobSystem::GetOverflowFiberCount()
{
    if (_jobs == nullptr)
    {
        return 0;
    }
    const ScopeLock lock(_jobs->OverflowLock);
    return static_cast<uint32>(_jobs->Overflow.Count());
}

namespace Jobs
{
void Run(Span<const JobDecl> jobs, JobCounter *counter)
{
    if (counter != nullptr)
    {
        JobSystemAccess::Value(*counter).FetchAdd(static_cast<uint32>(jobs.Count()));
    }
    const uint16 tag = CurrentTagSlot();
    if (_jobs == nullptr)
    {
        for (const JobDecl &decl : jobs)
        {
            ExecuteInline({decl.Entry, decl.Argument, counter, tag});
        }
        return;
    }
    // Workers wake once a batch is queued rather than once a job, which matters while a full queue takes one at a time.
    usize unannounced = 0;
    for (usize first = 0; first < jobs.Count();)
    {
        // Neighbouring jobs for the same queue claim their cells together.
        MpmcQueue<QueuedJob> *queue = QueueFor(jobs[first]);
        usize count = 1;
        while (count < RunBatch && first + count < jobs.Count() && QueueFor(jobs[first + count]) == queue)
        {
            ++count;
        }
        const JobDecl *decls = jobs.Data() + first;
        const usize pushed = queue->PushRange(count, [&](usize i) {
            MARLIN_ASSERT(decls[i].Entry != nullptr);
            return QueuedJob{decls[i].Entry, decls[i].Argument, counter, tag};
        });
        first += pushed;
        unannounced += pushed;
        // While the queue is full a sleeper is woken to help as soon as one is seen; the check needs no fence,
        // since this loop looks again until the last batch, which wakes properly.
        if (unannounced != 0 &&
            (unannounced >= RunBatch || first == jobs.Count() || (pushed == 0 && IsAnyWorkerSleeping())))
        {
            WakeWorkers(static_cast<uint32>(unannounced));
            unannounced = 0;
        }
        if (pushed != 0)
        {
            continue;
        }
        // The queue is full: run a queued job here to make room.
        const Worker *worker = CurrentWorker();
        QueuedJob other{};
        if (CanRunInline(worker) && PopJob(other, OnLargeStack(worker)))
        {
            ExecuteInline(other);
            continue;
        }
        PlatformThread::YieldThread();
    }
}

void Wait(JobCounter &counter, uint32 target)
{
    Worker *worker = CurrentWorker();
    if (worker == nullptr || worker->Current == nullptr)
    {
        HelpUntil(counter, target);
        return;
    }
    uint32 spins = 0;
    while (JobSystemAccess::Value(counter).Load() > target)
    {
        // An inline job may have parked this fiber and resumed it on another thread.
        worker = CurrentWorker();
        // Parking needs a fiber to switch to: a ready one first, so parked work drains, then a free one.
        JobFiber *self = worker->Current;
        JobFiber *next = nullptr;
        if (worker->Pinned->Pop(next) || _jobs->Ready->Pop(next) || _jobs->FreeSmall->Pop(next) ||
            _jobs->FreeLarge->Pop(next))
        {
            JobWaiter waiter{self, target, nullptr};
            worker->PendingCounter = &counter;
            worker->PendingWaiter = &waiter;
            SwitchFiber(worker, self, next, AfterSwitch::Park);
            ProcessAfterSwitch(CurrentWorker());
            return;
        }
        // Every fiber is parked or busy: run a queued job on this stack instead, while there is room.
        QueuedJob job{};
        if (!CanRunInline(worker))
        {
            next = CreateOverflowFiber();
            JobWaiter waiter{self, target, nullptr};
            worker->PendingCounter = &counter;
            worker->PendingWaiter = &waiter;
            SwitchFiber(worker, self, next, AfterSwitch::Park);
            ProcessAfterSwitch(CurrentWorker());
            return;
        }
        if (PopJob(job, self->Large))
        {
            ExecuteInline(job);
            spins = 0;
            continue;
        }
        if (++spins < 64)
        {
            PlatformArch::Pause();
        }
        else
        {
            PlatformThread::YieldThread();
        }
    }
}

bool IsInJob()
{
    const Worker *worker = CurrentWorker();
    return worker != nullptr && worker->Current != nullptr;
}

int32 GetWorkerIndex()
{
    const Worker *worker = CurrentWorker();
    return worker != nullptr ? static_cast<int32>(worker->Index) : -1;
}
} // namespace Jobs

JobScope JobScope::Pin()
{
    const Worker *worker = CurrentWorker();
    if (worker == nullptr || worker->Current == nullptr)
    {
        return {false, -1};
    }
    const int32 previous = worker->Current->PinnedWorker;
    worker->Current->PinnedWorker = static_cast<int32>(worker->Index);
    return {true, previous};
}

JobScope::~JobScope()
{
    if (_active)
    {
        CurrentWorker()->Current->PinnedWorker = _previous;
    }
}
} // namespace Marlin
