//! The runtime description of reflected types: kinds, metadata, fields, functions, operations and `TypeInfo`.

#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EnumFlags.h"
#include "Reflection/Markup.h"
#include "String/StringView.h"
#include "Templates/Optional.h"

namespace Marlin
{
class StringWriter;
struct Guid;

namespace Private
{
class TypeRegistry;

/// Type-erased operations hand out writable pointers into values their caller may only read.
[[nodiscard]] inline void *RemoveConst(const void *value)
{
    return const_cast<void *>(value); // NOLINT(cppcoreguidelines-pro-type-const-cast)
}
} // namespace Private

/// Returns a type's TypeInfo. Descriptors hold getters rather than pointers so they stay constant-initialized when
/// the type lives in another module.
using TypeGetter = const TypeInfo &(*)();

/// What a type is to reflection.
///
/// Containers describe their contents through their operations.
enum class TypeKind : uint8
{
    /// A `bool`.
    Bool,
    /// An 8-bit signed integer.
    Int8,
    /// A 16-bit signed integer.
    Int16,
    /// A 32-bit signed integer.
    Int32,
    /// A 64-bit signed integer.
    Int64,
    /// An 8-bit unsigned integer.
    UInt8,
    /// A 16-bit unsigned integer.
    UInt16,
    /// A 32-bit unsigned integer.
    UInt32,
    /// A 64-bit unsigned integer.
    UInt64,
    /// A 32-bit IEEE floating-point value.
    Float32,
    /// A 64-bit IEEE floating-point value.
    Float64,
    /// A `String`.
    String,
    /// A `StringView`.
    StringView,
    /// A `Name`.
    Name,
    /// An enum, with `EnumeratorInfo` values.
    Enum,
    /// A reflected value type (`MSTRUCT`).
    Struct,
    /// A reflected polymorphic class (`MCLASS`).
    Class,
    /// A reflected abstract base (`MINTERFACE`).
    Interface,
    /// A `List<T>`.
    List,
    /// A C array or `Array<T, N>`.
    FixedArray,
    /// A `HashMap<TKey, TValue>`.
    Map,
    /// A `HashSet<T>`.
    Set,
    /// An `Optional<T>`.
    Optional,
    /// A `UniquePtr<T>`.
    UniquePtr,
    /// A `SharedPtr<T>`.
    SharedPtr,
    /// A reference to an object by `Guid`, such as `AssetRef<T>`.
    ///
    /// The element is the type it refers to. Its operations are `LinkOperations`.
    ObjectLink,
    /// Known by name, size and operations only, such as a handle another module defines.
    ///
    /// That module reads `TypeInfo::GetExtension`.
    Opaque,
};

/// One `Key=Value` pair from the markup.
///
/// A bare key has an empty value. String literals are stored unquoted; anything else is stored as written, such as
/// `(0, 10000)` or `Cheat|Scalability`.
struct MetadataEntry
{
    /// The key.
    StringView Key;
    /// The value, or empty for a bare key.
    StringView Value;
};

/// A set of metadata entries read from the reflection markup.
class CORE_API Metadata
{
public:
    /// Creates an empty set.
    constexpr Metadata() = default;

    /// Creates a set over `entries`, which must outlive it.
    constexpr explicit Metadata(Span<const MetadataEntry> entries) : _entries(entries)
    {
    }

    /// Returns whether `key` is present.
    [[nodiscard]] bool Has(StringView key) const;
    /// Returns the value of `key`, or an empty optional.
    [[nodiscard]] Optional<StringView> Get(StringView key) const;
    /// Returns the value of `key`, or `fallback`.
    [[nodiscard]] StringView GetOr(StringView key, StringView fallback) const;
    /// Returns whether the value of `key` is a `|`-separated list that contains `flag`.
    [[nodiscard]] bool HasFlag(StringView key, StringView flag) const;

    /// Returns the entries.
    [[nodiscard]] constexpr Span<const MetadataEntry> GetEntries() const
    {
        return _entries;
    }

private:
    Span<const MetadataEntry> _entries;
};

/// The generic operations of a type.
///
/// An entry is null when the type does not support it.
struct TypeOperations
{
    /// Default-constructs at `target`; null if the type is not constructible.
    void (*Construct)(void *target);
    /// Destroys at `target`; null for an abstract type or one without a destructor.
    void (*Destruct)(void *target);
    /// Copy-constructs at `target` from `source`; null if the type is not copy-constructible.
    void (*CopyConstruct)(void *target, const void *source);
    /// Move-constructs at `target` from `source`; null if the type is not move-constructible.
    void (*MoveConstruct)(void *target, void *source);
    /// Copy-assigns `source` to `target`; null if the type is not copy-assignable.
    void (*CopyAssign)(void *target, const void *source);
    /// Move-assigns `source` to `target`; null if the type is not move-assignable.
    void (*MoveAssign)(void *target, void *source);
    /// Returns whether two values are equal; null if the type has no `operator==`.
    bool (*Equals)(const void *a, const void *b);
    /// Returns the hash of a value; null if the type is not hashable.
    uint64 (*Hash)(const void *value);
    /// Formats a value through its `FormatTo` member; null if it has none.
    void (*Format)(StringWriter &output, const void *value);
    /// Parses text through a static `bool TryParse(StringView, T &)`; null if the type has none.
    bool (*Parse)(StringView text, void *value);
    /// Allocates a default-constructed heap object, released with `Delete`; null if not constructible.
    void *(*New)();
    /// Destroys a heap object; null if the type cannot be deleted.
    void (*Delete)(void *object);
    /// Returns the dynamic type of a class object.
    const TypeInfo &(*GetDynamicType)(const void *object);
    /// Whether copying and moving are a byte copy.
    bool TriviallyCopyable = false;
    /// Whether `Destruct` does nothing.
    bool TriviallyDestructible = false;
};

/// The operations of a `List<T>`.
struct ListOperations
{
    /// Returns the number of elements.
    usize (*Count)(const void *list);
    /// Returns a pointer to the element at `index`.
    void *(*At)(void *list, usize index);
    /// Resizes the list to `count` elements.
    void (*Resize)(void *list, usize count);
    /// Removes the element at `index`.
    void (*RemoveAt)(void *list, usize index);
};

/// The operations of a `HashMap<TKey, TValue>`.
struct MapOperations
{
    /// Returns the number of entries.
    usize (*Count)(const void *map);
    /// Removes every entry.
    void (*Clear)(void *map);
    /// Returns the value of `key`, default-constructing it if it is missing.
    void *(*FindOrAdd)(void *map, const void *key);
    /// Returns the value of `key`, or null.
    void *(*Find)(void *map, const void *key);
    /// Removes `key` and returns whether it was present.
    bool (*Remove)(void *map, const void *key);
    /// Calls `visitor` for every entry, passing `context` through.
    void (*ForEach)(const void *map, void (*visitor)(const void *key, const void *value, void *context), void *context);
};

/// The operations of a `HashSet<T>`.
struct SetOperations
{
    /// Returns the number of elements.
    usize (*Count)(const void *set);
    /// Removes every element.
    void (*Clear)(void *set);
    /// Adds `key` and returns whether it was inserted.
    bool (*Add)(void *set, const void *key);
    /// Returns whether `key` is present.
    bool (*Contains)(const void *set, const void *key);
    /// Removes `key` and returns whether it was present.
    bool (*Remove)(void *set, const void *key);
    /// Calls `visitor` for every element, passing `context` through.
    void (*ForEach)(const void *set, void (*visitor)(const void *key, void *context), void *context);
};

/// The operations of an `Optional<T>`.
struct OptionalOperations
{
    /// Returns whether a value is present.
    bool (*HasValue)(const void *optional);
    /// Returns a pointer to the value, or null.
    void *(*Get)(void *optional);
    /// Default-constructs the value, replacing any.
    void *(*Emplace)(void *optional);
    /// Destroys the value, if any.
    void (*Reset)(void *optional);
};

/// The operations of a `UniquePtr<T>` or `SharedPtr<T>`.
struct PointerOperations
{
    /// Returns the owned object, or null.
    void *(*Get)(const void *pointer);
    /// Takes ownership of `object`, a heap object already adjusted to the element type, or null.
    void (*Reset)(void *pointer, void *object);
};

/// The operations of an object link, such as `AssetRef<T>`.
struct LinkOperations
{
    /// Writes the referenced `Guid` into `guid`.
    void (*GetGuid)(const void *link, Guid &guid);
    /// Points the link at `guid`; an invalid Guid clears it.
    void (*SetGuid)(void *link, const Guid &guid);
};

/// A reflected field of a type.
class CORE_API FieldInfo
{
public:
    /// Describes an instance field at `offset`.
    constexpr FieldInfo(StringView name, TypeGetter type, uint32 offset, Metadata metadata)
        : _name(name), _type(type), _offset(offset), _metadata(metadata)
    {
    }

    /// Describes a static field at `address`.
    constexpr FieldInfo(StringView name, TypeGetter type, void *address, Metadata metadata)
        : _name(name), _type(type), _address(address), _metadata(metadata)
    {
    }

    /// Describes a field of a type registered at run time, whose type is already known.
    constexpr FieldInfo(StringView name, const TypeInfo &type, uint32 offset, Metadata metadata)
        : _name(name), _type(nullptr), _resolvedType(&type), _offset(offset), _metadata(metadata)
    {
    }

    /// Returns the field's name.
    [[nodiscard]] constexpr StringView GetName() const
    {
        return _name;
    }

    /// Returns the field's type.
    [[nodiscard]] const TypeInfo &GetType() const
    {
        return _resolvedType != nullptr ? *_resolvedType : _type();
    }

    /// Returns the type that declares the field.
    [[nodiscard]] const TypeInfo &GetOwner() const
    {
        return *_owner;
    }

    /// Returns the field's offset in its owner, or zero for a static field.
    [[nodiscard]] constexpr uint32 GetOffset() const
    {
        return _offset;
    }

    /// Returns whether the field is static.
    [[nodiscard]] constexpr bool IsStatic() const
    {
        return _address != nullptr;
    }

    /// Returns the field's metadata.
    [[nodiscard]] constexpr const Metadata &GetMetadata() const
    {
        return _metadata;
    }

    /// Returns the field in `object`, of the owner's type or one derived from it.
    ///
    /// `object` is ignored for a static field.
    [[nodiscard]] void *GetValue(void *object) const
    {
        return _address != nullptr ? _address : static_cast<uint8 *>(object) + _offset;
    }

    /// Returns the field in `object` as const; `object` is ignored for a static field.
    [[nodiscard]] const void *GetValue(const void *object) const
    {
        return _address != nullptr ? _address : static_cast<const uint8 *>(object) + _offset;
    }

private:
    friend class Private::TypeRegistry;

    StringView _name;
    TypeGetter _type;
    const TypeInfo *_resolvedType = nullptr;
    const TypeInfo *_owner = nullptr;
    void *_address = nullptr;
    uint32 _offset = 0;
    Metadata _metadata;
};

/// How a function parameter is passed.
enum class ParameterFlags : uint8
{
    /// Passed by value.
    None = 0,
    /// Passed by reference; a non-const reference is an output.
    Reference = 1 << 0,
    /// Passed as const.
    Const = 1 << 1,
};
MARLIN_ENUM_FLAGS(ParameterFlags)

/// A function parameter as reflection sees it.
struct ParameterInfo
{
    /// The parameter's name.
    StringView Name;
    /// The parameter's type.
    TypeGetter Type = nullptr;
    /// How the parameter is passed.
    ParameterFlags Flags = ParameterFlags::None;
    /// The default argument as written, or empty.
    StringView DefaultValue;

    /// Returns whether the parameter is a non-const reference, so it is an output.
    [[nodiscard]] constexpr bool IsOutput() const
    {
        return Flags == ParameterFlags::Reference;
    }
};

/// Calls a reflected function.
///
/// `arguments` point to a value of each parameter's type and `result` to uninitialized storage for the return type,
/// ignored for void; `object` is ignored for a static function.
using FunctionInvoker = void (*)(void *object, void *const *arguments, void *result);

/// A reflected function of a type.
class CORE_API FunctionInfo
{
public:
    /// Describes a function with `name`, `returnType` (null for void) and `parameters`.
    constexpr FunctionInfo(StringView name, TypeGetter returnType, Span<const ParameterInfo> parameters,
                           FunctionInvoker invoker, bool isStatic, bool isConst, Metadata metadata)
        : _name(name), _returnType(returnType), _parameters(parameters), _invoker(invoker), _metadata(metadata),
          _isStatic(isStatic), _isConst(isConst)
    {
    }

    /// Returns the function's name.
    [[nodiscard]] constexpr StringView GetName() const
    {
        return _name;
    }

    /// Returns the return type, or null for void.
    [[nodiscard]] const TypeInfo *GetReturnType() const
    {
        return _returnType != nullptr ? &_returnType() : nullptr;
    }

    /// Returns the parameters.
    [[nodiscard]] constexpr Span<const ParameterInfo> GetParameters() const
    {
        return _parameters;
    }

    /// Returns the type that declares the function.
    [[nodiscard]] const TypeInfo &GetOwner() const
    {
        return *_owner;
    }

    /// Returns whether the function is static.
    [[nodiscard]] constexpr bool IsStatic() const
    {
        return _isStatic;
    }

    /// Returns whether the function is const.
    [[nodiscard]] constexpr bool IsConst() const
    {
        return _isConst;
    }

    /// Returns the function's metadata.
    [[nodiscard]] constexpr const Metadata &GetMetadata() const
    {
        return _metadata;
    }

    /// Calls the function.
    ///
    /// `arguments` point to a value of each parameter's type and `result` to uninitialized storage for the return
    /// type, ignored for void; `object` is ignored for a static function.
    void Invoke(void *object, void *const *arguments, void *result) const
    {
        _invoker(object, arguments, result);
    }

private:
    friend class Private::TypeRegistry;

    StringView _name;
    TypeGetter _returnType;
    Span<const ParameterInfo> _parameters;
    FunctionInvoker _invoker;
    const TypeInfo *_owner = nullptr;
    Metadata _metadata;
    bool _isStatic;
    bool _isConst;
};

/// One value of a reflected enum.
struct EnumeratorInfo
{
    /// The enumerator's name.
    StringView Name;
    /// The enumerator's value, widened to 64 bits according to the underlying type's signedness.
    int64 Value = 0;
    /// The enumerator's metadata.
    Metadata Meta;
};

/// One direct base of a class and how to reach it if it is an interface.
struct InterfaceEntry
{
    /// The interface's type, or null for a base that is not a reflected interface.
    TypeGetter Interface;
    /// Adjusts a class object to the base; null when `Interface` is null.
    void *(*Cast)(void *object);
};

// NOLINTBEGIN(readability-redundant-member-init)
/// What generated code and container specializations describe a type with.
///
/// Every member has an initializer, so designated initializers may leave any of them out.
struct TypeDescription
{
    /// The name types are found and serialized by, unique among registered types.
    StringView Name = {};
    /// The C++ name with namespaces.
    StringView FullName = {};
    /// The module that declares the type.
    StringView Module = {};
    /// What the type is.
    TypeKind Kind = TypeKind::Opaque;
    /// The size in bytes.
    uint32 Size = 0;
    /// The alignment in bytes.
    uint32 Alignment = 0;
    /// The schema version, from `MCLASS(Version=N)`.
    uint32 Version = 1;
    /// Whether the type is abstract.
    bool Abstract = false;
    /// Whether an integer type is signed.
    bool Signed = false;
    /// The generic operations.
    const TypeOperations *Operations = nullptr;
    /// The reflected parent, or null.
    TypeGetter Parent = nullptr;
    /// Adjusts an object to its parent; reflected parents must sit at offset zero.
    void *(*ToParent)(void *object) = nullptr;
    /// One entry per direct base; a base that is not a reflected interface contributes a null entry.
    Span<const InterfaceEntry> Interfaces = {};
    /// The fields the type declares.
    Span<FieldInfo> Fields = {};
    /// The functions the type declares.
    Span<FunctionInfo> Functions = {};
    /// The values of an enum.
    Span<const EnumeratorInfo> Enumerators = {};
    /// The type's metadata.
    Metadata Meta = {};
    /// The element of a list, array, set, optional or pointer, or the value type of a map.
    TypeGetter Element = nullptr;
    /// The key type of a map or set, or null.
    TypeGetter Key = nullptr;
    /// The number of elements of a fixed array.
    uint32 FixedCount = 0;
    /// The `ListOperations`, `MapOperations` and so on for the kind.
    const void *ContainerOperations = nullptr;
    /// The type's extension data, or null.
    const void *Extension = nullptr;
};
// NOLINTEND(readability-redundant-member-init)

/// Describes a reflected type.
///
/// Reflected types are constant-initialized by generated code and registered, in dependency order, by
/// `Reflection::RegisterModules`; container types are interned on first use.
class CORE_API TypeInfo
{
public:
    /// Describes a type from `description`.
    ///
    /// The data the description points to must outlive the type.
    constexpr explicit TypeInfo(const TypeDescription &description)
        : _description(description), _id(description.Name.GetHash())
    {
    }

    /// Destroys the descriptor.
    constexpr ~TypeInfo() = default;
    MARLIN_NONCOPYABLE(TypeInfo);

    /// Returns the name types are found and serialized by, unique among registered types.
    [[nodiscard]] constexpr StringView GetName() const
    {
        return _description.Name;
    }

    /// Returns the C++ name with namespaces.
    [[nodiscard]] constexpr StringView GetFullName() const
    {
        return _description.FullName;
    }

    /// Returns the module that declares the type.
    [[nodiscard]] constexpr StringView GetModule() const
    {
        return _description.Module;
    }

    /// Returns the hash of the name.
    [[nodiscard]] constexpr uint64 GetId() const
    {
        return _id;
    }

    /// Returns the kind.
    [[nodiscard]] constexpr TypeKind GetKind() const
    {
        return _description.Kind;
    }

    /// Returns the size in bytes.
    [[nodiscard]] constexpr uint32 GetSize() const
    {
        return _description.Size;
    }

    /// Returns the alignment in bytes.
    [[nodiscard]] constexpr uint32 GetAlignment() const
    {
        return _description.Alignment;
    }

    /// Returns the schema version.
    [[nodiscard]] constexpr uint32 GetVersion() const
    {
        return _description.Version;
    }

    /// Returns whether the type is abstract.
    [[nodiscard]] constexpr bool IsAbstract() const
    {
        return _description.Abstract;
    }

    /// Returns whether an integer type is signed.
    [[nodiscard]] constexpr bool IsSigned() const
    {
        return _description.Signed;
    }

    /// Returns whether the type is registered.
    [[nodiscard]] constexpr bool IsRegistered() const
    {
        return _registered;
    }

    /// Returns whether the type is a builtin primitive.
    [[nodiscard]] constexpr bool IsPrimitive() const
    {
        return _description.Kind <= TypeKind::Float64;
    }

    /// Returns whether the type is an integer.
    [[nodiscard]] constexpr bool IsInteger() const
    {
        return _description.Kind >= TypeKind::Int8 && _description.Kind <= TypeKind::UInt64;
    }

    /// Returns whether the type is a floating-point type.
    [[nodiscard]] constexpr bool IsFloat() const
    {
        return _description.Kind == TypeKind::Float32 || _description.Kind == TypeKind::Float64;
    }

    /// Returns whether the type is a struct, class or interface.
    [[nodiscard]] constexpr bool HasFields() const
    {
        return _description.Kind >= TypeKind::Struct && _description.Kind <= TypeKind::Interface;
    }

    /// Returns whether the enum is marked `MENUM(Flags)`, so its values combine with `|`.
    [[nodiscard]] bool IsFlags() const
    {
        return _description.Meta.Has("Flags");
    }

    /// Returns the type's metadata.
    [[nodiscard]] constexpr const Metadata &GetMetadata() const
    {
        return _description.Meta;
    }

    /// Returns the generic operations; individual entries may be null.
    [[nodiscard]] constexpr const TypeOperations &GetOperations() const
    {
        return *_description.Operations;
    }

    /// Returns the reflected parent, or null.
    [[nodiscard]] const TypeInfo *GetParent() const
    {
        return _description.Parent != nullptr ? &_description.Parent() : nullptr;
    }

    /// Returns the fields this type declares.
    [[nodiscard]] constexpr Span<const FieldInfo> GetOwnFields() const
    {
        return _description.Fields;
    }

    /// Returns the instance fields of this type and its parents, parents first.
    ///
    /// Built at registration.
    [[nodiscard]] Span<const FieldInfo *const> GetFields() const
    {
        return _fields;
    }

    /// Returns the instance or static field `name` of this type or a parent, or null.
    [[nodiscard]] const FieldInfo *FindField(StringView name) const;

    /// Returns the functions this type declares.
    [[nodiscard]] constexpr Span<const FunctionInfo> GetOwnFunctions() const
    {
        return _description.Functions;
    }

    /// Returns the function `name` of this type or a parent, or null.
    [[nodiscard]] const FunctionInfo *FindFunction(StringView name) const;

    /// Returns the values of the enum.
    [[nodiscard]] constexpr Span<const EnumeratorInfo> GetEnumerators() const
    {
        return _description.Enumerators;
    }

    /// Returns the enumerator named `name`, or null.
    [[nodiscard]] const EnumeratorInfo *FindEnumerator(StringView name) const;
    /// Returns the enumerator with `value`, or null.
    [[nodiscard]] const EnumeratorInfo *FindEnumerator(int64 value) const;

    /// Returns one entry per direct base; entries for non-interface bases have a null `Interface`.
    [[nodiscard]] constexpr Span<const InterfaceEntry> GetOwnInterfaces() const
    {
        return _description.Interfaces;
    }

    /// Returns the element type of a container or pointer, or null.
    [[nodiscard]] const TypeInfo *GetElementType() const
    {
        return _description.Element != nullptr ? &_description.Element() : nullptr;
    }

    /// Returns the key type of a map or set, or null.
    [[nodiscard]] const TypeInfo *GetKeyType() const
    {
        return _description.Key != nullptr ? &_description.Key() : nullptr;
    }

    /// Returns the number of elements of a fixed array.
    [[nodiscard]] constexpr uint32 GetFixedCount() const
    {
        return _description.FixedCount;
    }

    /// Returns the list operations; the type must be a list.
    [[nodiscard]] const ListOperations &GetListOperations() const;
    /// Returns the map operations; the type must be a map.
    [[nodiscard]] const MapOperations &GetMapOperations() const;
    /// Returns the set operations; the type must be a set.
    [[nodiscard]] const SetOperations &GetSetOperations() const;
    /// Returns the optional operations; the type must be an optional.
    [[nodiscard]] const OptionalOperations &GetOptionalOperations() const;
    /// Returns the pointer operations; the type must be a pointer.
    [[nodiscard]] const PointerOperations &GetPointerOperations() const;
    /// Returns the link operations; the type must be an object link.
    [[nodiscard]] const LinkOperations &GetLinkOperations() const;

    /// Returns the type's extension data, or null.
    [[nodiscard]] constexpr const void *GetExtension() const
    {
        return _description.Extension;
    }

    /// Returns whether this is `type`, derives from it, or implements it.
    [[nodiscard]] bool IsA(const TypeInfo &type) const;

    /// Adjusts `object`, of this type, to its base or interface `target`; null if it is neither.
    [[nodiscard]] void *CastTo(void *object, const TypeInfo &target) const;

    /// Returns a default-constructed heap object, or null for an abstract type or one without a default constructor.
    [[nodiscard]] void *New() const;

    /// Returns the dynamic type of `object`, which is of this class type or derived from it.
    ///
    /// Returns this type for other kinds.
    [[nodiscard]] const TypeInfo &GetDynamicType(const void *object) const;

private:
    friend class Private::TypeRegistry;

    TypeDescription _description;
    uint64 _id;
    Span<const FieldInfo *const> _fields;
    bool _registered = false;
};
} // namespace Marlin
