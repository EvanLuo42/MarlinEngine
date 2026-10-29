#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;
struct JsonMember;

enum class JsonKind : uint8
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

/// A parsed or built JSON value. Numbers keep their exact integer when the text was one, so 64-bit ids survive.
/// Object members keep their order; lookups are linear, which suits the small objects of messages.
class JSON_API JsonValue
{
public:
    JsonValue();
    ~JsonValue();
    JsonValue(const JsonValue &other);
    JsonValue(JsonValue &&other) noexcept;
    JsonValue &operator=(const JsonValue &other);
    JsonValue &operator=(JsonValue &&other) noexcept;

    [[nodiscard]] static JsonValue MakeBool(bool value);
    [[nodiscard]] static JsonValue MakeNumber(float64 value);
    [[nodiscard]] static JsonValue MakeInteger(int64 value);
    [[nodiscard]] static JsonValue MakeUnsigned(uint64 value);
    [[nodiscard]] static JsonValue MakeString(StringView value);
    [[nodiscard]] static JsonValue MakeArray();
    [[nodiscard]] static JsonValue MakeObject();

    [[nodiscard]] JsonKind GetKind() const
    {
        return _kind;
    }

    [[nodiscard]] bool IsNull() const
    {
        return _kind == JsonKind::Null;
    }

    [[nodiscard]] bool IsObject() const
    {
        return _kind == JsonKind::Object;
    }

    [[nodiscard]] bool IsArray() const
    {
        return _kind == JsonKind::Array;
    }

    [[nodiscard]] bool IsString() const
    {
        return _kind == JsonKind::String;
    }

    [[nodiscard]] bool IsNumber() const
    {
        return _kind == JsonKind::Number;
    }

    [[nodiscard]] bool AsBool(bool fallback = false) const;
    [[nodiscard]] float64 AsNumber(float64 fallback = 0) const;
    [[nodiscard]] StringView AsString() const;

    /// True if the number is integral and fits.
    [[nodiscard]] bool TryGetInteger(int64 &value) const;
    [[nodiscard]] bool TryGetUnsigned(uint64 &value) const;

    [[nodiscard]] Span<const JsonValue> GetElements() const;
    [[nodiscard]] Span<const JsonMember> GetMembers() const;
    [[nodiscard]] const JsonValue *Find(StringView key) const;
    [[nodiscard]] usize Count() const;

    /// Appends to an array.
    JsonValue &Add(JsonValue value);
    /// Sets a member of an object, replacing one with the same key.
    JsonValue &Set(StringView key, JsonValue value);

    /// Writes compact JSON.
    void FormatTo(StringWriter &output, StringView spec) const;
    [[nodiscard]] String ToString() const;

    bool operator==(const JsonValue &other) const;

private:
    /// Lets the parser append members whose keys it has checked itself.
    friend struct JsonValueBuilder;

    enum class NumberForm : uint8
    {
        Float,
        Signed,
        Unsigned,
    };

    JsonKind _kind = JsonKind::Null;
    NumberForm _form = NumberForm::Float;
    bool _bool = false;
    float64 _number = 0;
    uint64 _bits = 0;
    String _string;
    List<JsonValue> *_elements = nullptr;
    List<JsonMember> *_members = nullptr;
};

struct JsonMember
{
    String Key;
    JsonValue Value;
};

/// Where parsing stopped, 1-based.
struct JsonError
{
    uint32 Line = 0;
    uint32 Column = 0;
    String Message;
};

namespace Json
{
/// Parses one value per RFC 8259, with nothing but whitespace after it.
JSON_API bool Parse(StringView text, JsonValue &value, JsonError &error);
} // namespace Json
} // namespace Marlin
