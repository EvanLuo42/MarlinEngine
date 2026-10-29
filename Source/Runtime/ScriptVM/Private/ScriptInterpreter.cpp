#include "ScriptInterpreter.h"

#include "HAL/PlatformTime.h"
#include "Math/Float16.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "ScriptCollector.h"
#include "ScriptHeap.h"
#include "ScriptMath.h"
#include "String/Format.h"
#include "Templates/Bit.h"
#include "Templates/NumericLimits.h"

namespace Marlin
{
namespace
{
template <typename T> T Rd(const uint8 *slot)
{
    T value;
    MemCopy(&value, slot, sizeof(T));
    return value;
}

template <typename T> void Wr(uint8 *slot, T value)
{
    MemCopy(slot, &value, sizeof(T));
}

ScriptObject *RdRef(const uint8 *slot)
{
    return Rd<ScriptObject *>(slot);
}

/// Where the elements of an array with element word *word* start.
uint32 ElementDataOffset(uint32 word)
{
    const uint32 alignment = 1u << (word >> 24);
    const uint32 start = ScriptArrayLengthOffset + 4;
    return (start + alignment - 1) / alignment * alignment;
}

/// The bytes an access reads or writes in memory; 0 for Typed, whose size its shape gives.
uint32 AccessBytes(uint32 access)
{
    switch (GetScriptAccess(access))
    {
    case ScriptAccess::I8:
    case ScriptAccess::U8:
        return 1;
    case ScriptAccess::I16:
    case ScriptAccess::U16:
        return 2;
    case ScriptAccess::B32:
        return 4;
    case ScriptAccess::B64:
    case ScriptAccess::Reference:
        return 8;
    case ScriptAccess::Bytes:
        return GetScriptAccessPayload(access);
    case ScriptAccess::Typed:
        return 0;
    }
    return 0;
}

/// Copies *size* bytes, with constant-size copies for the sizes of vectors so they need no call.
void CopyBytes(uint8 *target, const uint8 *source, uint32 size)
{
    switch (size)
    {
    case 8:
        MemCopy(target, source, 8);
        return;
    case 12:
        MemCopy(target, source, 12);
        return;
    case 16:
        MemCopy(target, source, 16);
        return;
    default:
        MemCopy(target, source, size);
        return;
    }
}

/// Reads memory into registers: narrow integers are extended to 32 bits.
void ReadAccess(uint8 *registers, const uint8 *memory, uint32 access)
{
    switch (GetScriptAccess(access))
    {
    case ScriptAccess::I8:
        Wr<int32>(registers, static_cast<int8>(memory[0]));
        return;
    case ScriptAccess::U8:
        Wr<uint32>(registers, memory[0]);
        return;
    case ScriptAccess::I16:
        Wr<int32>(registers, Rd<int16>(memory));
        return;
    case ScriptAccess::U16:
        Wr<uint32>(registers, Rd<uint16>(memory));
        return;
    case ScriptAccess::Bytes:
        CopyBytes(registers, memory, GetScriptAccessPayload(access));
        return;
    default:
        MemCopy(registers, memory, AccessBytes(access));
        return;
    }
}

/// Writes registers to memory without references, which needs no write barrier, inline; false for the accesses
/// ScriptDomain::WriteAccess must do.
bool WritePlainAccess(uint8 *memory, const uint8 *registers, uint32 access)
{
    switch (GetScriptAccess(access))
    {
    case ScriptAccess::B32:
        MemCopy(memory, registers, 4);
        return true;
    case ScriptAccess::B64:
        MemCopy(memory, registers, 8);
        return true;
    case ScriptAccess::Bytes:
        CopyBytes(memory, registers, GetScriptAccessPayload(access));
        return true;
    default:
        return false;
    }
}

/// A register of *number* as a 64-bit signed integer; unsigned 64-bit values are reinterpreted.
int64 SignExtendRegister(const uint8 *slot, ScriptNumber number)
{
    switch (number)
    {
    case ScriptNumber::I8:
    case ScriptNumber::I16:
    case ScriptNumber::I32:
        return Rd<int32>(slot);
    case ScriptNumber::U8:
    case ScriptNumber::U16:
    case ScriptNumber::U32:
    case ScriptNumber::Bool:
        return Rd<uint32>(slot);
    default:
        return Rd<int64>(slot);
    }
}

/// A float converted to an integer type the way unchecked script conversions do: NaN is 0 and values outside the
/// range saturate.
template <typename TInt, typename TFloat> TInt Saturate(TFloat value)
{
    if (value != value)
    {
        return 0;
    }
    if (value <= static_cast<TFloat>(NumericLimits<TInt>::Min()))
    {
        return NumericLimits<TInt>::Min();
    }
    if (value >= static_cast<TFloat>(NumericLimits<TInt>::Max()))
    {
        return NumericLimits<TInt>::Max();
    }
    return static_cast<TInt>(value);
}

bool IsFloatKind(ScriptNumber number)
{
    return number == ScriptNumber::F16 || number == ScriptNumber::F32 || number == ScriptNumber::F64;
}

/// ConvChecked: false when the value does not fit *to* (MarlinScript §2.3).
bool ConvertChecked(ScriptNumber from, ScriptNumber to, const uint8 *source, uint8 *target)
{
    float64 real = 0;
    int64 integer = 0;
    uint64 unsignedValue = 0;
    bool isUnsigned64 = from == ScriptNumber::U64;
    if (IsFloatKind(from))
    {
        real = from == ScriptNumber::F64   ? Rd<float64>(source)
               : from == ScriptNumber::F32 ? static_cast<float64>(Rd<float32>(source))
                                           : static_cast<float64>(Float16::FromBits(Rd<uint16>(source)).ToFloat());
        if (real != real)
        {
            return false;
        }
    }
    else if (isUnsigned64)
    {
        unsignedValue = Rd<uint64>(source);
    }
    else
    {
        integer = SignExtendRegister(source, from);
    }
    if (IsFloatKind(to))
    {
        const float64 value = IsFloatKind(from) ? real
                              : isUnsigned64    ? static_cast<float64>(unsignedValue)
                                                : static_cast<float64>(integer);
        if (to == ScriptNumber::F64)
        {
            Wr<float64>(target, value);
        }
        else if (to == ScriptNumber::F32)
        {
            Wr<float32>(target, static_cast<float32>(value));
        }
        else
        {
            Wr<uint32>(target, Float16(static_cast<float32>(value)).Bits);
        }
        return true;
    }
    int64 low = 0;
    uint64 high = 0;
    switch (to)
    {
    case ScriptNumber::I8:
        low = -128;
        high = 127;
        break;
    case ScriptNumber::U8:
        high = 255;
        break;
    case ScriptNumber::I16:
        low = -32768;
        high = 32767;
        break;
    case ScriptNumber::U16:
        high = 65535;
        break;
    case ScriptNumber::I32:
        low = NumericLimits<int32>::Min();
        high = static_cast<uint64>(NumericLimits<int32>::Max());
        break;
    case ScriptNumber::U32:
    case ScriptNumber::Bool:
        high = NumericLimits<uint32>::Max();
        break;
    case ScriptNumber::I64:
        low = NumericLimits<int64>::Min();
        high = static_cast<uint64>(NumericLimits<int64>::Max());
        break;
    default:
        high = NumericLimits<uint64>::Max();
        break;
    }
    if (IsFloatKind(from))
    {
        const float64 truncated = real < 0 ? -__builtin_floor(-real) : __builtin_floor(real);
        if (truncated < static_cast<float64>(low) || truncated > static_cast<float64>(high))
        {
            return false;
        }
        if (truncated < 0)
        {
            integer = static_cast<int64>(truncated);
            isUnsigned64 = false;
        }
        else
        {
            unsignedValue = static_cast<uint64>(truncated);
            isUnsigned64 = true;
        }
    }
    if (isUnsigned64)
    {
        if (unsignedValue > high)
        {
            return false;
        }
        integer = static_cast<int64>(unsignedValue);
    }
    else if (integer < low || (integer >= 0 && static_cast<uint64>(integer) > high))
    {
        return false;
    }
    if (to == ScriptNumber::I64 || to == ScriptNumber::U64)
    {
        Wr<int64>(target, integer);
    }
    else
    {
        Wr<int32>(target, static_cast<int32>(integer));
    }
    return true;
}

using F32x4 = float32 __attribute__((ext_vector_type(4)));
using F64x2 = float64 __attribute__((ext_vector_type(2)));
using I32x4 = uint32 __attribute__((ext_vector_type(4)));
using I64x2 = uint64 __attribute__((ext_vector_type(2)));

// The dedicated F32 math instructions compute exactly what ExecuteScriptMath does for the same operations.
float32 MinOfF32(float32 a, float32 b)
{
    return a != a ? b : (b != b ? a : (b < a ? b : a));
}

float32 MaxOfF32(float32 a, float32 b)
{
    return a != a ? b : (b != b ? a : (b > a ? b : a));
}

float32 DotF32(const uint8 *a, const uint8 *b, uint32 lanes)
{
    float32 sum = 0;
    for (uint32 i = 0; i < lanes; ++i)
    {
        const float32 x = Rd<float32>(a + i * sizeof(float32));
        const float32 y = Rd<float32>(b + i * sizeof(float32));
        sum += x * y;
    }
    return sum;
}

// The lanes past the third are left as they are, as Math leaves them.
void CrossF32x3(uint8 *result, const uint8 *a, const uint8 *b)
{
    float32 x[3];
    float32 y[3];
    MemCopy(x, a, sizeof(x));
    MemCopy(y, b, sizeof(y));
    const float32 cross[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    MemCopy(result, cross, sizeof(cross));
}

void NormalizeF32x3(uint8 *result, const uint8 *a)
{
    const float32 length = __builtin_sqrtf(DotF32(a, a, 3));
    float32 x[3];
    MemCopy(x, a, sizeof(x));
    const float32 normalized[3] = {x[0] / length, x[1] / length, x[2] / length};
    MemCopy(result, normalized, sizeof(normalized));
}

/// The message of a Fault instruction without one.
StringView DefaultFaultMessage(ScriptFault kind)
{
    switch (kind)
    {
    case ScriptFault::NullReference:
        return "the value is null";
    case ScriptFault::Panic:
        return "unreachable code was reached";
    default:
        return GetScriptFaultName(kind);
    }
}
} // namespace

ScriptInterpreter::ScriptInterpreter(ScriptDomain &domain) : _domain(&domain)
{
}

ScriptInterpreter::~ScriptInterpreter()
{
    for (const UniquePtr<ScriptCoroutine> &coroutine : _coroutines)
    {
        for (const ScriptStackSegment &segment : coroutine->Segments)
        {
            Memory::Free(segment.Begin);
        }
        Memory::Free(coroutine->Reserved);
    }
}

ScriptCoroutine &ScriptInterpreter::NewCoroutine()
{
    ScriptCoroutine *coroutine = nullptr;
    if (!_freeCoroutines.IsEmpty())
    {
        coroutine = _coroutines[_freeCoroutines.Pop()].Get();
        coroutine->Free = false;
    }
    else
    {
        UniquePtr<ScriptCoroutine> created = MakeUnique<ScriptCoroutine>();
        created->Index = static_cast<uint32>(_coroutines.Count());
        coroutine = created.Get();
        _coroutines.Add(Move(created));
    }
    if (coroutine->Segments.IsEmpty())
    {
        const usize bytes = _domain->GetOptions().StackSegmentBytes;
        auto *begin = static_cast<uint8 *>(Memory::Allocate(bytes, ScriptSlotBytes));
        coroutine->Segments.Add({begin, begin + bytes});
        coroutine->StackBytes = bytes;
    }
    coroutine->Segment = 0;
    coroutine->Frames.Clear();
    coroutine->Owner = nullptr;
    coroutine->Entry = nullptr;
    return *coroutine;
}

void ScriptInterpreter::FreeCoroutine(uint32 index)
{
    if (index >= _coroutines.Count() || _coroutines[index]->Free)
    {
        return;
    }
    ScriptCoroutine &coroutine = *_coroutines[index];
    coroutine.Frames.Clear();
    coroutine.Owner = nullptr;
    coroutine.Entry = nullptr;
    coroutine.Segment = 0;
    Memory::Free(coroutine.Reserved);
    coroutine.Reserved = nullptr;
    // Keep the first segment for the next coroutine; the rest go.
    while (coroutine.Segments.Count() > 1)
    {
        const ScriptStackSegment segment = coroutine.Segments.Pop();
        coroutine.StackBytes -= static_cast<usize>(segment.End - segment.Begin);
        Memory::Free(segment.Begin);
    }
    coroutine.Free = true;
    _freeCoroutines.Add(index);
}

uint8 *ScriptInterpreter::TopOf(const ScriptCoroutine &coroutine) const
{
    if (coroutine.Frames.IsEmpty())
    {
        return coroutine.Segments[0].Begin;
    }
    const ScriptFrame &top = coroutine.Frames.Last();
    uint8 *end = top.Base + usize{top.Function->FrameSlots} * ScriptSlotBytes;
    const ScriptStackSegment &segment = coroutine.Segments[top.Segment];
    return end < segment.End ? end : segment.End;
}

uint8 *ScriptInterpreter::Reserve(ScriptCoroutine &coroutine, uint8 *at, usize bytes, uint32 &segment)
{
    if (segment < coroutine.Segments.Count() && at >= coroutine.Segments[segment].Begin &&
        at + bytes <= coroutine.Segments[segment].End)
    {
        return at;
    }
    const uint32 next = segment + 1;
    // A later segment that has room is reused; smaller ones are dropped.
    while (next < coroutine.Segments.Count())
    {
        const ScriptStackSegment &candidate = coroutine.Segments[next];
        if (static_cast<usize>(candidate.End - candidate.Begin) >= bytes)
        {
            segment = next;
            return candidate.Begin;
        }
        const ScriptStackSegment dropped = coroutine.Segments.Pop();
        coroutine.StackBytes -= static_cast<usize>(dropped.End - dropped.Begin);
        Memory::Free(dropped.Begin);
    }
    const ScriptDomainOptions &options = _domain->GetOptions();
    const usize size = bytes > options.StackSegmentBytes ? bytes : options.StackSegmentBytes;
    if (coroutine.StackBytes + size > options.MaxStackBytes)
    {
        return nullptr;
    }
    auto *begin = static_cast<uint8 *>(Memory::Allocate(size, ScriptSlotBytes));
    coroutine.Segments.Add({begin, begin + size});
    coroutine.StackBytes += size;
    segment = next;
    return begin;
}

bool ScriptInterpreter::PushFrameInNewSegment(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function,
                                              uint8 *window, uint8 *resultTo)
{
    uint32 segment = coroutine.Segment;
    const usize bytes = function.FrameBytes;
    uint8 *base = Reserve(coroutine, window, bytes, segment);
    if (base == nullptr)
    {
        return false;
    }
    if (base != window)
    {
        MemCopy(base, window, usize{function.ParameterSlots} * ScriptSlotBytes);
    }
    // Only the frame variables need zeroing: every other register a map names is written before that map applies.
    const usize parameterBytes = usize{function.ParameterSlots} * ScriptSlotBytes;
    const usize zeroBytes = usize{function.ZeroSlots} * ScriptSlotBytes;
    if (zeroBytes != 0)
    {
        MemZero(base + parameterBytes, parameterBytes + zeroBytes <= bytes ? zeroBytes : bytes - parameterBytes);
    }
    coroutine.Segment = segment;
    coroutine.Frames.Add({&function, 0, 0, base, resultTo, segment, false});
    return true;
}

void ScriptInterpreter::BeginEntry()
{
    if (_entryDepth++ == 0)
    {
        ResetWatchdog();
    }
}

void ScriptInterpreter::ResetWatchdog()
{
    const float64 limit = _domain->GetOptions().WatchdogSeconds;
    _watchdogLimit = limit > 0 ? limit : 0;
    _deadline = 0;
    _watchdogBudget = 1024;
}

void ScriptInterpreter::EndEntry()
{
    MARLIN_ASSERT(_entryDepth > 0);
    --_entryDepth;
}

bool ScriptInterpreter::CheckDeadline()
{
    _watchdogBudget = 1024;
    const float64 now = PlatformTime::Seconds();
    if (_deadline == 0)
    {
        _deadline = now + _watchdogLimit;
        return true;
    }
    return now <= _deadline;
}

void ScriptInterpreter::Raise(ScriptCoroutine &coroutine, ScriptFault kind, StringView message, StringView native)
{
    ScriptFaultInfo fault;
    fault.Kind = kind;
    fault.Message = String(message);
    if (!native.IsEmpty())
    {
        fault.Stack.Add({String(native), String(), 0, 0});
    }
    List<ScriptLineEntry> lines;
    for (usize i = coroutine.Frames.Count(); i-- > 0;)
    {
        const ScriptFrame &frame = coroutine.Frames[i];
        ScriptStackEntry entry;
        entry.Function = frame.Function->Name;
        const ScriptFunctionRecord *record = frame.Function->Record;
        lines.Clear();
        if (record != nullptr && DecodeScriptLineTable(record->LineTable.AsSpan(), lines))
        {
            const ScriptLineEntry *found = nullptr;
            for (const ScriptLineEntry &line : lines)
            {
                if (line.Pc <= frame.Pc)
                {
                    found = &line;
                }
            }
            const ScriptModuleData &data = *frame.Function->Module->Data;
            if (found != nullptr && found->File < data.Files.Count())
            {
                entry.File = String(frame.Function->Module->GetString(data.Files[found->File].Path));
                entry.Line = found->Line;
                entry.Column = found->Column;
            }
        }
        fault.Stack.Add(Move(entry));
    }
    _fault = Move(fault);
}

ScriptObject *ScriptInterpreter::AllocateAtSafepoint(const ScriptType &type, uint32 length, bool array)
{
    _domain->CollectIfRequested();
    ScriptObject *object =
        array ? static_cast<ScriptObject *>(_domain->NewArray(type, length)) : _domain->NewObject(type);
    if (object == nullptr)
    {
        // RECOVERY-DECISION S18: at a safepoint a failed allocation collects everything once and tries again
        // before the entry faults with "the script heap is exhausted".
        _domain->Collect(true);
        object = array ? static_cast<ScriptObject *>(_domain->NewArray(type, length)) : _domain->NewObject(type);
    }
    return object;
}

ScriptRunStatus ScriptInterpreter::CallNative(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function,
                                              uint8 *window)
{
    _roots.Add({window, &function, false, 0});
    ScriptCoroutine *saved = _running;
    _running = &coroutine;
    _domain->CollectIfRequested();
    ScriptCallContext context(*_domain, function, window);
    _fault.Stack.Clear();
    function.Native(context, function.NativeTarget);
    _running = saved;
    _roots.Pop();
    if (context.HasFaulted())
    {
        // A fault that ended a run nested in the native, such as an iterator's, keeps its stack above the native's.
        List<ScriptStackEntry> inner = Move(_fault.Stack);
        Raise(coroutine, context.GetFaultKind(), context.GetFaultMessage(), function.Name.AsView());
        if (!inner.IsEmpty())
        {
            inner.AddRange(_fault.Stack.AsSpan());
            _fault.Stack = Move(inner);
        }
        return ScriptRunStatus::Faulted;
    }
    return ScriptRunStatus::Returned;
}

ScriptRunStatus ScriptInterpreter::CallNested(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function,
                                              uint8 *window)
{
    if (function.IsNative())
    {
        return CallNative(coroutine, function, window);
    }
    if (function.Kind == ScriptFunctionKind::Async)
    {
        return StartAsync(coroutine, function, window) ? ScriptRunStatus::Returned : ScriptRunStatus::Faulted;
    }
    if (function.Kind == ScriptFunctionKind::Iterator)
    {
        return StartIterator(coroutine, function, window) ? ScriptRunStatus::Returned : ScriptRunStatus::Faulted;
    }
    if (function.Record == nullptr || function.Record->Code.IsEmpty())
    {
        Raise(coroutine, ScriptFault::Panic, "the method is abstract", function.Name.AsView());
        return ScriptRunStatus::Faulted;
    }
    const usize depth = coroutine.Frames.Count();
    if (!PushFrame(coroutine, function, window, window))
    {
        Raise(coroutine, ScriptFault::StackOverflow, "the script stack is full");
        return ScriptRunStatus::Faulted;
    }
    return Run(coroutine, depth);
}

ScriptResult<void> ScriptInterpreter::Invoke(const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments,
                                             Span<ScriptSlot> result)
{
    ScriptCoroutine &coroutine = BeginBatch();
    const bool ok = CallInBatch(coroutine, function, arguments, result);
    EndBatch(coroutine);
    return ok ? ScriptResult<void>() : ScriptResult<void>(_fault);
}

ScriptCoroutine &ScriptInterpreter::BeginBatch()
{
    BeginEntry();
    return NewCoroutine();
}

void ScriptInterpreter::EndBatch(ScriptCoroutine &coroutine)
{
    FreeCoroutine(coroutine.Index);
    EndEntry();
}

bool ScriptInterpreter::CallInBatch(ScriptCoroutine &coroutine, const ScriptFunctionInfo &function,
                                    Span<const ScriptSlot> arguments, Span<ScriptSlot> result)
{
    uint8 *window = coroutine.Segments[0].Begin;
    const usize windowBytes = usize{function.CallSlots == 0 ? 1 : function.CallSlots} * ScriptSlotBytes;
    if (windowBytes > static_cast<usize>(coroutine.Segments[0].End - window))
    {
        _fault = {};
        _fault.Kind = ScriptFault::StackOverflow;
        _fault.Message = String("the script stack is full");
        return false;
    }
    const usize count = arguments.Count() < function.ParameterSlots ? arguments.Count() : function.ParameterSlots;
    const usize argumentBytes = count * ScriptSlotBytes;
    if (count != 0)
    {
        MemCopy(window, arguments.Data(), argumentBytes);
    }
    MemZero(window + argumentBytes, windowBytes - argumentBytes);
    if (_entryDepth == 1)
    {
        ResetWatchdog();
    }
    ScriptCoroutine *saved = _running;
    ScriptRunStatus status = ScriptRunStatus::Returned;
    if (function.Code == nullptr || function.Kind != ScriptFunctionKind::Normal)
    {
        if (function.IsNative() || function.Kind != ScriptFunctionKind::Normal)
        {
            status = CallNested(coroutine, function, window);
        }
        else
        {
            Raise(coroutine, ScriptFault::Panic, "the method is abstract", function.Name.AsView());
            status = ScriptRunStatus::Faulted;
        }
    }
    else if (!PushFrame(coroutine, function, window, window))
    {
        Raise(coroutine, ScriptFault::StackOverflow, "the script stack is full");
        status = ScriptRunStatus::Faulted;
    }
    else
    {
        status = Run(coroutine, 0);
    }
    _running = saved;
    if (status == ScriptRunStatus::Returned)
    {
        const usize slots = result.Count() < function.ReturnSlots ? result.Count() : function.ReturnSlots;
        if (slots != 0)
        {
            MemCopy(result.Data(), window, slots * ScriptSlotBytes);
        }
        return true;
    }
    if (status != ScriptRunStatus::Faulted)
    {
        _fault = {};
        _fault.Kind = ScriptFault::Panic;
        _fault.Message = String("the task was canceled");
    }
    coroutine.Frames.Clear();
    coroutine.Segment = 0;
    return false;
}

ScriptRunStatus ScriptInterpreter::CallDelegate(ScriptCoroutine &coroutine, uint8 *window, uint32 slots,
                                                bool allowEmpty, uint32 resultSlots)
{
    auto *delegate = static_cast<ScriptDelegateObject *>(RdRef(window));
    if (delegate == nullptr)
    {
        if (allowEmpty)
        {
            MemZero(window, usize{resultSlots} * ScriptSlotBytes);
            return ScriptRunStatus::Returned;
        }
        Raise(coroutine, ScriptFault::NullReference, "the delegate is null");
        return ScriptRunStatus::Faulted;
    }
    const auto invocationAt = [](ScriptObject *object, uint32 index) {
        auto *combined = static_cast<ScriptDelegateObject *>(object);
        const ScriptArray *items = combined->Invocations;
        return static_cast<ScriptDelegateObject *>(
            Rd<ScriptObject *>(reinterpret_cast<const uint8 *>(items) + items->Type->DataOffset + usize{index} * 8));
    };
    // Each invocation of a combined delegate gets the same arguments, so they are kept aside: on the C++ stack when
    // they are few, which is nearly always, so a call allocates nothing.
    ScriptSlot inlineSaved[8];
    List<ScriptSlot> heapSaved;
    ScriptSlot *saved = inlineSaved;
    if (usize{slots} + 1 > sizeof(inlineSaved) / sizeof(inlineSaved[0]))
    {
        heapSaved.Resize(usize{slots} + 1);
        saved = heapSaved.Data();
    }
    MemCopy(saved, window, (usize{slots} + 1) * ScriptSlotBytes);
    const uint32 count = delegate->Invocations->Length;
    const ScriptFunctionInfo *first = invocationAt(delegate, 0)->Function;
    _roots.Add({saved->Bytes, first, true, first->ParameterSlots > slots ? 1u : 0u});
    ScriptRunStatus status = ScriptRunStatus::Returned;
    for (uint32 i = 0; i < count && status == ScriptRunStatus::Returned; ++i)
    {
        ScriptDelegateObject *invocation = invocationAt(RdRef(saved->Bytes), i);
        const ScriptFunctionInfo &function = *invocation->Function;
        if (function.ParameterSlots > slots)
        {
            Wr<ScriptObject *>(window, invocation->Target);
            MemCopy(window + ScriptSlotBytes, (saved + 1)->Bytes, usize{slots} * ScriptSlotBytes);
        }
        else
        {
            MemCopy(window, (saved + 1)->Bytes, usize{slots} * ScriptSlotBytes);
        }
        status = CallNested(coroutine, function, window);
    }
    _roots.Pop();
    return status;
}

ScriptRunStatus ScriptInterpreter::Run(ScriptCoroutine &coroutine, usize entryDepth)
{
    static const void *const labels[] = {
#define MARLIN_SCRIPT_OPCODE(name, operands, words) &&op_##name,
        MARLIN_SCRIPT_OPCODES(MARLIN_SCRIPT_OPCODE)
#undef MARLIN_SCRIPT_OPCODE
    };
    ScriptCoroutine *savedRunning = _running;
    _running = &coroutine;
    ScriptRunStatus status = ScriptRunStatus::Returned;
    ScriptFrame *frame = nullptr;
    const ScriptFunctionInfo *function = nullptr;
    const ScriptFunctionRecord *record = nullptr;
    const ScriptLoadedModule *module = nullptr;
    const uint32 *code = nullptr;
    uint8 *base = nullptr;
    uint32 pc = 0;
    uint32 start = 0;
    uint32 word = 0;
    uint32 prefix = 0;
    uint32 a = 0;
    uint32 b = 0;
    uint32 c = 0;
    uint32 bx = 0;
    int64 sbx = 0;
    // What the call instructions hand to their shared tail.
    const ScriptFunctionInfo *callee = nullptr;
    uint8 *window = nullptr;

#define R(index) (base + usize{index} * ScriptSlotBytes)
#define W(index) code[pc + (index)]
#define RESET_WIDE()                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        prefix = 0;                                                                                                    \
    } while (false)
#define ABC()                                                                                                          \
    a = ((word >> 8) & 0xFF) | (((prefix >> 8) & 0xFF) << 8);                                                         \
    b = ((word >> 16) & 0xFF) | (((prefix >> 16) & 0xFF) << 8);                                                       \
    c = (word >> 24) | ((prefix >> 24) << 8);                                                                          \
    (void)c
#define ABX()                                                                                                          \
    a = ((word >> 8) & 0xFF) | (((prefix >> 8) & 0xFF) << 8);                                                         \
    bx = (word >> 16) | ((prefix >> 16) << 16)
#define ASBX()                                                                                                         \
    a = ((word >> 8) & 0xFF) | (((prefix >> 8) & 0xFF) << 8);                                                         \
    sbx = prefix != 0 ? static_cast<int64>(static_cast<int32>((word >> 16) | ((prefix >> 16) << 16)))                \
                      : static_cast<int64>(static_cast<int16>(word >> 16));                                            \
    (void)sbx
#define SAX()                                                                                                          \
    sbx = prefix != 0 ? static_cast<int64>(static_cast<int32>((word >> 8) | (((prefix >> 16) & 0xFF) << 24)))        \
                      : static_cast<int64>(static_cast<int32>(word) >> 8)
// A Wide prefix is an instruction of its own, which sets the prefix and dispatches the next word without clearing it.
#define NEXT()                                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        start = pc;                                                                                                    \
        word = code[pc++];                                                                                             \
        if ((word & 0xFF) == 0)                                                                                        \
        {                                                                                                              \
            prefix = word;                                                                                             \
            word = code[pc++];                                                                                         \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            RESET_WIDE();                                                                                              \
        }                                                                                                              \
        goto *labels[word & 0xFF];                                                                                     \
    } while (false)
#define FAULT(kind, message)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        frame->Pc = start;                                                                                             \
        Raise(coroutine, kind, message);                                                                               \
        goto faulted;                                                                                                  \
    } while (false)
#define SAVE_PC() frame->Pc = start
#define RELOAD_FRAME()                                                                                                 \
    frame = &coroutine.Frames.Last();                                                                                  \
    function = frame->Function;                                                                                        \
    record = function->Record;                                                                                         \
    module = function->Module;                                                                                         \
    code = function->Code;                                                                                             \
    base = frame->Base
#define BINARY(T, expression)                                                                                          \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(b));                                                                                       \
        const T y = Rd<T>(R(c));                                                                                       \
        Wr<T>(R(a), static_cast<T>(expression));                                                                       \
        NEXT();                                                                                                        \
    }
#define COMPARE(T, expression)                                                                                         \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(b));                                                                                       \
        const T y = Rd<T>(R(c));                                                                                       \
        Wr<uint32>(R(a), (expression) ? 1u : 0u);                                                                      \
        NEXT();                                                                                                        \
    }
#define UNARY(TIn, TOut, expression)                                                                                   \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const TIn x = Rd<TIn>(R(b));                                                                                   \
        Wr<TOut>(R(a), static_cast<TOut>(expression));                                                                 \
        NEXT();                                                                                                        \
    }
#define CHECKED(T, builtin)                                                                                            \
    {                                                                                                                  \
        ABC();                                                                                                         \
        T result;                                                                                                      \
        if (builtin(Rd<T>(R(b)), Rd<T>(R(c)), &result))                                                                \
        {                                                                                                              \
            FAULT(ScriptFault::Overflow, "arithmetic overflow");                                                       \
        }                                                                                                              \
        Wr<T>(R(a), result);                                                                                           \
        NEXT();                                                                                                        \
    }
#define DIVIDE(T, operation, common)                                                                                   \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(b));                                                                                       \
        const T y = Rd<T>(R(c));                                                                                       \
        if (y == 0)                                                                                                    \
        {                                                                                                              \
            if (common)                                                                                                \
            {                                                                                                          \
                Wr<T>(R(a), 0);                                                                                        \
                NEXT();                                                                                                \
            }                                                                                                          \
            FAULT(ScriptFault::DivideByZero, "division by zero");                                                      \
        }                                                                                                              \
        if (NumericLimits<T>::Min() != 0 && x == NumericLimits<T>::Min() && y == static_cast<T>(-1))                   \
        {                                                                                                              \
            Wr<T>(R(a), static_cast<T>(operation == 0 ? x : 0));                                                       \
            NEXT();                                                                                                    \
        }                                                                                                              \
        Wr<T>(R(a), static_cast<T>(operation == 0 ? x / y : x % y));                                                   \
        NEXT();                                                                                                        \
    }
#define VECTOR(T, expression)                                                                                          \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(b));                                                                                       \
        const T y = Rd<T>(R(c));                                                                                       \
        Wr<T>(R(a), static_cast<T>(expression));                                                                       \
        NEXT();                                                                                                        \
    }
#define IMMEDIATE(T, k, expression)                                                                                    \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(b));                                                                                       \
        const T y = static_cast<T>(k);                                                                                 \
        pc += 1;                                                                                                       \
        Wr<T>(R(a), static_cast<T>(expression));                                                                       \
        NEXT();                                                                                                        \
    }
#define COMPARE_JUMP(T, expression)                                                                                    \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(a));                                                                                       \
        const T y = Rd<T>(R(b));                                                                                       \
        const uint32 offset = W(0);                                                                                    \
        pc += 1;                                                                                                       \
        if (expression)                                                                                                \
        {                                                                                                              \
            pc += offset;                                                                                              \
        }                                                                                                              \
        NEXT();                                                                                                        \
    }
#define COMPARE_JUMP_K(T, expression)                                                                                  \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const T x = Rd<T>(R(a));                                                                                       \
        const T y = static_cast<int32>(W(0));                                                                          \
        const uint32 offset = W(1);                                                                                    \
        pc += 2;                                                                                                       \
        if (expression)                                                                                                \
        {                                                                                                              \
            pc += offset;                                                                                              \
        }                                                                                                              \
        NEXT();                                                                                                        \
    }
#define LOAD_FIELD(T)                                                                                                  \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const uint32 offset = W(0);                                                                                    \
        pc += 1;                                                                                                       \
        ScriptObject *object = RdRef(R(b));                                                                            \
        if (object == nullptr)                                                                                         \
        {                                                                                                              \
            FAULT(ScriptFault::NullReference, "the object is null");                                                   \
        }                                                                                                              \
        Wr<T>(R(a), Rd<T>(object->GetData() + offset));                                                                \
        NEXT();                                                                                                        \
    }
#define STORE_FIELD(T)                                                                                                 \
    {                                                                                                                  \
        ABC();                                                                                                         \
        const uint32 offset = W(0);                                                                                    \
        pc += 1;                                                                                                       \
        ScriptObject *object = RdRef(R(a));                                                                            \
        if (object == nullptr)                                                                                         \
        {                                                                                                              \
            FAULT(ScriptFault::NullReference, "the object is null");                                                   \
        }                                                                                                              \
        Wr<T>(object->GetData() + offset, Rd<T>(R(b)));                                                                \
        NEXT();                                                                                                        \
    }
// The element *index* of the array in *array*, in `at`; faults on a null array or an index outside it.
#define ELEMENT_AT(array, index)                                                                                       \
    const uint32 element = W(0);                                                                                       \
    pc += 1;                                                                                                           \
    auto *elements = static_cast<ScriptArray *>(RdRef(array));                                                         \
    if (elements == nullptr)                                                                                           \
    {                                                                                                                  \
        FAULT(ScriptFault::NullReference, "the array is null");                                                        \
    }                                                                                                                  \
    const uint32 position = Rd<uint32>(index);                                                                         \
    if (position >= elements->Length)                                                                                  \
    {                                                                                                                  \
        FAULT(ScriptFault::IndexOutOfRange, "the index is outside the array");                                         \
    }                                                                                                                  \
    uint8 *at =                                                                                                        \
        reinterpret_cast<uint8 *>(elements) + ElementDataOffset(element) + usize{position} * (element & 0xFFFFFF)
#define LOAD_ELEMENT(T)                                                                                                \
    {                                                                                                                  \
        ABC();                                                                                                         \
        ELEMENT_AT(R(b), R(c));                                                                                        \
        Wr<T>(R(a), Rd<T>(at));                                                                                        \
        NEXT();                                                                                                        \
    }
#define STORE_ELEMENT(T)                                                                                               \
    {                                                                                                                  \
        ABC();                                                                                                         \
        ELEMENT_AT(R(a), R(b));                                                                                        \
        Wr<T>(at, Rd<T>(R(c)));                                                                                        \
        NEXT();                                                                                                        \
    }

    ScriptCollector &collector = *_domain->_collector;
    ScriptHeap &heap = *_domain->_heap;
    RELOAD_FRAME();
    pc = frame->Pc;
    frame->Entered = true;
    NEXT();

op_AddI32K:
    IMMEDIATE(uint32, W(0), x + y)
op_MulI32K:
    IMMEDIATE(uint32, W(0), x * y)
op_AddI64K:
    IMMEDIATE(uint64, static_cast<int64>(static_cast<int32>(W(0))), x + y)
op_Load32:
    {
        ABC();
        Wr<uint32>(R(a), Rd<uint32>(Rd<uint8 *>(R(b))));
        NEXT();
    }
op_Load64:
    {
        ABC();
        Wr<uint64>(R(a), Rd<uint64>(Rd<uint8 *>(R(b))));
        NEXT();
    }
op_Store32:
    {
        ABC();
        Wr<uint32>(Rd<uint8 *>(R(a)), Rd<uint32>(R(b)));
        NEXT();
    }
op_Store64:
    {
        ABC();
        Wr<uint64>(Rd<uint8 *>(R(a)), Rd<uint64>(R(b)));
        NEXT();
    }
op_StoreRef:
    {
        ABC();
        WriteScriptReference(collector, heap, Rd<ScriptObject **>(R(a)), RdRef(R(b)));
        NEXT();
    }
op_LoadField32:
    LOAD_FIELD(uint32)
op_LoadField64:
    LOAD_FIELD(uint64)
op_StoreField32:
    STORE_FIELD(uint32)
op_StoreField64:
    STORE_FIELD(uint64)
op_StoreFieldRef:
    {
        ABC();
        const uint32 offset = W(0);
        pc += 1;
        ScriptObject *object = RdRef(R(a));
        if (object == nullptr)
        {
            FAULT(ScriptFault::NullReference, "the object is null");
        }
        WriteScriptReference(collector, heap, reinterpret_cast<ScriptObject **>(object->GetData() + offset),
                             RdRef(R(b)));
        NEXT();
    }
op_LoadElem32:
    LOAD_ELEMENT(uint32)
op_LoadElem64:
    LOAD_ELEMENT(uint64)
op_StoreElem32:
    STORE_ELEMENT(uint32)
op_StoreElem64:
    STORE_ELEMENT(uint64)
op_StoreElemRef:
    {
        ABC();
        ELEMENT_AT(R(a), R(b));
        WriteScriptReference(collector, heap, reinterpret_cast<ScriptObject **>(at), RdRef(R(c)));
        NEXT();
    }
op_JumpEqI32:
    COMPARE_JUMP(uint32, x == y)
op_JumpNeI32:
    COMPARE_JUMP(uint32, x != y)
op_JumpLtI32:
    COMPARE_JUMP(int32, x < y)
op_JumpLeI32:
    COMPARE_JUMP(int32, x <= y)
op_JumpLtU32:
    COMPARE_JUMP(uint32, x < y)
op_JumpLeU32:
    COMPARE_JUMP(uint32, x <= y)
op_JumpEqI64:
    COMPARE_JUMP(uint64, x == y)
op_JumpNeI64:
    COMPARE_JUMP(uint64, x != y)
op_JumpLtI64:
    COMPARE_JUMP(int64, x < y)
op_JumpLeI64:
    COMPARE_JUMP(int64, x <= y)
op_JumpLtU64:
    COMPARE_JUMP(uint64, x < y)
op_JumpLeU64:
    COMPARE_JUMP(uint64, x <= y)
op_JumpEqF32:
    COMPARE_JUMP(float32, x == y)
op_JumpNeF32:
    COMPARE_JUMP(float32, x != y)
op_JumpLtF32:
    COMPARE_JUMP(float32, x < y)
op_JumpLeF32:
    COMPARE_JUMP(float32, x <= y)
op_JumpNotLtF32:
    COMPARE_JUMP(float32, !(x < y))
op_JumpNotLeF32:
    COMPARE_JUMP(float32, !(x <= y))
op_JumpEqF64:
    COMPARE_JUMP(float64, x == y)
op_JumpNeF64:
    COMPARE_JUMP(float64, x != y)
op_JumpLtF64:
    COMPARE_JUMP(float64, x < y)
op_JumpLeF64:
    COMPARE_JUMP(float64, x <= y)
op_JumpNotLtF64:
    COMPARE_JUMP(float64, !(x < y))
op_JumpNotLeF64:
    COMPARE_JUMP(float64, !(x <= y))
op_JumpEqI32K:
    COMPARE_JUMP_K(int32, x == y)
op_JumpNeI32K:
    COMPARE_JUMP_K(int32, x != y)
op_JumpLtI32K:
    COMPARE_JUMP_K(int32, x < y)
op_JumpLeI32K:
    COMPARE_JUMP_K(int32, x <= y)
op_JumpGtI32K:
    COMPARE_JUMP_K(int32, x > y)
op_JumpGeI32K:
    COMPARE_JUMP_K(int32, x >= y)
op_JumpEqI64K:
    COMPARE_JUMP_K(int64, x == y)
op_JumpNeI64K:
    COMPARE_JUMP_K(int64, x != y)

op_Wide:
    prefix = word;
    word = code[pc++];
    goto *labels[word & 0xFF];
op_Move:
{
    ABC();
    MemMove(R(a), R(b), ScriptSlotBytes);
    NEXT();
}
op_MoveN:
{
    ABC();
    MemMove(R(a), R(b), usize{c} * ScriptSlotBytes);
    NEXT();
}
op_LoadI:
{
    ASBX();
    Wr<int64>(R(a), sbx);
    NEXT();
}
op_LoadK:
{
    ABX();
    MemCopy(R(a), record->Constants[bx].Bytes, ScriptSlotBytes);
    NEXT();
}
op_LoadKN:
{
    ABX();
    const uint32 count = W(0);
    pc += 1;
    MemCopy(R(a), record->Constants[bx].Bytes, usize{count} * ScriptSlotBytes);
    NEXT();
}
op_Zero:
{
    ABC();
    MemZero(R(a), usize{b} * ScriptSlotBytes);
    NEXT();
}
op_LoadString:
{
    ABX();
    auto &strings = const_cast<ScriptLoadedModule *>(module)->Strings;
    if (strings[bx] == nullptr)
    {
        strings[bx] = _domain->NewPermanentString(module->GetString(bx));
        if (strings[bx] == nullptr)
        {
            FAULT(ScriptFault::Panic, "the script heap is exhausted");
        }
    }
    Wr<ScriptObject *>(R(a), strings[bx]);
    NEXT();
}
op_AddI32:
    BINARY(uint32, x + y)
op_SubI32:
    BINARY(uint32, x - y)
op_MulI32:
    BINARY(uint32, x * y)
op_AddI64:
    BINARY(uint64, x + y)
op_SubI64:
    BINARY(uint64, x - y)
op_MulI64:
    BINARY(uint64, x * y)
op_DivI32:
    DIVIDE(int32, 0, false)
op_DivU32:
    DIVIDE(uint32, 0, false)
op_RemI32:
    DIVIDE(int32, 1, false)
op_RemU32:
    DIVIDE(uint32, 1, false)
op_DivI64:
    DIVIDE(int64, 0, false)
op_DivU64:
    DIVIDE(uint64, 0, false)
op_RemI64:
    DIVIDE(int64, 1, false)
op_RemU64:
    DIVIDE(uint64, 1, false)
op_DivI32Common:
    DIVIDE(int32, 0, true)
op_DivU32Common:
    DIVIDE(uint32, 0, true)
op_RemI32Common:
    DIVIDE(int32, 1, true)
op_RemU32Common:
    DIVIDE(uint32, 1, true)
op_DivI64Common:
    DIVIDE(int64, 0, true)
op_DivU64Common:
    DIVIDE(uint64, 0, true)
op_RemI64Common:
    DIVIDE(int64, 1, true)
op_RemU64Common:
    DIVIDE(uint64, 1, true)
op_CheckedAddI32:
    CHECKED(int32, __builtin_add_overflow)
op_CheckedSubI32:
    CHECKED(int32, __builtin_sub_overflow)
op_CheckedMulI32:
    CHECKED(int32, __builtin_mul_overflow)
op_CheckedAddU32:
    CHECKED(uint32, __builtin_add_overflow)
op_CheckedSubU32:
    CHECKED(uint32, __builtin_sub_overflow)
op_CheckedMulU32:
    CHECKED(uint32, __builtin_mul_overflow)
op_CheckedAddI64:
    CHECKED(int64, __builtin_add_overflow)
op_CheckedSubI64:
    CHECKED(int64, __builtin_sub_overflow)
op_CheckedMulI64:
    CHECKED(int64, __builtin_mul_overflow)
op_CheckedAddU64:
    CHECKED(uint64, __builtin_add_overflow)
op_CheckedSubU64:
    CHECKED(uint64, __builtin_sub_overflow)
op_CheckedMulU64:
    CHECKED(uint64, __builtin_mul_overflow)
op_NegI32:
    UNARY(uint32, uint32, 0u - x)
op_NegI64:
    UNARY(uint64, uint64, uint64{0} - x)
op_CheckedNegI32:
{
    ABC();
    const int32 x = Rd<int32>(R(b));
    if (x == NumericLimits<int32>::Min())
    {
        FAULT(ScriptFault::Overflow, "arithmetic overflow");
    }
    Wr<int32>(R(a), -x);
    NEXT();
}
op_CheckedNegI64:
{
    ABC();
    const int64 x = Rd<int64>(R(b));
    if (x == NumericLimits<int64>::Min())
    {
        FAULT(ScriptFault::Overflow, "arithmetic overflow");
    }
    Wr<int64>(R(a), -x);
    NEXT();
}
op_And:
    VECTOR(I64x2, x & y)
op_Or:
    VECTOR(I64x2, x | y)
op_Xor:
    VECTOR(I64x2, x ^ y)
op_Not:
    UNARY(I64x2, I64x2, ~x)
op_NotBool:
    UNARY(uint32, uint32, x ^ 1u)
op_ShlI32:
    BINARY(uint32, x << (y & 31))
op_ShlI64:
    BINARY(uint64, x << (y & 63))
op_ShrI32:
    BINARY(int32, x >> (y & 31))
op_ShrU32:
    BINARY(uint32, x >> (y & 31))
op_ShrI64:
    BINARY(int64, x >> (y & 63))
op_ShrU64:
    BINARY(uint64, x >> (y & 63))
op_AddF32:
    BINARY(float32, x + y)
op_SubF32:
    BINARY(float32, x - y)
op_MulF32:
    BINARY(float32, x * y)
op_DivF32:
    BINARY(float32, x / y)
op_RemF32:
    BINARY(float32, __builtin_fmodf(x, y))
op_NegF32:
    UNARY(float32, float32, -x)
op_AddF64:
    BINARY(float64, x + y)
op_SubF64:
    BINARY(float64, x - y)
op_MulF64:
    BINARY(float64, x * y)
op_DivF64:
    BINARY(float64, x / y)
op_RemF64:
    BINARY(float64, __builtin_fmod(x, y))
op_NegF64:
    UNARY(float64, float64, -x)
op_EqI32:
    COMPARE(uint32, x == y)
op_NeI32:
    COMPARE(uint32, x != y)
op_EqI64:
    COMPARE(uint64, x == y)
op_NeI64:
    COMPARE(uint64, x != y)
op_LtI32:
    COMPARE(int32, x < y)
op_LeI32:
    COMPARE(int32, x <= y)
op_LtU32:
    COMPARE(uint32, x < y)
op_LeU32:
    COMPARE(uint32, x <= y)
op_LtI64:
    COMPARE(int64, x < y)
op_LeI64:
    COMPARE(int64, x <= y)
op_LtU64:
    COMPARE(uint64, x < y)
op_LeU64:
    COMPARE(uint64, x <= y)
op_EqF32:
    COMPARE(float32, x == y)
op_NeF32:
    COMPARE(float32, x != y)
op_LtF32:
    COMPARE(float32, x < y)
op_LeF32:
    COMPARE(float32, x <= y)
op_EqF64:
    COMPARE(float64, x == y)
op_NeF64:
    COMPARE(float64, x != y)
op_LtF64:
    COMPARE(float64, x < y)
op_LeF64:
    COMPARE(float64, x <= y)
op_SExt8:
    UNARY(uint32, int32, static_cast<int8>(x))
op_ZExt8:
    UNARY(uint32, uint32, x & 0xFFu)
op_SExt16:
    UNARY(uint32, int32, static_cast<int16>(x))
op_ZExt16:
    UNARY(uint32, uint32, x & 0xFFFFu)
op_SExt32:
    UNARY(int32, int64, x)
op_ZExt32:
    UNARY(uint32, uint64, x)
op_I32ToF32:
    UNARY(int32, float32, x)
op_U32ToF32:
    UNARY(uint32, float32, x)
op_I64ToF32:
    UNARY(int64, float32, x)
op_U64ToF32:
    UNARY(uint64, float32, x)
op_I32ToF64:
    UNARY(int32, float64, x)
op_U32ToF64:
    UNARY(uint32, float64, x)
op_I64ToF64:
    UNARY(int64, float64, x)
op_U64ToF64:
    UNARY(uint64, float64, x)
op_F32ToI32:
    UNARY(float32, int32, (Saturate<int32, float32>(x)))
op_F32ToU32:
    UNARY(float32, uint32, (Saturate<uint32, float32>(x)))
op_F32ToI64:
    UNARY(float32, int64, (Saturate<int64, float32>(x)))
op_F32ToU64:
    UNARY(float32, uint64, (Saturate<uint64, float32>(x)))
op_F64ToI32:
    UNARY(float64, int32, (Saturate<int32, float64>(x)))
op_F64ToU32:
    UNARY(float64, uint32, (Saturate<uint32, float64>(x)))
op_F64ToI64:
    UNARY(float64, int64, (Saturate<int64, float64>(x)))
op_F64ToU64:
    UNARY(float64, uint64, (Saturate<uint64, float64>(x)))
op_F32ToF64:
    UNARY(float32, float64, x)
op_F64ToF32:
    UNARY(float64, float32, x)
op_F16ToF32:
    UNARY(uint32, float32, Float16::FromBits(static_cast<uint16>(x)).ToFloat())
op_F32ToF16:
    UNARY(float32, uint32, Float16(x).Bits)
op_ConvChecked:
{
    ABC();
    if (!ConvertChecked(static_cast<ScriptNumber>(c >> 4), static_cast<ScriptNumber>(c & 0xF), R(b), R(a)))
    {
        FAULT(ScriptFault::Overflow, "the value does not fit the type it is converted to");
    }
    NEXT();
}
op_AddF32x4:
    VECTOR(F32x4, x + y)
op_SubF32x4:
    VECTOR(F32x4, x - y)
op_MulF32x4:
    VECTOR(F32x4, x *y)
op_DivF32x4:
    VECTOR(F32x4, x / y)
op_NegF32x4:
    UNARY(F32x4, F32x4, -x)
op_AddF64x2:
    VECTOR(F64x2, x + y)
op_SubF64x2:
    VECTOR(F64x2, x - y)
op_MulF64x2:
    VECTOR(F64x2, x *y)
op_DivF64x2:
    VECTOR(F64x2, x / y)
op_NegF64x2:
    UNARY(F64x2, F64x2, -x)
op_AddI32x4:
    VECTOR(I32x4, x + y)
op_SubI32x4:
    VECTOR(I32x4, x - y)
op_MulI32x4:
    VECTOR(I32x4, x *y)
op_NegI32x4:
    UNARY(I32x4, I32x4, I32x4(0) - x)
op_AddI64x2:
    VECTOR(I64x2, x + y)
op_SubI64x2:
    VECTOR(I64x2, x - y)
op_MulI64x2:
    VECTOR(I64x2, x *y)
op_NegI64x2:
    UNARY(I64x2, I64x2, I64x2(0) - x)
op_Splat32:
{
    ABC();
    const uint32 x = Rd<uint32>(R(b));
    const I32x4 splat = x;
    Wr<I32x4>(R(a), splat);
    NEXT();
}
op_Splat64:
{
    ABC();
    const uint64 x = Rd<uint64>(R(b));
    const I64x2 splat = x;
    Wr<I64x2>(R(a), splat);
    NEXT();
}
op_Extract:
{
    ABC();
    const uint32 offset = W(0);
    const uint32 access = W(1);
    pc += 2;
    ReadAccess(R(a), R(b) + offset, access);
    NEXT();
}
op_Insert:
{
    ABC();
    const uint32 offset = W(0);
    const uint32 access = W(1);
    pc += 2;
    MemMove(R(a) + offset, R(b), AccessBytes(access));
    NEXT();
}
op_ExtractDyn:
{
    ABC();
    const uint32 access = W(0);
    const int32 count = static_cast<int32>(W(1));
    pc += 2;
    int32 index = Rd<int32>(R(c));
    index = index < 0 ? 0 : index >= count ? count - 1 : index;
    ReadAccess(R(a), R(b) + usize(index) * AccessBytes(access), access);
    NEXT();
}
op_InsertDyn:
{
    ABC();
    const uint32 access = W(0);
    const int32 count = static_cast<int32>(W(1));
    pc += 2;
    int32 index = Rd<int32>(R(c));
    index = index < 0 ? 0 : index >= count ? count - 1 : index;
    MemMove(R(a) + usize(index) * AccessBytes(access), R(b), AccessBytes(access));
    NEXT();
}
op_RefLocal:
{
    ABC();
    Wr<uint8 *>(R(a), R(b));
    NEXT();
}
op_RefStatic:
{
    ABX();
    Wr<uint8 *>(R(a), module->Statics + bx);
    NEXT();
}
op_RefOffset:
{
    ABC();
    const uint32 offset = W(0);
    pc += 1;
    Wr<uint8 *>(R(a), Rd<uint8 *>(R(b)) + offset);
    NEXT();
}
op_RefIndex:
op_RefIndexClamp:
{
    ABC();
    const uint32 stride = W(0);
    const uint32 count = W(1);
    pc += 2;
    uint32 index = Rd<uint32>(R(c));
    if (index >= count)
    {
        if ((word & 0xFF) == static_cast<uint32>(ScriptOp::RefIndex))
        {
            FAULT(ScriptFault::IndexOutOfRange, "the index is outside the array");
        }
        index = count == 0 ? 0 : (static_cast<int32>(index) < 0 ? 0 : count - 1);
    }
    Wr<uint8 *>(R(a), Rd<uint8 *>(R(b)) + usize{index} * stride);
    NEXT();
}
op_Load:
{
    ABC();
    const uint32 access = W(0);
    pc += 1;
    ReadAccess(R(a), Rd<uint8 *>(R(b)), access);
    NEXT();
}
op_Store:
{
    ABC();
    const uint32 access = W(0);
    pc += 1;
    if (!WritePlainAccess(Rd<uint8 *>(R(a)), R(b), access))
    {
        _domain->WriteAccess(Rd<uint8 *>(R(a)), R(b), access, *module);
    }
    NEXT();
}
op_New:
{
    ABX();
    SAVE_PC();
    ScriptObject *object = AllocateAtSafepoint(*module->Types[bx], 0, false);
    if (object == nullptr)
    {
        FAULT(ScriptFault::Panic, "the script heap is exhausted");
    }
    Wr<ScriptObject *>(R(a), object);
    NEXT();
}
op_NewArray:
{
    ABC();
    const uint32 type = W(0);
    pc += 1;
    const int32 length = Rd<int32>(R(b));
    if (length < 0)
    {
        FAULT(ScriptFault::Overflow, "an array cannot have a negative length");
    }
    SAVE_PC();
    ScriptObject *array = AllocateAtSafepoint(*module->Types[type], static_cast<uint32>(length), true);
    if (array == nullptr)
    {
        FAULT(ScriptFault::Panic, "the script heap is exhausted");
    }
    Wr<ScriptObject *>(R(a), array);
    NEXT();
}
op_ArrayLength:
{
    ABC();
    const auto *array = static_cast<const ScriptArray *>(RdRef(R(b)));
    if (array == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the array is null");
    }
    Wr<uint32>(R(a), array->Length);
    NEXT();
}
op_RefField:
{
    ABC();
    const uint32 offset = W(0);
    pc += 1;
    ScriptObject *object = RdRef(R(b));
    if (object == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the object is null");
    }
    Wr<uint8 *>(R(a), object->GetData() + offset);
    NEXT();
}
op_LoadField:
{
    ABC();
    const uint32 offset = W(0);
    const uint32 access = W(1);
    pc += 2;
    ScriptObject *object = RdRef(R(b));
    if (object == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the object is null");
    }
    ReadAccess(R(a), object->GetData() + offset, access);
    NEXT();
}
op_StoreField:
{
    ABC();
    const uint32 offset = W(0);
    const uint32 access = W(1);
    pc += 2;
    ScriptObject *object = RdRef(R(a));
    if (object == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the object is null");
    }
    if (!WritePlainAccess(object->GetData() + offset, R(b), access))
    {
        _domain->WriteAccess(object->GetData() + offset, R(b), access, *module);
    }
    NEXT();
}
op_RefElem:
op_LoadElem:
op_StoreElem:
{
    ABC();
    const ScriptOp op = static_cast<ScriptOp>(word & 0xFF);
    const uint32 element = W(0);
    const uint32 access = op == ScriptOp::RefElem ? 0 : W(1);
    pc += op == ScriptOp::RefElem ? 1 : 2;
    auto *array = static_cast<ScriptArray *>(RdRef(op == ScriptOp::StoreElem ? R(a) : R(b)));
    if (array == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the array is null");
    }
    const uint32 index = Rd<uint32>(op == ScriptOp::StoreElem ? R(b) : R(c));
    if (index >= array->Length)
    {
        FAULT(ScriptFault::IndexOutOfRange, "the index is outside the array");
    }
    uint8 *at = reinterpret_cast<uint8 *>(array) + ElementDataOffset(element) + usize{index} * (element & 0xFFFFFF);
    if (op == ScriptOp::RefElem)
    {
        Wr<uint8 *>(R(a), at);
    }
    else if (op == ScriptOp::LoadElem)
    {
        ReadAccess(R(a), at, access);
    }
    else
    {
        if (!WritePlainAccess(at, R(c), access))
        {
            _domain->WriteAccess(at, R(c), access, *module);
        }
    }
    NEXT();
}
op_Box:
{
    ABC();
    const uint32 typeIndex = W(0);
    pc += 1;
    SAVE_PC();
    const ScriptType &type = *module->Types[typeIndex];
    ScriptObject *box = AllocateAtSafepoint(type, 0, false);
    if (box == nullptr)
    {
        FAULT(ScriptFault::Panic, "the script heap is exhausted");
    }
    const bool typed = !type.References.IsEmpty() && GetScriptAccess(type.ValueAccess) == ScriptAccess::Bytes;
    _domain->WriteAccess(box->GetData(), R(b),
                         typed ? MakeScriptAccess(ScriptAccess::Typed, type.Layout) : type.ValueAccess,
                         type.Module != nullptr ? *type.Module : *module);
    Wr<ScriptObject *>(R(a), box);
    NEXT();
}
op_Unbox:
{
    ABC();
    const uint32 typeIndex = W(0);
    pc += 1;
    ScriptObject *box = RdRef(R(b));
    if (box == nullptr)
    {
        FAULT(ScriptFault::NullReference, "unboxing null");
    }
    if (box->Type != module->Types[typeIndex])
    {
        FAULT(ScriptFault::InvalidCast, "the object is not a boxed value of that type");
    }
    ReadAccess(R(a), box->GetData(), box->Type->ValueAccess);
    NEXT();
}
op_IsInstance:
{
    ABC();
    const uint32 typeIndex = W(0);
    pc += 1;
    ScriptObject *object = RdRef(R(b));
    Wr<uint32>(R(a), object != nullptr && ScriptDomain::IsAssignable(*object->Type, *module->Types[typeIndex]));
    NEXT();
}
op_Cast:
op_TryCast:
{
    ABC();
    const uint32 typeIndex = W(0);
    pc += 1;
    ScriptObject *object = RdRef(R(b));
    if (object != nullptr && !ScriptDomain::IsAssignable(*object->Type, *module->Types[typeIndex]))
    {
        if ((word & 0xFF) == static_cast<uint32>(ScriptOp::Cast))
        {
            FAULT(ScriptFault::InvalidCast, "the object is not of that type");
        }
        object = nullptr;
    }
    Wr<ScriptObject *>(R(a), object);
    NEXT();
}
op_TypeOf:
{
    ABX();
    SAVE_PC();
    _domain->CollectIfRequested();
    Wr<ScriptObject *>(R(a), _domain->GetTypeInfo(*module->Types[bx]));
    NEXT();
}
op_NullCheck:
{
    ABC();
    ScriptObject *object = RdRef(R(b));
    if (object == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the value is null");
    }
    Wr<ScriptObject *>(R(a), object);
    NEXT();
}
op_MakeDelegate:
{
    ABC();
    const uint32 typeIndex = W(0);
    const uint32 target = W(1);
    pc += 2;
    SAVE_PC();
    auto *delegate = static_cast<ScriptDelegateObject *>(AllocateAtSafepoint(*module->Types[typeIndex], 0, false));
    if (delegate == nullptr)
    {
        FAULT(ScriptFault::Panic, "the script heap is exhausted");
    }
    ScriptObject *receiver = c == 0 ? nullptr : RdRef(R(b));
    const ScriptFunctionInfo *callee = nullptr;
    switch (c)
    {
    case 0:
    case 1:
        callee = module->Functions[target];
        break;
    case 2:
        if (receiver == nullptr)
        {
            FAULT(ScriptFault::NullReference, "the delegate's object is null");
        }
        callee = target < receiver->Type->VirtualSlots.Count() ? receiver->Type->VirtualSlots[target] : nullptr;
        break;
    default: {
        if (receiver == nullptr)
        {
            FAULT(ScriptFault::NullReference, "the delegate's object is null");
        }
        const ScriptInterfaceSite &site = record->InterfaceSites[target];
        const ScriptType *interface = module->Types[site.Interface];
        for (const ScriptType *walk = receiver->Type; walk != nullptr && callee == nullptr; walk = walk->Base)
        {
            for (const ScriptInterfaceMethods &methods : walk->Interfaces)
            {
                if (methods.Interface == interface && site.Method < methods.Methods.Count())
                {
                    callee = methods.Methods[site.Method];
                    break;
                }
            }
        }
        if (callee == nullptr)
        {
            FAULT(ScriptFault::InvalidCast, "the object does not implement the interface");
        }
        break;
    }
    }
    if (c == 1 && receiver == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the delegate's object is null");
    }
    if (callee == nullptr)
    {
        FAULT(ScriptFault::Panic, "the method is abstract");
    }
    delegate->Function = callee;
    _domain->WriteReference(&delegate->Target, receiver);
    Wr<ScriptObject *>(R(a), delegate);
    NEXT();
}
op_CombineDelegates:
op_RemoveDelegates:
{
    ABC();
    SAVE_PC();
    _domain->CollectIfRequested();
    ScriptObject *combined = _domain->CombineDelegates(RdRef(R(b)), RdRef(R(c)),
                                                       (word & 0xFF) == static_cast<uint32>(ScriptOp::RemoveDelegates));
    Wr<ScriptObject *>(R(a), combined);
    NEXT();
}
op_Jump:
{
    SAX();
    pc = static_cast<uint32>(static_cast<int64>(pc) + sbx);
    NEXT();
}
op_JumpIf:
{
    ASBX();
    if (Rd<uint32>(R(a)) != 0)
    {
        pc = static_cast<uint32>(static_cast<int64>(pc) + sbx);
    }
    NEXT();
}
op_JumpIfNot:
{
    ASBX();
    if (Rd<uint32>(R(a)) == 0)
    {
        pc = static_cast<uint32>(static_cast<int64>(pc) + sbx);
    }
    NEXT();
}
op_Switch:
{
    ABX();
    const ScriptSwitchTable &table = record->Switches[bx];
    const int64 value = table.Is64 ? Rd<int64>(R(a)) : static_cast<int64>(Rd<int32>(R(a)));
    // The cases are in value order (the loader checks): the first not below the value is the one, if any is.
    usize low = 0;
    usize high = table.Cases.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        if (table.Cases[middle].Value < value)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    pc = low < table.Cases.Count() && table.Cases[low].Value == value ? table.Cases[low].Target : table.Default;
    NEXT();
}
op_Loop:
{
    // Both checks are inline: the collection flag, then the watchdog's budget, which reads the clock only when it
    // runs out.
    if (_domain->_collectionRequested)
    {
        SAVE_PC();
        _domain->Collect(_domain->_fullCollectionRequested);
    }
    if (!CheckWatchdog())
    {
        FAULT(ScriptFault::Timeout, "the VM entry ran past the watchdog's limit");
    }
    SAX();
    pc = static_cast<uint32>(static_cast<int64>(pc) + sbx);
    NEXT();
}
op_Return:
{
    ABC();
    if (b == 1)
    {
        MemMove(frame->ResultTo, R(a), ScriptSlotBytes);
    }
    else
    {
        MemMove(frame->ResultTo, R(a), usize{b} * ScriptSlotBytes);
    }
    coroutine.Frames.Pop();
    if (coroutine.Frames.Count() <= entryDepth)
    {
        coroutine.Segment = coroutine.Frames.IsEmpty() ? 0 : coroutine.Frames.Last().Segment;
        status = ScriptRunStatus::Returned;
        goto done;
    }
    RELOAD_FRAME();
    coroutine.Segment = frame->Segment;
    pc = frame->ResumePc;
    NEXT();
}
op_Fault:
{
    ABX();
    const ScriptFault kind = static_cast<ScriptFault>(a);
    FAULT(kind, bx != 0 ? module->GetString(bx - 1) : DefaultFaultMessage(kind));
}
// The arguments sit in the caller's window, which its map leaves out: the collector runs only once the callee's frame
// (or the native's root window) covers them.
op_Call:
{
    ABX();
    callee = module->Functions[bx];
    window = R(a);
    goto call;
}
op_CallVirtual:
{
    ABX();
    window = R(a);
    ScriptObject *receiver = RdRef(window);
    if (receiver == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the object is null");
    }
    callee = bx < receiver->Type->VirtualSlots.Count() ? receiver->Type->VirtualSlots[bx] : nullptr;
    goto call;
}
op_CallInterface:
{
    ABX();
    window = R(a);
    ScriptObject *receiver = RdRef(window);
    if (receiver == nullptr)
    {
        FAULT(ScriptFault::NullReference, "the object is null");
    }
    ScriptInterfaceCache &cache = const_cast<ScriptFunctionInfo *>(function)->Sites[bx];
    if (cache.Type == receiver->Type)
    {
        callee = cache.Target;
        goto call;
    }
    callee = nullptr;
    const ScriptInterfaceSite &site = record->InterfaceSites[bx];
    const ScriptType *interface = module->Types[site.Interface];
    for (const ScriptType *walk = receiver->Type; walk != nullptr && callee == nullptr; walk = walk->Base)
    {
        for (const ScriptInterfaceMethods &methods : walk->Interfaces)
        {
            if (methods.Interface == interface && site.Method < methods.Methods.Count())
            {
                callee = methods.Methods[site.Method];
                break;
            }
        }
    }
    if (callee == nullptr)
    {
        FAULT(ScriptFault::InvalidCast, "the object does not implement the interface");
    }
    cache.Type = receiver->Type;
    cache.Target = callee;
    ++cache.Misses;
    goto call;
}
op_CallDelegate:
{
    ABC();
    SAVE_PC();
    window = R(a);
    auto *delegate = static_cast<ScriptDelegateObject *>(RdRef(window));
    if (delegate == nullptr || delegate->Invocations != nullptr)
    {
        const ScriptFunctionInfo *first =
            delegate != nullptr
                ? static_cast<ScriptDelegateObject *>(
                      Rd<ScriptObject *>(reinterpret_cast<const uint8 *>(delegate->Invocations) +
                                         delegate->Invocations->Type->DataOffset))
                      ->Function
                : nullptr;
        const ScriptRunStatus nested =
            CallDelegate(coroutine, window, b, c != 0, first != nullptr ? first->ReturnSlots : 0);
        if (nested == ScriptRunStatus::Faulted)
        {
            goto faulted;
        }
        RELOAD_FRAME();
        NEXT();
    }
    callee = delegate->Function;
    uint8 *calleeWindow = window;
    if (callee->ParameterSlots > b)
    {
        Wr<ScriptObject *>(window, delegate->Target);
    }
    else
    {
        calleeWindow = window + ScriptSlotBytes;
    }
    if (callee->Code == nullptr || callee->Kind != ScriptFunctionKind::Normal)
    {
        if (!callee->IsNative() && callee->Kind == ScriptFunctionKind::Normal)
        {
            FAULT(ScriptFault::Panic, "the method is abstract");
        }
        const ScriptRunStatus nested = CallNested(coroutine, *callee, calleeWindow);
        if (nested == ScriptRunStatus::Faulted)
        {
            goto faulted;
        }
        RELOAD_FRAME();
        if (calleeWindow != window)
        {
            MemMove(window, calleeWindow, usize{callee->ReturnSlots} * ScriptSlotBytes);
        }
        NEXT();
    }
    frame->ResumePc = pc;
    if (!PushFrame(coroutine, *callee, calleeWindow, window))
    {
        FAULT(ScriptFault::StackOverflow, "the script stack is full");
    }
    _domain->CollectIfRequested();
    RELOAD_FRAME();
    frame->Entered = true;
    pc = 0;
    NEXT();
}
call:
{
    SAVE_PC();
    if (callee == nullptr)
    {
        FAULT(ScriptFault::Panic, "the method is abstract");
    }
    if (callee->Empty)
    {
        NEXT();
    }
    if (!CheckWatchdog())
    {
        FAULT(ScriptFault::Timeout, "the VM entry ran past the watchdog's limit");
    }
    if (callee->Code == nullptr || callee->Kind != ScriptFunctionKind::Normal)
    {
        if (!callee->IsNative() && callee->Kind == ScriptFunctionKind::Normal)
        {
            FAULT(ScriptFault::Panic, "the method is abstract");
        }
        const ScriptRunStatus nested =
            callee->IsNative() ? CallNative(coroutine, *callee, window) : CallNested(coroutine, *callee, window);
        if (nested == ScriptRunStatus::Faulted)
        {
            goto faulted;
        }
        RELOAD_FRAME();
        NEXT();
    }
    frame->ResumePc = pc;
    if (!PushFrame(coroutine, *callee, window, window))
    {
        FAULT(ScriptFault::StackOverflow, "the script stack is full");
    }
    _domain->CollectIfRequested();
    frame = &coroutine.Frames.Last();
    function = callee;
    record = callee->Record;
    module = callee->Module;
    code = callee->Code;
    base = frame->Base;
    frame->Entered = true;
    pc = 0;
    NEXT();
}
op_Await:
{
    ABC();
    SAVE_PC();
    _domain->CollectIfRequested();
    ScriptObject *task = RdRef(R(b));
    if (task == nullptr)
    {
        FAULT(ScriptFault::NullReference, "awaiting a null task");
    }
    ScriptTaskState &state = StateOf<ScriptTaskState>(task);
    if (state.State == ScriptTaskStates::Completed)
    {
        if (c != 0)
        {
            if (state.Result != nullptr)
            {
                ReadAccess(R(a), state.Result->GetData(), state.Result->Type->ValueAccess);
            }
            else
            {
                MemZero(R(a), usize{c} * ScriptSlotBytes);
            }
        }
        NEXT();
    }
    if (state.State == ScriptTaskStates::Faulted)
    {
        const ScriptFaultInfo *inner = _taskFaults.Find(state.Id);
        const ScriptFault kind = inner != nullptr ? inner->Kind : ScriptFault::Panic;
        Raise(coroutine, kind, "the awaited task faulted");
        if (inner != nullptr)
        {
            _fault.Message = inner->Message;
            List<ScriptStackEntry> stack = inner->Stack;
            stack.AddRange(_fault.Stack.AsSpan());
            _fault.Stack = Move(stack);
        }
        goto faulted;
    }
    ScriptObject *current = coroutine.Owner;
    if (current == nullptr || current->Type->Builtin != ScriptBuiltin::Task)
    {
        FAULT(ScriptFault::Panic, "await outside a task");
    }
    if (state.State == ScriptTaskStates::Canceled ||
        StateOf<ScriptTaskState>(current).State == ScriptTaskStates::Canceled)
    {
        coroutine.Frames.Resize(entryDepth);
        status = ScriptRunStatus::Canceled;
        goto done;
    }
    AddWaiter(task, current);
    StateOf<ScriptTaskState>(current).Wait = ScriptTaskWaits::Task;
    status = ScriptRunStatus::Suspended;
    goto done;
}
op_Yield:
{
    ABC();
    SAVE_PC();
    _domain->CollectIfRequested();
    ScriptObject *iterator = coroutine.Owner;
    if (iterator == nullptr || iterator->Type->Builtin != ScriptBuiltin::Iterator)
    {
        FAULT(ScriptFault::Panic, "yield outside an iterator");
    }
    ScriptIteratorState &state = StateOf<ScriptIteratorState>(iterator);
    if (b == 0)
    {
        coroutine.Frames.Resize(entryDepth);
        status = ScriptRunStatus::Returned;
        goto done;
    }
    const uint32 slots = b < state.CurrentSlots ? b : state.CurrentSlots;
    MemCopy(coroutine.Reserved, R(a), usize{slots} * ScriptSlotBytes);
    state.Current = 1;
    status = ScriptRunStatus::Suspended;
    goto done;
}
op_Math:
{
    ABC();
    const uint32 math = W(0);
    pc += 1;
    if (!ExecuteScriptMath(math, R(a), R(b), R(c), R(math >> 16)))
    {
        FAULT(ScriptFault::Native, "the math operation does not apply to these operands");
    }
    NEXT();
}
op_MinF32:
    BINARY(float32, MinOfF32(x, y))
op_MaxF32:
    BINARY(float32, MaxOfF32(x, y))
op_AbsF32:
    UNARY(float32, float32, x < 0 ? -x : (x == 0 ? 0.0f : x))
op_SqrtF32:
    UNARY(float32, float32, __builtin_sqrtf(x))
op_FloorF32:
    UNARY(float32, float32, __builtin_floorf(x))
op_DotF32x3:
    {
        ABC();
        Wr<float32>(R(a), DotF32(R(b), R(c), 3));
        NEXT();
    }
op_DotF32x4:
    {
        ABC();
        Wr<float32>(R(a), DotF32(R(b), R(c), 4));
        NEXT();
    }
op_LengthF32x3:
    {
        ABC();
        Wr<float32>(R(a), __builtin_sqrtf(DotF32(R(b), R(b), 3)));
        NEXT();
    }
op_CrossF32x3:
    {
        ABC();
        CrossF32x3(R(a), R(b), R(c));
        NEXT();
    }
op_NormalizeF32x3:
    {
        ABC();
        NormalizeF32x3(R(a), R(b));
        NEXT();
    }
op_AndI32K:
    IMMEDIATE(uint32, W(0), x & y)
op_ShlI32K:
    IMMEDIATE(uint32, W(0), x << (y & 31))
op_ShrI32K:
    IMMEDIATE(int32, W(0), x >> (y & 31))
op_ShrU32K:
    IMMEDIATE(uint32, W(0), x >> (y & 31))
op_AddF32K:
    IMMEDIATE(float32, BitCast<float32>(W(0)), x + y)
op_MulF32K:
    IMMEDIATE(float32, BitCast<float32>(W(0)), x * y)

faulted:
    coroutine.Frames.Resize(entryDepth < coroutine.Frames.Count() ? entryDepth : coroutine.Frames.Count());
    coroutine.Segment = coroutine.Frames.IsEmpty() ? 0 : coroutine.Frames.Last().Segment;
    status = ScriptRunStatus::Faulted;
done:
    _running = savedRunning;
    return status;

#undef R
#undef W
#undef RESET_WIDE
#undef ABC
#undef ABX
#undef ASBX
#undef SAX
#undef NEXT
#undef FAULT
#undef SAVE_PC
#undef RELOAD_FRAME
#undef BINARY
#undef COMPARE
#undef UNARY
#undef CHECKED
#undef DIVIDE
#undef VECTOR
#undef IMMEDIATE
#undef COMPARE_JUMP
#undef COMPARE_JUMP_K
#undef LOAD_FIELD
#undef STORE_FIELD
#undef ELEMENT_AT
#undef LOAD_ELEMENT
#undef STORE_ELEMENT
}
} // namespace Marlin
