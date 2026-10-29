#include "LanguageIR/MirDebugFormat.h"

namespace Marlin
{
bool ParseMirDebugFormat(StringView format, List<MirFormatSegment> &segments, String &error)
{
    usize literal = 0;
    usize i = 0;
    auto flush = [&](usize end) {
        if (end > literal)
        {
            segments.Add({format.Substring(literal, end - literal)});
        }
    };
    while (i < format.Count())
    {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.Count() && format[i + 1] == c)
        {
            flush(i);
            segments.Add({format.Substring(i, 1)});
            i += 2;
            literal = i;
            continue;
        }
        if (c == '}')
        {
            error = String("a `}` without a `{`; write `}}` for the brace itself");
            return false;
        }
        if (c != '{')
        {
            ++i;
            continue;
        }
        flush(i);
        usize j = i + 1;
        uint32 argument = 0;
        const usize digits = j;
        while (j < format.Count() && format[j] >= '0' && format[j] <= '9')
        {
            argument = argument * 10 + static_cast<uint32>(format[j] - '0');
            ++j;
        }
        if (j == digits || j - digits > 3)
        {
            error = String("a `{` must start a reference such as `{0}`; write `{{` for the brace itself");
            return false;
        }
        bool hexadecimal = false;
        if (j + 1 < format.Count() && format[j] == ':' && format[j + 1] == 'x')
        {
            hexadecimal = true;
            j += 2;
        }
        if (j >= format.Count() || format[j] != '}')
        {
            error = String("a reference is `{n}` or `{n:x}`");
            return false;
        }
        segments.Add({{}, argument, hexadecimal});
        i = j + 1;
        literal = i;
    }
    flush(format.Count());
    return true;
}
} // namespace Marlin
