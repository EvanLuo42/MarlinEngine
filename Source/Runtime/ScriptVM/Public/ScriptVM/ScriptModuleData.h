#pragma once

#include "Containers/List.h"
#include "String/String.h"

namespace Marlin
{
/// The sections of a module image, in file order (MarlinScript §6.1).
enum class ScriptSection : uint8
{
    Strings,
    Shapes,
    Types,
    Functions,
    Statics,
    Imports,
    Exports,
    Debug,
    Attributes,
    Count,
};

/// The kind of a shape: the layout of a value in registers and memory, as MIR types describe it (MarlinLanguage
/// §10).
enum class ScriptShapeKind : uint8
{
    Void,
    Bool,
    Int,
    Float,
    Vector,
    Matrix,
    Array,
    Struct,
    Reference,
    Pointer,
    Function,
};

enum class ScriptPointerSpace : uint8
{
    Function,
    Managed,
    Static,
};

struct ScriptShapeField
{
    uint32 Name = 0;
    uint32 Offset = 0;
    uint32 Shape = 0;
};

/// A value layout. Indices name entries of the module's tables: Element a shape (a reference's script type, ~0u
/// when the module has none), Name a string.
struct ScriptShape
{
    ScriptShapeKind Kind = ScriptShapeKind::Void;
    uint8 Width = 0;
    bool Signed = false;
    ScriptPointerSpace Space = ScriptPointerSpace::Function;
    uint32 Count = 0;
    uint32 Rows = 0;
    uint32 Size = 0;
    uint32 Alignment = 0;
    uint32 Stride = 0;
    /// The element of a vector, matrix (its row), array or pointer; the result of a function; the script type of a
    /// reference.
    uint32 Element = ~0u;
    /// The name of a struct or of the type a reference refers to.
    uint32 Name = ~0u;
    List<ScriptShapeField> Fields;
    /// A function's parameters.
    List<uint32> Parameters;
};

enum class ScriptTypeKind : uint8
{
    Class,
    Interface,
    Delegate,
    Array,
    String,
    Value,
};

namespace ScriptTypeFlags
{
inline constexpr uint8 Abstract = 1 << 0;
inline constexpr uint8 Sealed = 1 << 1;
/// Declared by another module, linked by name when the module loads.
inline constexpr uint8 Imported = 1 << 2;
} // namespace ScriptTypeFlags

/// The methods, function indices, a class implements an interface with.
struct ScriptInterfaceTable
{
    uint32 Interface = 0;
    /// One per method of the interface, in its slot order.
    List<uint32> Methods;
};

/// An entry of the type table, which instructions name by index (MarlinScript §5.1).
struct ScriptTypeRecord
{
    ScriptTypeKind Kind = ScriptTypeKind::Class;
    uint8 Flags = 0;
    uint32 Name = 0;
    uint32 Base = ~0u;
    /// The shape of a class's fields, an array's element, a boxed value or a delegate's function; ~0u for none.
    uint32 Layout = ~0u;
    /// A class's virtual slots or an interface's methods, as function indices.
    List<uint32> VirtualSlots;
    List<ScriptInterfaceTable> Interfaces;
    List<uint32> Implements;
};

enum class ScriptFunctionKind : uint8
{
    Normal,
    Async,
    Iterator,
    /// Reserved for `[Kernel]` functions (08 §5.4).
    Kernel,
};

namespace ScriptFunctionFlags
{
/// Compiled from a `.mcommon` file: integer division by zero and out-of-range indices follow the shader rules.
inline constexpr uint8 Common = 1 << 0;
inline constexpr uint8 Imported = 1 << 1;
inline constexpr uint8 Native = 1 << 2;
inline constexpr uint8 StaticInitializer = 1 << 3;
/// Has a line table.
inline constexpr uint8 Debuggable = 1 << 4;
/// A `[Test]` function (MarlinScript §15).
inline constexpr uint8 Test = 1 << 5;
} // namespace ScriptFunctionFlags

/// A register's worth of bytes: a constant, an argument or a result.
struct ScriptSlot
{
    alignas(16) uint8 Bytes[16] = {};

    bool operator==(const ScriptSlot &other) const = default;
};

struct ScriptSwitchCase
{
    int64 Value = 0;
    uint32 Target = 0;
};

/// The cases of a Switch: values and the words they jump to.
struct ScriptSwitchTable
{
    /// Whether the selector is 64 bits.
    bool Is64 = false;
    uint32 Default = 0;
    List<ScriptSwitchCase> Cases;
};

struct ScriptInterfaceSite
{
    uint32 Interface = 0;
    uint32 Method = 0;
};

/// Where the collector may run in a function, and the map of the registers that hold references there
/// (MarlinScript §9.3).
struct ScriptSafepoint
{
    uint32 Pc = 0;
    uint32 Map = 0;
};

struct ScriptFunctionRecord
{
    uint32 Name = 0;
    ScriptFunctionKind Kind = ScriptFunctionKind::Normal;
    uint8 Flags = 0;
    /// The shape of the function type.
    uint32 Signature = 0;
    /// The name a Native function is registered as; ~0u for others.
    uint32 NativeName = ~0u;
    uint32 ParameterSlots = 0;
    uint32 FrameSlots = 0;
    /// The registers after the parameters that are zeroed when the frame is entered: those of the frame variables,
    /// which every map names. The other registers are written before any map names them.
    uint32 ZeroSlots = 0;
    uint32 ReturnSlots = 0;
    /// What an async function's task results in, or what an iterator yields; ~0u for none.
    uint32 ResultShape = ~0u;
    /// Bit i is set when parameter i is an `in` parameter, a pointer the function only reads through, such as a
    /// component a script system reads (MarlinScript §11.2).
    uint64 ReadOnlyParameters = 0;
    List<uint32> Code;
    List<ScriptSlot> Constants;
    /// The words that start basic blocks, sorted.
    List<uint32> Blocks;
    List<ScriptSwitchTable> Switches;
    List<ScriptInterfaceSite> InterfaceSites;
    /// Bytes of each map: a bit per 8-byte unit of the frame for object references, then as many for managed
    /// pointers.
    uint32 MapBytes = 0;
    /// The maps, MapBytes each.
    List<uint8> Maps;
    List<ScriptSafepoint> Safepoints;
    /// The delta-encoded rows of AppendScriptLineEntry.
    List<uint8> LineTable;
};

struct ScriptStaticField
{
    uint32 Name = 0;
    uint32 Offset = 0;
    uint32 Shape = 0;
};

enum class ScriptImportKind : uint8
{
    Function,
    Native,
    Type,
};

/// Something another module or the VM defines, linked by name when the module loads (MarlinScript §6.2).
struct ScriptImport
{
    ScriptImportKind Kind = ScriptImportKind::Function;
    /// The function or type of this module the import fills in.
    uint32 Index = 0;
    uint32 Name = 0;
    /// The signature hash an imported function must match; 0 for others.
    uint64 Hash = 0;
};

enum class ScriptExportKind : uint8
{
    Function,
    Type,
};

struct ScriptExport
{
    ScriptExportKind Kind = ScriptExportKind::Function;
    uint32 Index = 0;
    uint64 Hash = 0;
};

struct ScriptSourceFile
{
    uint32 Path = 0;
    uint64 ContentHash = 0;
};

enum class ScriptAttributeTarget : uint8
{
    /// Index is a struct shape.
    Shape,
    /// Index is a function.
    Function,
};

/// A field of an attribute and its value as text; both are strings.
struct ScriptAttributeArgument
{
    uint32 Name = 0;
    uint32 Value = 0;
};

/// A user-declared attribute on a struct or function of the module, kept for the engine to read, such as the ECS's
/// `Marlin.ComponentAttribute` and `Marlin.SystemAttribute` (MarlinScript §11.2). Name is the attribute struct's
/// qualified name.
struct ScriptAttribute
{
    ScriptAttributeTarget Target = ScriptAttributeTarget::Shape;
    uint32 Index = 0;
    uint32 Name = 0;
    List<ScriptAttributeArgument> Arguments;
};

/// A compiled script module: the bytecode of one package (MarlinScript §1.2, §6.1).
struct ScriptModuleData
{
    uint32 Package = 0;
    /// A hash of the exports, which modules that import from this one depend on (MarlinScript §1.2).
    uint64 InterfaceHash = 0;
    List<String> Strings;
    List<ScriptShape> Shapes;
    List<ScriptTypeRecord> Types;
    List<ScriptFunctionRecord> Functions;
    uint32 StaticSize = 0;
    uint32 StaticAlignment = 1;
    List<ScriptStaticField> Statics;
    List<uint32> StaticInitializers;
    List<ScriptImport> Imports;
    List<ScriptExport> Exports;
    List<ScriptSourceFile> Files;
    List<ScriptAttribute> Attributes;
};

SCRIPTVM_API void WriteScriptModule(const ScriptModuleData &module, List<uint8> &bytes);
/// Reads a module, checking its magic, version and the bounds of every section and index.
[[nodiscard]] SCRIPTVM_API bool ReadScriptModule(Span<const uint8> bytes, ScriptModuleData &module, String &error);

/// A row of a line table: from word *Pc* on, the code comes from *Line* and *Column* of file *File* (an index into
/// ScriptModuleData::Files).
struct ScriptLineEntry
{
    uint32 Pc = 0;
    uint32 File = 0;
    uint32 Line = 0;
    uint32 Column = 0;
};

/// Appends *entry* to *table*, delta-encoded against *previous*.
SCRIPTVM_API void AppendScriptLineEntry(List<uint8> &table, const ScriptLineEntry &previous, const ScriptLineEntry &entry);
[[nodiscard]] SCRIPTVM_API bool DecodeScriptLineTable(Span<const uint8> table, List<ScriptLineEntry> &entries);
} // namespace Marlin
