#include <initializer_list>

#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "ScriptInterpreter.h"
#include "String/Format.h"
#include "Templates/Utility.h"

namespace Marlin
{
namespace
{
ScriptObject *ElementOf(const ScriptArray *array, uint32 index)
{
    ScriptObject *element;
    MemCopy(&element, reinterpret_cast<const uint8 *>(array) + array->Type->DataOffset + usize{index} * 8, 8);
    return element;
}

ScriptObject **ElementAt(ScriptArray *array, uint32 index)
{
    return reinterpret_cast<ScriptObject **>(reinterpret_cast<uint8 *>(array) + array->Type->DataOffset +
                                             usize{index} * 8);
}

bool IsSettled(const ScriptTaskState &state)
{
    return state.State == ScriptTaskStates::Completed || state.State == ScriptTaskStates::Faulted ||
           state.State == ScriptTaskStates::Canceled;
}

/// The order of the delay heap: wake time, then the order the tasks were made in.
bool WakesBefore(ScriptObject *a, ScriptObject *b)
{
    const ScriptTaskState &first = StateOf<ScriptTaskState>(a);
    const ScriptTaskState &second = StateOf<ScriptTaskState>(b);
    return first.WakeTime < second.WakeTime || (first.WakeTime == second.WakeTime && first.Sequence < second.Sequence);
}

void SiftUp(List<ScriptObject *> &heap, usize index)
{
    while (index > 0 && WakesBefore(heap[index], heap[(index - 1) / 2]))
    {
        Swap(heap[index], heap[(index - 1) / 2]);
        index = (index - 1) / 2;
    }
}

void SiftDown(List<ScriptObject *> &heap, usize index)
{
    for (;;)
    {
        usize first = index;
        for (const usize child : {index * 2 + 1, index * 2 + 2})
        {
            if (child < heap.Count() && WakesBefore(heap[child], heap[first]))
            {
                first = child;
            }
        }
        if (first == index)
        {
            return;
        }
        Swap(heap[index], heap[first]);
        index = first;
    }
}

/// Removes the entry at *index* of the heap.
void RemoveFromHeap(List<ScriptObject *> &heap, usize index)
{
    heap[index] = heap.Last();
    heap.Pop();
    if (index < heap.Count())
    {
        SiftDown(heap, index);
        SiftUp(heap, index);
    }
}

bool BySequence(ScriptObject *a, ScriptObject *b)
{
    return StateOf<ScriptTaskState>(a).Sequence < StateOf<ScriptTaskState>(b).Sequence;
}

/// Words the instruction at *pc* takes, for stepping over the Yield an iterator resumes after.
uint32 InstructionWords(const ScriptFunctionRecord &record, uint32 pc)
{
    ScriptDecodedInstruction instruction;
    return DecodeScriptInstruction(record.Code.AsSpan(), pc, instruction) ? instruction.Words : 1;
}

/// How a boxed value of *shape* is read, as for an array element.
uint32 BoxAccessOf(const ScriptShape &shape)
{
    switch (shape.Kind)
    {
    case ScriptShapeKind::Bool:
        return MakeScriptAccess(ScriptAccess::B32);
    case ScriptShapeKind::Int:
        return shape.Width == 8    ? MakeScriptAccess(shape.Signed ? ScriptAccess::I8 : ScriptAccess::U8)
               : shape.Width == 16 ? MakeScriptAccess(shape.Signed ? ScriptAccess::I16 : ScriptAccess::U16)
               : shape.Width == 32 ? MakeScriptAccess(ScriptAccess::B32)
                                   : MakeScriptAccess(ScriptAccess::B64);
    case ScriptShapeKind::Float:
        return MakeScriptAccess(shape.Width == 16   ? ScriptAccess::U16
                                : shape.Width == 32 ? ScriptAccess::B32
                                                    : ScriptAccess::B64);
    case ScriptShapeKind::Reference:
        return MakeScriptAccess(ScriptAccess::Reference);
    case ScriptShapeKind::Pointer:
        return MakeScriptAccess(ScriptAccess::B64);
    default:
        return MakeScriptAccess(ScriptAccess::Bytes, shape.Size);
    }
}

/// Reads a boxed value into registers, extending narrow integers.
void ReadBoxed(uint8 *registers, const ScriptObject *box)
{
    const uint32 access = box->Type->ValueAccess;
    const uint8 *memory = box->GetData();
    switch (GetScriptAccess(access))
    {
    case ScriptAccess::I8: {
        const int32 value = static_cast<int8>(memory[0]);
        MemCopy(registers, &value, 4);
        return;
    }
    case ScriptAccess::U8: {
        const uint32 value = memory[0];
        MemCopy(registers, &value, 4);
        return;
    }
    case ScriptAccess::I16: {
        int16 narrow;
        MemCopy(&narrow, memory, 2);
        const int32 value = narrow;
        MemCopy(registers, &value, 4);
        return;
    }
    case ScriptAccess::U16: {
        uint16 narrow;
        MemCopy(&narrow, memory, 2);
        const uint32 value = narrow;
        MemCopy(registers, &value, 4);
        return;
    }
    case ScriptAccess::B32:
        MemCopy(registers, memory, 4);
        return;
    case ScriptAccess::B64:
    case ScriptAccess::Reference:
        MemCopy(registers, memory, 8);
        return;
    default:
        MemCopy(registers, memory, GetScriptAccessPayload(access));
        return;
    }
}
} // namespace

ScriptInterpreter &ScriptInterpreter::Of(ScriptCallContext &context)
{
    return *context.GetDomain()._interpreter;
}

const ScriptType &ScriptInterpreter::ResultBoxType(const ScriptFunctionInfo &function)
{
    const ScriptLoadedModule &module = *function.Module;
    const String name = Format("$Result:{0}#{1}", module.Package.AsView(), function.ResultShape);
    if (const UniquePtr<ScriptType> *found = _syntheticTypes.Find(name))
    {
        return **found;
    }
    // RECOVERY-DECISION S10: a Task<T> keeps its result in a box of a VM-made type per module and result shape.
    UniquePtr<ScriptType> type = MakeUnique<ScriptType>();
    const ScriptShape &shape = module.GetShape(function.ResultShape);
    type->Name = name;
    type->Kind = ScriptTypeKind::Value;
    type->Builtin = ScriptBuiltin::Box;
    type->Base = _domain->_objectType;
    type->Module = &module;
    type->Layout = function.ResultShape;
    type->InstanceSize = (ScriptObjectHeaderBytes + shape.Size + 7) & ~7u;
    type->PayloadOffset = type->InstanceSize;
    CollectScriptReferences(*module.Data, function.ResultShape, ScriptObjectHeaderBytes, type->References);
    type->ValueAccess = BoxAccessOf(shape);
    type->VirtualSlots = _domain->_objectType->VirtualSlots;
    const ScriptType &result = *type;
    _syntheticTypes.Add(name, Move(type));
    return result;
}

const ScriptType &ScriptInterpreter::IteratorType(const ScriptFunctionInfo &function)
{
    const String name = Format("$Iterator:{0}", function.Name.AsView());
    if (const UniquePtr<ScriptType> *found = _syntheticTypes.Find(name))
    {
        return **found;
    }
    const ScriptLoadedModule &module = *function.Module;
    const uint32 currentSlots = function.ResultShape != ~0u ? module.ShapeSlots[function.ResultShape] : 0;
    const auto addFunction = [&](StringView functionName, ScriptNativeThunk thunk, uint32 returnSlots,
                                 uint32 resultShape) {
        UniquePtr<ScriptFunctionInfo> info = MakeUnique<ScriptFunctionInfo>();
        info->Name = String(functionName);
        info->Module = &module;
        info->ParameterSlots = 1;
        info->FrameSlots = 1;
        info->ReturnSlots = returnSlots;
        info->CallSlots = returnSlots > 1 ? returnSlots : 1;
        info->ParameterOffsets.Add(0);
        info->ParameterReferences.Add(0);
        info->ResultShape = resultShape;
        info->Native = thunk;
        const ScriptFunctionInfo *result = info.Get();
        _syntheticFunctions.Add(Move(info));
        return result;
    };
    const ScriptFunctionInfo *moveNext = addFunction("Iterator.MoveNext", &IteratorMoveNext, 1, ~0u);
    const ScriptFunctionInfo *current = addFunction("Iterator.Current", &IteratorCurrent, currentSlots,
                                                    function.ResultShape);
    const ScriptFunctionInfo *getEnumerator =
        addFunction("Iterator.GetEnumerator", &IteratorGetEnumerator, 1, ~0u);
    UniquePtr<ScriptType> type = MakeUnique<ScriptType>();
    type->Name = name;
    type->Kind = ScriptTypeKind::Class;
    type->Builtin = ScriptBuiltin::Iterator;
    type->Flags = ScriptTypeFlags::Sealed;
    type->Base = _domain->_objectType;
    type->Module = &module;
    type->PayloadOffset = ScriptObjectHeaderBytes;
    type->InstanceSize = ScriptObjectHeaderBytes + sizeof(ScriptIteratorState);
    type->VirtualSlots = _domain->_objectType->VirtualSlots;
    // RECOVERY-DECISION S9: the iterator implements IEnumerable<T> and IEnumerator<T> by slot: GetEnumerator,
    // then MoveNext and Current, the order the core library declares them in.
    const ScriptShape &signature = module.GetShape(function.Record->Signature);
    StringView returned;
    if (signature.Element != ~0u && module.GetShape(signature.Element).Kind == ScriptShapeKind::Reference)
    {
        returned = module.GetString(module.GetShape(signature.Element).Name);
    }
    static constexpr StringView Enumerable = "Marlin.IEnumerable<";
    static constexpr StringView Enumerator = "Marlin.IEnumerator<";
    StringView arguments;
    if (returned.StartsWith(Enumerable))
    {
        arguments = returned.RemoveStart(Enumerable.Length());
    }
    else if (returned.StartsWith(Enumerator))
    {
        arguments = returned.RemoveStart(Enumerator.Length());
    }
    if (!arguments.IsEmpty())
    {
        const String enumerableName = Format("{0}{1}", Enumerable, arguments);
        const String enumeratorName = Format("{0}{1}", Enumerator, arguments);
        if (const ScriptType *enumerable = _domain->FindType(enumerableName.AsView()))
        {
            ScriptInterfaceMethods &methods = type->Interfaces.AddDefaulted();
            methods.Interface = enumerable;
            methods.Methods.Add(getEnumerator);
        }
        if (const ScriptType *enumerator = _domain->FindType(enumeratorName.AsView()))
        {
            ScriptInterfaceMethods &methods = type->Interfaces.AddDefaulted();
            methods.Interface = enumerator;
            methods.Methods.Add(moveNext);
            methods.Methods.Add(current);
        }
    }
    const ScriptType &result = *type;
    _syntheticTypes.Add(name, Move(type));
    return result;
}

ScriptObject *ScriptInterpreter::NewTaskObject(const ScriptType &type, uint32 state)
{
    ScriptObject *task = _domain->NewObject(type);
    if (task == nullptr)
    {
        return nullptr;
    }
    ScriptTaskState &taskState = StateOf<ScriptTaskState>(task);
    taskState.State = state;
    taskState.Id = ++_taskIds;
    taskState.Sequence = ++_sequence;
    return task;
}

bool ScriptInterpreter::StartAsync(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window)
{
    // The task's type: Task, or Task<T>, which the VM makes when no module declares it.
    const ScriptLoadedModule &module = *function.Module;
    const ScriptShape &signature = module.GetShape(function.Record->Signature);
    const ScriptType *type = nullptr;
    if (signature.Element != ~0u)
    {
        const ScriptShape &returned = module.GetShape(signature.Element);
        if (returned.Element != ~0u && returned.Element < module.Types.Count())
        {
            type = module.Types[returned.Element];
        }
        else if (returned.Kind == ScriptShapeKind::Reference)
        {
            const StringView name = module.GetString(returned.Name);
            type = _domain->FindType(name);
            if (type == nullptr)
            {
                const ScriptType *task = _domain->FindType("Marlin.Task");
                if (const UniquePtr<ScriptType> *found = _syntheticTypes.Find(name))
                {
                    type = found->Get();
                }
                else if (task != nullptr)
                {
                    UniquePtr<ScriptType> made = MakeUnique<ScriptType>(*task);
                    made->Name = String(name);
                    made->Base = task;
                    made->Info = nullptr;
                    type = made.Get();
                    _syntheticTypes.Add(String(name), Move(made));
                }
            }
        }
    }
    if (type == nullptr || type->Builtin != ScriptBuiltin::Task)
    {
        Raise(coroutine, ScriptFault::Panic, "an async function returns Task or Task<T>", function.Name.AsView());
        return false;
    }
    // The arguments wait in the caller's window, which no map covers, while the task object is made.
    _roots.Add({window, &function, false, 0});
    _domain->CollectIfRequested();
    ScriptObject *task = NewTaskObject(*type, ScriptTaskStates::Running);
    _roots.Pop();
    if (task == nullptr)
    {
        Raise(coroutine, ScriptFault::Panic, "the script heap is exhausted", function.Name.AsView());
        return false;
    }
    ScriptCoroutine &taskCoroutine = NewCoroutine();
    taskCoroutine.Owner = task;
    taskCoroutine.Entry = &function;
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    state.Coroutine = taskCoroutine.Index + 1;
    state.Unused = 1;
    uint8 *bottom = taskCoroutine.Segments[0].Begin;
    MemCopy(bottom, window, usize{function.ParameterSlots} * ScriptSlotBytes);
    if (!PushFrame(taskCoroutine, function, bottom, bottom))
    {
        FreeCoroutine(taskCoroutine.Index);
        Raise(coroutine, ScriptFault::StackOverflow, "the script stack is full");
        return false;
    }
    const uint32 index = taskCoroutine.Index;
    const ScriptRunStatus status = Run(taskCoroutine, 0);
    task = _coroutines[index]->Owner;
    FinishTask(task, *_coroutines[index], status);
    StateOf<ScriptTaskState>(task).Unused = 0;
    MemCopy(window, &task, sizeof(task));
    return true;
}

bool ScriptInterpreter::StartIterator(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function, uint8 *window)
{
    const ScriptType &type = IteratorType(function);
    _roots.Add({window, &function, false, 0});
    ScriptObject *iterator = AllocateAtSafepoint(type, 0, false);
    _roots.Pop();
    if (iterator == nullptr)
    {
        Raise(coroutine, ScriptFault::Panic, "the script heap is exhausted", function.Name.AsView());
        return false;
    }
    ScriptCoroutine &iteratorCoroutine = NewCoroutine();
    iteratorCoroutine.Owner = iterator;
    iteratorCoroutine.Entry = &function;
    ScriptIteratorState &state = StateOf<ScriptIteratorState>(iterator);
    state.Function = &function;
    state.State = 0;
    state.Coroutine = iteratorCoroutine.Index + 1;
    state.Current = 0;
    state.CurrentSlots = function.ResultShape != ~0u ? function.Module->ShapeSlots[function.ResultShape] : 0;
    const usize reserved = usize{state.CurrentSlots == 0 ? 1 : state.CurrentSlots} * ScriptSlotBytes;
    iteratorCoroutine.Reserved = static_cast<uint8 *>(Memory::Allocate(reserved, ScriptSlotBytes));
    MemZero(iteratorCoroutine.Reserved, reserved);
    uint8 *bottom = iteratorCoroutine.Segments[0].Begin;
    MemCopy(bottom, window, usize{function.ParameterSlots} * ScriptSlotBytes);
    if (!PushFrame(iteratorCoroutine, function, bottom, bottom))
    {
        FreeCoroutine(iteratorCoroutine.Index);
        Raise(coroutine, ScriptFault::StackOverflow, "the script stack is full");
        return false;
    }
    MemCopy(window, &iterator, sizeof(iterator));
    return true;
}

void ScriptInterpreter::FinishTask(ScriptObject *task, ScriptCoroutine &coroutine, ScriptRunStatus status)
{
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    if (status == ScriptRunStatus::Suspended && state.State != ScriptTaskStates::Canceled)
    {
        state.State = ScriptTaskStates::Pending;
        return;
    }
    const ScriptFunctionInfo *entry = coroutine.Entry;
    uint8 *bottom = coroutine.Segments[0].Begin;
    switch (status)
    {
    case ScriptRunStatus::Returned:
        if (entry != nullptr && entry->ResultShape != ~0u)
        {
            const ScriptType &boxType = ResultBoxType(*entry);
            ScriptObject *box = _domain->NewObject(boxType);
            if (box != nullptr)
            {
                const bool typed = !boxType.References.IsEmpty() &&
                                   GetScriptAccess(boxType.ValueAccess) == ScriptAccess::Bytes;
                _domain->WriteAccess(box->GetData(), bottom,
                                     typed ? MakeScriptAccess(ScriptAccess::Typed, boxType.Layout)
                                           : boxType.ValueAccess,
                                     *entry->Module);
                _domain->WriteReference(&state.Result, box);
            }
        }
        FreeCoroutine(coroutine.Index);
        state.Coroutine = 0;
        SettleTask(task, ScriptTaskStates::Completed);
        return;
    case ScriptRunStatus::Faulted:
        _taskFaults.Set(state.Id, _fault);
        FreeCoroutine(coroutine.Index);
        state.Coroutine = 0;
        if (state.WaiterCount == 0 && state.Unused == 0)
        {
            ReportUnobserved(task);
        }
        SettleTask(task, ScriptTaskStates::Faulted);
        return;
    default:
        FreeCoroutine(coroutine.Index);
        state.Coroutine = 0;
        SettleTask(task, ScriptTaskStates::Canceled);
        return;
    }
}

void ScriptInterpreter::ReportUnobserved(ScriptObject *task)
{
    const ScriptFaultInfo *fault = _taskFaults.Find(StateOf<ScriptTaskState>(task).Id);
    if (fault != nullptr)
    {
        MARLIN_LOG(LogScript, Warning, "A task faulted with no one awaiting it: {0}", fault->ToString().AsView());
    }
}

void ScriptInterpreter::AddWaiter(ScriptObject *task, ScriptObject *waiter)
{
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    if (state.Waiters == nullptr || state.WaiterCount == state.Waiters->Length)
    {
        const uint32 capacity = state.WaiterCount < 2 ? 4 : state.WaiterCount * 2;
        ScriptArray *grown = _domain->NewArray(*_domain->_referenceArrayType, capacity);
        if (grown == nullptr)
        {
            return;
        }
        for (uint32 i = 0; i < state.WaiterCount; ++i)
        {
            _domain->WriteReference(ElementAt(grown, i), ElementOf(state.Waiters, i));
        }
        _domain->WriteReference(reinterpret_cast<ScriptObject **>(&state.Waiters), grown);
    }
    _domain->WriteReference(ElementAt(state.Waiters, state.WaiterCount), waiter);
    ++state.WaiterCount;
}

bool ScriptInterpreter::CombinatorDone(ScriptObject *task)
{
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    if (IsSettled(state))
    {
        return true;
    }
    const auto *tasks = static_cast<const ScriptArray *>(state.Subject);
    const bool all = state.Wait == ScriptTaskWaits::All;
    bool done = all;
    for (uint32 i = 0; tasks != nullptr && i < tasks->Length; ++i)
    {
        ScriptObject *element = ElementOf(tasks, i);
        const bool settled = element == nullptr || IsSettled(StateOf<ScriptTaskState>(element));
        if (all && !settled)
        {
            done = false;
            break;
        }
        if (!all && settled)
        {
            done = true;
            break;
        }
    }
    if (done)
    {
        SettleTask(task, ScriptTaskStates::Completed);
    }
    return done;
}

void ScriptInterpreter::SettleTask(ScriptObject *task, uint32 state)
{
    StateOf<ScriptTaskState>(task).State = state;
    WakeWaiters(task);
}

void ScriptInterpreter::WakeWaiters(ScriptObject *task)
{
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    ScriptArray *waiters = state.Waiters;
    const uint32 count = state.WaiterCount;
    state.WaiterCount = 0;
    _domain->WriteReference(reinterpret_cast<ScriptObject **>(&state.Waiters), nullptr);
    for (uint32 i = 0; waiters != nullptr && i < count; ++i)
    {
        ScriptObject *waiter = ElementOf(waiters, i);
        if (waiter == nullptr)
        {
            continue;
        }
        ScriptTaskState &waiting = StateOf<ScriptTaskState>(waiter);
        if (IsSettled(waiting))
        {
            continue;
        }
        if (waiting.Wait == ScriptTaskWaits::All || waiting.Wait == ScriptTaskWaits::Any)
        {
            CombinatorDone(waiter);
        }
        else if (waiting.Coroutine != 0 && waiting.Queued == 0)
        {
            waiting.Wait = ScriptTaskWaits::None;
            waiting.Queued = 1;
            _ready.Add(waiter);
        }
    }
}

void ScriptInterpreter::Resume(ScriptObject *task)
{
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    if (IsSettled(state) || state.Coroutine == 0)
    {
        return;
    }
    const uint32 index = state.Coroutine - 1;
    state.State = ScriptTaskStates::Running;
    state.Wait = ScriptTaskWaits::None;
    BeginEntry();
    const ScriptRunStatus status = Run(*_coroutines[index], 0);
    EndEntry();
    FinishTask(_coroutines[index]->Owner, *_coroutines[index], status);
}

void ScriptInterpreter::RunReady()
{
    // Tasks woken meanwhile are added at the end and run in this pass too.
    while (_readyHead < _ready.Count())
    {
        ScriptObject *task = _ready[_readyHead];
        _ready[_readyHead++] = nullptr;
        if (task != nullptr)
        {
            StateOf<ScriptTaskState>(task).Queued = 0;
            Resume(task);
        }
    }
    _ready.Clear();
    _readyHead = 0;
}

void ScriptInterpreter::AdvanceTime(float64 seconds)
{
    _time += seconds;
}

ScriptRunStatus ScriptInterpreter::InvokeDelegate(uint8 *&window)
{
    ScriptCoroutine &coroutine = *_running;
    auto *delegate = static_cast<ScriptDelegateObject *>(*reinterpret_cast<ScriptObject **>(window));
    if (delegate == nullptr)
    {
        Raise(coroutine, ScriptFault::NullReference, "the delegate is null");
        return ScriptRunStatus::Faulted;
    }
    if (delegate->Invocations != nullptr)
    {
        return CallDelegate(coroutine, window, 0, false, 1);
    }
    const ScriptFunctionInfo &function = *delegate->Function;
    if (function.ParameterSlots > 0)
    {
        MemCopy(window, &delegate->Target, sizeof(ScriptObject *));
        return CallNested(coroutine, function, window);
    }
    const ScriptRunStatus status = CallNested(coroutine, function, window + ScriptSlotBytes);
    MemMove(window, window + ScriptSlotBytes, usize{function.ReturnSlots} * ScriptSlotBytes);
    return status;
}

void ScriptInterpreter::BeginFrame()
{
    // What is due: every frame waiter and the delays whose time has come, in the order the tasks were made. Tasks that
    // start waiting while this runs wait for the next frame.
    _due = Move(_frameWaiters);
    _frameWaiters.Clear();
    const usize frameCount = _due.Count();
    while (!_delays.IsEmpty() && _time >= StateOf<ScriptTaskState>(_delays[0]).WakeTime)
    {
        _due.Add(_delays[0]);
        RemoveFromHeap(_delays, 0);
    }
    if (_due.Count() != frameCount)
    {
        _due.StableSort(BySequence);
    }
    for (usize i = 0; i < _due.Count(); ++i)
    {
        ScriptTaskState &state = StateOf<ScriptTaskState>(_due[i]);
        if (IsSettled(state))
        {
            continue;
        }
        if (state.Wait != ScriptTaskWaits::Until)
        {
            SettleTask(_due[i], ScriptTaskStates::Completed);
            continue;
        }
        // Frame.Until: the condition runs on a stack of its own at the frame boundary.
        ScriptCoroutine &coroutine = NewCoroutine();
        const uint32 index = coroutine.Index;
        uint8 *window = coroutine.Segments[0].Begin;
        MemZero(window, 2 * ScriptSlotBytes);
        MemCopy(window, &state.Subject, sizeof(ScriptObject *));
        ScriptCoroutine *saved = _running;
        _running = &coroutine;
        BeginEntry();
        const ScriptRunStatus status = InvokeDelegate(window);
        EndEntry();
        _running = saved;
        uint32 met = 0;
        MemCopy(&met, window, sizeof(met));
        FreeCoroutine(index);
        ScriptObject *task = _due[i];
        if (status == ScriptRunStatus::Faulted)
        {
            _taskFaults.Set(StateOf<ScriptTaskState>(task).Id, _fault);
            SettleTask(task, ScriptTaskStates::Faulted);
        }
        else if (met != 0)
        {
            SettleTask(task, ScriptTaskStates::Completed);
        }
    }
    // The conditions not met yet wait on, ahead of the tasks that started waiting meanwhile.
    _due.RemoveAll([](ScriptObject *task) { return IsSettled(StateOf<ScriptTaskState>(task)); });
    _due.AddRange(_frameWaiters.AsSpan());
    _frameWaiters = Move(_due);
    _due.Clear();
}

void ScriptInterpreter::FixedTick()
{
    for (usize i = 0; i < _fixedWaiters.Count(); ++i)
    {
        if (!IsSettled(StateOf<ScriptTaskState>(_fixedWaiters[i])))
        {
            SettleTask(_fixedWaiters[i], ScriptTaskStates::Completed);
        }
    }
    _fixedWaiters.Clear();
}

void ScriptInterpreter::CancelTask(ScriptObject *task)
{
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    if (IsSettled(state))
    {
        return;
    }
    if (state.State == ScriptTaskStates::Running)
    {
        // A running task ends at its next suspension point.
        state.State = ScriptTaskStates::Canceled;
        return;
    }
    if (state.Coroutine != 0)
    {
        FreeCoroutine(state.Coroutine - 1);
        state.Coroutine = 0;
    }
    if (state.Queued != 0)
    {
        for (usize i = _readyHead; i < _ready.Count(); ++i)
        {
            _ready[i] = _ready[i] == task ? nullptr : _ready[i];
        }
        state.Queued = 0;
    }
    switch (state.Wait)
    {
    case ScriptTaskWaits::NextFrame:
    case ScriptTaskWaits::Until:
        _frameWaiters.Remove(task);
        break;
    case ScriptTaskWaits::NextFixed:
        _fixedWaiters.Remove(task);
        break;
    case ScriptTaskWaits::Delay:
        if (const usize index = _delays.IndexOf(task); index != static_cast<usize>(-1))
        {
            RemoveFromHeap(_delays, index);
        }
        break;
    default:
        break;
    }
    SettleTask(task, ScriptTaskStates::Canceled);
}

void ScriptInterpreter::CancelTasksOwnedBy(const ScriptObject *owner)
{
    if (owner == nullptr)
    {
        return;
    }
    List<ScriptObject *> owned;
    for (const UniquePtr<ScriptCoroutine> &coroutine : _coroutines)
    {
        ScriptObject *task = coroutine->Owner;
        if (task != nullptr && task->Type->Builtin == ScriptBuiltin::Task &&
            StateOf<ScriptTaskState>(task).Owner == owner)
        {
            owned.Add(task);
        }
    }
    for (const List<ScriptObject *> *waiters : {&_frameWaiters, &_fixedWaiters, &_delays})
    {
        for (ScriptObject *task : *waiters)
        {
            if (StateOf<ScriptTaskState>(task).Owner == owner)
            {
                owned.Add(task);
            }
        }
    }
    for (ScriptObject *task : owned)
    {
        CancelTask(task);
    }
}

ScriptTaskStatus ScriptInterpreter::GetTaskStatus(const ScriptObject *task, ScriptFaultInfo *fault) const
{
    if (task == nullptr)
    {
        return ScriptTaskStatus::Canceled;
    }
    const ScriptTaskState &state = StateOf<ScriptTaskState>(const_cast<ScriptObject *>(task));
    switch (state.State)
    {
    case ScriptTaskStates::Completed:
        return ScriptTaskStatus::Completed;
    case ScriptTaskStates::Faulted:
        if (fault != nullptr)
        {
            if (const ScriptFaultInfo *found = _taskFaults.Find(state.Id))
            {
                *fault = *found;
            }
        }
        return ScriptTaskStatus::Faulted;
    case ScriptTaskStates::Canceled:
        return ScriptTaskStatus::Canceled;
    default:
        return ScriptTaskStatus::Pending;
    }
}

uint32 ScriptInterpreter::GetTaskId(ScriptObject *task)
{
    return StateOf<ScriptTaskState>(task).Id;
}

ScriptObject *ScriptInterpreter::NewWaitTask(uint32 wait, float64 seconds, ScriptObject *subject)
{
    const ScriptType *type = _domain->FindType("Marlin.Task");
    if (type == nullptr)
    {
        return nullptr;
    }
    ScriptObject *task = NewTaskObject(*type, ScriptTaskStates::Pending);
    if (task == nullptr)
    {
        return nullptr;
    }
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    state.Wait = wait;
    state.WakeTime = _time + seconds;
    _domain->WriteReference(&state.Subject, subject);
    switch (wait)
    {
    case ScriptTaskWaits::NextFixed:
        _fixedWaiters.Add(task);
        break;
    case ScriptTaskWaits::Delay:
        _delays.Add(task);
        SiftUp(_delays, _delays.Count() - 1);
        break;
    default:
        _frameWaiters.Add(task);
        break;
    }
    return task;
}

void ScriptInterpreter::IteratorMoveNext(ScriptCallContext &context, const void *)
{
    ScriptInterpreter &self = Of(context);
    ScriptObject *iterator = context.Get<ScriptObject *>(0);
    ScriptIteratorState *state = &StateOf<ScriptIteratorState>(iterator);
    if (state->State == 2 || state->Coroutine == 0)
    {
        context.Return(false);
        return;
    }
    const uint32 index = state->Coroutine - 1;
    ScriptCoroutine &coroutine = *self._coroutines[index];
    if (state->State == 1 && !coroutine.Frames.IsEmpty())
    {
        ScriptFrame &frame = coroutine.Frames.Last();
        frame.Pc += InstructionWords(*frame.Function->Record, frame.Pc);
    }
    state->State = 1;
    state->Current = 0;
    const ScriptRunStatus status = self.Run(coroutine, 0);
    iterator = context.Get<ScriptObject *>(0);
    state = &StateOf<ScriptIteratorState>(iterator);
    if (status == ScriptRunStatus::Suspended)
    {
        context.Return(true);
        return;
    }
    state->State = 2;
    state->Current = 0;
    state->Coroutine = 0;
    self.FreeCoroutine(index);
    if (status == ScriptRunStatus::Faulted)
    {
        context.Fault(self._fault.Kind, self._fault.Message.AsView());
        return;
    }
    context.Return(false);
}

void ScriptInterpreter::IteratorCurrent(ScriptCallContext &context, const void *)
{
    ScriptInterpreter &self = Of(context);
    ScriptObject *iterator = context.Get<ScriptObject *>(0);
    const ScriptIteratorState &state = StateOf<ScriptIteratorState>(iterator);
    if (state.Current == 0 || state.Coroutine == 0)
    {
        context.Fault(ScriptFault::Panic, "Current is read before MoveNext or after the end");
        return;
    }
    const ScriptCoroutine &coroutine = *self._coroutines[state.Coroutine - 1];
    MemCopy(context.GetResult(), coroutine.Reserved, usize{state.CurrentSlots} * ScriptSlotBytes);
}

void ScriptInterpreter::IteratorGetEnumerator(ScriptCallContext &context, const void *)
{
    context.Return(context.Get<ScriptObject *>(0));
}

void ScriptInterpreter::TaskIsCompleted(ScriptCallContext &context, const void *)
{
    context.Return(IsSettled(StateOf<ScriptTaskState>(context.Get<ScriptObject *>(0))));
}

void ScriptInterpreter::TaskIsCanceled(ScriptCallContext &context, const void *)
{
    context.Return(StateOf<ScriptTaskState>(context.Get<ScriptObject *>(0)).State == ScriptTaskStates::Canceled);
}

void ScriptInterpreter::TaskIsFaulted(ScriptCallContext &context, const void *)
{
    context.Return(StateOf<ScriptTaskState>(context.Get<ScriptObject *>(0)).State == ScriptTaskStates::Faulted);
}

void ScriptInterpreter::TaskCancel(ScriptCallContext &context, const void *)
{
    Of(context).CancelTask(context.Get<ScriptObject *>(0));
}

void ScriptInterpreter::TaskWhen(ScriptCallContext &context, const void *target)
{
    ScriptInterpreter &self = Of(context);
    auto *tasks = context.Get<ScriptArray *>(0);
    if (tasks == nullptr)
    {
        context.Fault(ScriptFault::NullReference, "the task array is null");
        return;
    }
    const ScriptType *type = context.GetDomain().FindType("Marlin.Task");
    ScriptObject *combinator = type != nullptr ? self.NewTaskObject(*type, ScriptTaskStates::Pending) : nullptr;
    if (combinator == nullptr)
    {
        context.Fault(ScriptFault::Panic, "the script heap is exhausted");
        return;
    }
    ScriptTaskState &state = StateOf<ScriptTaskState>(combinator);
    state.Wait = static_cast<uint32>(reinterpret_cast<uintptr>(target));
    context.GetDomain().WriteReference(&state.Subject, tasks);
    if (!self.CombinatorDone(combinator))
    {
        for (uint32 i = 0; i < tasks->Length; ++i)
        {
            ScriptObject *element = ElementOf(tasks, i);
            if (element != nullptr && !IsSettled(StateOf<ScriptTaskState>(element)))
            {
                self.AddWaiter(element, combinator);
            }
        }
    }
    context.Return(combinator);
}

void ScriptInterpreter::TaskRun(ScriptCallContext &context, const void *)
{
    ScriptInterpreter &self = Of(context);
    if (context.Get<ScriptObject *>(0) == nullptr)
    {
        context.Fault(ScriptFault::NullReference, "the function is null");
        return;
    }
    // The function runs above the native's window, which keeps the owner.
    uint8 *window = context.GetParameter(0) + usize{context.GetFunction().CallSlots} * ScriptSlotBytes;
    MemZero(window, 2 * ScriptSlotBytes);
    ScriptObject *function = context.Get<ScriptObject *>(0);
    MemCopy(window, &function, sizeof(function));
    const ScriptRunStatus status = self.InvokeDelegate(window);
    if (status == ScriptRunStatus::Faulted)
    {
        context.Fault(self._fault.Kind, self._fault.Message.AsView());
        return;
    }
    ScriptObject *task;
    MemCopy(&task, window, sizeof(task));
    if (task != nullptr)
    {
        context.GetDomain().WriteReference(&StateOf<ScriptTaskState>(task).Owner, context.Get<ScriptObject *>(1));
    }
    context.Return(task);
}

void ScriptInterpreter::TaskResult(ScriptCallContext &context, const void *)
{
    ScriptInterpreter &self = Of(context);
    ScriptObject *task = context.Get<ScriptObject *>(0);
    const ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    switch (state.State)
    {
    case ScriptTaskStates::Completed:
        if (state.Result != nullptr)
        {
            ReadBoxed(context.GetResult(), state.Result);
        }
        else
        {
            MemZero(context.GetResult(), usize{context.GetFunction().ReturnSlots} * ScriptSlotBytes);
        }
        return;
    case ScriptTaskStates::Faulted: {
        const ScriptFaultInfo *fault = self._taskFaults.Find(state.Id);
        context.Fault(fault != nullptr ? fault->Kind : ScriptFault::Panic, "the task faulted");
        return;
    }
    case ScriptTaskStates::Canceled:
        context.Fault(ScriptFault::Panic, "the task was canceled");
        return;
    default:
        context.Fault(ScriptFault::Panic, "the task has not completed");
        return;
    }
}

void ScriptInterpreter::FrameWait(ScriptCallContext &context, const void *target)
{
    ScriptInterpreter &self = Of(context);
    const uint32 wait = static_cast<uint32>(reinterpret_cast<uintptr>(target));
    float64 seconds = 0;
    ScriptObject *subject = nullptr;
    if (wait == ScriptTaskWaits::Delay)
    {
        seconds = context.Get<float32>(0);
    }
    else if (wait == ScriptTaskWaits::Until)
    {
        subject = context.Get<ScriptObject *>(0);
        if (subject == nullptr)
        {
            context.Fault(ScriptFault::NullReference, "the condition is null");
            return;
        }
    }
    ScriptObject *task = self.NewWaitTask(wait, seconds, subject);
    if (task == nullptr)
    {
        context.Fault(ScriptFault::Panic, "the script heap is exhausted");
        return;
    }
    context.Return(task);
}

void ScriptInterpreter::RegisterNatives(ScriptNativeRegistry &registry)
{
    const auto wait = [](uint32 kind) { return reinterpret_cast<const void *>(static_cast<uintptr>(kind)); };
    registry.Register("Frame.Next", &FrameWait, wait(ScriptTaskWaits::NextFrame));
    registry.Register("Frame.NextFixed", &FrameWait, wait(ScriptTaskWaits::NextFixed));
    registry.Register("Frame.Delay", &FrameWait, wait(ScriptTaskWaits::Delay));
    registry.Register("Frame.Until", &FrameWait, wait(ScriptTaskWaits::Until));
    registry.Register("Task.IsCompleted", &TaskIsCompleted, nullptr);
    registry.Register("Task.IsCanceled", &TaskIsCanceled, nullptr);
    registry.Register("Task.IsFaulted", &TaskIsFaulted, nullptr);
    registry.Register("Task.Cancel", &TaskCancel, nullptr);
    registry.Register("Task.WhenAll", &TaskWhen, wait(ScriptTaskWaits::All));
    registry.Register("Task.WhenAny", &TaskWhen, wait(ScriptTaskWaits::Any));
    registry.Register("Task.Run", &TaskRun, nullptr);
    registry.Register("Task.Result", &TaskResult, nullptr);
    registry.Register("Iterator.MoveNext", &IteratorMoveNext, nullptr);
    registry.Register("Iterator.Current", &IteratorCurrent, nullptr);
    registry.Register("Iterator.GetEnumerator", &IteratorGetEnumerator, nullptr);
}
} // namespace Marlin
