#include "Misc/DateTime.h"

#include "HAL/PlatformTime.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
void AppendPadded(StringWriter &output, int64 value, int32 width)
{
    char buffer[MaxIntegerLength];
    const usize length = FormatUnsigned(static_cast<uint64>(value), buffer);
    if (static_cast<int32>(length) < width)
    {
        output.Append(static_cast<usize>(width) - length, '0');
    }
    output.Append(StringView(buffer, length));
}

/// Skips `c` at `index`.
bool Expect(StringView text, usize &index, char c)
{
    if (index >= text.Length() || text[index] != c)
    {
        return false;
    }
    ++index;
    return true;
}

/// Reads exactly `count` digits at `index`.
bool ReadDigits(StringView text, usize &index, usize count, int32 &value)
{
    if (index + count > text.Length())
    {
        return false;
    }
    value = 0;
    for (usize i = 0; i < count; ++i)
    {
        const char c = text[index + i];
        if (!Ascii::IsDigit(c))
        {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    index += count;
    return true;
}

/// Reads a '.' and 1 to 7 fraction digits as ticks, if present.
bool ReadFraction(StringView text, usize &index, int64 &ticks)
{
    ticks = 0;
    if (index >= text.Length() || text[index] != '.')
    {
        return true;
    }
    ++index;
    int64 scale = TimeSpan::TicksPerSecond;
    usize digits = 0;
    while (index < text.Length() && Ascii::IsDigit(text[index]))
    {
        if (++digits > 7)
        {
            return false;
        }
        scale /= 10;
        ticks += (text[index] - '0') * scale;
        ++index;
    }
    return digits != 0;
}

/// The length of the run of `c` starting at `index`.
usize RunLength(StringView text, usize index)
{
    usize length = 1;
    while (index + length < text.Length() && text[index + length] == text[index])
    {
        ++length;
    }
    return length;
}
} // namespace

void TimeSpan::FormatTo(StringWriter &output, StringView /*spec*/) const
{
    int64 ticks = _ticks;
    if (ticks < 0)
    {
        output.Append('-');
        ticks = -ticks;
    }
    const int64 days = ticks / TicksPerDay;
    if (days != 0)
    {
        AppendPadded(output, days, 1);
        output.Append('.');
    }
    AppendPadded(output, ticks / TicksPerHour % 24, 2);
    output.Append(':');
    AppendPadded(output, ticks / TicksPerMinute % 60, 2);
    output.Append(':');
    AppendPadded(output, ticks / TicksPerSecond % 60, 2);
    if (ticks % TicksPerSecond != 0)
    {
        output.Append('.');
        AppendPadded(output, ticks % TicksPerSecond, 7);
    }
}

bool TimeSpan::TryParse(StringView text, TimeSpan &span)
{
    usize index = 0;
    const bool negative = text.StartsWith('-');
    index += negative ? 1 : 0;
    int64 days = 0;
    const usize dot = text.IndexOf('.', index);
    const usize colon = text.IndexOf(':', index);
    if (dot != StringView::NotFound && dot < colon)
    {
        if (!Marlin::TryParse(text.Substring(index, dot - index), days) || days < 0)
        {
            return false;
        }
        index = dot + 1;
    }
    int32 hours = 0;
    int32 minutes = 0;
    int32 seconds = 0;
    int64 fraction = 0;
    if (!ReadDigits(text, index, 2, hours) || !Expect(text, index, ':') || !ReadDigits(text, index, 2, minutes) ||
        !Expect(text, index, ':') || !ReadDigits(text, index, 2, seconds) || !ReadFraction(text, index, fraction) ||
        index != text.Length() || hours > 23 || minutes > 59 || seconds > 59)
    {
        return false;
    }
    const int64 ticks =
        days * TicksPerDay + hours * TicksPerHour + minutes * TicksPerMinute + seconds * TicksPerSecond + fraction;
    span = TimeSpan(negative ? -ticks : ticks);
    return true;
}

DateTime DateTime::UtcNow()
{
    return DateTime(PlatformTime::UtcNow());
}

DateTime DateTime::Now()
{
    return DateTime(PlatformTime::UtcNow() + PlatformTime::LocalUtcOffset());
}

void DateTime::FormatTo(StringWriter &output, StringView spec) const
{
    if (spec.IsEmpty())
    {
        spec = "yyyy-MM-ddTHH:mm:ss.fffffff";
    }
    else if (spec == "s")
    {
        spec = "yyyy-MM-ddTHH:mm:ss";
    }
    else if (spec == "d")
    {
        spec = "yyyy-MM-dd";
    }
    else if (spec == "t")
    {
        spec = "HH:mm:ss";
    }
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    GetDate(year, month, day);
    const TimeSpan time = TimeOfDay();
    usize index = 0;
    while (index < spec.Length())
    {
        const char c = spec[index];
        if (c == '\'')
        {
            const usize end = spec.IndexOf('\'', index + 1);
            const usize stop = end == StringView::NotFound ? spec.Length() : end;
            output.Append(spec.Substring(index + 1, stop - index - 1));
            index = stop + 1;
            continue;
        }
        const usize run = RunLength(spec, index);
        const auto width = static_cast<int32>(run < 2 ? 1 : 2);
        switch (c)
        {
        case 'y':
            AppendPadded(output, run >= 4 ? year : year % 100, run >= 4 ? 4 : 2);
            break;
        case 'M':
            AppendPadded(output, month, width);
            break;
        case 'd':
            AppendPadded(output, day, width);
            break;
        case 'H':
            AppendPadded(output, time.Hours(), width);
            break;
        case 'm':
            AppendPadded(output, time.Minutes(), width);
            break;
        case 's':
            AppendPadded(output, time.Seconds(), width);
            break;
        case 'f': {
            const usize digits = run < 7 ? run : 7;
            int64 fraction = time.GetTicks() % TimeSpan::TicksPerSecond;
            for (usize i = digits; i < 7; ++i)
            {
                fraction /= 10;
            }
            AppendPadded(output, fraction, static_cast<int32>(digits));
            break;
        }
        default:
            output.Append(run, c);
            break;
        }
        index += run;
    }
}

bool DateTime::TryParse(StringView text, DateTime &dateTime)
{
    usize index = 0;
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    if (!ReadDigits(text, index, 4, year) || !Expect(text, index, '-') || !ReadDigits(text, index, 2, month) ||
        !Expect(text, index, '-') || !ReadDigits(text, index, 2, day))
    {
        return false;
    }
    int32 hour = 0;
    int32 minute = 0;
    int32 second = 0;
    int64 fraction = 0;
    if (index < text.Length() && (text[index] == 'T' || text[index] == ' '))
    {
        ++index;
        if (!ReadDigits(text, index, 2, hour) || !Expect(text, index, ':') || !ReadDigits(text, index, 2, minute))
        {
            return false;
        }
        if (index < text.Length() && text[index] == ':')
        {
            ++index;
            if (!ReadDigits(text, index, 2, second) || !ReadFraction(text, index, fraction))
            {
                return false;
            }
        }
    }
    if (index < text.Length() && text[index] == 'Z')
    {
        ++index;
    }
    if (index != text.Length() || !IsValid(year, month, day, hour, minute, second))
    {
        return false;
    }
    dateTime = DateTime(DateTime(year, month, day, hour, minute, second).GetTicks() + fraction);
    return true;
}
} // namespace Marlin
