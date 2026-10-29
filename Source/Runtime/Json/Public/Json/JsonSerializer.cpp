#include "Json/JsonSerializer.h"

#include "Memory/Memory.h"
#include "Misc/Guid.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"
#include "String/Name.h"
#include "String/Number.h"
#include "String/String.h"
#include "String/StringBuilder.h"
#include "Templates/NumericLimits.h"

namespace Marlin
{
namespace
{
bool IsTransient(const FieldInfo &field)
{
    return field.GetMetadata().Has("Transient");
}

bool IsTuple(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("Tuple");
}

bool IsTextual(const TypeInfo &type)
{
    const TypeOperations &operations = type.GetOperations();
    return operations.Format != nullptr && operations.Parse != nullptr &&
           (type.GetKind() == TypeKind::Opaque || type.GetFields().IsEmpty());
}

bool IsStringKey(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::String || type.GetKind() == TypeKind::Name || type.GetKind() == TypeKind::Enum ||
           IsTextual(type);
}

template <typename T> void WriteInteger(JsonWriter &writer, const void *value)
{
    if constexpr (IsSigned<T>)
    {
        writer.Integer(*static_cast<const T *>(value));
    }
    else
    {
        writer.Unsigned(*static_cast<const T *>(value));
    }
}

void WriteText(JsonWriter &writer, const TypeInfo &type, const void *value)
{
    StringBuilder<128> text;
    Reflection::FormatValue(text, type, value);
    writer.String(text.AsView());
}

template <typename T> bool ReadInteger(const JsonValue &json, void *target)
{
    if constexpr (IsSigned<T>)
    {
        int64 value = 0;
        if (!json.TryGetInteger(value) || value < NumericLimits<T>::Min() || value > NumericLimits<T>::Max())
        {
            return false;
        }
        *static_cast<T *>(target) = static_cast<T>(value);
    }
    else
    {
        uint64 value = 0;
        if (!json.TryGetUnsigned(value) || value > NumericLimits<T>::Max())
        {
            return false;
        }
        *static_cast<T *>(target) = static_cast<T>(value);
    }
    return true;
}

class Reader
{
public:
    explicit Reader(StringWriter &error) : _error(&error)
    {
    }

    bool Read(const JsonValue &json, const TypeInfo &type, void *target)
    {
        switch (type.GetKind())
        {
        case TypeKind::Bool:
            if (json.GetKind() != JsonKind::Bool)
            {
                return Fail("expected true or false");
            }
            *static_cast<bool *>(target) = json.AsBool();
            return true;
        case TypeKind::Int8:
            return ReadInteger<int8>(json, target) || Fail("expected an int8");
        case TypeKind::Int16:
            return ReadInteger<int16>(json, target) || Fail("expected an int16");
        case TypeKind::Int32:
            return ReadInteger<int32>(json, target) || Fail("expected an int32");
        case TypeKind::Int64:
            return ReadInteger<int64>(json, target) || Fail("expected an int64");
        case TypeKind::UInt8:
            return ReadInteger<uint8>(json, target) || Fail("expected a uint8");
        case TypeKind::UInt16:
            return ReadInteger<uint16>(json, target) || Fail("expected a uint16");
        case TypeKind::UInt32:
            return ReadInteger<uint32>(json, target) || Fail("expected a uint32");
        case TypeKind::UInt64:
            return ReadInteger<uint64>(json, target) || Fail("expected a uint64");
        case TypeKind::Float32:
            if (!json.IsNumber())
            {
                return Fail("expected a number");
            }
            *static_cast<float32 *>(target) = static_cast<float32>(json.AsNumber());
            return true;
        case TypeKind::Float64:
            if (!json.IsNumber())
            {
                return Fail("expected a number");
            }
            *static_cast<float64 *>(target) = json.AsNumber();
            return true;
        case TypeKind::String:
        case TypeKind::Name:
        case TypeKind::Enum:
            if (!json.IsString())
            {
                return Fail("expected a string");
            }
            return Reflection::ParseValue(json.AsString(), type, target) ||
                   Fail(Format("'{0}' is not a {1}", json.AsString(), type.GetName()));
        case TypeKind::StringView:
            if (!json.IsString())
            {
                return Fail("expected a string");
            }
            *static_cast<StringView *>(target) = json.AsString();
            return true;
        case TypeKind::List:
            return ReadList(json, type, target);
        case TypeKind::FixedArray:
            return ReadFixedArray(json, type, target);
        case TypeKind::Map:
            return ReadMap(json, type, target);
        case TypeKind::Set:
            return ReadSet(json, type, target);
        case TypeKind::Optional: {
            const OptionalOperations &optional = type.GetOptionalOperations();
            if (json.IsNull())
            {
                optional.Reset(target);
                return true;
            }
            return Read(json, *type.GetElementType(), optional.Emplace(target));
        }
        case TypeKind::UniquePtr:
        case TypeKind::SharedPtr:
            return ReadPointer(json, type, target);
        case TypeKind::ObjectLink: {
            Guid guid;
            if (!json.IsNull() && (!json.IsString() || !Guid::TryParse(json.AsString(), guid)))
            {
                return Fail("expected a Guid string or null");
            }
            type.GetLinkOperations().SetGuid(target, guid);
            return true;
        }
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Opaque:
            return ReadComposite(json, type, target);
        default:
            return Fail(Format("{0} cannot be read from JSON", type.GetName()));
        }
    }

private:
    bool Fail(StringView message)
    {
        if (_failed)
        {
            return false;
        }
        _failed = true;
        for (usize i = 0; i < _path.Count(); ++i)
        {
            if (i != 0 && _path[i].AsView().First() != '[')
            {
                _error->Append('.');
            }
            _error->Append(_path[i].AsView());
        }
        if (!_path.IsEmpty())
        {
            _error->Append(": ");
        }
        _error->Append(message);
        return false;
    }

    void PushIndex(usize index)
    {
        StringBuilder<24> text("[");
        char digits[24];
        text.Append(StringView(digits, FormatUnsigned(index, digits))).Append(']');
        _path.Add(text.ToString());
    }

    bool ReadList(const JsonValue &json, const TypeInfo &type, void *target)
    {
        if (!json.IsArray())
        {
            return Fail("expected an array");
        }
        const ListOperations &list = type.GetListOperations();
        const Span<const JsonValue> elements = json.GetElements();
        list.Resize(target, elements.Count());
        for (usize i = 0; i < elements.Count(); ++i)
        {
            PushIndex(i);
            const bool ok = Read(elements[i], *type.GetElementType(), list.At(target, i));
            _path.Pop();
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    bool ReadFixedArray(const JsonValue &json, const TypeInfo &type, void *target)
    {
        if (!json.IsArray() || json.Count() != type.GetFixedCount())
        {
            return Fail(Format("expected an array of {0}", type.GetFixedCount()));
        }
        const TypeInfo &element = *type.GetElementType();
        for (uint32 i = 0; i < type.GetFixedCount(); ++i)
        {
            PushIndex(i);
            const bool ok = Read(json.GetElements()[i], element,
                                 static_cast<uint8 *>(target) + static_cast<usize>(i) * element.GetSize());
            _path.Pop();
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    /// A key of a map or set in a temporary of the key type.
    struct KeyStorage
    {
        explicit KeyStorage(const TypeInfo &type)
            : Type(type), Data(Memory::Allocate(type.GetSize() != 0 ? type.GetSize() : 1, type.GetAlignment()))
        {
            type.GetOperations().Construct(Data);
        }

        ~KeyStorage()
        {
            Type.GetOperations().Destruct(Data);
            Memory::Free(Data);
        }

        MARLIN_NONCOPYABLE(KeyStorage);

        const TypeInfo &Type;
        void *Data;
    };

    bool ReadMap(const JsonValue &json, const TypeInfo &type, void *target)
    {
        const MapOperations &map = type.GetMapOperations();
        const TypeInfo &keyType = *type.GetKeyType();
        map.Clear(target);
        if (IsStringKey(keyType))
        {
            if (!json.IsObject())
            {
                return Fail("expected an object");
            }
            for (const JsonMember &member : json.GetMembers())
            {
                const KeyStorage key(keyType);
                if (!Reflection::ParseValue(member.Key, keyType, key.Data))
                {
                    return Fail(Format("'{0}' is not a {1}", member.Key, keyType.GetName()));
                }
                _path.Add(member.Key);
                const bool ok = Read(member.Value, *type.GetElementType(), map.FindOrAdd(target, key.Data));
                _path.Pop();
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }
        if (!json.IsArray())
        {
            return Fail("expected an array of [key, value] pairs");
        }
        for (usize i = 0; i < json.Count(); ++i)
        {
            const JsonValue &pair = json.GetElements()[i];
            PushIndex(i);
            const KeyStorage key(keyType);
            const bool ok = pair.IsArray() && pair.Count() == 2 && Read(pair.GetElements()[0], keyType, key.Data) &&
                            Read(pair.GetElements()[1], *type.GetElementType(), map.FindOrAdd(target, key.Data));
            _path.Pop();
            if (!ok)
            {
                return Fail("expected a [key, value] pair");
            }
        }
        return true;
    }

    bool ReadSet(const JsonValue &json, const TypeInfo &type, void *target)
    {
        if (!json.IsArray())
        {
            return Fail("expected an array");
        }
        const SetOperations &set = type.GetSetOperations();
        set.Clear(target);
        for (usize i = 0; i < json.Count(); ++i)
        {
            const KeyStorage key(*type.GetElementType());
            PushIndex(i);
            const bool ok = Read(json.GetElements()[i], *type.GetElementType(), key.Data);
            _path.Pop();
            if (!ok)
            {
                return false;
            }
            (void)set.Add(target, key.Data);
        }
        return true;
    }

    bool ReadPointer(const JsonValue &json, const TypeInfo &type, void *target)
    {
        const PointerOperations &pointer = type.GetPointerOperations();
        if (json.IsNull())
        {
            pointer.Reset(target, nullptr);
            return true;
        }
        const TypeInfo &element = *type.GetElementType();
        const JsonValue *name = json.IsObject() ? json.Find("$type") : nullptr;
        const TypeInfo *dynamic = &element;
        if (name != nullptr)
        {
            dynamic = Reflection::FindType(name->AsString());
            if (dynamic == nullptr || !dynamic->IsA(element))
            {
                return Fail(Format("'{0}' is not a {1}", name->AsString(), element.GetName()));
            }
        }
        void *object = dynamic->New();
        if (object == nullptr)
        {
            return Fail(Format("{0} cannot be constructed", dynamic->GetName()));
        }
        const bool ok = ReadComposite(json, *dynamic, object);
        pointer.Reset(target, dynamic->CastTo(object, element));
        return ok;
    }

    bool ReadComposite(const JsonValue &json, const TypeInfo &type, void *target)
    {
        if (IsTextual(type))
        {
            if (!json.IsString() || !type.GetOperations().Parse(json.AsString(), target))
            {
                return Fail(Format("expected a {0} string", type.GetName()));
            }
            return true;
        }
        if (IsTuple(type))
        {
            const Span<const FieldInfo *const> fields = type.GetFields();
            if (!json.IsArray() || json.Count() != fields.Count())
            {
                return Fail(Format("expected an array of {0}", fields.Count()));
            }
            for (usize i = 0; i < fields.Count(); ++i)
            {
                PushIndex(i);
                const bool ok = Read(json.GetElements()[i], fields[i]->GetType(), fields[i]->GetValue(target));
                _path.Pop();
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }
        if (!json.IsObject())
        {
            return Fail(Format("expected a {0} object", type.GetName()));
        }
        for (const JsonMember &member : json.GetMembers())
        {
            if (member.Key == "$type")
            {
                continue;
            }
            const FieldInfo *field = type.FindField(member.Key);
            if (field == nullptr || field->IsStatic() || IsTransient(*field))
            {
                return Fail(Format("{0} has no field {1}", type.GetName(), member.Key));
            }
            _path.Add(member.Key);
            const bool ok = Read(member.Value, field->GetType(), field->GetValue(target));
            _path.Pop();
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    StringWriter *_error;
    List<String> _path;
    bool _failed = false;
};

void WriteFields(JsonWriter &writer, const TypeInfo &type, const void *value)
{
    for (const FieldInfo *field : type.GetFields())
    {
        if (!IsTransient(*field))
        {
            writer.Key(field->GetName());
            Json::WriteValue(writer, field->GetType(), field->GetValue(value));
        }
    }
}

void WriteKey(JsonWriter &writer, const TypeInfo &type, const void *key)
{
    StringBuilder<128> text;
    Reflection::FormatValue(text, type, key);
    writer.Key(text.AsView());
}
} // namespace

namespace Json
{
void WriteValue(JsonWriter &writer, const TypeInfo &type, const void *value)
{
    switch (type.GetKind())
    {
    case TypeKind::Bool:
        writer.Bool(*static_cast<const bool *>(value));
        return;
    case TypeKind::Int8:
        WriteInteger<int8>(writer, value);
        return;
    case TypeKind::Int16:
        WriteInteger<int16>(writer, value);
        return;
    case TypeKind::Int32:
        WriteInteger<int32>(writer, value);
        return;
    case TypeKind::Int64:
        WriteInteger<int64>(writer, value);
        return;
    case TypeKind::UInt8:
        WriteInteger<uint8>(writer, value);
        return;
    case TypeKind::UInt16:
        WriteInteger<uint16>(writer, value);
        return;
    case TypeKind::UInt32:
        WriteInteger<uint32>(writer, value);
        return;
    case TypeKind::UInt64:
        WriteInteger<uint64>(writer, value);
        return;
    case TypeKind::Float32:
        writer.Number(*static_cast<const float32 *>(value));
        return;
    case TypeKind::Float64:
        writer.Number(*static_cast<const float64 *>(value));
        return;
    case TypeKind::String:
        writer.String(static_cast<const String *>(value)->AsView());
        return;
    case TypeKind::StringView:
        writer.String(*static_cast<const StringView *>(value));
        return;
    case TypeKind::Name:
        writer.String(static_cast<const Name *>(value)->ToView());
        return;
    case TypeKind::Enum:
        WriteText(writer, type, value);
        return;
    case TypeKind::List: {
        const ListOperations &list = type.GetListOperations();
        writer.BeginArray();
        for (usize i = 0; i < list.Count(value); ++i)
        {
            WriteValue(writer, *type.GetElementType(), list.At(Private::RemoveConst(value), i));
        }
        writer.EndArray();
        return;
    }
    case TypeKind::FixedArray: {
        const TypeInfo &element = *type.GetElementType();
        writer.BeginArray();
        for (uint32 i = 0; i < type.GetFixedCount(); ++i)
        {
            WriteValue(writer, element, static_cast<const uint8 *>(value) + static_cast<usize>(i) * element.GetSize());
        }
        writer.EndArray();
        return;
    }
    case TypeKind::Map: {
        const MapOperations &map = type.GetMapOperations();
        struct Context
        {
            JsonWriter *Writer;
            const TypeInfo *Key;
            const TypeInfo *Value;
            bool StringKeys;
        } context{&writer, type.GetKeyType(), type.GetElementType(), IsStringKey(*type.GetKeyType())};
        context.StringKeys ? writer.BeginObject() : writer.BeginArray();
        map.ForEach(
            value,
            [](const void *key, const void *element, void *state) {
                const auto &context = *static_cast<const Context *>(state);
                if (context.StringKeys)
                {
                    WriteKey(*context.Writer, *context.Key, key);
                    WriteValue(*context.Writer, *context.Value, element);
                    return;
                }
                context.Writer->BeginArray();
                WriteValue(*context.Writer, *context.Key, key);
                WriteValue(*context.Writer, *context.Value, element);
                context.Writer->EndArray();
            },
            &context);
        context.StringKeys ? writer.EndObject() : writer.EndArray();
        return;
    }
    case TypeKind::Set: {
        struct Context
        {
            JsonWriter *Writer;
            const TypeInfo *Element;
        } context{&writer, type.GetElementType()};
        writer.BeginArray();
        type.GetSetOperations().ForEach(
            value,
            [](const void *key, void *state) {
                const auto &context = *static_cast<const Context *>(state);
                WriteValue(*context.Writer, *context.Element, key);
            },
            &context);
        writer.EndArray();
        return;
    }
    case TypeKind::Optional: {
        const OptionalOperations &optional = type.GetOptionalOperations();
        if (!optional.HasValue(value))
        {
            writer.Null();
            return;
        }
        WriteValue(writer, *type.GetElementType(), optional.Get(Private::RemoveConst(value)));
        return;
    }
    case TypeKind::UniquePtr:
    case TypeKind::SharedPtr: {
        const void *object = type.GetPointerOperations().Get(value);
        if (object == nullptr)
        {
            writer.Null();
            return;
        }
        const TypeInfo &dynamic = type.GetElementType()->GetDynamicType(object);
        writer.BeginObject();
        writer.Key("$type").String(dynamic.GetFullName());
        WriteFields(writer, dynamic, object);
        writer.EndObject();
        return;
    }
    case TypeKind::ObjectLink: {
        Guid guid;
        type.GetLinkOperations().GetGuid(value, guid);
        if (!guid.IsValid())
        {
            writer.Null();
            return;
        }
        StringBuilder<40> text;
        guid.FormatTo(text, {});
        writer.String(text.AsView());
        return;
    }
    default:
        break;
    }
    if (IsTextual(type))
    {
        StringBuilder<128> text;
        type.GetOperations().Format(text, value);
        writer.String(text.AsView());
        return;
    }
    if (IsTuple(type))
    {
        writer.BeginArray();
        for (const FieldInfo *field : type.GetFields())
        {
            WriteValue(writer, field->GetType(), field->GetValue(value));
        }
        writer.EndArray();
        return;
    }
    const TypeInfo &dynamic = type.GetKind() == TypeKind::Class ? type.GetDynamicType(value) : type;
    writer.BeginObject();
    WriteFields(writer, dynamic, value);
    writer.EndObject();
}

bool ReadValue(const JsonValue &json, const TypeInfo &type, void *target, StringWriter &error)
{
    Reader reader(error);
    return reader.Read(json, type, target);
}
} // namespace Json

void JsonSchemaWriter::Write(JsonWriter &writer, const TypeInfo &type)
{
    const auto integer = [&](int64 minimum, uint64 maximum) {
        writer.BeginObject().Key("type").String("integer").Key("minimum").Integer(minimum);
        writer.Key("maximum").Unsigned(maximum).EndObject();
    };
    switch (type.GetKind())
    {
    case TypeKind::Bool:
        writer.BeginObject().Key("type").String("boolean").EndObject();
        return;
    case TypeKind::Int8:
        integer(-128, 127);
        return;
    case TypeKind::Int16:
        integer(-32768, 32767);
        return;
    case TypeKind::Int32:
        integer(NumericLimits<int32>::Min(), NumericLimits<int32>::Max());
        return;
    case TypeKind::Int64:
        integer(NumericLimits<int64>::Min(), static_cast<uint64>(NumericLimits<int64>::Max()));
        return;
    case TypeKind::UInt8:
        integer(0, 255);
        return;
    case TypeKind::UInt16:
        integer(0, 65535);
        return;
    case TypeKind::UInt32:
        integer(0, NumericLimits<uint32>::Max());
        return;
    case TypeKind::UInt64:
        integer(0, NumericLimits<uint64>::Max());
        return;
    case TypeKind::Float32:
    case TypeKind::Float64:
        writer.BeginObject().Key("type").String("number").EndObject();
        return;
    case TypeKind::String:
    case TypeKind::StringView:
    case TypeKind::Name:
        writer.BeginObject().Key("type").String("string").EndObject();
        return;
    case TypeKind::List:
    case TypeKind::Set:
        writer.BeginObject().Key("type").String("array").Key("items");
        Write(writer, *type.GetElementType());
        writer.EndObject();
        return;
    case TypeKind::FixedArray:
        writer.BeginObject().Key("type").String("array").Key("items");
        Write(writer, *type.GetElementType());
        writer.Key("minItems").Unsigned(type.GetFixedCount()).Key("maxItems").Unsigned(type.GetFixedCount());
        writer.EndObject();
        return;
    case TypeKind::Map:
        writer.BeginObject();
        if (IsStringKey(*type.GetKeyType()))
        {
            writer.Key("type").String("object").Key("additionalProperties");
            Write(writer, *type.GetElementType());
        }
        else
        {
            writer.Key("type").String("array").Key("items").BeginObject();
            writer.Key("type").String("array").Key("prefixItems").BeginArray();
            Write(writer, *type.GetKeyType());
            Write(writer, *type.GetElementType());
            writer.EndArray().Key("minItems").Unsigned(2).Key("maxItems").Unsigned(2).EndObject();
        }
        writer.EndObject();
        return;
    case TypeKind::Optional:
    case TypeKind::UniquePtr:
    case TypeKind::SharedPtr:
        writer.BeginObject().Key("anyOf").BeginArray();
        Write(writer, *type.GetElementType());
        writer.BeginObject().Key("type").String("null").EndObject();
        writer.EndArray().EndObject();
        return;
    case TypeKind::ObjectLink:
        writer.BeginObject().Key("type").BeginArray().String("string").String("null").EndArray();
        writer.Key("format").String("uuid").Key("description").String(Format("A {0}", type.GetName()));
        writer.EndObject();
        return;
    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::Class:
    case TypeKind::Interface:
        if (!_definitions.Contains(&type))
        {
            _definitions.Add(&type);
        }
        writer.BeginObject().Key("$ref").String(Format("#/$defs/{0}", type.GetFullName())).EndObject();
        return;
    default:
        writer.BeginObject().Key("type").String("string").EndObject();
        return;
    }
}

void JsonSchemaWriter::WriteDefinition(JsonWriter &writer, const TypeInfo &type)
{
    writer.BeginObject();
    const Optional<StringView> tooltip = type.GetMetadata().Get("Tooltip");
    if (tooltip.HasValue())
    {
        writer.Key("description").String(tooltip.Value());
    }
    if (type.GetKind() == TypeKind::Enum)
    {
        if (type.IsFlags())
        {
            writer.Key("type").String("string").Key("description").String("Enumerator names joined by '|'");
        }
        else
        {
            writer.Key("enum").BeginArray();
            for (const EnumeratorInfo &enumerator : type.GetEnumerators())
            {
                writer.String(enumerator.Name);
            }
            writer.EndArray();
        }
        writer.EndObject();
        return;
    }
    if (IsTextual(type))
    {
        writer.Key("type").String("string");
        if (&type == &TypeOf<Guid>())
        {
            writer.Key("format").String("uuid");
        }
        writer.EndObject();
        return;
    }
    if (IsTuple(type))
    {
        writer.Key("type").String("array").Key("prefixItems").BeginArray();
        for (const FieldInfo *field : type.GetFields())
        {
            Write(writer, field->GetType());
        }
        writer.EndArray().Key("minItems").Unsigned(type.GetFields().Count());
        writer.Key("maxItems").Unsigned(type.GetFields().Count()).EndObject();
        return;
    }
    writer.Key("type").String("object").Key("properties").BeginObject();
    for (const FieldInfo *field : type.GetFields())
    {
        if (IsTransient(*field))
        {
            continue;
        }
        writer.Key(field->GetName());
        const Optional<StringView> fieldTooltip = field->GetMetadata().Get("Tooltip");
        if (fieldTooltip.HasValue())
        {
            writer.BeginObject().Key("description").String(fieldTooltip.Value()).Key("allOf").BeginArray();
            Write(writer, field->GetType());
            writer.EndArray().EndObject();
        }
        else
        {
            Write(writer, field->GetType());
        }
    }
    writer.EndObject().Key("additionalProperties").Bool(false).EndObject();
}

void JsonSchemaWriter::WriteDefinitions(JsonWriter &writer)
{
    writer.Key("$defs").BeginObject();
    for (usize i = 0; i < _definitions.Count(); ++i)
    {
        const TypeInfo &type = *_definitions[i];
        writer.Key(type.GetFullName());
        WriteDefinition(writer, type);
    }
    writer.EndObject();
}

void JsonSchemaWriter::WriteDocument(JsonWriter &writer, const TypeInfo &type)
{
    JsonSchemaWriter schema;
    writer.BeginObject().Key("$schema").String("https://json-schema.org/draft/2020-12/schema").Key("allOf");
    writer.BeginArray();
    schema.Write(writer, type);
    writer.EndArray();
    schema.WriteDefinitions(writer);
    writer.EndObject();
}
} // namespace Marlin
