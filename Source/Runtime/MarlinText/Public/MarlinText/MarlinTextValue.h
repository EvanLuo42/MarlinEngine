#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "String/StringView.h"

namespace Marlin
{
struct MarlinTextField;
struct MarlinTextCondition;

enum class MarlinTextValueKind : uint8
{
    Null,
    Bool,
    /// `-3`, `0x1F`.
    Integer,
    /// `1.5`, `1e-3`.
    Float,
    /// `"text"`, with escapes decoded.
    String,
    /// An enumerator or flags: `BC7`, `Cheat|Scalability`.
    Identifier,
    /// `(1, 0, 0)`.
    Tuple,
    /// `[a, b]`.
    Array,
    /// `{ Key = value }`: a struct or a map.
    Block,
    /// `TypeName { Key = value }`: an object of a named type, such as one a pointer owns.
    Object,
    /// `<Path:Name#UUID>`; the UUID decides, the path is a hint for people.
    ObjectLink,
    /// `` `source:Path#GUID` ``.
    BinaryLink,
};

/// A parsed value. Text points into the document's source or arena and lives as long as the document.
struct MarlinTextValue
{
    MarlinTextValueKind Kind = MarlinTextValueKind::Null;
    /// The value as written.
    StringView Source;
    bool Bool = false;
    int64 Integer = 0;
    float64 Float = 0;
    /// The decoded string, the identifier, the object's type name, or a link's path.
    StringView Text;
    /// A link's object name.
    StringView LinkName;
    Guid LinkGuid;
    /// Tuple and array elements.
    Span<const MarlinTextValue> Elements;
    /// Block and object fields.
    Span<const MarlinTextField> Fields;
    uint32 Line = 0;

    [[nodiscard]] bool IsNumber() const
    {
        return Kind == MarlinTextValueKind::Integer || Kind == MarlinTextValueKind::Float;
    }

    [[nodiscard]] float64 AsFloat() const
    {
        return Kind == MarlinTextValueKind::Integer ? static_cast<float64>(Integer) : Float;
    }

    /// The field *key* of a block or object, if it is set unconditionally.
    [[nodiscard]] const MarlinTextField *FindField(StringView key) const;
};

enum class MarlinTextOperation : uint8
{
    /// `=`
    Assign,
    /// `+=`, adding to an array.
    Append,
    /// `-=`, removing from an array.
    Remove,
};

struct MarlinTextField
{
    /// The key without the `!` of a directive; a string or number key of a map block is kept decoded.
    StringView Key;
    MarlinTextOperation Operation = MarlinTextOperation::Assign;
    /// `!UUID`, `!Name`, `!Schema` and other directives describe the object rather than its type.
    bool IsDirective = false;
    MarlinTextValue Value;
    /// The `@if` the field is inside, and whether it is in the `@else`; null for an unconditional field.
    const MarlinTextCondition *Condition = nullptr;
    bool InElse = false;
    uint32 Line = 0;
    /// The source offsets of the field and of its value, for edits.
    uint32 Begin = 0;
    uint32 End = 0;
    uint32 ValueBegin = 0;
    uint32 ValueEnd = 0;
};

/// `@if Variable == Value { ... } @else { ... }`, allowed in config only. Conditions nest through Parent.
struct MarlinTextCondition
{
    StringView Variable;
    StringView Value;
    bool Equal = true;
    const MarlinTextCondition *Parent = nullptr;
    bool ParentElse = false;
    uint32 Line = 0;
};

/// The variables conditions test, such as Platform and Configuration; a null result is an unknown variable.
class MarlinTextConditions
{
public:
    struct Variable
    {
        StringView Name;
        StringView Value;
    };

    constexpr MarlinTextConditions() = default;

    constexpr explicit MarlinTextConditions(Span<const Variable> variables) : _variables(variables)
    {
    }

    [[nodiscard]] const StringView *Find(StringView name) const
    {
        for (const Variable &variable : _variables)
        {
            if (variable.Name == name)
            {
                return &variable.Value;
            }
        }
        return nullptr;
    }

    /// True if *condition* and those around it hold; unknown variables never hold.
    [[nodiscard]] bool Holds(const MarlinTextCondition *condition, bool inElse) const
    {
        for (; condition != nullptr; inElse = condition->ParentElse, condition = condition->Parent)
        {
            const StringView *value = Find(condition->Variable);
            const bool test = value != nullptr && (*value == condition->Value) == condition->Equal;
            if (test == inElse)
            {
                return false;
            }
        }
        return true;
    }

private:
    Span<const Variable> _variables;
};

inline const MarlinTextField *MarlinTextValue::FindField(StringView key) const
{
    for (const MarlinTextField &field : Fields)
    {
        if (field.Condition == nullptr && !field.IsDirective && field.Key == key)
        {
            return &field;
        }
    }
    return nullptr;
}
} // namespace Marlin
