#include "Json/JsonWriter.h"

#include "Json/JsonValue.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
void JsonWriter::BeforeMember()
{
    if (_pendingKey)
    {
        _pendingKey = false;
        return;
    }
    BeforeValue();
}

void JsonWriter::BeforeValue()
{
    if (_first.IsEmpty())
    {
        return;
    }
    bool &first = _first.Last();
    if (first)
    {
        first = false;
    }
    else
    {
        _output->Append(',');
    }
}

JsonWriter &JsonWriter::BeginObject()
{
    BeforeMember();
    _output->Append('{');
    _first.Add(true);
    return *this;
}

JsonWriter &JsonWriter::EndObject()
{
    MARLIN_ASSERT(!_first.IsEmpty());
    (void)_first.Pop();
    _output->Append('}');
    return *this;
}

JsonWriter &JsonWriter::BeginArray()
{
    BeforeMember();
    _output->Append('[');
    _first.Add(true);
    return *this;
}

JsonWriter &JsonWriter::EndArray()
{
    MARLIN_ASSERT(!_first.IsEmpty());
    (void)_first.Pop();
    _output->Append(']');
    return *this;
}

JsonWriter &JsonWriter::Key(StringView key)
{
    BeforeValue();
    WriteString(*_output, key);
    _output->Append(':');
    _pendingKey = true;
    return *this;
}

JsonWriter &JsonWriter::Null()
{
    BeforeMember();
    _output->Append("null");
    return *this;
}

JsonWriter &JsonWriter::Bool(bool value)
{
    BeforeMember();
    _output->Append(value ? "true" : "false");
    return *this;
}

JsonWriter &JsonWriter::Integer(int64 value)
{
    BeforeMember();
    char digits[24];
    _output->Append(StringView(digits, FormatSigned(value, digits)));
    return *this;
}

JsonWriter &JsonWriter::Unsigned(uint64 value)
{
    BeforeMember();
    char digits[24];
    _output->Append(StringView(digits, FormatUnsigned(value, digits)));
    return *this;
}

JsonWriter &JsonWriter::Number(float64 value)
{
    BeforeMember();
    if (!__builtin_isfinite(value))
    {
        _output->Append("null");
        return *this;
    }
    AppendFloat(*_output, value);
    return *this;
}

JsonWriter &JsonWriter::String(StringView value)
{
    BeforeMember();
    WriteString(*_output, value);
    return *this;
}

JsonWriter &JsonWriter::Raw(StringView json)
{
    BeforeMember();
    _output->Append(json);
    return *this;
}

JsonWriter &JsonWriter::Value(const JsonValue &value)
{
    switch (value.GetKind())
    {
    case JsonKind::Null:
        return Null();
    case JsonKind::Bool:
        return Bool(value.AsBool());
    case JsonKind::Number: {
        int64 integer = 0;
        uint64 unsignedValue = 0;
        if (value.TryGetInteger(integer) && static_cast<float64>(integer) == value.AsNumber())
        {
            return Integer(integer);
        }
        if (value.TryGetUnsigned(unsignedValue))
        {
            return Unsigned(unsignedValue);
        }
        return Number(value.AsNumber());
    }
    case JsonKind::String:
        return String(value.AsString());
    case JsonKind::Array:
        BeginArray();
        for (const JsonValue &element : value.GetElements())
        {
            Value(element);
        }
        return EndArray();
    case JsonKind::Object:
        BeginObject();
        for (const JsonMember &member : value.GetMembers())
        {
            Key(member.Key);
            Value(member.Value);
        }
        return EndObject();
    }
    return *this;
}

void JsonWriter::WriteString(StringWriter &output, StringView text)
{
    output.Append('"');
    usize start = 0;
    for (usize i = 0; i < text.Length(); ++i)
    {
        const auto c = static_cast<uint8>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
        {
            continue;
        }
        output.Append(text.Substring(start, i - start));
        start = i + 1;
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
        case '\r':
            output.Append("\\r");
            break;
        case '\t':
            output.Append("\\t");
            break;
        default: {
            constexpr char Hex[] = "0123456789abcdef";
            const char escape[] = {'\\', 'u', '0', '0', Hex[c >> 4], Hex[c & 0xF]};
            output.Append(StringView(escape, sizeof(escape)));
            break;
        }
        }
    }
    output.Append(text.Substring(start));
    output.Append('"');
}
} // namespace Marlin
