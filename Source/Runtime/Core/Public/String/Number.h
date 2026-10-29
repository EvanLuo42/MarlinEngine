//! Number formatting and parsing for integers and floats.
//!
//! Integers are written in any base from 2 to 36. Floats use the shortest round-trip form by default and can be
//! written fixed, scientific or with significant digits; parsing is correctly rounded.

#pragma once

#include "CoreTypes.h"
#include "String/Ascii.h"
#include "String/StringView.h"
#include "Templates/NumericLimits.h"
#include "Templates/Optional.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
class StringWriter;

/// The layout [`AppendFloat`] uses for a value.
///
/// `Fixed`, `Exponent` and `General` with a nonzero precision round the exact binary value, ties away from zero.
enum class FloatStyle : uint8
{
    /// The fewest digits that parse back to the same value; scientific below 1E-04 and from 1E+15.
    Shortest,
    /// `precision` digits after the point, rounded from the exact binary value.
    Fixed,
    /// One digit, the point, `precision` digits, then the exponent: `1.500000E+003`.
    Exponent,
    /// `precision` significant digits, or the same as `Shortest` when `precision` is 0; scientific when the
    /// exponent is below -4 or not below the precision, with trailing zeros dropped.
    General,
};

/// The bytes [`FormatUnsigned`] and [`FormatSigned`] may write: 64 binary digits and a sign.
inline constexpr usize MaxIntegerLength = 65;

/// Writes `value` in `base` to `buffer` and returns the length in bytes.
///
/// `upperCase` selects upper-case letters for digits above 9. Zero is written as `0`, and the result is not
/// null-terminated.
///
/// # Preconditions
/// `base` is 2 to 36 and `buffer` has room for 64 bytes; neither is checked.
constexpr usize FormatUnsigned(uint64 value, char *buffer, uint32 base = 10, bool upperCase = false)
{
    const char *digits = upperCase ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" : "0123456789abcdefghijklmnopqrstuvwxyz";
    char reversed[64] = {};
    usize length = 0;
    do
    {
        reversed[length++] = digits[value % base];
        value /= base;
    } while (value != 0);
    for (usize i = 0; i < length; ++i)
    {
        buffer[i] = reversed[length - 1 - i];
    }
    return length;
}

/// Writes `value` in `base` to `buffer` and returns the length in bytes.
///
/// A negative value starts with `-`, including the minimum `int64`. The result is not null-terminated.
///
/// # Preconditions
/// `base` is 2 to 36 and `buffer` has room for [`MaxIntegerLength`] bytes; neither is checked.
constexpr usize FormatSigned(int64 value, char *buffer, uint32 base = 10, bool upperCase = false)
{
    if (value < 0)
    {
        buffer[0] = '-';
        return 1 + FormatUnsigned(0 - static_cast<uint64>(value), buffer + 1, base, upperCase);
    }
    return FormatUnsigned(static_cast<uint64>(value), buffer, base, upperCase);
}

/// Writes `value` to `output` using `style`.
///
/// NaN and infinities are written as `NaN`, `Infinity` and `-Infinity`; the sign of a NaN is ignored. For `Fixed`
/// and `Exponent`, `precision` is the digits after the point; for `General` it is the significant digits, and 0
/// selects `Shortest`; `Shortest` ignores it. A negative `precision` is treated as 0. `upperCase` selects `E` or
/// `e` in an exponent. `Fixed`, `Exponent` and `General` with a nonzero precision round the true binary value,
/// ties away from zero.
///
/// # Examples
///
/// ```cpp
/// StringBuilder<> text;
/// AppendFloat(text, 1.5, FloatStyle::Fixed, 2);
/// String fixed = text.ToString(); // -> "1.50"
/// ```
CORE_API void AppendFloat(StringWriter &output, float64 value, FloatStyle style = FloatStyle::Shortest,
                          int32 precision = 0, bool upperCase = true);
/// Writes a `float32` to `output`; `Shortest` picks the fewest digits that parse back to the same `float32`.
///
/// The parameters and output match the `float64` overload.
CORE_API void AppendFloat(StringWriter &output, float32 value, FloatStyle style = FloatStyle::Shortest,
                          int32 precision = 0, bool upperCase = true);

/// Parses the whole of `text` as an integer: an optional sign, then digits in `base`.
///
/// Returns false, leaving `value` unchanged, when the text is empty, holds a byte that is not a digit in `base`,
/// or does not fit in `T`. A negative sign is accepted only when `T` is signed.
///
/// # Preconditions
/// `base` is 2 to 36; it is not checked.
template <Integral T> constexpr bool TryParse(StringView text, T &value, uint32 base = 10)
{
    usize index = 0;
    bool negative = false;
    if (!text.IsEmpty() && (text[0] == '+' || text[0] == '-'))
    {
        negative = text[0] == '-';
        if (negative && !IsSigned<T>)
        {
            return false;
        }
        ++index;
    }
    if (index == text.Length())
    {
        return false;
    }
    using TUnsigned = MakeUnsigned<T>;
    const auto limit = static_cast<TUnsigned>(negative ? static_cast<TUnsigned>(NumericLimits<T>::Max()) + 1u
                                                       : static_cast<TUnsigned>(NumericLimits<T>::Max()));
    TUnsigned result = 0;
    for (; index < text.Length(); ++index)
    {
        const int32 digit = Ascii::DigitValue(text[index]);
        if (digit < 0 || static_cast<uint32>(digit) >= base)
        {
            return false;
        }
        if (result > (limit - static_cast<TUnsigned>(digit)) / base)
        {
            return false;
        }
        result = static_cast<TUnsigned>(result * base + static_cast<TUnsigned>(digit));
    }
    value = negative ? static_cast<T>(0 - result) : static_cast<T>(result);
    return true;
}

/// Parses the whole of `text` as a floating-point value.
///
/// The accepted form is an optional sign, digits with an optional point, and an optional exponent; alternatively
/// `inf`, `infinity` or `nan` in any case. The result is correctly rounded, ties to even. Very large exponents
/// saturate to an infinity or to zero instead of failing. `value` is unchanged when the function returns false.
CORE_API bool TryParse(StringView text, float64 &value);
/// Parses a `float32`; the accepted text and the rounding match the `float64` overload.
CORE_API bool TryParse(StringView text, float32 &value);

/// Parses the whole of `text` as a `T`, returning `NullOptional` on failure.
///
/// # Examples
///
/// ```cpp
/// Optional<int32> count = Parse<int32>("42");
/// int32 value = count.ValueOr(0); // -> 42
/// ```
template <typename T> [[nodiscard]] constexpr Optional<T> Parse(StringView text)
{
    T value{};
    if (TryParse(text, value))
    {
        return value;
    }
    return NullOptional;
}
} // namespace Marlin
