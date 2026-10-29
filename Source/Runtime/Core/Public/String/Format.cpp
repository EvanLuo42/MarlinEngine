#include "String/Format.h"

#include "String/Number.h"

namespace Marlin
{
namespace
{
using Private::FormatArg;
using Private::FormatArgType;

/// A standard spec: a letter and an optional number.
struct Spec
{
    char Letter = 0;
    bool Upper = true;
    int32 Precision = -1;
};

Spec ParseSpec(StringView text)
{
    Spec spec;
    if (text.IsEmpty() || !Ascii::IsLetter(text[0]))
    {
        return spec;
    }
    int32 precision = -1;
    if (text.Length() > 1 && !TryParse(text.Substring(1), precision))
    {
        return spec;
    }
    spec.Letter = Ascii::ToUpper(text[0]);
    spec.Upper = Ascii::IsUpper(text[0]);
    spec.Precision = precision;
    return spec;
}

int32 PrecisionOr(const Spec &spec, int32 fallback)
{
    return spec.Precision >= 0 ? spec.Precision : fallback;
}

/// Inserts ',' between groups of three digits in the integer part written since `start`.
void GroupDigits(StringWriter &output, usize start)
{
    const StringView written = output.AsView().Substring(start);
    const usize digitsStart = written.StartsWith('-') ? 1 : 0;
    usize digitsEnd = written.IndexOf('.');
    digitsEnd = digitsEnd == StringView::NotFound ? written.Length() : digitsEnd;
    const usize digitCount = digitsEnd - digitsStart;
    if (digitCount <= 3)
    {
        return;
    }
    const usize separators = (digitCount - 1) / 3;
    const usize oldLength = output.Length();
    output.AppendUninitialized(separators);
    char *data = output.Data() + start;
    const usize tail = oldLength - start - digitsEnd;
    MemMove(data + digitsEnd + separators, data + digitsEnd, tail);
    usize target = digitsEnd + separators;
    for (usize i = 0; i < digitCount; ++i)
    {
        if (i != 0 && i % 3 == 0)
        {
            data[--target] = ',';
        }
        data[target - 1] = data[digitsEnd - 1 - i];
        --target;
    }
}

void WriteDigits(StringWriter &output, uint64 value, uint32 base, bool upper, int32 minimumDigits)
{
    char buffer[MaxIntegerLength];
    const usize length = FormatUnsigned(value, buffer, base, upper);
    if (minimumDigits > static_cast<int32>(length))
    {
        output.Append(static_cast<usize>(minimumDigits) - length, '0');
    }
    output.Append(StringView(buffer, length));
}

template <typename T> void WriteFloat(StringWriter &output, T value, const Spec &spec)
{
    const usize start = output.Length();
    switch (spec.Letter)
    {
    case 'F':
        AppendFloat(output, value, FloatStyle::Fixed, PrecisionOr(spec, 2));
        break;
    case 'E':
        AppendFloat(output, value, FloatStyle::Exponent, PrecisionOr(spec, 6), spec.Upper);
        break;
    case 'G':
        AppendFloat(output, value, FloatStyle::General, PrecisionOr(spec, 0), spec.Upper);
        break;
    case 'N':
        AppendFloat(output, value, FloatStyle::Fixed, PrecisionOr(spec, 2));
        GroupDigits(output, start);
        break;
    case 'P':
        AppendFloat(output, static_cast<float64>(value) * 100, FloatStyle::Fixed, PrecisionOr(spec, 2));
        output.Append('%');
        break;
    default:
        AppendFloat(output, value, FloatStyle::Shortest);
        break;
    }
}

void WriteInteger(StringWriter &output, uint64 magnitude, bool negative, uint8 size, const Spec &spec)
{
    const uint64 mask = size >= 8 ? ~uint64{0} : (uint64{1} << (size * 8u)) - 1;
    const uint64 bits = (negative ? 0 - magnitude : magnitude) & mask;
    const usize start = output.Length();
    switch (spec.Letter)
    {
    case 'X':
        WriteDigits(output, bits, 16, spec.Upper, spec.Precision);
        return;
    case 'B':
        WriteDigits(output, bits, 2, false, spec.Precision);
        return;
    case 'E':
    case 'P': {
        const auto value = static_cast<float64>(magnitude);
        WriteFloat(output, negative ? -value : value, spec);
        return;
    }
    default:
        break;
    }
    if (negative)
    {
        output.Append('-');
    }
    if (spec.Letter == 'D')
    {
        WriteDigits(output, magnitude, 10, false, spec.Precision);
        return;
    }
    WriteDigits(output, magnitude, 10, false, 0);
    if (spec.Letter == 'F' || spec.Letter == 'N')
    {
        const int32 decimals = PrecisionOr(spec, 2);
        if (decimals > 0)
        {
            output.Append('.');
            output.Append(static_cast<usize>(decimals), '0');
        }
        if (spec.Letter == 'N')
        {
            GroupDigits(output, start);
        }
    }
}

void WriteArg(StringWriter &output, const FormatArg &arg, StringView specText)
{
    const Spec spec = ParseSpec(specText);
    switch (arg.Type)
    {
    case FormatArgType::Bool:
        output.Append(arg.Bool ? "true" : "false");
        break;
    case FormatArgType::Char:
        output.Append(arg.Char);
        break;
    case FormatArgType::Signed:
        WriteInteger(output, arg.Signed < 0 ? 0 - static_cast<uint64>(arg.Signed) : static_cast<uint64>(arg.Signed),
                     arg.Signed < 0, arg.Size, spec);
        break;
    case FormatArgType::Unsigned:
        WriteInteger(output, arg.Unsigned, false, arg.Size, spec);
        break;
    case FormatArgType::Float32:
        WriteFloat(output, arg.Float32, spec);
        break;
    case FormatArgType::Float64:
        WriteFloat(output, arg.Float64, spec);
        break;
    case FormatArgType::String:
        output.Append(StringView(arg.Text.Data, arg.Text.Length));
        break;
    case FormatArgType::Pointer:
        output.Append("0x");
        WriteDigits(output, reinterpret_cast<uintptr>(arg.Pointer), 16, true, sizeof(void *) * 2);
        break;
    case FormatArgType::Custom:
        arg.Custom.Write(output, arg.Custom.Object, specText);
        break;
    }
}

/// Pads what was written since `start` to `width`: on the left for a positive width, on the right for a negative
/// one.
void Align(StringWriter &output, usize start, int32 width)
{
    const usize written = output.Length() - start;
    const auto target = static_cast<usize>(width < 0 ? -width : width);
    if (written >= target)
    {
        return;
    }
    const usize padding = target - written;
    if (width < 0)
    {
        output.Append(padding, ' ');
        return;
    }
    output.AppendUninitialized(padding);
    char *data = output.Data() + start;
    MemMove(data + padding, data, written);
    MemSet(data, ' ', padding);
}
} // namespace

namespace Private
{
void FormatArgs(StringWriter &output, StringView format, const FormatArg *args, usize count)
{
    const usize length = format.Length();
    usize literal = 0;
    usize i = 0;
    while (i < length)
    {
        const char c = format[i];
        if (c != '{' && c != '}')
        {
            ++i;
            continue;
        }
        output.Append(format.Substring(literal, i - literal));
        if (i + 1 < length && format[i + 1] == c)
        {
            output.Append(c);
            i += 2;
            literal = i;
            continue;
        }
        if (c == '}')
        {
            output.Append(c);
            literal = ++i;
            continue;
        }

        usize cursor = i + 1;
        usize index = 0;
        const usize digitsStart = cursor;
        while (cursor < length && Ascii::IsDigit(format[cursor]))
        {
            index = index * 10 + static_cast<usize>(format[cursor] - '0');
            ++cursor;
        }
        bool valid = cursor != digitsStart;
        int32 width = 0;
        if (valid && cursor < length && format[cursor] == ',')
        {
            ++cursor;
            const bool left = cursor < length && format[cursor] == '-';
            cursor += left ? 1 : 0;
            const usize widthStart = cursor;
            while (cursor < length && Ascii::IsDigit(format[cursor]))
            {
                width = width < 100000 ? width * 10 + (format[cursor] - '0') : width;
                ++cursor;
            }
            valid = cursor != widthStart;
            width = left ? -width : width;
        }
        StringView spec;
        if (valid && cursor < length && format[cursor] == ':')
        {
            const usize specStart = ++cursor;
            while (cursor < length && format[cursor] != '}' && format[cursor] != '{')
            {
                ++cursor;
            }
            spec = format.Substring(specStart, cursor - specStart);
        }
        valid = valid && cursor < length && format[cursor] == '}' && index < count;
        if (!valid)
        {
            output.Append(c);
            literal = ++i;
            continue;
        }
        const usize start = output.Length();
        WriteArg(output, args[index], spec);
        Align(output, start, width);
        i = cursor + 1;
        literal = i;
    }
    output.Append(format.Substring(literal));
}
} // namespace Private
} // namespace Marlin
