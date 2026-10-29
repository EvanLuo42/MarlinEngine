#pragma once

#include "Diagnostics/Log.h"
#include "Memory/UniquePtr.h"
#include "ScriptVM/ScriptVM.h"
#include "Templates/Utility.h"

MARLIN_DECLARE_LOG_CATEGORY(SCRIPTVM_API, LogScript, Info, Warning);

namespace Marlin
{
class World;
class ScriptCollector;
class ScriptHeap;
class ScriptInterpreter;
struct ScriptCoroutine;

/// One function of a fault's call stack, innermost first.
struct ScriptStackEntry
{
    String Function;
    String File;
    uint32 Line = 0;
    uint32 Column = 0;
};

/// A fault that ended a VM entry (MarlinScript §8): its kind, message and script call stack.
struct ScriptFaultInfo
{
    ScriptFault Kind = ScriptFault::Panic;
    String Message;
    List<ScriptStackEntry> Stack;

    /// The kind and message, then one line per stack entry.
    [[nodiscard]] SCRIPTVM_API String ToString() const;
};

/// The outcome of a call into the VM: a value or the fault that ended it.
template <typename T> class ScriptResult
{
public:
    ScriptResult(T value) : _value(Move(value)), _ok(true)
    {
    }

    ScriptResult(ScriptFaultInfo fault) : _value(), _fault(Move(fault)), _ok(false)
    {
    }

    [[nodiscard]] bool IsOk() const
    {
        return _ok;
    }

    explicit operator bool() const
    {
        return _ok;
    }

    [[nodiscard]] const T &GetValue() const
    {
        return _value;
    }

    [[nodiscard]] const ScriptFaultInfo &GetFault() const
    {
        return _fault;
    }

private:
    T _value;
    ScriptFaultInfo _fault;
    bool _ok;
};

template <> class ScriptResult<void>
{
public:
    ScriptResult() : _ok(true)
    {
    }

    ScriptResult(ScriptFaultInfo fault) : _fault(Move(fault)), _ok(false)
    {
    }

    [[nodiscard]] bool IsOk() const
    {
        return _ok;
    }

    explicit operator bool() const
    {
        return _ok;
    }

    [[nodiscard]] const ScriptFaultInfo &GetFault() const
    {
        return _fault;
    }

private:
    ScriptFaultInfo _fault;
    bool _ok;
};

/// The sizes and budgets of a domain (MarlinScript §9.2, §9.4, §9.6).
// RECOVERY-DECISION S3: the default sizes and budgets: a 512 MiB reservation, a 4 MiB nursery, 1 MiB survivor
// spaces, 64 KiB stack segments up to 1 MiB, a 5 s watchdog and 0.5 ms of collection per frame (08 §5.2).
struct ScriptDomainOptions
{
    /// Address space the heap reserves; the old generation grows into it.
    usize HeapReserveBytes = usize{512} << 20;
    /// The young generation: the nursery and each of its two survivor spaces.
    usize NurseryBytes = usize{4} << 20;
    usize SurvivorBytes = usize{1} << 20;
    /// A VM stack grows by segments of this size, up to the limit.
    usize StackSegmentBytes = usize{64} << 10;
    usize MaxStackBytes = usize{1} << 20;
    /// The longest a VM entry may run before it faults with Timeout; 0 for no limit.
    float64 WatchdogSeconds = 5.0;
    /// Time the collector gets at each frame boundary.
    float64 GcBudgetSeconds = 0.0005;
};

template <typename TSignature> class ScriptFunction;

/// The state of a task (MarlinScript §8.1).
enum class ScriptTaskStatus : uint8
{
    Pending,
    Completed,
    Faulted,
    Canceled,
};

/// All the script state of a world (MarlinScript §9.6): the heap, the types, the loaded modules with their statics,
/// the interpreter and the task scheduler.
class SCRIPTVM_API ScriptDomain
{
public:
    explicit ScriptDomain(ScriptVM &vm, const ScriptDomainOptions &options = {});
    ~ScriptDomain();
    MARLIN_NONCOPYABLE(ScriptDomain);

    [[nodiscard]] ScriptVM &GetVM() const
    {
        return *_vm;
    }

    [[nodiscard]] const ScriptDomainOptions &GetOptions() const
    {
        return _options;
    }

    /// The world whose entities the ECS natives work on (MarlinScript §11.2); null for a domain of no world.
    [[nodiscard]] World *GetWorld() const
    {
        return _world;
    }

    void SetWorld(World *world)
    {
        _world = world;
    }

    /// Seconds the current step covers, which `Time.Delta` reads: set by the frame and by each script system.
    [[nodiscard]] float64 GetDeltaTime() const
    {
        return _deltaTime;
    }

    void SetDeltaTime(float64 seconds)
    {
        _deltaTime = seconds;
    }

    /// Links *module* to the natives and the modules loaded before it, then runs its static initializers
    /// (MarlinScript §6.2, §6.3). False with *error* when something does not link or an initializer faults.
    bool Load(const ScriptModuleData &module, String &error);

    [[nodiscard]] const ScriptFunctionInfo *FindFunction(StringView name) const;
    [[nodiscard]] const ScriptType *FindType(StringView name) const;
    [[nodiscard]] Span<const UniquePtr<ScriptLoadedModule>> GetModules() const
    {
        return _modules.AsSpan();
    }

    /// Runs *function* (MarlinScript §8.1) with *arguments* in its parameter registers; on success the result's
    /// registers are copied to *result*.
    ScriptResult<void> Invoke(const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments,
                              Span<ScriptSlot> result = {});
    /// Invoke without making a ScriptResult: false when the call faulted, its fault then in GetLastFault.
    bool TryInvoke(const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments, Span<ScriptSlot> result = {});
    /// The fault of the last call that faulted.
    [[nodiscard]] const ScriptFaultInfo &GetLastFault() const;

    /// *name* as a function of *TSignature*, or an empty handle when there is none or its signature differs.
    template <typename TSignature> [[nodiscard]] ScriptFunction<TSignature> FindFunction(StringView name);

    [[nodiscard]] const ScriptType &GetObjectType() const
    {
        return *_objectType;
    }

    [[nodiscard]] const ScriptType &GetStringType() const
    {
        return *_stringType;
    }

    /// The array of bytes StringBuilder keeps its text in.
    [[nodiscard]] const ScriptType &GetByteArrayType() const
    {
        return *_byteArrayType;
    }

    /// Moves the domain's clock, which Frame.Delay counts in, by *seconds* of World time (MarlinScript §4.1).
    void AdvanceTime(float64 seconds);
    /// At a frame boundary: resumes what waits for the next frame and gives the collector its budget.
    void BeginFrame();
    /// At each fixed tick: resumes what waits for it.
    void FixedTick();
    /// Resumes the tasks that are ready, until none is.
    void RunReady();
    /// Cancels the tasks started with *owner* (MarlinScript §4.2).
    void CancelTasksOwnedBy(const ScriptObject *owner);
    /// The status of a task object, and with *fault* the fault of a faulted one.
    [[nodiscard]] ScriptTaskStatus GetTaskStatus(const ScriptObject *task, ScriptFaultInfo *fault = nullptr) const;

    /// A new object of a class or boxed type, with zeroed fields; null when the heap is exhausted.
    ScriptObject *NewObject(const ScriptType &type);
    /// A new array of an array type, with zeroed elements; null when the heap is exhausted.
    ScriptArray *NewArray(const ScriptType &type, uint32 length);
    ScriptArray *NewString(StringView text);
    /// A string the collector never frees or moves, such as a module's literals.
    ScriptArray *NewPermanentString(StringView text);
    /// The TypeInfo object describing *type* (MarlinScript §3.10).
    ScriptObject *GetTypeInfo(const ScriptType &type);
    /// A seed for `Random`, different for every call (MarlinScript §10.1).
    uint64 TakeEntropy();

    /// Weak references (MarlinScript §9.2): the collector does not trace their targets and clears them when the
    /// targets die.
    void RegisterWeakRef(ScriptObject *weakRef, ScriptObject *target);
    void SetWeakTarget(ScriptObject *weakRef, ScriptObject *target);
    [[nodiscard]] ScriptObject *GetWeakTarget(ScriptObject *weakRef);

    /// Stores *value* in the reference field at *field*, with the write barriers.
    void WriteReference(ScriptObject **field, ScriptObject *value);

    /// Copies elements between arrays of the same type, as if through a temporary buffer.
    void CopyElements(const ScriptArray &source, uint32 sourceIndex, ScriptArray &destination,
                      uint32 destinationIndex, uint32 count);
    /// Sets *count* elements from *start* to zero.
    void ClearElements(ScriptArray &array, uint32 start, uint32 count);

    /// A handle that keeps *object* alive and follows it when it moves (MarlinScript §7.2).
    uint32 CreateHandle(ScriptObject *object);
    void DestroyHandle(uint32 handle);
    [[nodiscard]] ScriptObject *GetHandle(uint32 handle) const
    {
        return handle < _handles.Count() ? _handles[handle] : nullptr;
    }

    /// Whether a value of *type* may be used as a *target*: the same type, a base or an implemented interface.
    [[nodiscard]] static bool IsAssignable(const ScriptType &type, const ScriptType &target);

    /// Asks for a collection at the next safepoint.
    void RequestCollection(bool full = false);
    /// Collects now: the young generation, or with *full* everything.
    void Collect(bool full = false);
    [[nodiscard]] uint64 GetCollectionCount() const;
    [[nodiscard]] usize GetHeapBytes() const;

private:
    friend class ScriptCollector;
    friend class ScriptInterpreter;
    friend class ScriptHeap;
    friend class ScriptBatchCall;

    ScriptType &NewType(StringView name, ScriptTypeKind kind);
    void CompleteType(ScriptLoadedModule &module, uint32 index, ScriptType &type);
    bool LinkFunction(ScriptLoadedModule &module, uint32 index, String &error);
    void SetUpFunction(ScriptLoadedModule &module, uint32 index, ScriptFunctionInfo &function);
    ScriptObject *Allocate(const ScriptType &type, usize bytes, bool permanent);
    /// Writes the register value at *registers* to *memory* by *access*, with the write barriers.
    void WriteAccess(uint8 *memory, const uint8 *registers, uint32 access, const ScriptLoadedModule &module);
    ScriptObject *CombineDelegates(ScriptObject *left, ScriptObject *right, bool remove);
    /// Collects if a collection was requested; only at safepoints.
    void CollectIfRequested()
    {
        if (_collectionRequested)
        {
            Collect(_fullCollectionRequested);
        }
    }
    /// Gives the collector its frame budget.
    void StepCollector();

    ScriptVM *_vm;
    ScriptDomainOptions _options;
    World *_world = nullptr;
    float64 _deltaTime = 0;
    UniquePtr<ScriptHeap> _heap;
    UniquePtr<ScriptCollector> _collector;
    List<ScriptObject *> _handles;
    List<uint32> _freeHandles;
    UniquePtr<ScriptInterpreter> _interpreter;
    List<UniquePtr<ScriptLoadedModule>> _modules;
    List<UniquePtr<ScriptType>> _types;
    List<UniquePtr<ScriptFunctionInfo>> _functions;
    HashMap<String, ScriptType *> _typesByName;
    HashMap<String, ScriptFunctionInfo *> _functionsByName;
    ScriptType *_objectType = nullptr;
    ScriptType *_stringType = nullptr;
    ScriptType *_byteArrayType = nullptr;
    ScriptType *_referenceArrayType = nullptr;
    uint32 _hashCounter = 0;
    uint64 _entropyCounter = 0;
    /// The registered weak reference objects.
    List<ScriptObject *> _weakRefs;
    bool _collectionRequested = false;
    bool _fullCollectionRequested = false;
    uint64 _collections = 0;
};

/// Calls one function many times in a single VM entry, as a script system does once per entity (MarlinScript
/// §11.2): the entry and its stack are set up once, not per call. Each call is still an entry of its own to the
/// watchdog and ends with its own fault.
class SCRIPTVM_API ScriptBatchCall
{
public:
    ScriptBatchCall(ScriptDomain &domain, const ScriptFunctionInfo &function);
    ~ScriptBatchCall();
    MARLIN_NONCOPYABLE(ScriptBatchCall);

    /// Calls the function with *arguments* in its parameter registers; false when it faulted, the fault then in
    /// GetFault until the next call.
    bool Call(Span<const ScriptSlot> arguments, Span<ScriptSlot> result = {});
    [[nodiscard]] const ScriptFaultInfo &GetFault() const;

private:
    ScriptInterpreter *_interpreter;
    const ScriptFunctionInfo *_function;
    ScriptCoroutine *_coroutine;
};

/// What MatchesScriptSignature compares: the kind of a C++ parameter or result type.
namespace ScriptValueKinds
{
inline constexpr uint32 Void = 0;
inline constexpr uint32 Bool = 1;
inline constexpr uint32 Reference = 2;
inline constexpr uint32 String = 3;

[[nodiscard]] constexpr uint32 Number(uint32 bits, bool isFloat, bool isSigned)
{
    return 4 | (bits << 8) | (isFloat ? 0x10u : 0u) | (isSigned ? 0x20u : 0u);
}
} // namespace ScriptValueKinds

namespace Private
{
template <typename T> [[nodiscard]] constexpr uint32 ScriptValueKindOf()
{
    using TValue = RemoveCvRef<T>;
    if constexpr (IsSame<TValue, void>)
    {
        return ScriptValueKinds::Void;
    }
    else if constexpr (IsSame<TValue, bool>)
    {
        return ScriptValueKinds::Bool;
    }
    else if constexpr (IsSame<TValue, StringView> || IsSame<TValue, Marlin::String>)
    {
        return ScriptValueKinds::String;
    }
    else if constexpr (IsPointer<TValue>)
    {
        return ScriptValueKinds::Reference;
    }
    else
    {
        static_assert(IsArithmetic<TValue>, "a script function takes and returns numbers, bools, strings and objects");
        return ScriptValueKinds::Number(static_cast<uint32>(sizeof(TValue) * 8), IsFloatingPoint<TValue>,
                                        IsSigned<TValue>);
    }
}
} // namespace Private

/// A script function C++ calls with typed arguments (MarlinScript §7.2).
template <typename TResult, typename... TArgs> class ScriptFunction<TResult(TArgs...)>
{
public:
    ScriptFunction() = default;

    ScriptFunction(ScriptDomain &domain, const ScriptFunctionInfo &function) : _domain(&domain), _function(&function)
    {
    }

    [[nodiscard]] bool IsValid() const
    {
        return _function != nullptr;
    }

    explicit operator bool() const
    {
        return _function != nullptr;
    }

    ScriptResult<TResult> operator()(TArgs... args) const
    {
        // Calls of few slots, nearly all of them, keep their registers on the C++ stack.
        ScriptSlot fixedArguments[8];
        ScriptSlot fixedResult[4];
        List<ScriptSlot> moreArguments;
        List<ScriptSlot> moreResult;
        const Span<ScriptSlot> arguments = SlotsFor(fixedArguments, moreArguments, _function->ParameterSlots);
        const Span<ScriptSlot> result = SlotsFor(fixedResult, moreResult, _function->ReturnSlots);
        WriteArguments(arguments, MakeIndexSequence<sizeof...(TArgs)>{}, args...);
        if (!_domain->TryInvoke(*_function, arguments, result))
        {
            return ScriptResult<TResult>(_domain->GetLastFault());
        }
        if constexpr (IsSame<TResult, void>)
        {
            return ScriptResult<void>();
        }
        else
        {
            return ScriptResult<TResult>(ScriptMarshal<TResult>::Read(result[0].Bytes));
        }
    }

private:
    template <usize TCount>
    static Span<ScriptSlot> SlotsFor(ScriptSlot (&fixed)[TCount], List<ScriptSlot> &more, uint32 count)
    {
        if (count <= TCount)
        {
            return Span<ScriptSlot>(fixed, count);
        }
        more.Resize(count);
        return more.AsSpan();
    }

    template <usize... TIndices>
    void WriteArguments(Span<ScriptSlot> arguments, IndexSequence<TIndices...>, TArgs... args) const
    {
        (ScriptMarshal<TArgs>::Write(*_domain, arguments[_function->ParameterOffsets[TIndices]].Bytes, args), ...);
    }

    ScriptDomain *_domain = nullptr;
    const ScriptFunctionInfo *_function = nullptr;
};

template <typename TSignature> ScriptFunction<TSignature> ScriptDomain::FindFunction(StringView name)
{
    return FindTypedFunction(*this, name, static_cast<TSignature *>(nullptr));
}

/// Whether *function* takes parameters and gives a result of the ScriptValueKinds given.
[[nodiscard]] SCRIPTVM_API bool MatchesScriptSignature(const ScriptFunctionInfo &function,
                                                       Span<const uint32> parameters, uint32 result);

template <typename TResult, typename... TArgs>
[[nodiscard]] ScriptFunction<TResult(TArgs...)> FindTypedFunction(ScriptDomain &domain, StringView name,
                                                                  TResult (*)(TArgs...))
{
    const ScriptFunctionInfo *function = domain.FindFunction(name);
    const uint32 parameters[] = {Private::ScriptValueKindOf<TArgs>()..., 0};
    if (function == nullptr ||
        !MatchesScriptSignature(*function, Span<const uint32>(parameters, sizeof...(TArgs)),
                                Private::ScriptValueKindOf<TResult>()))
    {
        return {};
    }
    return ScriptFunction<TResult(TArgs...)>(domain, *function);
}
} // namespace Marlin
