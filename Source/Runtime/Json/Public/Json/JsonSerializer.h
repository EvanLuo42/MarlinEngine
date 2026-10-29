#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Json/JsonValue.h"
#include "Json/JsonWriter.h"
#include "Reflection/TypeInfo.h"
#include "Reflection/TypeOf.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;

/// Reflected values as JSON. Structs and classes are objects of their fields, Transient ones left out; a struct
/// marked Tuple is an array; enums are their enumerator names, flags joined by '|'; a type with FormatTo and
/// TryParse, such as Guid, is a string; object links are their Guid; UniquePtr and SharedPtr are objects with a
/// "$type" member, or null. 64-bit integers beyond 2^53 are written as numbers all the same and read back exactly.
namespace Json
{
JSON_API void WriteValue(JsonWriter &writer, const TypeInfo &type, const void *value);

/// Reads *json* into *target*, which holds a valid value of *type*. A StringView points into *json*, which must
/// outlive it. On failure *error* says where, such as "Settings.Width: expected a number".
JSON_API bool ReadValue(const JsonValue &json, const TypeInfo &type, void *target, StringWriter &error);

template <typename T> void Write(JsonWriter &writer, const T &value)
{
    WriteValue(writer, TypeOf<T>(), &value);
}

template <typename T> bool Read(const JsonValue &json, T &value, StringWriter &error)
{
    return ReadValue(json, TypeOf<T>(), &value, error);
}
} // namespace Json

/// Writes JSON Schema (2020-12) for reflected types. Structs, classes and enums go to "$defs" and are referenced,
/// so recursive types work; WriteDefinitions writes the "$defs" member once every schema that refers to them has
/// been written. Tooltip metadata becomes "description".
class JSON_API JsonSchemaWriter
{
public:
    /// Writes the schema of *type* as one value.
    void Write(JsonWriter &writer, const TypeInfo &type);
    /// Writes the key "$defs" and its object.
    void WriteDefinitions(JsonWriter &writer);

    /// A complete schema document for *type*.
    static void WriteDocument(JsonWriter &writer, const TypeInfo &type);

private:
    void WriteDefinition(JsonWriter &writer, const TypeInfo &type);

    List<const TypeInfo *> _definitions;
};
} // namespace Marlin
