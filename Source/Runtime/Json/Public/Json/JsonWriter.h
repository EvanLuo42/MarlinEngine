#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/StringView.h"

namespace Marlin
{
class JsonValue;
class StringWriter;

/// Streams JSON into a StringWriter, adding the commas and colons; Key comes before each member's value.
class JSON_API JsonWriter
{
public:
    explicit JsonWriter(StringWriter &output) : _output(&output)
    {
    }

    JsonWriter &BeginObject();
    JsonWriter &EndObject();
    JsonWriter &BeginArray();
    JsonWriter &EndArray();
    JsonWriter &Key(StringView key);

    JsonWriter &Null();
    JsonWriter &Bool(bool value);
    JsonWriter &Integer(int64 value);
    JsonWriter &Unsigned(uint64 value);
    /// NaN and infinities have no JSON form and are written as null.
    JsonWriter &Number(float64 value);
    JsonWriter &String(StringView value);
    /// Writes *json*, already valid JSON, as one value.
    JsonWriter &Raw(StringView json);
    JsonWriter &Value(const JsonValue &value);

    [[nodiscard]] StringWriter &GetOutput() const
    {
        return *_output;
    }

    /// Writes *text* as a JSON string, quotes included.
    static void WriteString(StringWriter &output, StringView text);

private:
    /// Writes the comma before a value, unless it is the value of the key just written.
    void BeforeMember();
    void BeforeValue();

    StringWriter *_output;
    /// Per open container: true until its first element is written.
    InlineList<bool, 16> _first;
    bool _pendingKey = false;
};
} // namespace Marlin
