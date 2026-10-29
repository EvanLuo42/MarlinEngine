#pragma once

#include "Containers/HashMap.h"
#include "Memory/MemoryOps.h"
#include "ScriptVM/ScriptDomain.h"

namespace Marlin
{
/// A piece of a VM stack: frames are laid out in segments, a new one taken when the next frame does not fit
/// (MarlinScript §4.3, §6.5).
struct ScriptStackSegment
{
    uint8 *Begin = nullptr;
    uint8 *End = nullptr;
};

/// A running or suspended call.
struct ScriptFrame
{
    const ScriptFunctionInfo *Function = nullptr;
    /// The instruction being executed, or the one to resume at.
    uint32 Pc = 0;
    /// Where Return resumes the frame: the instruction after the call it is making.
    uint32 ResumePc = 0;
    uint8 *Base = nullptr;
    /// Where Return copies the result: the caller's window, which is Base unless the frame moved to a new segment.
    uint8 *ResultTo = nullptr;
    uint32 Segment = 0;
    /// False until the frame first runs: then only its parameters are live.
    bool Entered = false;
};

/// A VM stack: the one an entry from C++ runs on, or a task's or iterator's (MarlinScript §4.3).
struct ScriptCoroutine
{
    List<ScriptStackSegment> Segments;
    uint32 Segment = 0;
    usize StackBytes = 0;
    List<ScriptFrame> Frames;
    /// The task or iterator object the coroutine runs for; null for an entry from C++.
    ScriptObject *Owner = nullptr;
    uint32 Index = 0;
    /// An iterator's current value, kept below its frames.
    uint8 *Reserved = nullptr;
    /// The function the coroutine was started with.
    const ScriptFunctionInfo *Entry = nullptr;
    /// Whether the coroutine is on the free list.
    bool Free = false;
};

/// The VM's part of a task object (MarlinScript §9.9). The first ScriptTaskReferences members are references the
/// collector traces.
struct ScriptTaskState
{
    /// The tasks waiting for this one, an array of references; WaiterCount of its elements are used.
    ScriptArray *Waiters = nullptr;
    /// The boxed result of a completed Task<T>.
    ScriptObject *Result = nullptr;
    ScriptObject *Owner = nullptr;
    /// What a wait is about: the condition of Frame.Until or the task array of Task.WhenAll and Task.WhenAny.
    ScriptObject *Subject = nullptr;
    uint32 State = 0;
    uint32 WaiterCount = 0;
    /// What the task waits for, one of ScriptTaskWaits.
    uint32 Wait = 0;
    /// The index of its coroutine plus one, or 0 for a task without one.
    uint32 Coroutine = 0;
    float64 WakeTime = 0;
    uint64 Sequence = 0;
    uint32 Id = 0;
    uint32 Unused = 0;
    /// Whether the task waits in the ready queue, so waking it again does not queue it twice.
    uint32 Queued = 0;
};

inline constexpr uint32 ScriptTaskReferences = 4;

namespace ScriptTaskStates
{
inline constexpr uint32 Pending = 0;
inline constexpr uint32 Running = 1;
inline constexpr uint32 Completed = 2;
inline constexpr uint32 Faulted = 3;
inline constexpr uint32 Canceled = 4;
} // namespace ScriptTaskStates

/// What a task without code waits for.
namespace ScriptTaskWaits
{
inline constexpr uint32 None = 0;
inline constexpr uint32 Task = 1;
inline constexpr uint32 NextFrame = 2;
inline constexpr uint32 NextFixed = 3;
inline constexpr uint32 Delay = 4;
inline constexpr uint32 Until = 5;
inline constexpr uint32 All = 6;
inline constexpr uint32 Any = 7;
} // namespace ScriptTaskWaits

/// The VM's part of an iterator object (MarlinScript §4.3).
struct ScriptIteratorState
{
    const ScriptFunctionInfo *Function = nullptr;
    /// 0 before the first MoveNext, 1 while it yields, 2 when it has ended.
    uint32 State = 0;
    /// The index of its coroutine plus one, or 0 once it has ended.
    uint32 Coroutine = 0;
    /// Whether Current holds a value.
    uint32 Current = 0;
    uint32 CurrentSlots = 0;
};

/// The VM's part of a StringBuilder: its text in a byte array of which Length bytes are used.
struct ScriptStringBuilderState
{
    ScriptArray *Buffer = nullptr;
    uint32 Length = 0;
    uint32 Unused = 0;
};

/// The VM's part of a WeakRef: its target, which the collector does not trace.
struct ScriptWeakRefState
{
    ScriptObject *Target = nullptr;
};

/// The VM's part of a TypeInfo object.
struct ScriptTypeInfoState
{
    const ScriptType *Described = nullptr;
};

template <typename TState> [[nodiscard]] TState &StateOf(ScriptObject *object)
{
    return *reinterpret_cast<TState *>(reinterpret_cast<uint8 *>(object) + object->Type->PayloadOffset);
}

/// Visits the references the collector must treat as roots or update when objects move.
class ScriptRootVisitor
{
public:
    virtual ~ScriptRootVisitor() = default;
    /// A slot holding a reference, possibly null.
    virtual void VisitObject(ScriptObject **reference) = 0;
    /// A slot holding a managed pointer, which may point into an object or elsewhere.
    virtual void VisitManaged(uint8 **pointer) = 0;
};

/// How a run of a coroutine ended (MarlinScript §8.1).
enum class ScriptRunStatus : uint8
{
    Returned,
    Suspended,
    Faulted,
    Canceled,
};

/// A register window outside any frame the collector must scan: the arguments of a native call, or those a combined
/// delegate is called with again for each of its invocations.
struct ScriptRootWindow
{
    uint8 *Window = nullptr;
    const ScriptFunctionInfo *Function = nullptr;
    /// The window starts with the delegate being called; the arguments follow it.
    bool Delegate = false;
    /// The parameters of Function the window leaves out at its start, such as a delegate's receiver.
    uint32 Skip = 0;
};

/// Runs bytecode (MarlinScript §8.1, §14): a register machine over 16-byte slots, dispatched with computed goto,
/// and the task scheduler (§4.2, §9.7).
class ScriptInterpreter
{
public:
    explicit ScriptInterpreter(ScriptDomain &domain);
    ~ScriptInterpreter();
    MARLIN_NONCOPYABLE(ScriptInterpreter);

    ScriptResult<void> Invoke(const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments,
                              Span<ScriptSlot> result);

    /// A VM entry that calls functions one after another on the same stack (ScriptBatchCall).
    ScriptCoroutine &BeginBatch();
    /// Runs *function* to its end on *coroutine*, a batch's, as Invoke does; false when it faulted, the fault in
    /// GetFault.
    bool CallInBatch(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments,
                     Span<ScriptSlot> result);
    void EndBatch(ScriptCoroutine &coroutine);
    /// The fault of the last call that faulted.
    [[nodiscard]] const ScriptFaultInfo &GetFault() const
    {
        return _fault;
    }

    /// The scheduler (MarlinScript §4.2).
    void RunReady();
    void AdvanceTime(float64 seconds);
    /// Wakes what waits for the next frame and evaluates the Frame.Until conditions.
    void BeginFrame();
    void FixedTick();
    void CancelTasksOwnedBy(const ScriptObject *owner);
    /// Cancels *task* and what waits for it.
    void CancelTask(ScriptObject *task);
    [[nodiscard]] ScriptTaskStatus GetTaskStatus(const ScriptObject *task, ScriptFaultInfo *fault) const;

    /// Registers the functions the VM implements itself (MarlinScript §9.7): Frame, Task and Iterator.
    static void RegisterNatives(ScriptNativeRegistry &registry);

    [[nodiscard]] uint32 GetTaskId(ScriptObject *task);

    /// Visits the stacks, the scheduler's queues and the native windows (MarlinScript §9.2).
    void VisitRoots(ScriptRootVisitor &visitor);

    [[nodiscard]] Span<const UniquePtr<ScriptCoroutine>> GetCoroutines() const
    {
        return _coroutines.AsSpan();
    }

private:
    friend class ScriptCollector;

    /// Runs *coroutine* until its frames drop to *entryDepth*, it suspends or it faults.
    ScriptRunStatus Run(ScriptCoroutine &coroutine, usize entryDepth);
    ScriptRunStatus CallNative(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window);
    /// Calls *function* with the arguments at *window* and runs it to its end, whatever it is: for natives, delegates
    /// and the scheduler.
    ScriptRunStatus CallNested(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window);
    /// Pushes a frame for *function*, a function with Code, with its parameters at *window*; false when the stack is
    /// full. The frame stays at *window* when it fits the current segment, as nearly every one does.
    bool PushFrame(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window, uint8 *resultTo)
    {
        const uint32 segment = coroutine.Segment;
        const ScriptStackSegment &current = coroutine.Segments[segment];
        if (window < current.Begin || window + function.FrameBytes > current.End)
        {
            return PushFrameInNewSegment(coroutine, function, window, resultTo);
        }
        // Only the frame variables need zeroing: every other register a map names is written before that map applies.
        const usize parameterBytes = usize{function.ParameterSlots} * ScriptSlotBytes;
        const usize zeroBytes = usize{function.ZeroSlots} * ScriptSlotBytes;
        if (zeroBytes != 0)
        {
            MemZero(window + parameterBytes,
                    parameterBytes + zeroBytes <= function.FrameBytes ? zeroBytes : function.FrameBytes - parameterBytes);
        }
        coroutine.Frames.Add({&function, 0, 0, window, resultTo, segment, false});
        return true;
    }
    bool PushFrameInNewSegment(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window,
                               uint8 *resultTo);
    /// Room for *bytes* at *at* or in a later segment; null when the stack would pass its limit.
    uint8 *Reserve(ScriptCoroutine &coroutine, uint8 *at, usize bytes, uint32 &segment);
    [[nodiscard]] uint8 *TopOf(const ScriptCoroutine &coroutine) const;
    ScriptCoroutine &NewCoroutine();
    void FreeCoroutine(uint32 index);
    /// Records a fault with the stack of *coroutine*; *native* names a native function it happened in.
    void Raise(ScriptCoroutine &coroutine, ScriptFault kind, StringView message, StringView native = {});
    ScriptObject *AllocateAtSafepoint(const ScriptType &type, uint32 length, bool array);
    /// False once the entry has run past the watchdog's limit; reads the clock once every 1024 checks.
    bool CheckWatchdog()
    {
        return _watchdogLimit == 0 || --_watchdogBudget != 0 || CheckDeadline();
    }
    /// The slow path of CheckWatchdog: refills the budget and compares the clock with the deadline. The first refill
    /// of an entry sets the deadline, so an entry that ends before it never reads the clock.
    bool CheckDeadline();
    /// Restarts the watchdog of the outermost entry.
    void ResetWatchdog();

    ScriptRunStatus CallDelegate(ScriptCoroutine &coroutine, uint8 *window, uint32 slots, bool allowEmpty,
                                 uint32 resultSlots);
    /// Starts *function*, an async one, as a task, and leaves the task in the window (MarlinScript §4.2).
    bool StartAsync(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window);
    /// Makes the iterator object of *function* and leaves it in the window (MarlinScript §4.3).
    bool StartIterator(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window);
    void Resume(ScriptObject *task);
    void FinishTask(ScriptObject *task, ScriptCoroutine &coroutine, ScriptRunStatus status);
    /// Ends a task without code in *state* and wakes its waiters.
    void SettleTask(ScriptObject *task, uint32 state);
    void AddWaiter(ScriptObject *task, ScriptObject *waiter);
    void BeginEntry();
    void EndEntry();
    void WakeWaiters(ScriptObject *task);
    /// Whether a Task.WhenAll or Task.WhenAny task is done, settling it if so.
    bool CombinatorDone(ScriptObject *task);
    /// The box type a Task<T> keeps its result in.
    const ScriptType &ResultBoxType(const ScriptFunctionInfo &function);
    ScriptObject *NewWaitTask(uint32 wait, float64 seconds, ScriptObject *subject);

    /// Calls the delegate in the window's first slot with no arguments to its end, for Frame.Until.
    ScriptRunStatus InvokeDelegate(uint8 *&window);
    ScriptObject *NewTaskObject(const ScriptType &type, uint32 state);

    static ScriptInterpreter &Of(ScriptCallContext &context);
    static void IteratorMoveNext(ScriptCallContext &context, const void *);
    static void IteratorCurrent(ScriptCallContext &context, const void *);
    static void IteratorGetEnumerator(ScriptCallContext &context, const void *);
    static void TaskIsCompleted(ScriptCallContext &context, const void *);
    static void TaskIsCanceled(ScriptCallContext &context, const void *);
    static void TaskIsFaulted(ScriptCallContext &context, const void *);
    static void TaskCancel(ScriptCallContext &context, const void *);
    static void TaskWhen(ScriptCallContext &context, const void *target);
    static void TaskRun(ScriptCallContext &context, const void *);
    static void TaskResult(ScriptCallContext &context, const void *);
    static void FrameWait(ScriptCallContext &context, const void *target);
    /// The type of the iterator objects of *function*, which implements its IEnumerable and IEnumerator.
    const ScriptType &IteratorType(const ScriptFunctionInfo &function);
    void ReportUnobserved(ScriptObject *task);

    ScriptDomain *_domain;
    List<UniquePtr<ScriptCoroutine>> _coroutines;
    List<uint32> _freeCoroutines;
    ScriptCoroutine *_running = nullptr;
    List<ScriptRootWindow> _roots;
    ScriptFaultInfo _fault;
    uint32 _entryDepth = 0;
    /// The watchdog of the outermost entry: its limit in seconds (0 for none) and its deadline once set.
    float64 _watchdogLimit = 0;
    float64 _deadline = 0;
    uint32 _watchdogBudget = 0;
    /// Tasks whose coroutine can resume, in order: RunReady takes them from _readyHead on, and a canceled one leaves a
    /// null behind.
    List<ScriptObject *> _ready;
    usize _readyHead = 0;
    /// Tasks without code: waiting for the next frame or a Frame.Until condition, for the next fixed tick, or for the
    /// clock (a min-heap by wake time, then sequence). Each list is in the order the tasks were made.
    List<ScriptObject *> _frameWaiters;
    List<ScriptObject *> _fixedWaiters;
    List<ScriptObject *> _delays;
    /// The waiters BeginFrame goes through, roots while their conditions run.
    List<ScriptObject *> _due;
    uint64 _sequence = 0;
    uint32 _taskIds = 0;
    float64 _time = 0;
    /// The faults of faulted tasks, by task id.
    HashMap<uint32, ScriptFaultInfo> _taskFaults;
    HashMap<String, UniquePtr<ScriptType>> _syntheticTypes;
    List<UniquePtr<ScriptFunctionInfo>> _syntheticFunctions;
};
} // namespace Marlin
