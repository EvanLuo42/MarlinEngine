#pragma once

#include "Containers/List.h"
#include "ScriptVM/ScriptBytecode.h"
#include "ScriptVM/ScriptModuleData.h"
#include "String/String.h"

namespace Marlin
{
class ScriptCallContext;
class ScriptDomain;
struct ScriptFunctionInfo;
struct ScriptLoadedModule;
struct ScriptType;

/// The header every object on the script heap starts with (MarlinScript §9.1).
struct ScriptObject
{
    const ScriptType *Type;
    /// The GC bits (ScriptGcBits) in the low half and the hash ordinal in the high half.
    uint64 Gc;

    [[nodiscard]] uint8 *GetData()
    {
        return reinterpret_cast<uint8 *>(this) + ScriptObjectHeaderBytes;
    }

    [[nodiscard]] const uint8 *GetData() const
    {
        return reinterpret_cast<const uint8 *>(this) + ScriptObjectHeaderBytes;
    }

    [[nodiscard]] uint32 GetHashOrdinal() const
    {
        return static_cast<uint32>(Gc >> 32);
    }
};

static_assert(sizeof(ScriptObject) == ScriptObjectHeaderBytes);

/// An array or a string: the length after the header, the elements from the type's DataOffset.
struct ScriptArray : ScriptObject
{
    uint32 Length;
};

/// A delegate: a function with its receiver, or with Invocations, the delegates of a combined one in call order.
struct ScriptDelegateObject : ScriptObject
{
    ScriptObject *Target;
    const ScriptFunctionInfo *Function;
    ScriptArray *Invocations;
};

/// The objects the VM itself knows (MarlinScript §9.8); their VM state follows their fields.
enum class ScriptBuiltin : uint8
{
    None,
    Object,
    String,
    Array,
    Delegate,
    Box,
    StringBuilder,
    TypeInfo,
    Task,
    Iterator,
    WeakRef,
};

struct ScriptInterfaceMethods
{
    const ScriptType *Interface = nullptr;
    List<const ScriptFunctionInfo *> Methods;
};

/// A type of a domain. There is one per name, shared by every module that names it (MarlinScript §6.2).
struct ScriptType
{
    ScriptTypeKind Kind = ScriptTypeKind::Class;
    ScriptBuiltin Builtin = ScriptBuiltin::None;
    uint8 Flags = 0;
    String Name;
    const ScriptType *Base = nullptr;
    /// Bytes of an instance; arrays add their elements.
    uint32 InstanceSize = ScriptObjectHeaderBytes;
    /// Where a builtin's VM state starts, after the fields.
    uint32 PayloadOffset = ScriptObjectHeaderBytes;
    /// The byte offsets of the references in an instance, the VM state's ones included.
    List<uint32> References;

    /// Of an array: the element's size, the distance between elements and where they start.
    uint32 ElementSize = 0;
    uint32 ElementStride = 0;
    uint32 DataOffset = 0;
    /// The byte offsets of the references in an element.
    List<uint32> ElementReferences;
    /// How a boxed value or an array element is read, as a ScriptAccess word.
    uint32 ValueAccess = 0;
    List<const ScriptFunctionInfo *> VirtualSlots;
    List<ScriptInterfaceMethods> Interfaces;
    /// The interfaces an interface inherits.
    List<const ScriptType *> Implements;
    const ScriptLoadedModule *Module = nullptr;
    /// The layout shape in Module; ~0u for none.
    uint32 Layout = ~0u;
    /// The TypeInfo object describing this type, made on first use.
    ScriptObject *Info = nullptr;

    [[nodiscard]] bool IsReferenceArray() const
    {
        return (Kind == ScriptTypeKind::Array || Kind == ScriptTypeKind::String) &&
               GetScriptAccess(ValueAccess) == ScriptAccess::Reference;
    }
};

/// The inline cache of an interface call site (MarlinScript §14.2): the receiver type it saw last and what it found.
struct ScriptInterfaceCache
{
    const ScriptType *Type = nullptr;
    const ScriptFunctionInfo *Target = nullptr;
    uint32 Misses = 0;
};

/// How the VM calls a native function: *target* is what it was registered with.
using ScriptNativeThunk = void (*)(ScriptCallContext &context, const void *target);

/// A function of a domain: its module's record linked to what it calls.
struct ScriptFunctionInfo
{
    String Name;
    ScriptFunctionKind Kind = ScriptFunctionKind::Normal;
    uint8 Flags = 0;
    const ScriptLoadedModule *Module = nullptr;
    const ScriptFunctionRecord *Record = nullptr;
    uint32 ParameterSlots = 0;
    uint32 FrameSlots = 0;
    /// The registers after the parameters PushFrame zeroes (ScriptFunctionRecord::ZeroSlots).
    uint32 ZeroSlots = 0;
    /// Slots of the result, which a call leaves at the start of its window.
    uint32 ReturnSlots = 0;
    /// Slots a call window takes: the larger of the parameters and the result.
    uint32 CallSlots = 0;
    /// The first slot of each parameter in the window.
    List<uint32> ParameterOffsets;
    /// The shape of each parameter in Module.
    List<uint32> ParameterShapes;
    uint32 ResultShape = ~0u;
    /// The byte offsets in the window of the references and managed pointers the parameters hold.
    List<uint32> ParameterReferences;
    List<uint32> ParameterManaged;
    uint64 SignatureHash = 0;
    ScriptNativeThunk Native = nullptr;
    const void *NativeTarget = nullptr;
    /// The caches of the record's interface call sites.
    List<ScriptInterfaceCache> Sites;
    /// The function's bytecode; null for natives and abstract methods. A call runs a Normal function with code in a
    /// frame of its own and hands the others to the slow path.
    const uint32 *Code = nullptr;
    /// Bytes the frame takes on the stack: the larger of FrameSlots and CallSlots.
    uint32 FrameBytes = 0;
    /// Whether a call does nothing and can be skipped: the function returns nothing and only moves registers and
    /// calls other such functions, as the constructors of classes without field initializers do.
    bool Empty = false;

    [[nodiscard]] bool IsNative() const
    {
        return Native != nullptr;
    }
};

/// A module loaded into a domain.
struct ScriptLoadedModule
{
    const ScriptModuleData *Data = nullptr;
    String Package;
    List<ScriptFunctionInfo *> Functions;
    List<ScriptType *> Types;
    uint8 *Statics = nullptr;
    /// The module's strings as permanent string objects.
    List<ScriptArray *> Strings;
    /// The byte offsets of the references in the statics.
    List<uint32> StaticReferences;
    /// For each shape, its slots and the byte offsets of its references.
    List<uint32> ShapeSlots;
    List<List<uint32>> ShapeReferences;

    [[nodiscard]] const ScriptShape &GetShape(uint32 index) const
    {
        return Data->Shapes[index];
    }

    [[nodiscard]] StringView GetString(uint32 index) const
    {
        return index < Data->Strings.Count() ? Data->Strings[index].AsView() : StringView();
    }
};

/// The registers a value of *shape* takes (MarlinScript §14.1).
[[nodiscard]] SCRIPTVM_API uint32 ScriptSlotsOfShape(const ScriptModuleData &module, uint32 shape);
/// Adds the byte offsets of the references in a value of *shape* at *offset*.
SCRIPTVM_API void CollectScriptReferences(const ScriptModuleData &module, uint32 shape, uint32 offset,
                                          List<uint32> &references);
} // namespace Marlin
