#include "AssetPipeline/AssetCanonical.h"

#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextValue.h"
#include "Misc/Guid.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin::AssetCanonical
{
namespace
{
void WriteGuid(StringWriter &output, const Guid &guid)
{
    guid.FormatTo(output, "N");
}

void WriteFields(StringWriter &output, Span<const MarlinTextField> fields)
{
    output.Append('{');
    for (const MarlinTextField &field : fields)
    {
        output.Append(field.IsDirective ? "!" : "");
        WriteText(output, field.Key);
        output.Append(field.Operation == MarlinTextOperation::Append   ? "+="
                      : field.Operation == MarlinTextOperation::Remove ? "-="
                                                                       : "=");
        WriteValue(output, field.Value);
        output.Append(';');
    }
    output.Append('}');
}
} // namespace

void WriteNumber(StringWriter &output, uint64 value)
{
    char digits[24];
    output.Append(StringView(digits, FormatUnsigned(value, digits)));
}

void WriteText(StringWriter &output, StringView text)
{
    WriteNumber(output, text.Length());
    output.Append(':').Append(text);
}

void WriteValue(StringWriter &output, const MarlinTextValue &value)
{
    switch (value.Kind)
    {
    case MarlinTextValueKind::Null:
        output.Append('n');
        return;
    case MarlinTextValueKind::Bool:
        output.Append(value.Bool ? 't' : 'f');
        return;
    case MarlinTextValueKind::Integer: {
        char digits[24];
        output.Append('i').Append(StringView(digits, FormatSigned(value.Integer, digits)));
        return;
    }
    case MarlinTextValueKind::Float:
        output.Append('d');
        AppendFloat(output, value.Float);
        return;
    case MarlinTextValueKind::String:
        output.Append('s');
        WriteText(output, value.Text);
        return;
    case MarlinTextValueKind::Identifier:
        output.Append('e');
        WriteText(output, value.Text);
        return;
    case MarlinTextValueKind::Tuple:
    case MarlinTextValueKind::Array:
        output.Append(value.Kind == MarlinTextValueKind::Tuple ? '(' : '[');
        for (const MarlinTextValue &element : value.Elements)
        {
            WriteValue(output, element);
            output.Append(',');
        }
        output.Append(value.Kind == MarlinTextValueKind::Tuple ? ')' : ']');
        return;
    case MarlinTextValueKind::Block:
        WriteFields(output, value.Fields);
        return;
    case MarlinTextValueKind::Object:
        output.Append('o');
        WriteText(output, value.Text);
        WriteFields(output, value.Fields);
        return;
    case MarlinTextValueKind::ObjectLink:
        output.Append('<');
        WriteGuid(output, value.LinkGuid);
        output.Append('>');
        return;
    case MarlinTextValueKind::BinaryLink:
        output.Append('`');
        WriteGuid(output, value.LinkGuid);
        output.Append('`');
        return;
    }
}

void Write(const MarlinTextObject &object, StringWriter &output)
{
    output.Append('o');
    WriteText(output, object.GetTypeName());
    if (const MarlinTextValue *parent = object.GetParent())
    {
        output.Append(':');
        WriteValue(output, *parent);
    }
    WriteFields(output, object.GetFields());
}

Hash128 Hash(const MarlinTextObject &object)
{
    StringBuilder<1024> text;
    Write(object, text);
    return Xxh3Hash128(text.Data(), text.Length());
}
} // namespace Marlin::AssetCanonical
