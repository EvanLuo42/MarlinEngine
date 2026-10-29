//! Durations and calendar dates, in 100 ns ticks.
//!
//! `TimeSpan` is a signed duration. `DateTime` is a tick count in the proleptic Gregorian calendar from
//! 0001-01-01. Both format to and parse from the text forms .NET uses.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Hash/Hash.h"
#include "String/StringView.h"

#include <compare>

#include "Misc/DateTime.gen.h"

namespace Marlin
{
class StringWriter;

/// A duration in 100 ns ticks, as in .NET.
///
/// The sign belongs to the whole span, so every component accessor shares it. Arithmetic is unchecked: a result
/// that does not fit in `int64` ticks is undefined. The factories that take a floating-point unit round to the
/// nearest tick, ties away from zero.
MSTRUCT()
class CORE_API TimeSpan
{
    MGENERATED_BODY()

public:
    /// Ticks in a microsecond.
    static constexpr int64 TicksPerMicrosecond = 10;
    /// Ticks in a millisecond.
    static constexpr int64 TicksPerMillisecond = 10000;
    /// Ticks in a second.
    static constexpr int64 TicksPerSecond = 1000 * TicksPerMillisecond;
    /// Ticks in a minute.
    static constexpr int64 TicksPerMinute = 60 * TicksPerSecond;
    /// Ticks in an hour.
    static constexpr int64 TicksPerHour = 60 * TicksPerMinute;
    /// Ticks in a day.
    static constexpr int64 TicksPerDay = 24 * TicksPerHour;

    /// Creates a zero span.
    constexpr TimeSpan() = default;

    /// Creates a span of `ticks` 100 ns ticks.
    constexpr explicit TimeSpan(int64 ticks) : _ticks(ticks)
    {
    }

    /// Creates a span from days, hours, minutes, seconds and milliseconds.
    ///
    /// The components are summed as given and may be negative or exceed their usual range.
    constexpr TimeSpan(int32 days, int32 hours, int32 minutes, int32 seconds, int32 milliseconds = 0)
        : _ticks(days * TicksPerDay + hours * TicksPerHour + minutes * TicksPerMinute + seconds * TicksPerSecond +
                 milliseconds * TicksPerMillisecond)
    {
    }

    /// Creates a span from a fractional number of days.
    [[nodiscard]] static constexpr TimeSpan FromDays(float64 days)
    {
        return FromUnits(days, TicksPerDay);
    }

    /// Creates a span from a fractional number of hours.
    [[nodiscard]] static constexpr TimeSpan FromHours(float64 hours)
    {
        return FromUnits(hours, TicksPerHour);
    }

    /// Creates a span from a fractional number of minutes.
    [[nodiscard]] static constexpr TimeSpan FromMinutes(float64 minutes)
    {
        return FromUnits(minutes, TicksPerMinute);
    }

    /// Creates a span from a fractional number of seconds.
    [[nodiscard]] static constexpr TimeSpan FromSeconds(float64 seconds)
    {
        return FromUnits(seconds, TicksPerSecond);
    }

    /// Creates a span from a fractional number of milliseconds.
    [[nodiscard]] static constexpr TimeSpan FromMilliseconds(float64 milliseconds)
    {
        return FromUnits(milliseconds, TicksPerMillisecond);
    }

    /// Creates a span from a whole number of microseconds.
    [[nodiscard]] static constexpr TimeSpan FromMicroseconds(int64 microseconds)
    {
        return TimeSpan(microseconds * TicksPerMicrosecond);
    }

    /// Returns the tick count.
    [[nodiscard]] constexpr int64 GetTicks() const
    {
        return _ticks;
    }

    /// Returns the whole days; the components share the sign of the span.
    [[nodiscard]] constexpr int32 Days() const
    {
        return static_cast<int32>(_ticks / TicksPerDay);
    }

    /// Returns the hour component, -23 to 23.
    [[nodiscard]] constexpr int32 Hours() const
    {
        return static_cast<int32>(_ticks / TicksPerHour % 24);
    }

    /// Returns the minute component, -59 to 59.
    [[nodiscard]] constexpr int32 Minutes() const
    {
        return static_cast<int32>(_ticks / TicksPerMinute % 60);
    }

    /// Returns the second component, -59 to 59.
    [[nodiscard]] constexpr int32 Seconds() const
    {
        return static_cast<int32>(_ticks / TicksPerSecond % 60);
    }

    /// Returns the millisecond component, -999 to 999.
    [[nodiscard]] constexpr int32 Milliseconds() const
    {
        return static_cast<int32>(_ticks / TicksPerMillisecond % 1000);
    }

    /// Returns the total number of days as a floating-point value.
    [[nodiscard]] constexpr float64 TotalDays() const
    {
        return static_cast<float64>(_ticks) / TicksPerDay;
    }

    /// Returns the total number of hours as a floating-point value.
    [[nodiscard]] constexpr float64 TotalHours() const
    {
        return static_cast<float64>(_ticks) / TicksPerHour;
    }

    /// Returns the total number of minutes as a floating-point value.
    [[nodiscard]] constexpr float64 TotalMinutes() const
    {
        return static_cast<float64>(_ticks) / TicksPerMinute;
    }

    /// Returns the total number of seconds as a floating-point value.
    [[nodiscard]] constexpr float64 TotalSeconds() const
    {
        return static_cast<float64>(_ticks) / TicksPerSecond;
    }

    /// Returns the total number of milliseconds as a floating-point value.
    [[nodiscard]] constexpr float64 TotalMilliseconds() const
    {
        return static_cast<float64>(_ticks) / TicksPerMillisecond;
    }

    /// Returns the absolute value of the span.
    [[nodiscard]] constexpr TimeSpan Duration() const
    {
        return TimeSpan(_ticks < 0 ? -_ticks : _ticks);
    }

    /// Returns the negated span.
    constexpr TimeSpan operator-() const
    {
        return TimeSpan(-_ticks);
    }

    /// Returns the sum of two spans.
    constexpr TimeSpan operator+(TimeSpan other) const
    {
        return TimeSpan(_ticks + other._ticks);
    }

    /// Returns the difference of two spans.
    constexpr TimeSpan operator-(TimeSpan other) const
    {
        return TimeSpan(_ticks - other._ticks);
    }

    /// Returns the span scaled by `factor`, rounded to the nearest tick.
    constexpr TimeSpan operator*(float64 factor) const
    {
        return FromUnits(factor, _ticks);
    }

    /// Returns the span divided by `divisor`, rounded to the nearest tick.
    ///
    /// # Preconditions
    /// `divisor` is nonzero; unchecked.
    constexpr TimeSpan operator/(float64 divisor) const
    {
        return FromUnits(1 / divisor, _ticks);
    }

    /// Returns the ratio of the two spans as a floating-point value.
    ///
    /// # Preconditions
    /// `other` is nonzero; unchecked.
    constexpr float64 operator/(TimeSpan other) const
    {
        return static_cast<float64>(_ticks) / static_cast<float64>(other._ticks);
    }

    /// Adds `other` to this span and returns it.
    constexpr TimeSpan &operator+=(TimeSpan other)
    {
        _ticks += other._ticks;
        return *this;
    }

    /// Subtracts `other` from this span and returns it.
    constexpr TimeSpan &operator-=(TimeSpan other)
    {
        _ticks -= other._ticks;
        return *this;
    }

    /// Compares tick counts.
    constexpr bool operator==(const TimeSpan &) const = default;
    /// Orders by tick count.
    constexpr std::strong_ordering operator<=>(const TimeSpan &) const = default;

    /// Returns a hash of the tick count.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashInteger(static_cast<uint64>(_ticks));
    }

    /// Writes the constant form .NET uses: `[-][d.]hh:mm:ss[.fffffff]`.
    ///
    /// The day part is omitted when it is zero, and the fraction is written only when it is nonzero. `spec` is
    /// ignored.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// StringBuilder<> text;
    /// TimeSpan(1, 2, 3, 4, 5).FormatTo(text, {});
    /// String result = text.ToString(); // -> "1.02:03:04.0050000"
    /// ```
    void FormatTo(StringWriter &output, StringView spec) const;

    /// Parses the constant form `[-][d.]hh:mm:ss[.fffffff]`.
    ///
    /// The fraction has 1 to 7 digits; hours, minutes and seconds are exactly two digits and must be in range.
    /// A `+` may precede the day count; it is accepted and ignored. Returns false on any other text.
    ///
    /// # Preconditions
    /// The parsed tick count fits in `int64`; the arithmetic is unchecked.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// TimeSpan span;
    /// bool parsed = TimeSpan::TryParse("-00:01:30", span); // -> true
    /// float64 seconds = span.TotalSeconds(); // -> -90.0
    /// ```
    [[nodiscard]] static bool TryParse(StringView text, TimeSpan &span);

private:
    static constexpr TimeSpan FromUnits(float64 value, int64 ticksPerUnit)
    {
        const float64 ticks = value * static_cast<float64>(ticksPerUnit);
        return TimeSpan(static_cast<int64>(ticks < 0 ? ticks - 0.5 : ticks + 0.5));
    }

    int64 _ticks = 0;
};

/// The day of the week, with `Sunday` as 0 as in .NET.
enum class DayOfWeek : uint8
{
    Sunday,
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
};

/// A point in time as 100 ns ticks since 0001-01-01 00:00 in the proleptic Gregorian calendar.
///
/// `UtcNow` gives UTC and `Now` gives local wall-clock time, which is only for display. Arithmetic is unchecked:
/// a result outside years 1 to 9999 triggers the constructor's assertion when assertions are on and is invalid
/// otherwise.
MSTRUCT()
class CORE_API DateTime
{
    MGENERATED_BODY()

public:
    /// The ticks of the last representable instant, 9999-12-31 23:59:59.9999999.
    static constexpr int64 MaxTicks = 3652059 * TimeSpan::TicksPerDay - 1;
    /// The ticks of 1970-01-01 00:00.
    static constexpr int64 UnixEpochTicks = 719162 * TimeSpan::TicksPerDay;

    /// Creates 0001-01-01 00:00.
    constexpr DateTime() = default;

    /// Creates an instant from a tick count.
    ///
    /// # Preconditions
    /// `ticks` is between 0 and [`MaxTicks`]. Checked by `MARLIN_ASSERT` when assertions are on; out of range is
    /// undefined behavior otherwise.
    constexpr explicit DateTime(int64 ticks) : _ticks(ticks)
    {
        MARLIN_ASSERT(ticks >= 0 && ticks <= MaxTicks);
    }

    /// Creates an instant from calendar fields.
    ///
    /// # Preconditions
    /// The fields must name a real date and time, as [`IsValid`] checks. Checked by `MARLIN_ASSERT` when assertions
    /// are on; out of range is undefined behavior otherwise.
    constexpr DateTime(int32 year, int32 month, int32 day, int32 hour = 0, int32 minute = 0, int32 second = 0,
                       int32 millisecond = 0)
        : _ticks(DaysFromDate(year, month, day) * TimeSpan::TicksPerDay + hour * TimeSpan::TicksPerHour +
                 minute * TimeSpan::TicksPerMinute + second * TimeSpan::TicksPerSecond +
                 millisecond * TimeSpan::TicksPerMillisecond)
    {
        MARLIN_ASSERT(IsValid(year, month, day, hour, minute, second, millisecond));
    }

    /// Returns the current UTC time from the platform clock.
    [[nodiscard]] static DateTime UtcNow();
    /// Returns the current local wall-clock time.
    ///
    /// It is `UtcNow` plus the platform's current UTC offset, so it carries no time zone or daylight-saving history;
    /// use it for display only.
    [[nodiscard]] static DateTime Now();

    /// Returns whether `year` is a leap year in the proleptic Gregorian calendar.
    [[nodiscard]] static constexpr bool IsLeapYear(int32 year)
    {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    /// Returns the number of days in `month` of `year`.
    ///
    /// # Preconditions
    /// `month` is 1 to 12; unchecked.
    [[nodiscard]] static constexpr int32 DaysInMonth(int32 year, int32 month)
    {
        constexpr int32 Days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return month == 2 && IsLeapYear(year) ? 29 : Days[month - 1];
    }

    /// Returns whether the fields name a real date and time.
    ///
    /// The ranges are year 1 to 9999, month 1 to 12, a day within the month, hour 0 to 23, minute and second 0 to
    /// 59, and millisecond 0 to 999.
    [[nodiscard]] static constexpr bool IsValid(int32 year, int32 month, int32 day, int32 hour = 0, int32 minute = 0,
                                                int32 second = 0, int32 millisecond = 0)
    {
        return year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month) &&
               hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60 && millisecond >= 0 &&
               millisecond < 1000;
    }

    /// Creates an instant from seconds since the Unix epoch.
    ///
    /// # Preconditions
    /// The result lies between 0001-01-01 and 9999-12-31; the constructor asserts it when assertions are on.
    [[nodiscard]] static constexpr DateTime FromUnixSeconds(int64 seconds)
    {
        return DateTime(UnixEpochTicks + seconds * TimeSpan::TicksPerSecond);
    }

    /// Creates an instant from milliseconds since the Unix epoch.
    ///
    /// # Preconditions
    /// The result lies between 0001-01-01 and 9999-12-31; the constructor asserts it when assertions are on.
    [[nodiscard]] static constexpr DateTime FromUnixMilliseconds(int64 milliseconds)
    {
        return DateTime(UnixEpochTicks + milliseconds * TimeSpan::TicksPerMillisecond);
    }

    /// Returns the whole seconds since the Unix epoch, rounding toward negative infinity.
    ///
    /// Times before the epoch give negative values.
    [[nodiscard]] constexpr int64 ToUnixSeconds() const
    {
        return FloorDivide(_ticks - UnixEpochTicks, TimeSpan::TicksPerSecond);
    }

    /// Returns the whole milliseconds since the Unix epoch, rounding toward negative infinity.
    ///
    /// Times before the epoch give negative values.
    [[nodiscard]] constexpr int64 ToUnixMilliseconds() const
    {
        return FloorDivide(_ticks - UnixEpochTicks, TimeSpan::TicksPerMillisecond);
    }

    /// Returns the tick count.
    [[nodiscard]] constexpr int64 GetTicks() const
    {
        return _ticks;
    }

    /// Returns midnight of the same day.
    [[nodiscard]] constexpr DateTime Date() const
    {
        return DateTime(_ticks - _ticks % TimeSpan::TicksPerDay);
    }

    /// Returns the time since midnight.
    [[nodiscard]] constexpr TimeSpan TimeOfDay() const
    {
        return TimeSpan(_ticks % TimeSpan::TicksPerDay);
    }

    /// Returns the year, 1 to 9999.
    [[nodiscard]] constexpr int32 Year() const
    {
        int32 year = 0;
        int32 month = 0;
        int32 day = 0;
        GetDate(year, month, day);
        return year;
    }

    /// Returns the month, 1 to 12.
    [[nodiscard]] constexpr int32 Month() const
    {
        int32 year = 0;
        int32 month = 0;
        int32 day = 0;
        GetDate(year, month, day);
        return month;
    }

    /// Returns the day of the month, 1 to 31.
    [[nodiscard]] constexpr int32 Day() const
    {
        int32 year = 0;
        int32 month = 0;
        int32 day = 0;
        GetDate(year, month, day);
        return day;
    }

    /// Returns the hour, 0 to 23.
    [[nodiscard]] constexpr int32 Hour() const
    {
        return TimeOfDay().Hours();
    }

    /// Returns the minute, 0 to 59.
    [[nodiscard]] constexpr int32 Minute() const
    {
        return TimeOfDay().Minutes();
    }

    /// Returns the second, 0 to 59.
    [[nodiscard]] constexpr int32 Second() const
    {
        return TimeOfDay().Seconds();
    }

    /// Returns the millisecond, 0 to 999.
    [[nodiscard]] constexpr int32 Millisecond() const
    {
        return TimeOfDay().Milliseconds();
    }

    /// Returns the day of the week; 0001-01-01 was a Monday.
    [[nodiscard]] constexpr DayOfWeek GetDayOfWeek() const
    {
        return static_cast<DayOfWeek>((_ticks / TimeSpan::TicksPerDay + 1) % 7);
    }

    /// Returns the day of the year, 1 to 366.
    [[nodiscard]] constexpr int32 DayOfYear() const
    {
        return static_cast<int32>(_ticks / TimeSpan::TicksPerDay - DaysFromDate(Year(), 1, 1)) + 1;
    }

    /// Writes the year, month and day of this instant.
    constexpr void GetDate(int32 &year, int32 &month, int32 &day) const
    {
        int64 days = _ticks / TimeSpan::TicksPerDay;
        const int64 cycles400 = days / 146097;
        days -= cycles400 * 146097;
        int64 cycles100 = days / 36524;
        cycles100 = cycles100 == 4 ? 3 : cycles100;
        days -= cycles100 * 36524;
        const int64 cycles4 = days / 1461;
        days -= cycles4 * 1461;
        int64 years = days / 365;
        years = years == 4 ? 3 : years;
        days -= years * 365;
        year = static_cast<int32>(cycles400 * 400 + cycles100 * 100 + cycles4 * 4 + years + 1);
        month = 1;
        while (days >= DaysInMonth(year, month))
        {
            days -= DaysInMonth(year, month);
            ++month;
        }
        day = static_cast<int32>(days) + 1;
    }

    /// Adds calendar months, clamping the day to the length of the target month.
    ///
    /// The time of day is kept.
    [[nodiscard]] constexpr DateTime AddMonths(int32 months) const
    {
        int32 year = 0;
        int32 month = 0;
        int32 day = 0;
        GetDate(year, month, day);
        const int32 total = year * 12 + (month - 1) + months;
        year = total / 12;
        month = total % 12 + 1;
        day = day < DaysInMonth(year, month) ? day : DaysInMonth(year, month);
        return DateTime(DaysFromDate(year, month, day) * TimeSpan::TicksPerDay + _ticks % TimeSpan::TicksPerDay);
    }

    /// Adds calendar years, clamping the day as [`AddMonths`] does.
    [[nodiscard]] constexpr DateTime AddYears(int32 years) const
    {
        return AddMonths(years * 12);
    }

    /// Returns this instant moved forward by `span`.
    constexpr DateTime operator+(TimeSpan span) const
    {
        return DateTime(_ticks + span.GetTicks());
    }

    /// Returns this instant moved back by `span`.
    constexpr DateTime operator-(TimeSpan span) const
    {
        return DateTime(_ticks - span.GetTicks());
    }

    /// Returns the span between two instants.
    constexpr TimeSpan operator-(DateTime other) const
    {
        return TimeSpan(_ticks - other._ticks);
    }

    /// Moves this instant forward by `span` and returns it.
    constexpr DateTime &operator+=(TimeSpan span)
    {
        *this = *this + span;
        return *this;
    }

    /// Moves this instant back by `span` and returns it.
    constexpr DateTime &operator-=(TimeSpan span)
    {
        *this = *this - span;
        return *this;
    }

    /// Compares tick counts.
    constexpr bool operator==(const DateTime &) const = default;
    /// Orders by tick count.
    constexpr std::strong_ordering operator<=>(const DateTime &) const = default;

    /// Returns a hash of the tick count.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashInteger(static_cast<uint64>(_ticks));
    }

    /// Writes the instant in the format selected by `spec`.
    ///
    /// An empty spec writes ISO 8601 with seven fraction digits: `yyyy-MM-ddTHH:mm:ss.fffffff`. `s` drops the
    /// fraction, `d` writes the date, and `t` the time. Any other spec is a pattern of `yyyy`, `yy`, `y`, `MM`, `M`,
    /// `dd`, `d`, `HH`, `H`, `mm`, `m`, `ss`, `s` and `f` to `fffffff`; every other character, including text in single
    /// quotes, is copied as is. `y` and `yyy` behave like `yy`, more than seven `f`s behave like seven, and a shorter
    /// `f` run truncates the fraction rather than rounding it.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const DateTime moment(2024, 7, 4, 13, 5, 9, 250);
    /// StringBuilder<> text;
    /// moment.FormatTo(text, "dd/MM/yy H:m 'at' fff");
    /// String result = text.ToString(); // -> "04/07/24 13:5 at 250"
    /// ```
    void FormatTo(StringWriter &output, StringView spec) const;

    /// Parses an ISO 8601 instant: `yyyy-MM-dd`, optionally followed by `T` or a space and `HH:mm`, then
    /// optionally `:ss` and a fraction of 1 to 7 digits, and an optional trailing `Z`.
    ///
    /// The fraction is accepted only after seconds, and offsets other than `Z` are rejected. Returns false when the
    /// text is not a valid instant or has trailing characters.
    [[nodiscard]] static bool TryParse(StringView text, DateTime &dateTime);

private:
    static constexpr int64 DaysFromDate(int32 year, int32 month, int32 day)
    {
        constexpr int32 Before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        const int64 previous = year - 1;
        const int64 leap = month > 2 && IsLeapYear(year) ? 1 : 0;
        return previous * 365 + previous / 4 - previous / 100 + previous / 400 + Before[month - 1] + leap + day - 1;
    }

    static constexpr int64 FloorDivide(int64 value, int64 divisor)
    {
        const int64 quotient = value / divisor;
        return quotient * divisor > value ? quotient - 1 : quotient;
    }

    int64 _ticks = 0;
};
} // namespace Marlin
