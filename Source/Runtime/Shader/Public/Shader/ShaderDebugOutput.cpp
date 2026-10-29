#include "Shader/ShaderDebugOutput.h"

#include "Hash/Xxh3.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace
{
/// The kinds of a record's argument descriptor (MirDebugArgument).
enum class ArgumentKind : uint8
{
    Unsigned,
    Signed,
    Float,
    Bool,
};

struct Argument
{
    ArgumentKind Kind = ArgumentKind::Unsigned;
    uint32 Bits = 32;
    uint32 Components = 1;
    /// The argument's first component word in the record.
    uint32 First = 0;
};

void WriteComponent(StringWriter &output, Span<const uint32> record, const Argument &argument, uint32 component,
                    bool hexadecimal)
{
    const uint32 words = argument.Bits == 64 ? 2 : 1;
    const uint32 at = argument.First + component * words;
    const uint64 bits = words == 2 ? uint64{record[at]} | (uint64{record[at + 1]} << 32) : uint64{record[at]};
    switch (argument.Kind)
    {
    case ArgumentKind::Bool:
        output.Append(bits != 0 ? "true" : "false");
        return;
    case ArgumentKind::Float:
        if (words == 2)
        {
            FormatTo(output, "{0}", BitCast<float64>(bits));
        }
        else
        {
            FormatTo(output, "{0}", BitCast<float32>(static_cast<uint32>(bits)));
        }
        return;
    case ArgumentKind::Signed:
        if (!hexadecimal)
        {
            if (words == 2)
            {
                FormatTo(output, "{0}", static_cast<int64>(bits));
            }
            else
            {
                FormatTo(output, "{0}", static_cast<int32>(static_cast<uint32>(bits)));
            }
            return;
        }
        break;
    case ArgumentKind::Unsigned:
        break;
    }
    if (hexadecimal)
    {
        FormatTo(output, "{0:x}", bits);
    }
    else
    {
        FormatTo(output, "{0}", bits);
    }
}

void WriteArgument(StringWriter &output, Span<const uint32> record, const Argument &argument, bool hexadecimal)
{
    if (argument.Components == 1)
    {
        WriteComponent(output, record, argument, 0, hexadecimal);
        return;
    }
    output.Append('(');
    for (uint32 component = 0; component < argument.Components; ++component)
    {
        output.Append(component == 0 ? "" : ", ");
        WriteComponent(output, record, argument, component, hexadecimal);
    }
    output.Append(')');
}

/// Writes *format* with the arguments of *record*; false if the format names an argument the record lacks.
bool WriteFormatted(StringWriter &output, StringView format, Span<const uint32> record, Span<const Argument> arguments)
{
    for (usize i = 0; i < format.Length(); ++i)
    {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.Length() && format[i + 1] == c)
        {
            output.Append(c);
            ++i;
            continue;
        }
        if (c != '{')
        {
            output.Append(c);
            continue;
        }
        uint32 index = 0;
        usize at = i + 1;
        while (at < format.Length() && format[at] >= '0' && format[at] <= '9')
        {
            index = index * 10 + static_cast<uint32>(format[at++] - '0');
        }
        const bool hexadecimal = at + 1 < format.Length() && format[at] == ':' && format[at + 1] == 'x';
        at += hexadecimal ? 2 : 0;
        if (at >= format.Length() || format[at] != '}' || index >= arguments.Count())
        {
            return false;
        }
        WriteArgument(output, record, arguments[index], hexadecimal);
        i = at;
    }
    return true;
}
} // namespace

uint32 GetShaderDebugFormatHash(StringView format)
{
    return static_cast<uint32>(Xxh3Hash64(format.Data(), format.Length()));
}

uint32 ReadShaderDebugOutput(Span<const uint32> words, const HashMap<uint32, String> &formats,
                             List<ShaderDebugMessage> &messages)
{
    if (words.Count() < ShaderDebugHeaderWords)
    {
        return 0;
    }
    const uint32 written = words[ShaderDebugCount];
    const uint32 capacity = words[ShaderDebugCapacity];
    const uint32 end = Min(Min(written, capacity), static_cast<uint32>(words.Count() - ShaderDebugHeaderWords));
    const Span<const uint32> records = words.Slice(ShaderDebugHeaderWords, end);
    List<Argument> arguments;
    for (uint32 at = 0; at + 2 <= records.Count();)
    {
        const uint32 length = records[at];
        if (length < 2 || at + length > records.Count())
        {
            break;
        }
        const Span<const uint32> record = records.Slice(at, length);
        at += length;
        arguments.Clear();
        bool valid = true;
        for (uint32 word = 2; word < length && valid;)
        {
            Argument &argument = arguments.AddDefaulted();
            argument.Kind = static_cast<ArgumentKind>(record[word] & 0xFFu);
            argument.Bits = (record[word] >> 8) & 0xFFu;
            argument.Components = (record[word] >> 16) & 0xFFu;
            argument.First = word + 1;
            word += 1 + argument.Components * (argument.Bits == 64 ? 2 : 1);
            valid = argument.Kind <= ArgumentKind::Bool && argument.Components >= 1 && argument.Components <= 4 &&
                    word <= length;
        }
        ShaderDebugMessage &message = messages.AddDefaulted();
        StringBuilder<512> text;
        const String *format = formats.Find(record[1]);
        if (format == nullptr || !valid || !WriteFormatted(text, format->AsView(), record, arguments.AsSpan()))
        {
            text.Clear();
            FormatTo(text, "debug output of an unknown format {0:x}", record[1]);
        }
        else
        {
            // Debug.Assert's formats read `file:line: assertion failed: message` (§9.11).
            message.Assertion = format->AsView().Contains(": assertion failed: ");
        }
        message.Text = String(text.AsView());
    }
    return written > capacity ? written - capacity : 0;
}
} // namespace Marlin
