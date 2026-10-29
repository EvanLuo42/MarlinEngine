#include "Reflection/Reflection.h"

#include "Concurrency/ScopeLock.h"
#include "Console/Console.h"
#include "Containers/HashMap.h"
#include "Diagnostics/AssertFormat.h"
#include "HAL/PlatformLibrary.h"
#include "HAL/PlatformSync.h"
#include "Memory/Memory.h"
#include "Reflection/Generated.h"
#include "String/Ascii.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Templates/NumericLimits.h"

namespace Marlin
{
namespace Private
{
namespace
{
template <typename T> consteval TypeDescription DescribeBuiltin(StringView name, TypeKind kind)
{
    return {
        .Name = name,
        .FullName = name,
        .Module = "Core",
        .Kind = kind,
        .Size = sizeof(T),
        .Alignment = alignof(T),
        .Signed = IsSigned<T>,
        .Operations = &OperationsFor<T>::Value,
    };
}

constinit TypeInfo _boolType{DescribeBuiltin<bool>("bool", TypeKind::Bool)};
constinit TypeInfo _int8Type{DescribeBuiltin<int8>("int8", TypeKind::Int8)};
constinit TypeInfo _int16Type{DescribeBuiltin<int16>("int16", TypeKind::Int16)};
constinit TypeInfo _int32Type{DescribeBuiltin<int32>("int32", TypeKind::Int32)};
constinit TypeInfo _int64Type{DescribeBuiltin<int64>("int64", TypeKind::Int64)};
constinit TypeInfo _uInt8Type{DescribeBuiltin<uint8>("uint8", TypeKind::UInt8)};
constinit TypeInfo _uInt16Type{DescribeBuiltin<uint16>("uint16", TypeKind::UInt16)};
constinit TypeInfo _uInt32Type{DescribeBuiltin<uint32>("uint32", TypeKind::UInt32)};
constinit TypeInfo _uInt64Type{DescribeBuiltin<uint64>("uint64", TypeKind::UInt64)};
constinit TypeInfo _float32Type{DescribeBuiltin<float32>("float32", TypeKind::Float32)};
constinit TypeInfo _float64Type{DescribeBuiltin<float64>("float64", TypeKind::Float64)};
constinit TypeInfo _stringType{DescribeBuiltin<String>("String", TypeKind::String)};
constinit TypeInfo _stringViewType{DescribeBuiltin<StringView>("StringView", TypeKind::StringView)};
constinit TypeInfo _nameType{DescribeBuiltin<Name>("Name", TypeKind::Name)};

TypeInfo *const BuiltinTypes[] = {
    &_boolType,   &_int8Type,   &_int16Type,   &_int32Type,   &_int64Type,  &_uInt8Type,      &_uInt16Type,
    &_uInt32Type, &_uInt64Type, &_float32Type, &_float64Type, &_stringType, &_stringViewType, &_nameType,
};
} // namespace

const TypeInfo &GetBuiltinType(const bool * /*tag*/)
{
    return _boolType;
}

const TypeInfo &GetBuiltinType(const int8 * /*tag*/)
{
    return _int8Type;
}

const TypeInfo &GetBuiltinType(const int16 * /*tag*/)
{
    return _int16Type;
}

const TypeInfo &GetBuiltinType(const int32 * /*tag*/)
{
    return _int32Type;
}

const TypeInfo &GetBuiltinType(const int64 * /*tag*/)
{
    return _int64Type;
}

const TypeInfo &GetBuiltinType(const uint8 * /*tag*/)
{
    return _uInt8Type;
}

const TypeInfo &GetBuiltinType(const uint16 * /*tag*/)
{
    return _uInt16Type;
}

const TypeInfo &GetBuiltinType(const uint32 * /*tag*/)
{
    return _uInt32Type;
}

const TypeInfo &GetBuiltinType(const uint64 * /*tag*/)
{
    return _uInt64Type;
}

const TypeInfo &GetBuiltinType(const float32 * /*tag*/)
{
    return _float32Type;
}

const TypeInfo &GetBuiltinType(const float64 * /*tag*/)
{
    return _float64Type;
}

const TypeInfo &GetBuiltinType(const String * /*tag*/)
{
    return _stringType;
}

const TypeInfo &GetBuiltinType(const StringView * /*tag*/)
{
    return _stringViewType;
}

const TypeInfo &GetBuiltinType(const Name * /*tag*/)
{
    return _nameType;
}

class TypeRegistry
{
public:
    /// Built on first use and never destroyed, so types stay valid through static destruction.
    static TypeRegistry &Get()
    {
        alignas(TypeRegistry) static uint8 storage[sizeof(TypeRegistry)];
        static TypeRegistry *registry = ConstructAt(reinterpret_cast<TypeRegistry *>(storage));
        return *registry;
    }

    TypeRegistry()
    {
        for (TypeInfo *type : BuiltinTypes)
        {
            type->_registered = true;
            AddLocked(*type);
        }
    }

    const TypeInfo *Find(uint64 id)
    {
        const SharedScopeLock lock(_lock);
        TypeInfo *const *type = _byId.Find(id);
        return type != nullptr ? *type : nullptr;
    }

    void ForEach(FunctionRef<void(const TypeInfo &)> visitor)
    {
        List<const TypeInfo *> types;
        {
            const SharedScopeLock lock(_lock);
            types.AddRange(Span<const TypeInfo *const>(_types.Data(), _types.Count()));
        }
        for (const TypeInfo *type : types)
        {
            visitor(*type);
        }
    }

    bool IsModuleRegistered(StringView name)
    {
        const SharedScopeLock lock(_lock);
        return _modules.Contains(name);
    }

    void RegisterModule(StringView module, Span<TypeInfo *const> types)
    {
        {
            const ScopeLock lock(_lock);
            if (_modules.Contains(module))
            {
                return;
            }
            _modules.Add(module);
            for (TypeInfo *type : types)
            {
                AddLocked(*type);
            }
        }
        for (TypeInfo *type : types)
        {
            Link(*type);
        }
        for (const TypeInfo *type : types)
        {
            RegisterConsoleObjects(*type);
        }
    }

    bool RegisterDynamic(TypeInfo &type)
    {
        {
            const ScopeLock lock(_lock);
            if (_byId.Contains(type.GetId()))
            {
                return false;
            }
            _byId.Add(type.GetId(), &type);
            _types.Add(&type);
        }
        Link(type);
        return true;
    }

    const TypeInfo &Intern(const TypeDescription &description, uint64 key)
    {
        {
            const SharedScopeLock lock(_lock);
            if (TypeInfo *const *existing = _byKey.Find(key))
            {
                return **existing;
            }
        }
        StringBuilder<256> name;
        name.Append(description.Name);
        if (description.Kind != TypeKind::Opaque)
        {
            name.Append('<');
            if (description.Key != nullptr && description.Kind == TypeKind::Map)
            {
                name.Append(description.Key().GetName()).Append(", ");
            }
            name.Append(description.Element().GetName());
            if (description.Kind == TypeKind::FixedArray)
            {
                char digits[24];
                name.Append(", ").Append(StringView(digits, FormatUnsigned(description.FixedCount, digits)));
            }
            name.Append('>');
        }

        const ScopeLock lock(_lock);
        if (TypeInfo *const *existing = _byKey.Find(key))
        {
            return **existing;
        }
        auto *text = static_cast<char *>(Memory::Allocate(name.Length()));
        MemCopy(text, name.Data(), name.Length());
        TypeDescription interned = description;
        interned.Name = StringView(text, name.Length());
        interned.FullName = interned.Name;
        auto *type = new TypeInfo(interned);
        type->_registered = true;
        _byKey.Add(key, type);
        if (!_byId.Contains(type->GetId()))
        {
            _byId.Add(type->GetId(), type);
        }
        _types.Add(type);
        return *type;
    }

private:
    void AddLocked(TypeInfo &type)
    {
        if (TypeInfo *const *existing = _byId.Find(type.GetId()))
        {
            MARLIN_FATALF("Reflected type {0} of module {1} has the same name as {2} of module {3}", type.GetFullName(),
                          type.GetModule(), (*existing)->GetFullName(), (*existing)->GetModule());
        }
        _byId.Add(type.GetId(), &type);
        _types.Add(&type);
    }

    /// Resolves what needs the parent, which may be registered by the same module.
    void Link(TypeInfo &type)
    {
        if (type._registered)
        {
            return;
        }
        type._registered = true;
        for (FieldInfo &field : type._description.Fields)
        {
            field._owner = &type;
        }
        for (FunctionInfo &function : type._description.Functions)
        {
            function._owner = &type;
        }

        Span<const FieldInfo *const> inherited;
        if (type._description.Parent != nullptr)
        {
            TypeInfo *parent = nullptr;
            {
                const SharedScopeLock lock(_lock);
                TypeInfo *const *found = _byId.Find(type._description.Parent().GetId());
                parent = found != nullptr ? *found : nullptr;
            }
            if (parent == nullptr)
            {
                MARLIN_FATALF("The parent of {0} is not registered", type.GetFullName());
            }
            Link(*parent);
            inherited = parent->_fields;
            alignas(16) static uint8 probe[64];
            if (type._description.ToParent(probe) != static_cast<void *>(probe))
            {
                MARLIN_FATALF("{0} must list its reflected parent {1} as its first base", type.GetFullName(),
                              parent->GetFullName());
            }
        }

        usize count = inherited.Count();
        for (const FieldInfo &field : type._description.Fields)
        {
            count += field.IsStatic() ? 0 : 1;
        }
        if (count == 0)
        {
            return;
        }
        auto **fields = static_cast<const FieldInfo **>(Memory::Allocate(count * sizeof(const FieldInfo *)));
        usize index = 0;
        for (const FieldInfo *field : inherited)
        {
            fields[index++] = field;
        }
        for (const FieldInfo &field : type._description.Fields)
        {
            if (!field.IsStatic())
            {
                fields[index++] = &field;
            }
        }
        type._fields = Span<const FieldInfo *const>(fields, count);
    }

    ReadWriteLock _lock;
    HashMap<uint64, TypeInfo *> _byId;
    HashMap<uint64, TypeInfo *> _byKey;
    List<const TypeInfo *> _types;
    List<StringView> _modules;
};

const TypeInfo &InternType(const TypeDescription &description, uint64 key)
{
    return TypeRegistry::Get().Intern(description, key);
}

ModuleRegistrar::ModuleRegistrar(StringView module) : _module(module)
{
}

void ModuleRegistrar::Add(TypeInfo &type)
{
    _types.Add(&type);
}

void ModuleRegistrar::Commit()
{
    TypeRegistry::Get().RegisterModule(_module, _types.AsSpan());
    _types.Clear();
}
} // namespace Private

bool Metadata::Has(StringView key) const
{
    return Get(key).HasValue();
}

Optional<StringView> Metadata::Get(StringView key) const
{
    for (const MetadataEntry &entry : _entries)
    {
        if (entry.Key == key)
        {
            return entry.Value;
        }
    }
    return {};
}

StringView Metadata::GetOr(StringView key, StringView fallback) const
{
    return Get(key).ValueOr(fallback);
}

bool Metadata::HasFlag(StringView key, StringView flag) const
{
    Optional<StringView> value = Get(key);
    if (!value.HasValue())
    {
        return false;
    }
    for (const StringView part : value.Value().Split('|'))
    {
        if (part.Trim() == flag)
        {
            return true;
        }
    }
    return false;
}

const FieldInfo *TypeInfo::FindField(StringView name) const
{
    for (const TypeInfo *type = this; type != nullptr; type = type->GetParent())
    {
        for (const FieldInfo &field : type->_description.Fields)
        {
            if (field.GetName() == name)
            {
                return &field;
            }
        }
    }
    return nullptr;
}

const FunctionInfo *TypeInfo::FindFunction(StringView name) const
{
    for (const TypeInfo *type = this; type != nullptr; type = type->GetParent())
    {
        for (const FunctionInfo &function : type->_description.Functions)
        {
            if (function.GetName() == name)
            {
                return &function;
            }
        }
    }
    return nullptr;
}

const EnumeratorInfo *TypeInfo::FindEnumerator(StringView name) const
{
    for (const EnumeratorInfo &enumerator : _description.Enumerators)
    {
        if (enumerator.Name == name)
        {
            return &enumerator;
        }
    }
    return nullptr;
}

const EnumeratorInfo *TypeInfo::FindEnumerator(int64 value) const
{
    for (const EnumeratorInfo &enumerator : _description.Enumerators)
    {
        if (enumerator.Value == value)
        {
            return &enumerator;
        }
    }
    return nullptr;
}

const ListOperations &TypeInfo::GetListOperations() const
{
    MARLIN_ASSERT(_description.Kind == TypeKind::List);
    return *static_cast<const ListOperations *>(_description.ContainerOperations);
}

const MapOperations &TypeInfo::GetMapOperations() const
{
    MARLIN_ASSERT(_description.Kind == TypeKind::Map);
    return *static_cast<const MapOperations *>(_description.ContainerOperations);
}

const SetOperations &TypeInfo::GetSetOperations() const
{
    MARLIN_ASSERT(_description.Kind == TypeKind::Set);
    return *static_cast<const SetOperations *>(_description.ContainerOperations);
}

const OptionalOperations &TypeInfo::GetOptionalOperations() const
{
    MARLIN_ASSERT(_description.Kind == TypeKind::Optional);
    return *static_cast<const OptionalOperations *>(_description.ContainerOperations);
}

const PointerOperations &TypeInfo::GetPointerOperations() const
{
    MARLIN_ASSERT(_description.Kind == TypeKind::UniquePtr || _description.Kind == TypeKind::SharedPtr);
    return *static_cast<const PointerOperations *>(_description.ContainerOperations);
}

const LinkOperations &TypeInfo::GetLinkOperations() const
{
    MARLIN_ASSERT(_description.Kind == TypeKind::ObjectLink);
    return *static_cast<const LinkOperations *>(_description.ContainerOperations);
}

bool TypeInfo::IsA(const TypeInfo &type) const
{
    for (const TypeInfo *current = this; current != nullptr; current = current->GetParent())
    {
        if (current == &type)
        {
            return true;
        }
        for (const InterfaceEntry &entry : current->_description.Interfaces)
        {
            if (entry.Interface != nullptr && entry.Interface().IsA(type))
            {
                return true;
            }
        }
    }
    return false;
}

void *TypeInfo::CastTo(void *object, const TypeInfo &target) const
{
    for (const TypeInfo *current = this; current != nullptr; current = current->GetParent())
    {
        if (current == &target)
        {
            return object;
        }
    }
    for (const TypeInfo *current = this; current != nullptr; current = current->GetParent())
    {
        for (const InterfaceEntry &entry : current->_description.Interfaces)
        {
            if (entry.Interface == nullptr)
            {
                continue;
            }
            const TypeInfo &interface = entry.Interface();
            if (interface.IsA(target))
            {
                return interface.CastTo(entry.Cast(object), target);
            }
        }
    }
    return nullptr;
}

void *TypeInfo::New() const
{
    return _description.Operations->New != nullptr ? _description.Operations->New() : nullptr;
}

const TypeInfo &TypeInfo::GetDynamicType(const void *object) const
{
    if (_description.Kind == TypeKind::Class && _description.Operations->GetDynamicType != nullptr)
    {
        return _description.Operations->GetDynamicType(object);
    }
    return *this;
}

ReflectedValue::ReflectedValue(const TypeInfo &type) : _type(type)
{
    const TypeOperations &operations = type.GetOperations();
    if (operations.Construct == nullptr || operations.Destruct == nullptr)
    {
        return;
    }
    _value = Memory::Allocate(type.GetSize() != 0 ? type.GetSize() : 1, Max<usize>(type.GetAlignment(), 1));
    operations.Construct(_value);
}

ReflectedValue::~ReflectedValue()
{
    if (_value != nullptr)
    {
        _type.GetOperations().Destruct(_value);
        Memory::Free(_value);
    }
}

namespace Reflection
{
namespace
{
template <typename T> int64 ReadInteger(const void *value)
{
    return static_cast<int64>(*static_cast<const T *>(value));
}

template <typename T> void WriteInteger(void *value, int64 number)
{
    *static_cast<T *>(value) = static_cast<T>(number);
}

bool ParseInteger(StringView text, uint64 &magnitude, bool &negative)
{
    text = text.Trim();
    negative = text.StartsWith('-');
    if (negative || text.StartsWith('+'))
    {
        text = text.RemoveStart(1);
    }
    if (text.StartsWith("0x") || text.StartsWith("0X"))
    {
        return TryParse(text.RemoveStart(2), magnitude, 16);
    }
    return TryParse(text, magnitude);
}

template <typename T> bool ParseIntegerAs(StringView text, void *value)
{
    uint64 magnitude = 0;
    bool negative = false;
    if (!ParseInteger(text, magnitude, negative))
    {
        return false;
    }
    if constexpr (IsSigned<T>)
    {
        const uint64 limit = static_cast<uint64>(NumericLimits<T>::Max()) + (negative ? 1 : 0);
        if (magnitude > limit)
        {
            return false;
        }
        *static_cast<T *>(value) = negative ? static_cast<T>(0 - magnitude) : static_cast<T>(magnitude);
    }
    else
    {
        if (negative || magnitude > NumericLimits<T>::Max())
        {
            return false;
        }
        *static_cast<T *>(value) = static_cast<T>(magnitude);
    }
    return true;
}

void FormatEnum(StringWriter &output, const TypeInfo &type, const void *value)
{
    const int64 number = GetEnumValue(type, value);
    if (const EnumeratorInfo *enumerator = type.FindEnumerator(number))
    {
        output.Append(enumerator->Name);
        return;
    }
    if (type.IsFlags() && number != 0)
    {
        auto remaining = static_cast<uint64>(number);
        bool first = true;
        for (const EnumeratorInfo &enumerator : type.GetEnumerators())
        {
            const auto bits = static_cast<uint64>(enumerator.Value);
            if (bits != 0 && (remaining & bits) == bits)
            {
                output.Append(first ? "" : "|").Append(enumerator.Name);
                remaining &= ~bits;
                first = false;
            }
        }
        if (remaining == 0)
        {
            return;
        }
        output.Truncate(0);
    }
    char digits[24];
    output.Append(StringView(digits, FormatSigned(number, digits)));
}

bool ParseEnum(StringView text, const TypeInfo &type, void *value)
{
    uint64 bits = 0;
    for (const StringView part : text.Split('|'))
    {
        if (const EnumeratorInfo *enumerator = type.FindEnumerator(part.Trim()))
        {
            bits |= static_cast<uint64>(enumerator->Value);
            continue;
        }
        int64 parsed = 0;
        if (!ParseIntegerAs<int64>(part.Trim(), &parsed))
        {
            return false;
        }
        bits |= static_cast<uint64>(parsed);
    }
    SetEnumValue(type, value, static_cast<int64>(bits));
    return true;
}

/// Containers compare and copy element by element, so that pointers inside them are followed.
bool IsContainer(const TypeInfo &type)
{
    return type.GetKind() >= TypeKind::List && type.GetKind() <= TypeKind::SharedPtr;
}

struct FormatContext
{
    StringWriter *Output;
    const TypeInfo *Key;
    const TypeInfo *Value;
    bool First;
};
} // namespace

void RegisterModules(Span<const ModuleTableEntry> table)
{
    for (const ModuleTableEntry &entry : table)
    {
        if (IsModuleRegistered(entry.Name))
        {
            continue;
        }
        void (*registerModule)() = entry.Register;
        if (registerModule == nullptr)
        {
            const LibraryHandle library = PlatformLibrary::Load(entry.Library);
            if (!library.IsValid())
            {
                MARLIN_FATALF("Cannot load {0} for module {1}", entry.Library, entry.Name);
            }
            StringBuilder<128> symbol("Register_");
            symbol.Append(entry.Name);
            registerModule = reinterpret_cast<void (*)()>(PlatformLibrary::FindSymbol(library, symbol.CStr()));
            if (registerModule == nullptr)
            {
                MARLIN_FATALF("{0} does not export {1}", entry.Library, symbol.AsView());
            }
        }
        registerModule();
    }
}

bool IsModuleRegistered(StringView name)
{
    return Private::TypeRegistry::Get().IsModuleRegistered(name);
}

bool RegisterDynamicType(TypeInfo &type)
{
    return Private::TypeRegistry::Get().RegisterDynamic(type);
}

const TypeInfo *FindType(StringView name)
{
    const TypeInfo *type = FindType(name.GetHash());
    return type != nullptr && type->GetName() == name ? type : nullptr;
}

const TypeInfo *FindType(uint64 id)
{
    return Private::TypeRegistry::Get().Find(id);
}

void ForEachType(FunctionRef<void(const TypeInfo &)> visitor)
{
    Private::TypeRegistry::Get().ForEach(visitor);
}

void GetDerivedTypes(const TypeInfo &base, List<const TypeInfo *> &types)
{
    ForEachType([&](const TypeInfo &type) {
        if (&type != &base && type.HasFields() && type.IsA(base))
        {
            types.Add(&type);
        }
    });
}

int64 GetEnumValue(const TypeInfo &type, const void *value)
{
    switch (type.GetSize())
    {
    case 1:
        return type.IsSigned() ? ReadInteger<int8>(value) : ReadInteger<uint8>(value);
    case 2:
        return type.IsSigned() ? ReadInteger<int16>(value) : ReadInteger<uint16>(value);
    case 4:
        return type.IsSigned() ? ReadInteger<int32>(value) : ReadInteger<uint32>(value);
    default:
        return ReadInteger<int64>(value);
    }
}

void SetEnumValue(const TypeInfo &type, void *value, int64 number)
{
    switch (type.GetSize())
    {
    case 1:
        WriteInteger<uint8>(value, number);
        break;
    case 2:
        WriteInteger<uint16>(value, number);
        break;
    case 4:
        WriteInteger<uint32>(value, number);
        break;
    default:
        WriteInteger<int64>(value, number);
        break;
    }
}

bool Equals(const TypeInfo &type, const void *a, const void *b)
{
    const TypeOperations &operations = type.GetOperations();
    if (operations.Equals != nullptr && !IsContainer(type))
    {
        return operations.Equals(a, b);
    }
    switch (type.GetKind())
    {
    case TypeKind::Struct:
    case TypeKind::Class:
        for (const FieldInfo *field : type.GetFields())
        {
            if (!Equals(field->GetType(), field->GetValue(a), field->GetValue(b)))
            {
                return false;
            }
        }
        return true;
    case TypeKind::List: {
        const ListOperations &list = type.GetListOperations();
        const usize count = list.Count(a);
        if (count != list.Count(b))
        {
            return false;
        }
        const TypeInfo &element = *type.GetElementType();
        for (usize i = 0; i < count; ++i)
        {
            if (!Equals(element, list.At(Private::RemoveConst(a), i), list.At(Private::RemoveConst(b), i)))
            {
                return false;
            }
        }
        return true;
    }
    case TypeKind::FixedArray: {
        const TypeInfo &element = *type.GetElementType();
        for (uint32 i = 0; i < type.GetFixedCount(); ++i)
        {
            const usize offset = static_cast<usize>(i) * element.GetSize();
            if (!Equals(element, static_cast<const uint8 *>(a) + offset, static_cast<const uint8 *>(b) + offset))
            {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Map: {
        const MapOperations &map = type.GetMapOperations();
        if (map.Count(a) != map.Count(b))
        {
            return false;
        }
        struct Context
        {
            const MapOperations *Map;
            const TypeInfo *Value;
            void *Other;
            bool Equal;
        } context{&map, type.GetElementType(), Private::RemoveConst(b), true};
        map.ForEach(
            a,
            [](const void *key, const void *value, void *state) {
                auto &context = *static_cast<Context *>(state);
                const void *other = context.Equal ? context.Map->Find(context.Other, key) : nullptr;
                context.Equal = other != nullptr && Equals(*context.Value, value, other);
            },
            &context);
        return context.Equal;
    }
    case TypeKind::Set: {
        const SetOperations &set = type.GetSetOperations();
        if (set.Count(a) != set.Count(b))
        {
            return false;
        }
        struct Context
        {
            const SetOperations *Set;
            const void *Other;
            bool Equal;
        } context{&set, b, true};
        set.ForEach(
            a,
            [](const void *key, void *state) {
                auto &context = *static_cast<Context *>(state);
                context.Equal = context.Equal && context.Set->Contains(context.Other, key);
            },
            &context);
        return context.Equal;
    }
    case TypeKind::Optional: {
        const OptionalOperations &optional = type.GetOptionalOperations();
        const bool hasA = optional.HasValue(a);
        if (hasA != optional.HasValue(b))
        {
            return false;
        }
        return !hasA || Equals(*type.GetElementType(), optional.Get(Private::RemoveConst(a)),
                               optional.Get(Private::RemoveConst(b)));
    }
    case TypeKind::UniquePtr:
    case TypeKind::SharedPtr: {
        const PointerOperations &pointer = type.GetPointerOperations();
        const void *objectA = pointer.Get(a);
        const void *objectB = pointer.Get(b);
        if (objectA == objectB)
        {
            return true;
        }
        if (objectA == nullptr || objectB == nullptr)
        {
            return false;
        }
        const TypeInfo &element = *type.GetElementType();
        const TypeInfo &dynamic = element.GetDynamicType(objectA);
        return &dynamic == &element.GetDynamicType(objectB) && Equals(dynamic, objectA, objectB);
    }
    default:
        return false;
    }
}

bool Copy(const TypeInfo &type, void *target, const void *source)
{
    const TypeOperations &operations = type.GetOperations();
    if (operations.CopyAssign != nullptr && !IsContainer(type))
    {
        operations.CopyAssign(target, source);
        return true;
    }
    switch (type.GetKind())
    {
    case TypeKind::Struct:
    case TypeKind::Class: {
        bool copied = true;
        for (const FieldInfo *field : type.GetFields())
        {
            copied &= Copy(field->GetType(), field->GetValue(target), field->GetValue(source));
        }
        return copied;
    }
    case TypeKind::List: {
        const ListOperations &list = type.GetListOperations();
        const usize count = list.Count(source);
        list.Resize(target, count);
        const TypeInfo &element = *type.GetElementType();
        bool copied = true;
        for (usize i = 0; i < count; ++i)
        {
            copied &= Copy(element, list.At(target, i), list.At(Private::RemoveConst(source), i));
        }
        return copied;
    }
    case TypeKind::FixedArray: {
        const TypeInfo &element = *type.GetElementType();
        bool copied = true;
        for (uint32 i = 0; i < type.GetFixedCount(); ++i)
        {
            const usize offset = static_cast<usize>(i) * element.GetSize();
            copied &= Copy(element, static_cast<uint8 *>(target) + offset, static_cast<const uint8 *>(source) + offset);
        }
        return copied;
    }
    case TypeKind::Map: {
        const MapOperations &map = type.GetMapOperations();
        map.Clear(target);
        struct Context
        {
            const MapOperations *Map;
            const TypeInfo *Value;
            void *Target;
            bool Copied;
        } context{&map, type.GetElementType(), target, true};
        map.ForEach(
            source,
            [](const void *key, const void *value, void *state) {
                auto &context = *static_cast<Context *>(state);
                context.Copied &= Copy(*context.Value, context.Map->FindOrAdd(context.Target, key), value);
            },
            &context);
        return context.Copied;
    }
    case TypeKind::Set: {
        const SetOperations &set = type.GetSetOperations();
        set.Clear(target);
        struct Context
        {
            const SetOperations *Set;
            void *Target;
        } context{&set, target};
        set.ForEach(
            source,
            [](const void *key, void *state) {
                const auto &context = *static_cast<const Context *>(state);
                (void)context.Set->Add(context.Target, key);
            },
            &context);
        return true;
    }
    case TypeKind::Optional: {
        const OptionalOperations &optional = type.GetOptionalOperations();
        if (!optional.HasValue(source))
        {
            optional.Reset(target);
            return true;
        }
        return Copy(*type.GetElementType(), optional.Emplace(target), optional.Get(Private::RemoveConst(source)));
    }
    case TypeKind::UniquePtr:
    case TypeKind::SharedPtr: {
        const PointerOperations &pointer = type.GetPointerOperations();
        const void *object = pointer.Get(source);
        if (object == nullptr)
        {
            pointer.Reset(target, nullptr);
            return true;
        }
        const TypeInfo &element = *type.GetElementType();
        const TypeInfo &dynamic = element.GetDynamicType(object);
        void *copy = dynamic.New();
        if (copy == nullptr)
        {
            return false;
        }
        const bool copied = Copy(dynamic, copy, object);
        pointer.Reset(target, dynamic.CastTo(copy, element));
        return copied;
    }
    default:
        return false;
    }
}

void FormatValue(StringWriter &output, const TypeInfo &type, const void *value)
{
    char digits[24];
    switch (type.GetKind())
    {
    case TypeKind::Bool:
        output.Append(*static_cast<const bool *>(value) ? "true" : "false");
        return;
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
        output.Append(StringView(digits, FormatSigned(GetEnumValue(type, value), digits)));
        return;
    case TypeKind::UInt8:
        output.Append(StringView(digits, FormatUnsigned(*static_cast<const uint8 *>(value), digits)));
        return;
    case TypeKind::UInt16:
        output.Append(StringView(digits, FormatUnsigned(*static_cast<const uint16 *>(value), digits)));
        return;
    case TypeKind::UInt32:
        output.Append(StringView(digits, FormatUnsigned(*static_cast<const uint32 *>(value), digits)));
        return;
    case TypeKind::UInt64:
        output.Append(StringView(digits, FormatUnsigned(*static_cast<const uint64 *>(value), digits)));
        return;
    case TypeKind::Float32:
        AppendFloat(output, *static_cast<const float32 *>(value));
        return;
    case TypeKind::Float64:
        AppendFloat(output, *static_cast<const float64 *>(value));
        return;
    case TypeKind::String:
        output.Append(static_cast<const String *>(value)->AsView());
        return;
    case TypeKind::StringView:
        output.Append(*static_cast<const StringView *>(value));
        return;
    case TypeKind::Name:
        output.Append(static_cast<const Name *>(value)->ToView());
        return;
    case TypeKind::Enum: {
        StringBuilder<64> text;
        FormatEnum(text, type, value);
        output.Append(text.AsView());
        return;
    }
    default:
        break;
    }
    if (type.GetOperations().Format != nullptr)
    {
        type.GetOperations().Format(output, value);
        return;
    }
    switch (type.GetKind())
    {
    case TypeKind::Struct:
    case TypeKind::Class: {
        const TypeInfo &dynamic = type.GetDynamicType(value);
        output.Append('{');
        bool first = true;
        for (const FieldInfo *field : dynamic.GetFields())
        {
            output.Append(first ? "" : ", ").Append(field->GetName()).Append(" = ");
            FormatValue(output, field->GetType(), field->GetValue(value));
            first = false;
        }
        output.Append('}');
        return;
    }
    case TypeKind::List:
    case TypeKind::FixedArray: {
        const TypeInfo &element = *type.GetElementType();
        const bool isList = type.GetKind() == TypeKind::List;
        const usize count = isList ? type.GetListOperations().Count(value) : type.GetFixedCount();
        output.Append('[');
        for (usize i = 0; i < count; ++i)
        {
            const void *item = isList ? type.GetListOperations().At(Private::RemoveConst(value), i)
                                      : static_cast<const uint8 *>(value) + i * element.GetSize();
            output.Append(i == 0 ? "" : ", ");
            FormatValue(output, element, item);
        }
        output.Append(']');
        return;
    }
    case TypeKind::Map: {
        FormatContext context{&output, type.GetKeyType(), type.GetElementType(), true};
        output.Append('{');
        type.GetMapOperations().ForEach(
            value,
            [](const void *key, const void *item, void *state) {
                auto &context = *static_cast<FormatContext *>(state);
                context.Output->Append(context.First ? "" : ", ");
                FormatValue(*context.Output, *context.Key, key);
                context.Output->Append(" = ");
                FormatValue(*context.Output, *context.Value, item);
                context.First = false;
            },
            &context);
        output.Append('}');
        return;
    }
    case TypeKind::Set: {
        FormatContext context{&output, type.GetKeyType(), nullptr, true};
        output.Append('[');
        type.GetSetOperations().ForEach(
            value,
            [](const void *key, void *state) {
                auto &context = *static_cast<FormatContext *>(state);
                context.Output->Append(context.First ? "" : ", ");
                FormatValue(*context.Output, *context.Key, key);
                context.First = false;
            },
            &context);
        output.Append(']');
        return;
    }
    case TypeKind::Optional: {
        const OptionalOperations &optional = type.GetOptionalOperations();
        if (optional.HasValue(value))
        {
            FormatValue(output, *type.GetElementType(), optional.Get(Private::RemoveConst(value)));
        }
        else
        {
            output.Append("None");
        }
        return;
    }
    case TypeKind::UniquePtr:
    case TypeKind::SharedPtr: {
        const void *object = type.GetPointerOperations().Get(value);
        if (object == nullptr)
        {
            output.Append("null");
            return;
        }
        const TypeInfo &dynamic = type.GetElementType()->GetDynamicType(object);
        output.Append(dynamic.GetName());
        FormatValue(output, dynamic, object);
        return;
    }
    default:
        output.Append('<').Append(type.GetName()).Append('>');
        return;
    }
}

bool ParseValue(StringView text, const TypeInfo &type, void *value)
{
    switch (type.GetKind())
    {
    case TypeKind::Bool: {
        const StringView trimmed = text.Trim();
        if (trimmed.EqualsIgnoreCase("true") || trimmed == "1")
        {
            *static_cast<bool *>(value) = true;
            return true;
        }
        if (trimmed.EqualsIgnoreCase("false") || trimmed == "0")
        {
            *static_cast<bool *>(value) = false;
            return true;
        }
        return false;
    }
    case TypeKind::Int8:
        return ParseIntegerAs<int8>(text, value);
    case TypeKind::Int16:
        return ParseIntegerAs<int16>(text, value);
    case TypeKind::Int32:
        return ParseIntegerAs<int32>(text, value);
    case TypeKind::Int64:
        return ParseIntegerAs<int64>(text, value);
    case TypeKind::UInt8:
        return ParseIntegerAs<uint8>(text, value);
    case TypeKind::UInt16:
        return ParseIntegerAs<uint16>(text, value);
    case TypeKind::UInt32:
        return ParseIntegerAs<uint32>(text, value);
    case TypeKind::UInt64:
        return ParseIntegerAs<uint64>(text, value);
    case TypeKind::Float32:
        return TryParse(text.Trim(), *static_cast<float32 *>(value));
    case TypeKind::Float64:
        return TryParse(text.Trim(), *static_cast<float64 *>(value));
    case TypeKind::String:
        *static_cast<String *>(value) = text;
        return true;
    case TypeKind::Name:
        *static_cast<Name *>(value) = Name(text);
        return true;
    case TypeKind::Enum:
        return ParseEnum(text, type, value);
    default:
        return type.GetOperations().Parse != nullptr && type.GetOperations().Parse(text.Trim(), value);
    }
}
} // namespace Reflection
} // namespace Marlin
