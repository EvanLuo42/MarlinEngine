#include "Json/JsonValue.h"

#include "Containers/HashMap.h"
#include "Json/JsonWriter.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "String/Utf.h"

namespace Marlin
{
JsonValue::JsonValue() = default;

JsonValue::~JsonValue()
{
    delete _elements;
    delete _members;
}

JsonValue::JsonValue(const JsonValue &other)
    : _kind(other._kind), _form(other._form), _bool(other._bool), _number(other._number), _bits(other._bits),
      _string(other._string)
{
    if (other._elements != nullptr)
    {
        _elements = new List<JsonValue>(*other._elements);
    }
    if (other._members != nullptr)
    {
        _members = new List<JsonMember>(*other._members);
    }
}

JsonValue::JsonValue(JsonValue &&other) noexcept
    : _kind(other._kind), _form(other._form), _bool(other._bool), _number(other._number), _bits(other._bits),
      _string(Move(other._string)), _elements(Exchange(other._elements, nullptr)),
      _members(Exchange(other._members, nullptr))
{
    other._kind = JsonKind::Null;
}

JsonValue &JsonValue::operator=(const JsonValue &other)
{
    if (this != &other)
    {
        *this = JsonValue(other);
    }
    return *this;
}

JsonValue &JsonValue::operator=(JsonValue &&other) noexcept
{
    if (this != &other)
    {
        delete _elements;
        delete _members;
        _kind = Exchange(other._kind, JsonKind::Null);
        _form = other._form;
        _bool = other._bool;
        _number = other._number;
        _bits = other._bits;
        _string = Move(other._string);
        _elements = Exchange(other._elements, nullptr);
        _members = Exchange(other._members, nullptr);
    }
    return *this;
}

JsonValue JsonValue::MakeBool(bool value)
{
    JsonValue result;
    result._kind = JsonKind::Bool;
    result._bool = value;
    return result;
}

JsonValue JsonValue::MakeNumber(float64 value)
{
    JsonValue result;
    result._kind = JsonKind::Number;
    result._number = value;
    return result;
}

JsonValue JsonValue::MakeInteger(int64 value)
{
    JsonValue result;
    result._kind = JsonKind::Number;
    result._form = NumberForm::Signed;
    result._bits = static_cast<uint64>(value);
    result._number = static_cast<float64>(value);
    return result;
}

JsonValue JsonValue::MakeUnsigned(uint64 value)
{
    JsonValue result;
    result._kind = JsonKind::Number;
    result._form = NumberForm::Unsigned;
    result._bits = value;
    result._number = static_cast<float64>(value);
    return result;
}

JsonValue JsonValue::MakeString(StringView value)
{
    JsonValue result;
    result._kind = JsonKind::String;
    result._string = value;
    return result;
}

JsonValue JsonValue::MakeArray()
{
    JsonValue result;
    result._kind = JsonKind::Array;
    result._elements = new List<JsonValue>();
    return result;
}

JsonValue JsonValue::MakeObject()
{
    JsonValue result;
    result._kind = JsonKind::Object;
    result._members = new List<JsonMember>();
    return result;
}

bool JsonValue::AsBool(bool fallback) const
{
    return _kind == JsonKind::Bool ? _bool : fallback;
}

float64 JsonValue::AsNumber(float64 fallback) const
{
    return _kind == JsonKind::Number ? _number : fallback;
}

StringView JsonValue::AsString() const
{
    return _kind == JsonKind::String ? _string.AsView() : StringView();
}

bool JsonValue::TryGetInteger(int64 &value) const
{
    if (_kind != JsonKind::Number)
    {
        return false;
    }
    switch (_form)
    {
    case NumberForm::Signed:
        value = static_cast<int64>(_bits);
        return true;
    case NumberForm::Unsigned:
        if (_bits > static_cast<uint64>(0x7FFFFFFFFFFFFFFF))
        {
            return false;
        }
        value = static_cast<int64>(_bits);
        return true;
    default:
        if (_number != __builtin_trunc(_number) || _number < -9.2233720368547758e18 || _number >= 9.2233720368547758e18)
        {
            return false;
        }
        value = static_cast<int64>(_number);
        return true;
    }
}

bool JsonValue::TryGetUnsigned(uint64 &value) const
{
    if (_kind != JsonKind::Number)
    {
        return false;
    }
    switch (_form)
    {
    case NumberForm::Unsigned:
        value = _bits;
        return true;
    case NumberForm::Signed:
        if (static_cast<int64>(_bits) < 0)
        {
            return false;
        }
        value = _bits;
        return true;
    default:
        if (_number != __builtin_trunc(_number) || _number < 0 || _number >= 1.8446744073709552e19)
        {
            return false;
        }
        value = static_cast<uint64>(_number);
        return true;
    }
}

Span<const JsonValue> JsonValue::GetElements() const
{
    return _elements != nullptr ? _elements->AsSpan() : Span<const JsonValue>();
}

Span<const JsonMember> JsonValue::GetMembers() const
{
    return _members != nullptr ? _members->AsSpan() : Span<const JsonMember>();
}

const JsonValue *JsonValue::Find(StringView key) const
{
    if (_members == nullptr)
    {
        return nullptr;
    }
    for (const JsonMember &member : *_members)
    {
        if (member.Key == key)
        {
            return &member.Value;
        }
    }
    return nullptr;
}

usize JsonValue::Count() const
{
    if (_elements != nullptr)
    {
        return _elements->Count();
    }
    return _members != nullptr ? _members->Count() : 0;
}

JsonValue &JsonValue::Add(JsonValue value)
{
    MARLIN_ASSERT(_kind == JsonKind::Array);
    return _elements->Add(Move(value));
}

JsonValue &JsonValue::Set(StringView key, JsonValue value)
{
    MARLIN_ASSERT(_kind == JsonKind::Object);
    for (JsonMember &member : *_members)
    {
        if (member.Key == key)
        {
            member.Value = Move(value);
            return member.Value;
        }
    }
    JsonMember &member = _members->AddDefaulted();
    member.Key = key;
    member.Value = Move(value);
    return member.Value;
}

void JsonValue::FormatTo(StringWriter &output, StringView /*spec*/) const
{
    JsonWriter writer(output);
    writer.Value(*this);
}

String JsonValue::ToString() const
{
    StringBuilder<256> text;
    FormatTo(text, {});
    return text.ToString();
}

bool JsonValue::operator==(const JsonValue &other) const
{
    if (_kind != other._kind)
    {
        return false;
    }
    switch (_kind)
    {
    case JsonKind::Null:
        return true;
    case JsonKind::Bool:
        return _bool == other._bool;
    case JsonKind::Number: {
        if (_form != NumberForm::Float && other._form != NumberForm::Float)
        {
            return _bits == other._bits && (_form == other._form || static_cast<int64>(_bits) >= 0);
        }
        return _number == other._number;
    }
    case JsonKind::String:
        return _string == other._string;
    case JsonKind::Array: {
        const Span<const JsonValue> a = GetElements();
        const Span<const JsonValue> b = other.GetElements();
        if (a.Count() != b.Count())
        {
            return false;
        }
        for (usize i = 0; i < a.Count(); ++i)
        {
            if (!(a[i] == b[i]))
            {
                return false;
            }
        }
        return true;
    }
    case JsonKind::Object: {
        if (Count() != other.Count())
        {
            return false;
        }
        for (const JsonMember &member : GetMembers())
        {
            const JsonValue *value = other.Find(member.Key);
            if (value == nullptr || !(*value == member.Value))
            {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

struct JsonValueBuilder
{
    static List<JsonMember> &MembersOf(JsonValue &object)
    {
        return *object._members;
    }
};

namespace
{
constexpr uint32 MaxDepth = 512;

class Parser
{
public:
    Parser(StringView text, JsonError &error) : _text(text), _error(&error)
    {
    }

    bool ParseDocument(JsonValue &value)
    {
        SkipWhitespace();
        if (!ParseValue(value, 0))
        {
            return false;
        }
        SkipWhitespace();
        return _position == _text.Length() || Fail("unexpected text after the value");
    }

private:
    bool Fail(StringView message)
    {
        _error->Line = 1;
        _error->Column = 1;
        for (usize i = 0; i < _position && i < _text.Length(); ++i)
        {
            if (_text[i] == '\n')
            {
                ++_error->Line;
                _error->Column = 1;
            }
            else
            {
                ++_error->Column;
            }
        }
        _error->Message = message;
        return false;
    }

    [[nodiscard]] bool AtEnd() const
    {
        return _position >= _text.Length();
    }

    [[nodiscard]] char Peek() const
    {
        return AtEnd() ? '\0' : _text[_position];
    }

    void SkipWhitespace()
    {
        while (!AtEnd())
        {
            const char c = _text[_position];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                return;
            }
            ++_position;
        }
    }

    bool Expect(StringView word)
    {
        if (_text.Substring(_position).StartsWith(word))
        {
            _position += word.Length();
            return true;
        }
        return Fail("invalid literal");
    }

    bool ParseValue(JsonValue &value, uint32 depth)
    {
        if (depth > MaxDepth)
        {
            return Fail("nesting is too deep");
        }
        switch (Peek())
        {
        case '{':
            return ParseObject(value, depth);
        case '[':
            return ParseArray(value, depth);
        case '"': {
            String text;
            if (!ParseString(text))
            {
                return false;
            }
            value = JsonValue::MakeString(text);
            return true;
        }
        case 't':
            value = JsonValue::MakeBool(true);
            return Expect("true");
        case 'f':
            value = JsonValue::MakeBool(false);
            return Expect("false");
        case 'n':
            value = JsonValue();
            return Expect("null");
        default:
            if (Peek() == '-' || Ascii::IsDigit(Peek()))
            {
                return ParseNumber(value);
            }
            return Fail(AtEnd() ? StringView("unexpected end of text") : StringView("unexpected character"));
        }
    }

    bool ParseObject(JsonValue &value, uint32 depth)
    {
        ++_position;
        value = JsonValue::MakeObject();
        SkipWhitespace();
        if (Peek() == '}')
        {
            ++_position;
            return true;
        }
        // A repeated key replaces the earlier value in its place, as Set does. Small objects look for it by scanning;
        // larger ones index their keys.
        constexpr usize ScanLimit = 16;
        HashMap<String, usize> indices;
        while (true)
        {
            SkipWhitespace();
            if (Peek() != '"')
            {
                return Fail("expected a member name");
            }
            String key;
            if (!ParseString(key))
            {
                return false;
            }
            SkipWhitespace();
            if (Peek() != ':')
            {
                return Fail("expected ':'");
            }
            ++_position;
            SkipWhitespace();
            JsonValue member;
            if (!ParseValue(member, depth + 1))
            {
                return false;
            }
            List<JsonMember> &members = JsonValueBuilder::MembersOf(value);
            usize existing = members.Count();
            if (members.Count() <= ScanLimit)
            {
                for (usize i = 0; i < members.Count() && existing == members.Count(); ++i)
                {
                    existing = members[i].Key == key ? i : existing;
                }
            }
            else
            {
                if (indices.IsEmpty())
                {
                    for (usize i = 0; i < members.Count(); ++i)
                    {
                        indices.Set(members[i].Key, i);
                    }
                }
                if (const usize *found = indices.Find(key.AsView()))
                {
                    existing = *found;
                }
                else
                {
                    indices.Set(key, members.Count());
                }
            }
            if (existing != members.Count())
            {
                members[existing].Value = Move(member);
            }
            else
            {
                JsonMember &added = members.AddDefaulted();
                added.Key = Move(key);
                added.Value = Move(member);
            }
            SkipWhitespace();
            if (Peek() == ',')
            {
                ++_position;
                continue;
            }
            if (Peek() == '}')
            {
                ++_position;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(JsonValue &value, uint32 depth)
    {
        ++_position;
        value = JsonValue::MakeArray();
        SkipWhitespace();
        if (Peek() == ']')
        {
            ++_position;
            return true;
        }
        while (true)
        {
            SkipWhitespace();
            JsonValue element;
            if (!ParseValue(element, depth + 1))
            {
                return false;
            }
            value.Add(Move(element));
            SkipWhitespace();
            if (Peek() == ',')
            {
                ++_position;
                continue;
            }
            if (Peek() == ']')
            {
                ++_position;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseHex4(uint32 &value)
    {
        value = 0;
        for (int32 i = 0; i < 4; ++i)
        {
            const char c = Peek();
            if (!Ascii::IsHexDigit(c))
            {
                return Fail("invalid \\u escape");
            }
            value = (value << 4) | static_cast<uint32>(Ascii::DigitValue(c));
            ++_position;
        }
        return true;
    }

    bool ParseString(String &text)
    {
        ++_position;
        StringBuilder<128> builder;
        while (true)
        {
            if (AtEnd())
            {
                return Fail("unterminated string");
            }
            const char c = _text[_position++];
            if (c == '"')
            {
                text = builder.AsView();
                return true;
            }
            if (static_cast<uint8>(c) < 0x20)
            {
                return Fail("control character in a string");
            }
            if (c != '\\')
            {
                builder.Append(c);
                continue;
            }
            const char escape = Peek();
            ++_position;
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                builder.Append(escape);
                break;
            case 'b':
                builder.Append('\b');
                break;
            case 'f':
                builder.Append('\f');
                break;
            case 'n':
                builder.Append('\n');
                break;
            case 'r':
                builder.Append('\r');
                break;
            case 't':
                builder.Append('\t');
                break;
            case 'u': {
                uint32 unit = 0;
                if (!ParseHex4(unit))
                {
                    return false;
                }
                char32_t codePoint = unit;
                if (unit >= 0xD800 && unit <= 0xDBFF)
                {
                    uint32 low = 0;
                    if (!Expect("\\u") || !ParseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                    {
                        return Fail("unpaired surrogate");
                    }
                    codePoint = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (unit >= 0xDC00 && unit <= 0xDFFF)
                {
                    return Fail("unpaired surrogate");
                }
                char encoded[4];
                builder.Append(StringView(encoded, Utf8::Encode(codePoint, encoded)));
                break;
            }
            default:
                return Fail("invalid escape");
            }
        }
    }

    bool ParseNumber(JsonValue &value)
    {
        const usize start = _position;
        const bool negative = Peek() == '-';
        if (negative)
        {
            ++_position;
        }
        if (Peek() == '0')
        {
            ++_position;
        }
        else if (Ascii::IsDigit(Peek()))
        {
            while (Ascii::IsDigit(Peek()))
            {
                ++_position;
            }
        }
        else
        {
            return Fail("invalid number");
        }
        bool integral = true;
        if (Peek() == '.')
        {
            integral = false;
            ++_position;
            if (!Ascii::IsDigit(Peek()))
            {
                return Fail("invalid number");
            }
            while (Ascii::IsDigit(Peek()))
            {
                ++_position;
            }
        }
        if (Peek() == 'e' || Peek() == 'E')
        {
            integral = false;
            ++_position;
            if (Peek() == '+' || Peek() == '-')
            {
                ++_position;
            }
            if (!Ascii::IsDigit(Peek()))
            {
                return Fail("invalid number");
            }
            while (Ascii::IsDigit(Peek()))
            {
                ++_position;
            }
        }
        const StringView text = _text.Substring(start, _position - start);
        if (integral)
        {
            int64 signedValue = 0;
            uint64 unsignedValue = 0;
            if (negative && TryParse(text, signedValue))
            {
                value = JsonValue::MakeInteger(signedValue);
                return true;
            }
            if (!negative && TryParse(text, unsignedValue))
            {
                value = JsonValue::MakeUnsigned(unsignedValue);
                return true;
            }
        }
        float64 number = 0;
        if (!TryParse(text, number))
        {
            return Fail("invalid number");
        }
        value = JsonValue::MakeNumber(number);
        return true;
    }

    StringView _text;
    usize _position = 0;
    JsonError *_error;
};
} // namespace

namespace Json
{
bool Parse(StringView text, JsonValue &value, JsonError &error)
{
    Parser parser(text, error);
    value = JsonValue();
    return parser.ParseDocument(value);
}
} // namespace Json
} // namespace Marlin
