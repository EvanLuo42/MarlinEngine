#include "MarlinText/MarlinTextSerializer.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformSync.h"
#include "MarlinText/BinaryFileLink.h"
#include "MarlinText/MarlinTextSchema.h"
#include "Math/MathUtility.h"
#include "Reflection/Reflection.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Templates/NumericLimits.h"

namespace Marlin
{
namespace
{
constexpr uint32 MaxParentDepth = 32;
constexpr usize MaxInlineArrayLength = 80;

struct FormatRegistry
{
    Mutex Lock;
    HashMap<uint64, MarlinTextFormat> Formats;

    /// Built on first use and never destroyed.
    static FormatRegistry &Get()
    {
        alignas(FormatRegistry) static uint8 storage[sizeof(FormatRegistry)];
        static FormatRegistry *registry = ConstructAt(reinterpret_cast<FormatRegistry *>(storage));
        return *registry;
    }

    bool Find(const TypeInfo &type, MarlinTextFormat &format)
    {
        const ScopeLock lock(Lock);
        const MarlinTextFormat *found = Formats.Find(type.GetId());
        if (found != nullptr)
        {
            format = *found;
        }
        return found != nullptr;
    }
};

void Indent(StringWriter &output, uint32 indent)
{
    output.Append(static_cast<usize>(indent) * 4, ' ');
}

bool IsTransient(const FieldInfo &field)
{
    return field.GetMetadata().Has("Transient");
}

bool IsTuple(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("Tuple");
}

/// A type written as a string through its FormatTo and TryParse.
bool IsTextual(const TypeInfo &type)
{
    const TypeOperations &operations = type.GetOperations();
    return operations.Format != nullptr && operations.Parse != nullptr &&
           (type.GetKind() == TypeKind::Opaque || type.GetFields().IsEmpty());
}

bool IsIdentifier(StringView text)
{
    if (text.IsEmpty() || (!Ascii::IsLetter(text[0]) && text[0] != '_'))
    {
        return false;
    }
    for (const char c : text)
    {
        if (!Ascii::IsLetterOrDigit(c) && c != '_')
        {
            return false;
        }
    }
    return text != "true" && text != "false" && text != "null";
}

void WriteString(StringWriter &output, StringView text)
{
    output.Append('"');
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            output.Append("\\\"");
            break;
        case '\\':
            output.Append("\\\\");
            break;
        case '\n':
            output.Append("\\n");
            break;
        case '\t':
            output.Append("\\t");
            break;
        case '\r':
            output.Append("\\r");
            break;
        default:
            output.Append(c);
            break;
        }
    }
    output.Append('"');
}

template <typename T> void WriteFloat(StringWriter &output, T value)
{
    if (Math::IsNaN(value))
    {
        output.Append("nan");
    }
    else if (value > NumericLimits<T>::Max() || value < -NumericLimits<T>::Max())
    {
        output.Append(value < 0 ? "-inf" : "inf");
    }
    else
    {
        AppendFloat(output, value);
    }
}

/// True if a value of *type* always fits on one line.
bool IsSimple(const TypeInfo &type)
{
    switch (type.GetKind())
    {
    case TypeKind::Struct:
        return IsTuple(type) || IsTextual(type);
    case TypeKind::Class:
    case TypeKind::Interface:
    case TypeKind::List:
    case TypeKind::FixedArray:
    case TypeKind::Map:
    case TypeKind::Set:
    case TypeKind::UniquePtr:
    case TypeKind::SharedPtr:
        return false;
    case TypeKind::Optional:
        return IsSimple(*type.GetElementType());
    default:
        return true;
    }
}

void WriteFields(StringWriter &output, const TypeInfo &type, const void *value, uint32 indent,
                 const MarlinTextWriteOptions &options);

void WriteSequence(StringWriter &output, const TypeInfo &element, usize count, FunctionRef<const void *(usize)> at,
                   uint32 indent, const MarlinTextWriteOptions &options)
{
    if (count == 0)
    {
        output.Append("[]");
        return;
    }
    if (IsSimple(element))
    {
        StringBuilder<256> line("[");
        for (usize i = 0; i < count; ++i)
        {
            line.Append(i == 0 ? "" : ", ");
            MarlinTextSerializer::WriteValue(line, element, at(i), indent, options);
        }
        line.Append(']');
        if (line.Length() <= MaxInlineArrayLength)
        {
            output.Append(line.AsView());
            return;
        }
    }
    output.Append("[\n");
    for (usize i = 0; i < count; ++i)
    {
        Indent(output, indent + 1);
        MarlinTextSerializer::WriteValue(output, element, at(i), indent + 1, options);
        output.Append(",\n");
    }
    Indent(output, indent);
    output.Append(']');
}

void WriteKey(StringWriter &output, const TypeInfo &type, const void *key)
{
    StringBuilder<64> text;
    Reflection::FormatValue(text, type, key);
    const bool textual = type.GetKind() == TypeKind::String || type.GetKind() == TypeKind::Name || IsTextual(type);
    if (textual && !IsIdentifier(text.AsView()))
    {
        WriteString(output, text.AsView());
    }
    else
    {
        output.Append(text.AsView());
    }
}

/// A value of *type* constructed for comparisons and reads, or invalid if the type cannot be constructed.
class DefaultValue
{
public:
    explicit DefaultValue(const TypeInfo &type) : _value(type)
    {
    }

    [[nodiscard]] const void *Get() const
    {
        return _value.Get();
    }

private:
    ReflectedValue _value;
};

void WriteFields(StringWriter &output, const TypeInfo &type, const void *value, uint32 indent,
                 const MarlinTextWriteOptions &options)
{
    const DefaultValue defaults(type);
    for (const FieldInfo *field : type.GetFields())
    {
        if (IsTransient(*field))
        {
            continue;
        }
        const void *fieldValue = field->GetValue(value);
        if (!options.WriteDefaults && defaults.Get() != nullptr &&
            Reflection::Equals(field->GetType(), fieldValue, field->GetValue(defaults.Get())))
        {
            continue;
        }
        Indent(output, indent);
        output.Append(field->GetName()).Append(" = ");
        MarlinTextSerializer::WriteValue(output, field->GetType(), fieldValue, indent, options);
        output.Append('\n');
    }
}

/// Writes `{ fields }` or, for a class, `TypeName { fields }`.
void WriteComposite(StringWriter &output, const TypeInfo &type, const void *value, uint32 indent,
                    const MarlinTextWriteOptions &options, bool named)
{
    if (named)
    {
        output.Append(type.GetName()).Append(' ');
    }
    StringBuilder<512> fields;
    WriteFields(fields, type, value, indent + 1, options);
    if (fields.IsEmpty())
    {
        output.Append("{}");
        return;
    }
    output.Append("{\n").Append(fields.AsView());
    Indent(output, indent);
    output.Append('}');
}

class Reader
{
public:
    explicit Reader(MarlinTextDiagnostics &diagnostics) : _diagnostics(&diagnostics)
    {
    }

    bool Fail(const MarlinTextValue &value, StringView message)
    {
        _diagnostics->Error(value.Line, message);
        return false;
    }

    bool Mismatch(const MarlinTextValue &value, const TypeInfo &type)
    {
        return Fail(value, Format("'{0}' is not a {1}", value.Source, type.GetName()));
    }

    template <typename T> bool ReadInteger(const MarlinTextValue &value, const TypeInfo &type, void *target)
    {
        if (value.Kind != MarlinTextValueKind::Integer)
        {
            return Mismatch(value, type);
        }
        const int64 number = value.Integer;
        if constexpr (IsSigned<T>)
        {
            if (number < static_cast<int64>(NumericLimits<T>::Min()) ||
                number > static_cast<int64>(NumericLimits<T>::Max()))
            {
                return Fail(value, Format("{0} does not fit in {1}", value.Source, type.GetName()));
            }
        }
        else if constexpr (sizeof(T) < sizeof(uint64))
        {
            if (number < 0 || static_cast<uint64>(number) > NumericLimits<T>::Max())
            {
                return Fail(value, Format("{0} does not fit in {1}", value.Source, type.GetName()));
            }
        }
        else if (number < 0 && !value.Source.StartsWith("0x") && !value.Source.StartsWith("0X"))
        {
            return Fail(value, Format("{0} does not fit in {1}", value.Source, type.GetName()));
        }
        *static_cast<T *>(target) = static_cast<T>(number);
        return true;
    }

    bool ReadFields(Span<const MarlinTextField> fields, const TypeInfo &type, void *target,
                    const MarlinTextConditions *conditions, bool staticOnly = false, bool instanceOnly = false)
    {
        bool ok = true;
        for (const MarlinTextField &field : fields)
        {
            if (field.IsDirective)
            {
                continue;
            }
            if (field.Condition != nullptr &&
                (conditions == nullptr || !conditions->Holds(field.Condition, field.InElse)))
            {
                continue;
            }
            const FieldInfo *info = type.FindField(field.Key);
            if (info == nullptr || IsTransient(*info))
            {
                _diagnostics->Warning(field.Line, Format("{0} has no field {1}", type.GetName(), field.Key));
                continue;
            }
            if ((staticOnly && !info->IsStatic()) || (instanceOnly && info->IsStatic()))
            {
                continue;
            }
            ok &= ReadField(field, *info, target);
        }
        return ok;
    }

    bool ReadField(const MarlinTextField &field, const FieldInfo &info, void *target)
    {
        const TypeInfo &type = info.GetType();
        void *value = info.GetValue(target);
        if (field.Operation == MarlinTextOperation::Assign)
        {
            return Read(field.Value, type, value);
        }
        if (field.Value.Kind != MarlinTextValueKind::Array ||
            (type.GetKind() != TypeKind::List && type.GetKind() != TypeKind::Set))
        {
            return Fail(field.Value,
                        Format("+= and -= take an array and need a list or set field, not {0}", type.GetName()));
        }
        const TypeInfo &element = *type.GetElementType();
        bool ok = true;
        for (const MarlinTextValue &item : field.Value.Elements)
        {
            const ReflectedValue temporary(element);
            if (!temporary.IsValid())
            {
                return Fail(item, Format("{0} cannot be constructed", element.GetName()));
            }
            if (!Read(item, element, temporary.Get()))
            {
                ok = false;
                continue;
            }
            if (type.GetKind() == TypeKind::Set)
            {
                const SetOperations &set = type.GetSetOperations();
                (void)(field.Operation == MarlinTextOperation::Append ? set.Add(value, temporary.Get())
                                                                      : set.Remove(value, temporary.Get()));
                continue;
            }
            const ListOperations &list = type.GetListOperations();
            if (field.Operation == MarlinTextOperation::Append)
            {
                const usize count = list.Count(value);
                list.Resize(value, count + 1);
                Reflection::Copy(element, list.At(value, count), temporary.Get());
                continue;
            }
            for (usize i = list.Count(value); i-- > 0;)
            {
                if (Reflection::Equals(element, list.At(value, i), temporary.Get()))
                {
                    list.RemoveAt(value, i);
                }
            }
        }
        return ok;
    }

    bool ReadSequence(const MarlinTextValue &value, const TypeInfo &type, void *target)
    {
        if (value.Kind != MarlinTextValueKind::Array)
        {
            return Mismatch(value, type);
        }
        const TypeInfo &element = *type.GetElementType();
        const usize count = value.Elements.Count();
        bool ok = true;
        if (type.GetKind() == TypeKind::FixedArray)
        {
            if (count > type.GetFixedCount())
            {
                return Fail(value, Format("{0} values do not fit in {1}", count, type.GetName()));
            }
            for (usize i = 0; i < count; ++i)
            {
                ok &= Read(value.Elements[i], element, static_cast<uint8 *>(target) + i * element.GetSize());
            }
            return ok;
        }
        if (type.GetKind() == TypeKind::List)
        {
            const ListOperations &list = type.GetListOperations();
            list.Resize(target, 0);
            list.Resize(target, count);
            for (usize i = 0; i < count; ++i)
            {
                ok &= Read(value.Elements[i], element, list.At(target, i));
            }
            return ok;
        }
        const SetOperations &set = type.GetSetOperations();
        set.Clear(target);
        for (const MarlinTextValue &item : value.Elements)
        {
            const ReflectedValue key(element);
            if (!key.IsValid() || !Read(item, element, key.Get()))
            {
                ok = false;
                continue;
            }
            (void)set.Add(target, key.Get());
        }
        return ok;
    }

    bool ReadMap(const MarlinTextValue &value, const TypeInfo &type, void *target)
    {
        if (value.Kind != MarlinTextValueKind::Block)
        {
            return Mismatch(value, type);
        }
        const MapOperations &map = type.GetMapOperations();
        const TypeInfo &keyType = *type.GetKeyType();
        map.Clear(target);
        bool ok = true;
        for (const MarlinTextField &field : value.Fields)
        {
            const ReflectedValue key(keyType);
            if (!key.IsValid() || !Reflection::ParseValue(field.Key, keyType, key.Get()))
            {
                ok = Fail(field.Value, Format("'{0}' is not a key of {1}", field.Key, type.GetName()));
                continue;
            }
            ok &= Read(field.Value, *type.GetElementType(), map.FindOrAdd(target, key.Get()));
        }
        return ok;
    }

    bool ReadPointer(const MarlinTextValue &value, const TypeInfo &type, void *target)
    {
        const PointerOperations &pointer = type.GetPointerOperations();
        if (value.Kind == MarlinTextValueKind::Null)
        {
            pointer.Reset(target, nullptr);
            return true;
        }
        const TypeInfo &element = *type.GetElementType();
        const TypeInfo *dynamic = &element;
        if (value.Kind == MarlinTextValueKind::Object)
        {
            dynamic = Reflection::FindType(value.Text);
            if (dynamic == nullptr || !dynamic->IsA(element))
            {
                return Fail(value, Format("{0} is not a type derived from {1}", value.Text, element.GetName()));
            }
        }
        else if (value.Kind != MarlinTextValueKind::Block)
        {
            return Mismatch(value, type);
        }
        void *object = pointer.Get(target);
        if (object == nullptr || &element.GetDynamicType(object) != dynamic)
        {
            object = dynamic->New();
            if (object == nullptr)
            {
                return Fail(value, Format("{0} cannot be constructed", dynamic->GetName()));
            }
            pointer.Reset(target, dynamic->CastTo(object, element));
        }
        return ReadFields(value.Fields, *dynamic, object, nullptr);
    }

    bool Read(const MarlinTextValue &value, const TypeInfo &type, void *target)
    {
        MarlinTextFormat format{};
        if (FormatRegistry::Get().Find(type, format))
        {
            return format.Read(value, target, *_diagnostics);
        }
        if (&type == &TypeOf<BinaryFileLink>())
        {
            auto &link = *static_cast<BinaryFileLink *>(target);
            if (value.Kind == MarlinTextValueKind::Null)
            {
                link = BinaryFileLink();
                return true;
            }
            if (value.Kind != MarlinTextValueKind::BinaryLink)
            {
                return Mismatch(value, type);
            }
            if (!value.LinkGuid.IsValid())
            {
                return Fail(value, "a binary link needs a #GUID");
            }
            constexpr StringView Scheme = "source:";
            StringView path = value.Text;
            if (path.StartsWith(Scheme))
            {
                path = path.RemoveStart(Scheme.Length());
            }
            link.Path = path;
            link.Uuid = value.LinkGuid;
            return true;
        }
        switch (type.GetKind())
        {
        case TypeKind::Bool:
            if (value.Kind != MarlinTextValueKind::Bool)
            {
                return Mismatch(value, type);
            }
            *static_cast<bool *>(target) = value.Bool;
            return true;
        case TypeKind::Int8:
            return ReadInteger<int8>(value, type, target);
        case TypeKind::Int16:
            return ReadInteger<int16>(value, type, target);
        case TypeKind::Int32:
            return ReadInteger<int32>(value, type, target);
        case TypeKind::Int64:
            return ReadInteger<int64>(value, type, target);
        case TypeKind::UInt8:
            return ReadInteger<uint8>(value, type, target);
        case TypeKind::UInt16:
            return ReadInteger<uint16>(value, type, target);
        case TypeKind::UInt32:
            return ReadInteger<uint32>(value, type, target);
        case TypeKind::UInt64:
            return ReadInteger<uint64>(value, type, target);
        case TypeKind::Float32:
            if (!value.IsNumber())
            {
                return Mismatch(value, type);
            }
            *static_cast<float32 *>(target) = static_cast<float32>(value.AsFloat());
            return true;
        case TypeKind::Float64:
            if (!value.IsNumber())
            {
                return Mismatch(value, type);
            }
            *static_cast<float64 *>(target) = value.AsFloat();
            return true;
        case TypeKind::String:
        case TypeKind::Name:
            // A bare identifier is a string too: `Tags += [Foliage]`.
            if (value.Kind != MarlinTextValueKind::String && value.Kind != MarlinTextValueKind::Identifier)
            {
                return Mismatch(value, type);
            }
            return Reflection::ParseValue(value.Text, type, target);
        case TypeKind::Enum:
            if (value.Kind == MarlinTextValueKind::Integer)
            {
                Reflection::SetEnumValue(type, target, value.Integer);
                return true;
            }
            if (value.Kind != MarlinTextValueKind::Identifier || !Reflection::ParseValue(value.Text, type, target))
            {
                return Mismatch(value, type);
            }
            return true;
        case TypeKind::List:
        case TypeKind::FixedArray:
        case TypeKind::Set:
            return ReadSequence(value, type, target);
        case TypeKind::Map:
            return ReadMap(value, type, target);
        case TypeKind::Optional: {
            const OptionalOperations &optional = type.GetOptionalOperations();
            if (value.Kind == MarlinTextValueKind::Null)
            {
                optional.Reset(target);
                return true;
            }
            return Read(value, *type.GetElementType(), optional.Emplace(target));
        }
        case TypeKind::UniquePtr:
        case TypeKind::SharedPtr:
            return ReadPointer(value, type, target);
        case TypeKind::ObjectLink:
            if (value.Kind == MarlinTextValueKind::Null)
            {
                type.GetLinkOperations().SetGuid(target, Guid());
                return true;
            }
            if (value.Kind != MarlinTextValueKind::ObjectLink)
            {
                return Mismatch(value, type);
            }
            if (!value.LinkGuid.IsValid())
            {
                return Fail(value, Format("a link to {0} needs a #UUID", type.GetElementType()->GetName()));
            }
            type.GetLinkOperations().SetGuid(target, value.LinkGuid);
            return true;
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Opaque:
            break;
        default:
            return Fail(value, Format("{0} cannot be read from MarlinText", type.GetName()));
        }

        if (IsTextual(type))
        {
            if (value.Kind != MarlinTextValueKind::String || !type.GetOperations().Parse(value.Text, target))
            {
                return Mismatch(value, type);
            }
            return true;
        }
        if (value.Kind == MarlinTextValueKind::Tuple && type.GetKind() == TypeKind::Struct)
        {
            const Span<const FieldInfo *const> fields = type.GetFields();
            if (value.Elements.Count() != fields.Count())
            {
                return Fail(value, Format("{0} takes {1} values", type.GetName(), fields.Count()));
            }
            bool ok = true;
            for (usize i = 0; i < fields.Count(); ++i)
            {
                ok &= Read(value.Elements[i], fields[i]->GetType(), fields[i]->GetValue(target));
            }
            return ok;
        }
        if (value.Kind == MarlinTextValueKind::Object && value.Text != type.GetName())
        {
            return Fail(value, Format("{0} is not a {1}", value.Text, type.GetName()));
        }
        if (value.Kind != MarlinTextValueKind::Block && value.Kind != MarlinTextValueKind::Object)
        {
            return Mismatch(value, type);
        }
        return ReadFields(value.Fields, type.GetDynamicType(target), target, nullptr);
    }

    bool ReadObject(const MarlinTextObject &object, const TypeInfo &type, void *target,
                    const MarlinTextReadOptions &options, uint32 depth)
    {
        const TypeInfo &dynamic = target != nullptr ? type.GetDynamicType(target) : type;
        if (!options.IgnoreTypeName && object.GetTypeName() != dynamic.GetName())
        {
            _diagnostics->Error(object.GetLine(),
                                Format("expected a {0}, not a {1}", dynamic.GetName(), object.GetTypeName()));
            return false;
        }
        bool ok = true;
        if (const MarlinTextValue *link = object.GetParent())
        {
            const MarlinTextObject *parent = options.ResolveParent ? options.ResolveParent(*link) : nullptr;
            if (parent == nullptr || depth >= MaxParentDepth)
            {
                _diagnostics->Error(link->Line, Format("cannot find {0}, which this object overrides", link->Source));
                return false;
            }
            ok &= ReadObject(*parent, type, target, options, depth + 1);
        }

        const uint32 schema = object.GetSchema();
        const uint32 version = dynamic.GetVersion();
        if (schema > version)
        {
            _diagnostics->Error(object.GetLine(), Format("{0} is written at schema {1}, newer than this build's {2}",
                                                         dynamic.GetName(), schema, version));
            return false;
        }
        if (schema < version)
        {
            MarlinTextObject upgraded = object;
            if (!MarlinTextSchema::Upgrade(dynamic.GetName(), upgraded, schema, version, *_diagnostics))
            {
                return false;
            }
            return ReadFields(upgraded.GetFields(), dynamic, target, options.Conditions, options.StaticFieldsOnly,
                              options.InstanceFieldsOnly) &&
                   ok;
        }
        return ReadFields(object.GetFields(), dynamic, target, options.Conditions, options.StaticFieldsOnly,
                          options.InstanceFieldsOnly) &&
               ok;
    }

private:
    MarlinTextDiagnostics *_diagnostics;
};
} // namespace

namespace
{
/// Brings the text of an object at an older schema up to date by repeating what its upgrades did to its fields:
/// fields they removed or renamed lose their lines, and fields they added or changed are set.
bool UpgradeText(MarlinTextDocument &document, usize index, const TypeInfo &type, MarlinTextDiagnostics &diagnostics)
{
    const MarlinTextObject *current = document.GetObject(index, diagnostics);
    MarlinTextObject upgraded = *current;
    if (!MarlinTextSchema::Upgrade(type.GetName(), upgraded, current->GetSchema(), type.GetVersion(), diagnostics))
    {
        return false;
    }
    List<String> removed;
    struct Change
    {
        String Key;
        String Value;
    };
    List<Change> changed;
    for (const MarlinTextField &field : current->GetFields())
    {
        if (field.Condition == nullptr && !field.IsDirective && upgraded.Find(field.Key) == nullptr)
        {
            removed.Add(String(field.Key));
        }
    }
    for (const MarlinTextField &field : upgraded.GetFields())
    {
        if (field.Condition != nullptr || field.IsDirective)
        {
            continue;
        }
        const MarlinTextField *original = current->Find(field.Key);
        if (original == nullptr || original->Value.Source != field.Value.Source)
        {
            changed.Add({String(field.Key), String(field.Value.Source)});
        }
    }
    for (const String &key : removed)
    {
        if (!document.RemoveField(index, key, diagnostics))
        {
            return false;
        }
    }
    for (const Change &field : changed)
    {
        if (!document.SetField(index, field.Key, field.Value, diagnostics))
        {
            return false;
        }
    }
    char digits[16];
    return document.SetField(index, "Schema", StringView(digits, FormatUnsigned(type.GetVersion(), digits)),
                             diagnostics, true);
}
} // namespace

namespace MarlinTextSerializer
{
void RegisterFormat(const TypeInfo &type, MarlinTextFormat format)
{
    FormatRegistry &registry = FormatRegistry::Get();
    const ScopeLock lock(registry.Lock);
    registry.Formats.Set(type.GetId(), format);
}

void WriteObject(StringWriter &output, const TypeInfo &type, const void *object, const Guid &uuid, StringView name,
                 const MarlinTextWriteOptions &options)
{
    const TypeInfo &dynamic = type.GetDynamicType(object);
    output.Append(dynamic.GetName()).Append("\n{\n");
    if (uuid.IsValid())
    {
        Indent(output, 1);
        output.Append("!UUID = \"");
        uuid.FormatTo(output, {});
        output.Append("\"\n");
    }
    if (!name.IsEmpty())
    {
        Indent(output, 1);
        output.Append("!Name = ");
        WriteString(output, name);
        output.Append('\n');
    }
    if (dynamic.GetVersion() > 1)
    {
        char digits[16];
        Indent(output, 1);
        output.Append("!Schema = ")
            .Append(StringView(digits, FormatUnsigned(dynamic.GetVersion(), digits)))
            .Append('\n');
    }
    WriteFields(output, dynamic, object, 1, options);
    output.Append("}\n");
}

void WriteValue(StringWriter &output, const TypeInfo &type, const void *value, uint32 indent,
                const MarlinTextWriteOptions &options)
{
    MarlinTextFormat format{};
    if (FormatRegistry::Get().Find(type, format))
    {
        format.Write(output, value);
        return;
    }
    if (&type == &TypeOf<BinaryFileLink>())
    {
        const auto &link = *static_cast<const BinaryFileLink *>(value);
        if (!link.IsValid())
        {
            output.Append("null");
            return;
        }
        output.Append("`source:").Append(link.Path).Append('#');
        link.Uuid.FormatTo(output, {});
        output.Append('`');
        return;
    }
    switch (type.GetKind())
    {
    case TypeKind::Bool:
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::Enum:
        Reflection::FormatValue(output, type, value);
        return;
    case TypeKind::Float32:
        WriteFloat(output, *static_cast<const float32 *>(value));
        return;
    case TypeKind::Float64:
        WriteFloat(output, *static_cast<const float64 *>(value));
        return;
    case TypeKind::String:
        WriteString(output, static_cast<const String *>(value)->AsView());
        return;
    case TypeKind::StringView:
        WriteString(output, *static_cast<const StringView *>(value));
        return;
    case TypeKind::Name:
        WriteString(output, static_cast<const Name *>(value)->ToView());
        return;
    case TypeKind::List: {
        const ListOperations &list = type.GetListOperations();
        WriteSequence(
            output, *type.GetElementType(), list.Count(value),
            [&](usize i) -> const void * { return list.At(Private::RemoveConst(value), i); }, indent, options);
        return;
    }
    case TypeKind::FixedArray: {
        const TypeInfo &element = *type.GetElementType();
        WriteSequence(
            output, element, type.GetFixedCount(),
            [&](usize i) -> const void * { return static_cast<const uint8 *>(value) + i * element.GetSize(); }, indent,
            options);
        return;
    }
    case TypeKind::Set: {
        List<const void *> keys;
        type.GetSetOperations().ForEach(
            value, [](const void *key, void *context) { static_cast<List<const void *> *>(context)->Add(key); }, &keys);
        WriteSequence(output, *type.GetElementType(), keys.Count(), [&](usize i) { return keys[i]; }, indent, options);
        return;
    }
    case TypeKind::Map: {
        struct Context
        {
            StringWriter *Output;
            const TypeInfo *Key;
            const TypeInfo *Value;
            uint32 Indent;
            const MarlinTextWriteOptions *Options;
        } context{&output, type.GetKeyType(), type.GetElementType(), indent, &options};
        if (type.GetMapOperations().Count(value) == 0)
        {
            output.Append("{}");
            return;
        }
        output.Append("{\n");
        type.GetMapOperations().ForEach(
            value,
            [](const void *key, const void *item, void *state) {
                const auto &context = *static_cast<const Context *>(state);
                Indent(*context.Output, context.Indent + 1);
                WriteKey(*context.Output, *context.Key, key);
                context.Output->Append(" = ");
                WriteValue(*context.Output, *context.Value, item, context.Indent + 1, *context.Options);
                context.Output->Append('\n');
            },
            &context);
        Indent(output, indent);
        output.Append('}');
        return;
    }
    case TypeKind::Optional: {
        const OptionalOperations &optional = type.GetOptionalOperations();
        if (!optional.HasValue(value))
        {
            output.Append("null");
            return;
        }
        WriteValue(output, *type.GetElementType(), optional.Get(Private::RemoveConst(value)), indent, options);
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
        WriteComposite(output, dynamic, object, indent, options, true);
        return;
    }
    case TypeKind::ObjectLink: {
        Guid guid;
        type.GetLinkOperations().GetGuid(value, guid);
        if (!guid.IsValid())
        {
            output.Append("null");
            return;
        }
        output.Append('<');
        if (options.WriteLinkHint)
        {
            options.WriteLinkHint(guid, output);
        }
        output.Append('#');
        guid.FormatTo(output, {});
        output.Append('>');
        return;
    }
    default:
        break;
    }

    if (IsTextual(type))
    {
        StringBuilder<64> text;
        type.GetOperations().Format(text, value);
        WriteString(output, text.AsView());
        return;
    }
    if (IsTuple(type))
    {
        output.Append('(');
        bool first = true;
        for (const FieldInfo *field : type.GetFields())
        {
            output.Append(first ? "" : ", ");
            WriteValue(output, field->GetType(), field->GetValue(value), indent, options);
            first = false;
        }
        output.Append(')');
        return;
    }
    if (type.HasFields())
    {
        WriteComposite(output, type.GetDynamicType(value), value, indent, options, false);
        return;
    }
    output.Append("null");
}

bool ReadObject(const MarlinTextObject &object, const TypeInfo &type, void *target, MarlinTextDiagnostics &diagnostics,
                const MarlinTextReadOptions &options)
{
    return Reader(diagnostics).ReadObject(object, type, target, options, 0);
}

bool ReadValue(const MarlinTextValue &value, const TypeInfo &type, void *target, MarlinTextDiagnostics &diagnostics)
{
    return Reader(diagnostics).Read(value, type, target);
}

bool UpdateObject(MarlinTextDocument &document, usize index, const TypeInfo &type, const void *object,
                  MarlinTextDiagnostics &diagnostics, const MarlinTextWriteOptions &options,
                  const MarlinTextReadOptions &readOptions)
{
    const TypeInfo &dynamic = type.GetDynamicType(object);
    const MarlinTextObject *current = document.GetObject(index, diagnostics);
    const ReflectedValue written(dynamic);
    if (current == nullptr || !written.IsValid())
    {
        return false;
    }
    if (current->GetSchema() < dynamic.GetVersion() && !UpgradeText(document, index, dynamic, diagnostics))
    {
        return false;
    }
    current = document.GetObject(index, diagnostics);
    if (current == nullptr)
    {
        return false;
    }
    MarlinTextDiagnostics ignored;
    (void)Reader(ignored).ReadObject(*current, dynamic, written.Get(), readOptions, 0);

    for (const FieldInfo *field : dynamic.GetFields())
    {
        if (IsTransient(*field))
        {
            continue;
        }
        const TypeInfo &fieldType = field->GetType();
        const void *value = field->GetValue(object);
        current = document.GetObject(index, diagnostics);
        if (current == nullptr)
        {
            return false;
        }
        const MarlinTextField *existing = current->Find(field->GetName());
        const bool same = Reflection::Equals(fieldType, field->GetValue(written.Get()), value);
        if (same && (existing != nullptr || !options.WriteDefaults))
        {
            continue;
        }
        while (existing != nullptr && existing->Operation != MarlinTextOperation::Assign)
        {
            if (!document.RemoveField(index, field->GetName(), diagnostics))
            {
                return false;
            }
            current = document.GetObject(index, diagnostics);
            existing = current != nullptr ? current->Find(field->GetName()) : nullptr;
        }

        uint32 indent = 1;
        if (existing != nullptr)
        {
            const StringView text = document.GetText();
            uint32 start = existing->Begin;
            while (start > 0 && text[start - 1] != '\n')
            {
                --start;
            }
            indent = (existing->Begin - start) / 4;
        }
        StringBuilder<256> valueText;
        WriteValue(valueText, fieldType, value, indent, options);
        if (!document.SetField(index, field->GetName(), valueText.AsView(), diagnostics))
        {
            return false;
        }
    }
    return true;
}
} // namespace MarlinTextSerializer
} // namespace Marlin
