#include "ScriptVM/ScriptDomain.h"

#include "HAL/PlatformTime.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "ScriptCollector.h"
#include "ScriptHeap.h"
#include "ScriptInterpreter.h"
#include "ScriptVM/ScriptWorld.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

MARLIN_DEFINE_LOG_CATEGORY(LogScript);

namespace Marlin
{
namespace
{
uint32 AlignUp(uint32 value, uint32 alignment)
{
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

/// How a value of *shape* is read from memory into registers, as a ScriptAccess word; references keep their own
/// access so arrays of them are known.
uint32 ReadAccessOfShape(const ScriptModuleData &module, uint32 shape)
{
    const ScriptShape &value = module.Shapes[shape];
    switch (value.Kind)
    {
    case ScriptShapeKind::Bool:
        return MakeScriptAccess(ScriptAccess::B32);
    case ScriptShapeKind::Int:
        switch (value.Width)
        {
        case 8:
            return MakeScriptAccess(value.Signed ? ScriptAccess::I8 : ScriptAccess::U8);
        case 16:
            return MakeScriptAccess(value.Signed ? ScriptAccess::I16 : ScriptAccess::U16);
        case 32:
            return MakeScriptAccess(ScriptAccess::B32);
        default:
            return MakeScriptAccess(ScriptAccess::B64);
        }
    case ScriptShapeKind::Float:
        return MakeScriptAccess(value.Width == 16   ? ScriptAccess::U16
                                : value.Width == 32 ? ScriptAccess::B32
                                                    : ScriptAccess::B64);
    case ScriptShapeKind::Reference:
        return MakeScriptAccess(ScriptAccess::Reference);
    case ScriptShapeKind::Pointer:
        return MakeScriptAccess(ScriptAccess::B64);
    default:
        return MakeScriptAccess(ScriptAccess::Bytes, value.Size);
    }
}

// RECOVERY-DECISION S7: the VM knows its builtin classes by their qualified names in the core library.
ScriptBuiltin BuiltinOfName(StringView name)
{
    if (name == "Marlin.object")
    {
        return ScriptBuiltin::Object;
    }
    if (name == "Marlin.string")
    {
        return ScriptBuiltin::String;
    }
    if (name == "Marlin.StringBuilder")
    {
        return ScriptBuiltin::StringBuilder;
    }
    if (name == "Marlin.TypeInfo")
    {
        return ScriptBuiltin::TypeInfo;
    }
    if (name == "Marlin.Task" || name.StartsWith("Marlin.Task<"))
    {
        return ScriptBuiltin::Task;
    }
    if (name.StartsWith("Marlin.WeakRef<"))
    {
        return ScriptBuiltin::WeakRef;
    }
    return ScriptBuiltin::None;
}

/// Checks that every instruction of *function* decodes and names what exists (MarlinScript §6.1); the error, or empty.
String VerifyCode(const ScriptModuleData &module, const ScriptFunctionRecord &function)
{
    if (function.FrameSlots < function.ParameterSlots)
    {
        return String("the function's frame is smaller than its parameters");
    }
    const Span<const uint32> code = function.Code.AsSpan();
    List<bool> starts;
    starts.Resize(code.Count() + 1, false);
    ScriptDecodedInstruction instruction;
    for (uint32 pc = 0; pc < code.Count(); pc += instruction.Words)
    {
        if (!DecodeScriptInstruction(code, pc, instruction))
        {
            return Format("the instruction at {0} does not decode", pc);
        }
        starts[pc] = true;
    }
    const auto isStart = [&](int64 target) {
        return target >= 0 && target < static_cast<int64>(code.Count()) && starts[static_cast<usize>(target)];
    };
    for (uint32 pc = 0; pc < code.Count(); pc += instruction.Words)
    {
        (void)DecodeScriptInstruction(code, pc, instruction);
        const uint32 types = static_cast<uint32>(module.Types.Count());
        int64 target = 0;
        if (GetScriptJumpTarget(instruction, pc, target) && !isStart(target))
        {
            return Format("the jump at {0} leaves the function", pc);
        }
        switch (instruction.Op)
        {
        case ScriptOp::Switch:
            if (instruction.Bx >= function.Switches.Count())
            {
                return Format("the switch at {0} names no table", pc);
            }
            if (!isStart(function.Switches[instruction.Bx].Default))
            {
                return String("a switch target is not an instruction");
            }
            for (usize i = 0; i < function.Switches[instruction.Bx].Cases.Count(); ++i)
            {
                const Span<const ScriptSwitchCase> cases = function.Switches[instruction.Bx].Cases.AsSpan();
                if (!isStart(cases[i].Target))
                {
                    return String("a switch target is not an instruction");
                }
                // Switch searches the cases, so they must be in value order.
                if (i > 0 && cases[i].Value < cases[i - 1].Value)
                {
                    return Format("the cases of the switch at {0} are not in order", pc);
                }
            }
            break;
        case ScriptOp::Call:
            if (instruction.Bx >= module.Functions.Count())
            {
                return Format("the call at {0} names no function", pc);
            }
            break;
        case ScriptOp::CallInterface:
            if (instruction.Bx >= function.InterfaceSites.Count() ||
                function.InterfaceSites[instruction.Bx].Interface >= types)
            {
                return Format("the call at {0} names no call site", pc);
            }
            break;
        case ScriptOp::New:
        case ScriptOp::TypeOf:
            if (instruction.Bx >= types)
            {
                return Format("the instruction at {0} names no type", pc);
            }
            break;
        case ScriptOp::NewArray:
        case ScriptOp::Box:
        case ScriptOp::Unbox:
        case ScriptOp::IsInstance:
        case ScriptOp::Cast:
        case ScriptOp::TryCast:
            if (instruction.Trailing[0] >= types)
            {
                return Format("the instruction at {0} names no type", pc);
            }
            break;
        case ScriptOp::MakeDelegate: {
            const bool named = instruction.C <= 1;
            const bool site = instruction.C == 3;
            if (instruction.Trailing[0] >= types ||
                (named && instruction.Trailing[1] >= module.Functions.Count()) ||
                (site && instruction.Trailing[1] >= function.InterfaceSites.Count()) || instruction.C > 3)
            {
                return Format("the instruction at {0} names no type or function", pc);
            }
            break;
        }
        case ScriptOp::LoadK:
            if (instruction.Bx >= function.Constants.Count())
            {
                return Format("the constant at {0} is out of range", pc);
            }
            break;
        case ScriptOp::LoadKN:
            if (static_cast<uint64>(instruction.Bx) + instruction.Trailing[0] > function.Constants.Count())
            {
                return Format("the constants at {0} are out of range", pc);
            }
            break;
        case ScriptOp::LoadString:
            if (instruction.Bx >= module.Strings.Count())
            {
                return Format("the string at {0} is out of range", pc);
            }
            break;
        case ScriptOp::Fault:
            if (instruction.Bx > module.Strings.Count())
            {
                return Format("the string at {0} is out of range", pc);
            }
            break;
        default:
            break;
        }
    }
    return {};
}

/// Whether *function* only moves registers and calls functions already found empty, then returns nothing
/// (ScriptFunctionInfo::Empty).
bool IsEmptyFunction(const ScriptLoadedModule &module, const ScriptFunctionInfo &function)
{
    if (function.Code == nullptr || function.Kind != ScriptFunctionKind::Normal || function.ReturnSlots != 0)
    {
        return false;
    }
    const Span<const uint32> code = function.Record->Code.AsSpan();
    ScriptDecodedInstruction instruction;
    for (uint32 pc = 0; pc < code.Count(); pc += instruction.Words)
    {
        if (!DecodeScriptInstruction(code, pc, instruction))
        {
            return false;
        }
        switch (instruction.Op)
        {
        case ScriptOp::Move:
        case ScriptOp::MoveN:
        case ScriptOp::LoadI:
        case ScriptOp::Zero:
            break;
        case ScriptOp::Call:
            if (instruction.Bx >= module.Functions.Count() || !module.Functions[instruction.Bx]->Empty)
            {
                return false;
            }
            break;
        case ScriptOp::Return:
            return instruction.B == 0;
        default:
            return false;
        }
    }
    return false;
}

/// Adds the byte offsets of the managed pointers in a value of *shape* at *offset*.
void CollectManagedPointers(const ScriptModuleData &module, uint32 shape, uint32 offset, List<uint32> &pointers)
{
    const ScriptShape &value = module.Shapes[shape];
    switch (value.Kind)
    {
    case ScriptShapeKind::Pointer:
        if (value.Space != ScriptPointerSpace::Static)
        {
            pointers.Add(offset);
        }
        break;
    case ScriptShapeKind::Array:
        for (uint32 i = 0; i < value.Count; ++i)
        {
            CollectManagedPointers(module, value.Element, offset + i * value.Stride, pointers);
        }
        break;
    case ScriptShapeKind::Struct:
        for (const ScriptShapeField &field : value.Fields)
        {
            CollectManagedPointers(module, field.Shape, offset + field.Offset, pointers);
        }
        break;
    default:
        break;
    }
}

/// The ScriptValueKinds of a value of *shape*.
uint32 ValueKindOfShape(const ScriptLoadedModule &module, uint32 shape)
{
    const ScriptShape &value = module.GetShape(shape);
    switch (value.Kind)
    {
    case ScriptShapeKind::Void:
        return ScriptValueKinds::Void;
    case ScriptShapeKind::Bool:
        return ScriptValueKinds::Bool;
    case ScriptShapeKind::Int:
        return ScriptValueKinds::Number(value.Width, false, value.Signed);
    case ScriptShapeKind::Float:
        return ScriptValueKinds::Number(value.Width, true, true);
    case ScriptShapeKind::Reference:
        if (value.Element != ~0u && value.Element < module.Data->Types.Count() &&
            module.Data->Types[value.Element].Kind == ScriptTypeKind::String)
        {
            return ScriptValueKinds::String;
        }
        return ScriptValueKinds::Reference;
    default:
        return ~0u;
    }
}

/// Whether a C++ kind fits a script one: floats match whatever C++ says of their sign, and a string is also an
/// object reference.
bool KindsMatch(uint32 wanted, uint32 actual)
{
    if (wanted == actual)
    {
        return true;
    }
    if ((wanted & 0x10) != 0 && (actual & 0x10) != 0)
    {
        return (wanted & ~0x20u) == (actual & ~0x20u);
    }
    return wanted == ScriptValueKinds::Reference && actual == ScriptValueKinds::String;
}

// RECOVERY-DECISION S17: natives called on an object fault with NullReference when the object is null, as script
// calls do, instead of each native checking its receiver.
constexpr StringView InstanceNatives[] = {
    "Object.Equals",       "Object.GetHashCode",      "Object.ToString",        "Object.GetType",
    "String.Length",       "String.Equals",           "String.Substring",       "String.Contains",
    "String.StartsWith",   "String.EndsWith",         "String.CompareTo",       "String.GetHashCode",
    "String.IndexOf",      "String.LastIndexOf",      "String.GetByte",         "String.Replace",
    "String.Trim",         "String.TrimStart",        "String.TrimEnd",         "String.ToUpperAscii",
    "String.ToLowerAscii", "StringBuilder.AppendString", "StringBuilder.AppendInt32", "StringBuilder.AppendUInt32",
    "StringBuilder.AppendInt64", "StringBuilder.AppendUInt64", "StringBuilder.AppendFloat",
    "StringBuilder.AppendDouble", "StringBuilder.AppendBool", "StringBuilder.AppendRune",
    "StringBuilder.AppendInt32Format", "StringBuilder.AppendUInt32Format", "StringBuilder.AppendInt64Format",
    "StringBuilder.AppendUInt64Format", "StringBuilder.AppendFloatFormat", "StringBuilder.AppendDoubleFormat",
    "StringBuilder.ToString", "Array.Length", "Array.CopyTo", "Array.Clear", "TypeInfo.Name",
    "TypeInfo.IsAssignableFrom", "WeakRef.GetTarget", "WeakRef.SetTarget", "Task.IsCompleted", "Task.IsCanceled",
    "Task.IsFaulted", "Task.Cancel", "Task.Result"};

void CheckReceiver(ScriptCallContext &context, const void *target)
{
    if (context.Get<ScriptObject *>(0) == nullptr)
    {
        context.Fault(ScriptFault::NullReference, "the object is null");
        return;
    }
    const auto *original = static_cast<const ScriptNativeEntry *>(target);
    original->Thunk(context, original->Target);
}

uint64 MixEntropy(uint64 value)
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}
} // namespace

uint32 ScriptSlotsOfShape(const ScriptModuleData &module, uint32 shape)
{
    const ScriptShape &value = module.Shapes[shape];
    switch (value.Kind)
    {
    case ScriptShapeKind::Void:
        return 0;
    case ScriptShapeKind::Bool:
    case ScriptShapeKind::Int:
    case ScriptShapeKind::Float:
    case ScriptShapeKind::Reference:
    case ScriptShapeKind::Pointer:
        return 1;
    default:
        return value.Size == 0 ? 1 : (value.Size + ScriptSlotBytes - 1) / ScriptSlotBytes;
    }
}

void CollectScriptReferences(const ScriptModuleData &module, uint32 shape, uint32 offset, List<uint32> &references)
{
    const ScriptShape &value = module.Shapes[shape];
    switch (value.Kind)
    {
    case ScriptShapeKind::Reference:
        references.Add(offset);
        break;
    case ScriptShapeKind::Array:
        for (uint32 i = 0; i < value.Count; ++i)
        {
            CollectScriptReferences(module, value.Element, offset + i * value.Stride, references);
        }
        break;
    case ScriptShapeKind::Struct:
        for (const ScriptShapeField &field : value.Fields)
        {
            CollectScriptReferences(module, field.Shape, offset + field.Offset, references);
        }
        break;
    default:
        break;
    }
}

String ScriptFaultInfo::ToString() const
{
    StringBuilder<256> text;
    text.Append(GetScriptFaultName(Kind)).Append(": ").Append(Message.AsView());
    for (const ScriptStackEntry &entry : Stack)
    {
        text.Append('\n').Append(Format("  at {0}", entry.Function.AsView()).AsView());
        if (!entry.File.IsEmpty())
        {
            text.Append(Format(" ({0}:{1}:{2})", entry.File.AsView(), entry.Line, entry.Column).AsView());
        }
    }
    return text.ToString();
}

ScriptArray *NewScriptString(ScriptDomain &domain, StringView text)
{
    return domain.NewString(text);
}

void ScriptCallContext::Fault(ScriptFault kind, StringView message)
{
    if (_faulted)
    {
        return;
    }
    _faulted = true;
    _faultKind = kind;
    _faultMessage = String(message);
}

void ScriptNativeRegistry::Register(StringView name, ScriptNativeThunk thunk, const void *target)
{
    _entries.Set(String(name), ScriptNativeEntry{thunk, target});
}

ScriptVM::ScriptVM()
{
    RegisterScriptCoreNatives(_natives);
    RegisterScriptWorldNatives(_natives);
    ScriptInterpreter::RegisterNatives(_natives);
    static ScriptNativeEntry originals[sizeof(InstanceNatives) / sizeof(InstanceNatives[0])];
    for (usize i = 0; i < sizeof(InstanceNatives) / sizeof(InstanceNatives[0]); ++i)
    {
        if (const ScriptNativeEntry *entry = _natives.Find(InstanceNatives[i]))
        {
            originals[i] = *entry;
            _natives.Register(InstanceNatives[i], &CheckReceiver, &originals[i]);
        }
    }
}

const ScriptModuleData *ScriptVM::AddModule(Span<const uint8> bytes, String &error)
{
    ScriptModuleData module;
    if (!ReadScriptModule(bytes, module, error))
    {
        return nullptr;
    }
    return AddModule(Move(module));
}

const ScriptModuleData *ScriptVM::AddModule(ScriptModuleData module)
{
    const StringView package = module.Package < module.Strings.Count() ? module.Strings[module.Package].AsView() : "";
    UniquePtr<ScriptModuleData> image = MakeUnique<ScriptModuleData>(Move(module));
    ++_generation;
    // Its components and systems join the ECS for every world (MarlinScript §11.2).
    List<String> errors;
    if (!ScriptEcs::Register(*image, errors))
    {
        for (const String &error : errors)
        {
            MARLIN_LOG(LogScript, Error, "{0}: {1}", package, error);
        }
    }
    for (UniquePtr<ScriptModuleData> &existing : _modules)
    {
        if (existing->Strings[existing->Package].AsView() == package)
        {
            existing = Move(image);
            return existing.Get();
        }
    }
    _modules.Add(Move(image));
    return _modules.Last().Get();
}

const ScriptModuleData *ScriptVM::FindModule(StringView package) const
{
    for (const UniquePtr<ScriptModuleData> &module : _modules)
    {
        if (module->Strings[module->Package].AsView() == package)
        {
            return module.Get();
        }
    }
    return nullptr;
}

ScriptDomain::ScriptDomain(ScriptVM &vm, const ScriptDomainOptions &options) : _vm(&vm), _options(options)
{
    _heap = MakeUnique<ScriptHeap>(_options);
    _collector = MakeUnique<ScriptCollector>(*this, *_heap);
    _interpreter = MakeUnique<ScriptInterpreter>(*this);
    _entropyCounter = MixEntropy(PlatformTime::Ticks() ^ reinterpret_cast<uintptr>(this));
    _objectType = &NewType("Marlin.object", ScriptTypeKind::Class);
    _objectType->Builtin = ScriptBuiltin::Object;
    _stringType = &NewType("Marlin.string", ScriptTypeKind::String);
    _stringType->Builtin = ScriptBuiltin::String;
    _byteArrayType = &NewType("$byte[]", ScriptTypeKind::Array);
    _byteArrayType->Builtin = ScriptBuiltin::Array;
    for (ScriptType *bytes : {_stringType, _byteArrayType})
    {
        bytes->Base = _objectType;
        bytes->ElementSize = 1;
        bytes->ElementStride = 1;
        bytes->DataOffset = ScriptArrayLengthOffset + 4;
        bytes->InstanceSize = bytes->DataOffset;
        bytes->ValueAccess = MakeScriptAccess(ScriptAccess::U8);
    }
    _referenceArrayType = &NewType("$object[]", ScriptTypeKind::Array);
    _referenceArrayType->Builtin = ScriptBuiltin::Array;
    _referenceArrayType->Base = _objectType;
    _referenceArrayType->ElementSize = 8;
    _referenceArrayType->ElementStride = 8;
    _referenceArrayType->DataOffset = AlignUp(ScriptArrayLengthOffset + 4, 8);
    _referenceArrayType->InstanceSize = _referenceArrayType->DataOffset;
    _referenceArrayType->ElementReferences.Add(0);
    _referenceArrayType->ValueAccess = MakeScriptAccess(ScriptAccess::Reference);
}

ScriptDomain::~ScriptDomain()
{
    _interpreter = nullptr;
    for (const UniquePtr<ScriptLoadedModule> &module : _modules)
    {
        Memory::Free(module->Statics);
    }
    _collector = nullptr;
    _heap = nullptr;
}

ScriptType &ScriptDomain::NewType(StringView name, ScriptTypeKind kind)
{
    if (ScriptType **found = _typesByName.Find(name))
    {
        return **found;
    }
    UniquePtr<ScriptType> type = MakeUnique<ScriptType>();
    type->Name = String(name);
    type->Kind = kind;
    ScriptType &result = *type;
    _typesByName.Add(String(name), &result);
    _types.Add(Move(type));
    return result;
}

const ScriptFunctionInfo *ScriptDomain::FindFunction(StringView name) const
{
    ScriptFunctionInfo *const *found = _functionsByName.Find(name);
    return found != nullptr ? *found : nullptr;
}

const ScriptType *ScriptDomain::FindType(StringView name) const
{
    ScriptType *const *found = _typesByName.Find(name);
    return found != nullptr ? *found : nullptr;
}

void ScriptDomain::SetUpFunction(ScriptLoadedModule &module, uint32 index, ScriptFunctionInfo &function)
{
    const ScriptModuleData &data = *module.Data;
    const ScriptFunctionRecord &record = data.Functions[index];
    function.Name = String(module.GetString(record.Name));
    function.Kind = record.Kind;
    function.Flags = record.Flags;
    function.Module = &module;
    function.Record = &record;
    const ScriptShape &signature = data.Shapes[record.Signature];
    uint32 offset = 0;
    for (const uint32 parameter : signature.Parameters)
    {
        function.ParameterOffsets.Add(offset);
        function.ParameterShapes.Add(parameter);
        CollectScriptReferences(data, parameter, offset * ScriptSlotBytes, function.ParameterReferences);
        CollectManagedPointers(data, parameter, offset * ScriptSlotBytes, function.ParameterManaged);
        offset += module.ShapeSlots[parameter];
    }
    function.ParameterSlots = offset > record.ParameterSlots ? offset : record.ParameterSlots;
    function.FrameSlots = record.FrameSlots > function.ParameterSlots ? record.FrameSlots : function.ParameterSlots;
    function.ZeroSlots = record.ZeroSlots;
    function.ReturnSlots = record.ReturnSlots;
    function.CallSlots = function.ParameterSlots > function.ReturnSlots ? function.ParameterSlots : function.ReturnSlots;
    function.FrameBytes =
        (function.FrameSlots > function.CallSlots ? function.FrameSlots : function.CallSlots) * ScriptSlotBytes;
    const bool hasCode = (record.Flags & (ScriptFunctionFlags::Native | ScriptFunctionFlags::Imported)) == 0;
    function.Code = hasCode && !record.Code.IsEmpty() ? record.Code.Data() : nullptr;
    function.ResultShape = record.ResultShape;
    function.Sites.Resize(record.InterfaceSites.Count());
    for (const ScriptExport &entry : data.Exports)
    {
        if (entry.Kind == ScriptExportKind::Function && entry.Index == index)
        {
            function.SignatureHash = entry.Hash;
        }
    }
}

bool ScriptDomain::LinkFunction(ScriptLoadedModule &module, uint32 index, String &error)
{
    const ScriptModuleData &data = *module.Data;
    const ScriptFunctionRecord &record = data.Functions[index];
    ScriptFunctionInfo &function = *module.Functions[index];
    if ((record.Flags & ScriptFunctionFlags::Native) != 0)
    {
        const StringView name = module.GetString(record.NativeName);
        const ScriptNativeEntry *entry = _vm->GetNatives().Find(name);
        if (entry == nullptr)
        {
            error = Format("{0}: no native function is registered as \"{1}\"", function.Name.AsView(), name);
            return false;
        }
        function.Native = entry->Thunk;
        function.NativeTarget = entry->Target;
        return true;
    }
    if ((record.Flags & ScriptFunctionFlags::Imported) == 0)
    {
        return true;
    }
    uint64 hash = 0;
    for (const ScriptImport &import : data.Imports)
    {
        if (import.Kind == ScriptImportKind::Function && import.Index == index)
        {
            hash = import.Hash;
        }
    }
    ScriptFunctionInfo *const *found = _functionsByName.Find(function.Name.AsView());
    if (found == nullptr || ((*found)->Flags & ScriptFunctionFlags::Imported) != 0)
    {
        error = Format("{0} imports {1}, which no loaded module exports", module.Package.AsView(), function.Name.AsView());
        return false;
    }
    if (hash != 0 && (*found)->SignatureHash != 0 && hash != (*found)->SignatureHash)
    {
        error = Format("{0} imports {1} with another signature than its module exports", module.Package.AsView(),
                       function.Name.AsView());
        return false;
    }
    module.Functions[index] = *found;
    return true;
}

void ScriptDomain::CompleteType(ScriptLoadedModule &module, uint32 index, ScriptType &type)
{
    const ScriptModuleData &data = *module.Data;
    const ScriptTypeRecord &record = data.Types[index];
    type.Kind = record.Kind;
    type.Flags = record.Flags;
    type.Module = &module;
    type.Layout = record.Layout;
    if (record.Base != ~0u && record.Base < module.Types.Count())
    {
        type.Base = module.Types[record.Base];
    }
    else if (&type != _objectType && record.Kind != ScriptTypeKind::Interface)
    {
        type.Base = _objectType;
    }
    type.Builtin = BuiltinOfName(type.Name.AsView());
    const uint32 layoutSize = record.Layout != ~0u ? data.Shapes[record.Layout].Size : 0;
    type.References.Clear();
    switch (record.Kind)
    {
    case ScriptTypeKind::Class: {
        if (record.Layout != ~0u)
        {
            CollectScriptReferences(data, record.Layout, ScriptObjectHeaderBytes, type.References);
        }
        type.PayloadOffset = AlignUp(ScriptObjectHeaderBytes + layoutSize, 8);
        uint32 state = 0;
        switch (type.Builtin)
        {
        case ScriptBuiltin::StringBuilder:
            state = sizeof(ScriptStringBuilderState);
            type.References.Add(type.PayloadOffset);
            break;
        case ScriptBuiltin::TypeInfo:
            state = sizeof(ScriptTypeInfoState);
            break;
        case ScriptBuiltin::Task:
            state = sizeof(ScriptTaskState);
            for (uint32 i = 0; i < ScriptTaskReferences; ++i)
            {
                type.References.Add(type.PayloadOffset + i * 8);
            }
            break;
        case ScriptBuiltin::WeakRef:
            state = sizeof(ScriptWeakRefState);
            break;
        default:
            break;
        }
        type.InstanceSize = type.PayloadOffset + state;
        break;
    }
    case ScriptTypeKind::Interface:
        break;
    case ScriptTypeKind::Delegate:
        type.Builtin = ScriptBuiltin::Delegate;
        type.InstanceSize = sizeof(ScriptDelegateObject);
        type.PayloadOffset = ScriptObjectHeaderBytes;
        type.References.Add(16);
        type.References.Add(32);
        break;
    case ScriptTypeKind::Array: {
        type.Builtin = ScriptBuiltin::Array;
        const ScriptShape &element = data.Shapes[record.Layout];
        const uint32 alignment = element.Alignment == 0 ? 1 : element.Alignment;
        type.ElementSize = element.Size;
        type.ElementStride = AlignUp(element.Size, alignment);
        type.DataOffset = AlignUp(ScriptArrayLengthOffset + 4, alignment);
        type.InstanceSize = type.DataOffset;
        type.ElementReferences.Clear();
        CollectScriptReferences(data, record.Layout, 0, type.ElementReferences);
        type.ValueAccess = ReadAccessOfShape(data, record.Layout);
        break;
    }
    case ScriptTypeKind::String:
        break;
    case ScriptTypeKind::Value:
        type.Builtin = ScriptBuiltin::Box;
        type.InstanceSize = AlignUp(ScriptObjectHeaderBytes + layoutSize, 8);
        type.PayloadOffset = type.InstanceSize;
        CollectScriptReferences(data, record.Layout, ScriptObjectHeaderBytes, type.References);
        type.ValueAccess = ReadAccessOfShape(data, record.Layout);
        break;
    }
    type.VirtualSlots.Clear();
    for (const uint32 slot : record.VirtualSlots)
    {
        type.VirtualSlots.Add(slot < module.Functions.Count() ? module.Functions[slot] : nullptr);
    }
    type.Interfaces.Clear();
    for (const ScriptInterfaceTable &table : record.Interfaces)
    {
        ScriptInterfaceMethods &methods = type.Interfaces.AddDefaulted();
        methods.Interface = module.Types[table.Interface];
        for (const uint32 method : table.Methods)
        {
            methods.Methods.Add(method < module.Functions.Count() ? module.Functions[method] : nullptr);
        }
    }
    type.Implements.Clear();
    for (const uint32 inherited : record.Implements)
    {
        type.Implements.Add(module.Types[inherited]);
    }
}

bool ScriptDomain::Load(const ScriptModuleData &data, String &error)
{
    UniquePtr<ScriptLoadedModule> owned = MakeUnique<ScriptLoadedModule>();
    ScriptLoadedModule &module = *owned;
    module.Data = &data;
    module.Package = String(module.GetString(data.Package));
    for (uint32 shape = 0; shape < data.Shapes.Count(); ++shape)
    {
        module.ShapeSlots.Add(ScriptSlotsOfShape(data, shape));
        List<uint32> &references = module.ShapeReferences.AddDefaulted();
        CollectScriptReferences(data, shape, 0, references);
    }
    for (const ScriptFunctionRecord &record : data.Functions)
    {
        const String problem = VerifyCode(data, record);
        if (!problem.IsEmpty())
        {
            error = Format("{0}: {1}", module.GetString(record.Name), problem.AsView());
            return false;
        }
    }
    // Types: one per name in the domain; the first module that declares a type completes it.
    List<bool> complete;
    for (const ScriptTypeRecord &record : data.Types)
    {
        const StringView name = module.GetString(record.Name);
        ScriptType *type = nullptr;
        bool owns = false;
        if (record.Kind == ScriptTypeKind::String)
        {
            type = _stringType;
        }
        else if ((record.Flags & ScriptTypeFlags::Imported) != 0)
        {
            ScriptType **found = _typesByName.Find(name);
            if (found == nullptr)
            {
                error = Format("{0} imports the type {1}, which no loaded module declares", module.Package.AsView(),
                               name);
                return false;
            }
            type = *found;
        }
        else
        {
            type = &NewType(name, record.Kind);
            owns = type->Module == nullptr;
        }
        module.Types.Add(type);
        complete.Add(owns);
    }
    for (uint32 i = 0; i < data.Functions.Count(); ++i)
    {
        UniquePtr<ScriptFunctionInfo> function = MakeUnique<ScriptFunctionInfo>();
        SetUpFunction(module, i, *function);
        module.Functions.Add(function.Get());
        _functions.Add(Move(function));
    }
    for (uint32 i = 0; i < data.Functions.Count(); ++i)
    {
        if (!LinkFunction(module, i, error))
        {
            return false;
        }
    }
    // A function is empty once everything it calls is, so passes repeat until none changes: a constructor calls its
    // base's, which may come later in the module.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (ScriptFunctionInfo *function : module.Functions)
        {
            if (function->Module == &module && !function->Empty && IsEmptyFunction(module, *function))
            {
                function->Empty = true;
                changed = true;
            }
        }
    }
    for (uint32 i = 0; i < data.Types.Count(); ++i)
    {
        if (complete[i])
        {
            CompleteType(module, i, *module.Types[i]);
        }
    }
    // Arrays, strings, boxes and delegates answer the virtual members of object.
    for (uint32 i = 0; i < data.Types.Count(); ++i)
    {
        ScriptType &type = *module.Types[i];
        if (type.VirtualSlots.IsEmpty() && type.Kind != ScriptTypeKind::Class && type.Kind != ScriptTypeKind::Interface)
        {
            type.VirtualSlots = _objectType->VirtualSlots;
        }
    }
    if (_stringType->VirtualSlots.IsEmpty())
    {
        _stringType->VirtualSlots = _objectType->VirtualSlots;
    }
    for (ScriptType *synthetic : {_byteArrayType, _referenceArrayType})
    {
        if (synthetic->VirtualSlots.IsEmpty())
        {
            synthetic->VirtualSlots = _objectType->VirtualSlots;
        }
    }
    module.Strings.Resize(data.Strings.Count(), nullptr);
    if (data.StaticSize != 0)
    {
        module.Statics = static_cast<uint8 *>(
            Memory::Allocate(data.StaticSize, data.StaticAlignment > 16 ? data.StaticAlignment : 16));
        MemZero(module.Statics, data.StaticSize);
    }
    for (const ScriptStaticField &field : data.Statics)
    {
        CollectScriptReferences(data, field.Shape, field.Offset, module.StaticReferences);
    }
    for (uint32 i = 0; i < data.Functions.Count(); ++i)
    {
        const ScriptFunctionRecord &record = data.Functions[i];
        if ((record.Flags & (ScriptFunctionFlags::Imported | ScriptFunctionFlags::Native)) == 0 &&
            !record.Code.IsEmpty())
        {
            _functionsByName.TryAdd(module.Functions[i]->Name, module.Functions[i]);
        }
    }
    _modules.Add(Move(owned));
    for (const uint32 initializer : data.StaticInitializers)
    {
        const ScriptResult<void> result = Invoke(*module.Functions[initializer], {}, {});
        if (!result)
        {
            error = Format("a static initializer of {0} faulted: {1}", module.Package.AsView(),
                           result.GetFault().ToString().AsView());
            return false;
        }
    }
    return true;
}

ScriptResult<void> ScriptDomain::Invoke(const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments,
                                        Span<ScriptSlot> result)
{
    return _interpreter->Invoke(function, arguments, result);
}

bool ScriptDomain::TryInvoke(const ScriptFunctionInfo &function, Span<const ScriptSlot> arguments,
                             Span<ScriptSlot> result)
{
    ScriptCoroutine &coroutine = _interpreter->BeginBatch();
    const bool ok = _interpreter->CallInBatch(coroutine, function, arguments, result);
    _interpreter->EndBatch(coroutine);
    return ok;
}

const ScriptFaultInfo &ScriptDomain::GetLastFault() const
{
    return _interpreter->GetFault();
}

ScriptBatchCall::ScriptBatchCall(ScriptDomain &domain, const ScriptFunctionInfo &function)
    : _interpreter(domain._interpreter.Get()), _function(&function), _coroutine(&_interpreter->BeginBatch())
{
}

ScriptBatchCall::~ScriptBatchCall()
{
    _interpreter->EndBatch(*_coroutine);
}

bool ScriptBatchCall::Call(Span<const ScriptSlot> arguments, Span<ScriptSlot> result)
{
    return _interpreter->CallInBatch(*_coroutine, *_function, arguments, result);
}

const ScriptFaultInfo &ScriptBatchCall::GetFault() const
{
    return _interpreter->GetFault();
}

ScriptObject *ScriptDomain::Allocate(const ScriptType &type, usize bytes, bool permanent)
{
    bytes = (bytes + 15) & ~usize{15};
    bool old = permanent;
    uint8 *memory = nullptr;
    if (permanent)
    {
        memory = _heap->AllocateOld(bytes);
    }
    else
    {
        const bool nurseryFull = bytes <= ScriptHeap::LargeObjectBytes && _heap->IsNurseryFull(bytes);
        memory = _heap->Allocate(bytes, old);
        if (nurseryFull)
        {
            RequestCollection(false);
        }
    }
    if (memory == nullptr)
    {
        RequestCollection(true);
        return nullptr;
    }
    auto *object = reinterpret_cast<ScriptObject *>(memory);
    object->Type = &type;
    if (++_hashCounter == 0)
    {
        ++_hashCounter;
    }
    uint64 gc = static_cast<uint64>(_hashCounter) << 32;
    if (old)
    {
        gc |= ScriptGcBits::Old | _collector->OldMark();
    }
    if (permanent)
    {
        gc |= ScriptGcBits::Permanent;
    }
    object->Gc = gc;
    return object;
}

ScriptObject *ScriptDomain::NewObject(const ScriptType &type)
{
    return Allocate(type, type.InstanceSize, false);
}

ScriptArray *ScriptDomain::NewArray(const ScriptType &type, uint32 length)
{
    const uint64 bytes = uint64{type.DataOffset} + uint64{length} * type.ElementStride;
    if (bytes > (uint64{1} << 40))
    {
        return nullptr;
    }
    auto *array = static_cast<ScriptArray *>(Allocate(type, static_cast<usize>(bytes), false));
    if (array != nullptr)
    {
        array->Length = length;
    }
    return array;
}

ScriptArray *ScriptDomain::NewString(StringView text)
{
    ScriptArray *string = NewArray(*_stringType, static_cast<uint32>(text.Length()));
    if (string != nullptr && !text.IsEmpty())
    {
        MemCopy(reinterpret_cast<uint8 *>(string) + _stringType->DataOffset, text.Data(), text.Length());
    }
    return string;
}

ScriptArray *ScriptDomain::NewPermanentString(StringView text)
{
    auto *string =
        static_cast<ScriptArray *>(Allocate(*_stringType, _stringType->DataOffset + text.Length(), true));
    if (string == nullptr)
    {
        return nullptr;
    }
    string->Length = static_cast<uint32>(text.Length());
    if (!text.IsEmpty())
    {
        MemCopy(reinterpret_cast<uint8 *>(string) + _stringType->DataOffset, text.Data(), text.Length());
    }
    return string;
}

uint64 ScriptDomain::TakeEntropy()
{
    // RECOVERY-DECISION S8: entropy is the clock mixed with a per-domain counter through splitmix64, so seeds taken in
    // the same tick still differ.
    _entropyCounter = MixEntropy(_entropyCounter ^ PlatformTime::Ticks());
    return _entropyCounter;
}

ScriptObject *ScriptDomain::GetTypeInfo(const ScriptType &type)
{
    if (type.Info != nullptr)
    {
        return type.Info;
    }
    ScriptType **infoType = _typesByName.Find(StringView("Marlin.TypeInfo"));
    if (infoType == nullptr || (*infoType)->Builtin != ScriptBuiltin::TypeInfo)
    {
        return nullptr;
    }
    ScriptObject *info = Allocate(**infoType, (*infoType)->InstanceSize, true);
    if (info == nullptr)
    {
        return nullptr;
    }
    StateOf<ScriptTypeInfoState>(info).Described = &type;
    const_cast<ScriptType &>(type).Info = info;
    return info;
}

void ScriptDomain::WriteAccess(uint8 *memory, const uint8 *registers, uint32 access, const ScriptLoadedModule &module)
{
    const uint32 payload = GetScriptAccessPayload(access);
    switch (GetScriptAccess(access))
    {
    case ScriptAccess::I8:
    case ScriptAccess::U8:
        memory[0] = registers[0];
        return;
    case ScriptAccess::I16:
    case ScriptAccess::U16:
        MemCopy(memory, registers, 2);
        return;
    case ScriptAccess::B32:
        MemCopy(memory, registers, 4);
        return;
    case ScriptAccess::B64:
        MemCopy(memory, registers, 8);
        return;
    case ScriptAccess::Bytes:
        MemCopy(memory, registers, payload);
        return;
    case ScriptAccess::Reference: {
        ScriptObject *value;
        MemCopy(&value, registers, sizeof(value));
        WriteReference(reinterpret_cast<ScriptObject **>(memory), value);
        return;
    }
    case ScriptAccess::Typed: {
        if (payload >= module.ShapeReferences.Count())
        {
            return;
        }
        for (const uint32 offset : module.ShapeReferences[payload])
        {
            ScriptObject *value;
            MemCopy(&value, registers + offset, sizeof(value));
            WriteReference(reinterpret_cast<ScriptObject **>(memory + offset), value);
        }
        MemCopy(memory, registers, module.GetShape(payload).Size);
        return;
    }
    }
}

bool ScriptDomain::IsAssignable(const ScriptType &type, const ScriptType &target)
{
    if (&type == &target || target.Builtin == ScriptBuiltin::Object)
    {
        return true;
    }
    for (const ScriptType *base = type.Base; base != nullptr; base = base->Base)
    {
        if (base == &target)
        {
            return true;
        }
    }
    if (target.Kind != ScriptTypeKind::Interface)
    {
        return false;
    }
    for (const ScriptType *walk = &type; walk != nullptr; walk = walk->Base)
    {
        for (const ScriptInterfaceMethods &methods : walk->Interfaces)
        {
            if (methods.Interface == &target)
            {
                return true;
            }
        }
        for (const ScriptType *inherited : walk->Implements)
        {
            if (inherited == &target || IsAssignable(*inherited, target))
            {
                return true;
            }
        }
    }
    return false;
}

ScriptObject *ScriptDomain::CombineDelegates(ScriptObject *left, ScriptObject *right, bool remove)
{
    if (right == nullptr)
    {
        return left;
    }
    if (left == nullptr)
    {
        return remove ? nullptr : right;
    }
    // The single delegates of each side, in call order.
    List<ScriptDelegateObject *> invocations;
    List<ScriptDelegateObject *> removed;
    const auto flatten = [](ScriptObject *object, List<ScriptDelegateObject *> &into) {
        auto *delegate = static_cast<ScriptDelegateObject *>(object);
        if (delegate->Invocations == nullptr)
        {
            into.Add(delegate);
            return;
        }
        auto **items = reinterpret_cast<ScriptDelegateObject **>(reinterpret_cast<uint8 *>(delegate->Invocations) +
                                                                 delegate->Invocations->Type->DataOffset);
        for (uint32 i = 0; i < delegate->Invocations->Length; ++i)
        {
            into.Add(items[i]);
        }
    };
    flatten(left, invocations);
    flatten(right, removed);
    if (remove)
    {
        // The last run of invocations equal to the removed ones goes, as in C#.
        const auto same = [](const ScriptDelegateObject *a, const ScriptDelegateObject *b) {
            return a->Function == b->Function && a->Target == b->Target;
        };
        bool found = false;
        for (usize start = invocations.Count() >= removed.Count() ? invocations.Count() - removed.Count() + 1 : 0;
             start-- > 0 && !found;)
        {
            bool match = true;
            for (usize i = 0; i < removed.Count() && match; ++i)
            {
                match = same(invocations[start + i], removed[i]);
            }
            if (match)
            {
                invocations.RemoveAt(start, removed.Count());
                found = true;
            }
        }
        if (!found)
        {
            return left;
        }
        if (invocations.IsEmpty())
        {
            return nullptr;
        }
        if (invocations.Count() == 1)
        {
            return invocations[0];
        }
    }
    else
    {
        invocations.AddRange(removed.AsSpan());
    }
    ScriptArray *items = NewArray(*_referenceArrayType, static_cast<uint32>(invocations.Count()));
    if (items == nullptr)
    {
        return nullptr;
    }
    auto **slots = reinterpret_cast<ScriptObject **>(reinterpret_cast<uint8 *>(items) + _referenceArrayType->DataOffset);
    for (usize i = 0; i < invocations.Count(); ++i)
    {
        WriteReference(&slots[i], invocations[i]);
    }
    auto *combined = static_cast<ScriptDelegateObject *>(NewObject(*left->Type));
    if (combined == nullptr)
    {
        return nullptr;
    }
    combined->Function = invocations.Last()->Function;
    WriteReference(&combined->Target, invocations.Last()->Target);
    WriteReference(reinterpret_cast<ScriptObject **>(&combined->Invocations), items);
    return combined;
}

void ScriptDomain::AdvanceTime(float64 seconds)
{
    _interpreter->AdvanceTime(seconds);
}

void ScriptDomain::BeginFrame()
{
    _interpreter->BeginFrame();
    StepCollector();
}

void ScriptDomain::FixedTick()
{
    _interpreter->FixedTick();
}

void ScriptDomain::RunReady()
{
    _interpreter->RunReady();
}

void ScriptDomain::CancelTasksOwnedBy(const ScriptObject *owner)
{
    _interpreter->CancelTasksOwnedBy(owner);
}

ScriptTaskStatus ScriptDomain::GetTaskStatus(const ScriptObject *task, ScriptFaultInfo *fault) const
{
    return _interpreter->GetTaskStatus(task, fault);
}

void ScriptDomain::RequestCollection(bool full)
{
    _collectionRequested = true;
    _fullCollectionRequested = _fullCollectionRequested || full;
}

uint64 ScriptDomain::GetCollectionCount() const
{
    return _collections;
}

usize ScriptDomain::GetHeapBytes() const
{
    return _heap->GetUsedBytes();
}

bool MatchesScriptSignature(const ScriptFunctionInfo &function, Span<const uint32> parameters, uint32 result)
{
    if (function.ParameterShapes.Count() != parameters.Count() || function.Module == nullptr)
    {
        return false;
    }
    const ScriptLoadedModule &module = *function.Module;
    for (usize i = 0; i < parameters.Count(); ++i)
    {
        if (!KindsMatch(parameters[i], ValueKindOfShape(module, function.ParameterShapes[i])))
        {
            return false;
        }
    }
    const ScriptShape &signature = module.GetShape(function.Record->Signature);
    const uint32 actual = signature.Element != ~0u ? ValueKindOfShape(module, signature.Element) : ScriptValueKinds::Void;
    return KindsMatch(result, actual);
}
} // namespace Marlin
