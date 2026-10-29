#include "String/Number.h"

#include "String/BigInteger.h"
#include "String/Dragonbox.h"
#include "String/StringBuilder.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace
{
using Private::BigInteger;

/// The binary layout of a float type.
template <typename T> struct FloatTraits;

template <> struct FloatTraits<float64>
{
    using Bits = uint64;
    static constexpr uint32 MantissaBits = 52;
    static constexpr uint32 ExponentBits = 11;
    static constexpr int32 Bias = 1023;
};

template <> struct FloatTraits<float32>
{
    using Bits = uint32;
    static constexpr uint32 MantissaBits = 23;
    static constexpr uint32 ExponentBits = 8;
    static constexpr int32 Bias = 127;
};

/// A finite value as Mantissa * 2^Exponent.
struct FloatParts
{
    uint64 Mantissa;
    int32 Exponent;
};

template <typename T> FloatParts Decompose(T value)
{
    using Traits = FloatTraits<T>;
    const auto bits = static_cast<uint64>(BitCast<typename Traits::Bits>(value));
    const uint64 fraction = bits & ((uint64{1} << Traits::MantissaBits) - 1);
    const auto biased = static_cast<int32>((bits >> Traits::MantissaBits) & ((1u << Traits::ExponentBits) - 1));
    if (biased == 0)
    {
        return {fraction, 1 - Traits::Bias - static_cast<int32>(Traits::MantissaBits)};
    }
    return {
        fraction | (uint64{1} << Traits::MantissaBits),
        biased - Traits::Bias - static_cast<int32>(Traits::MantissaBits),
    };
}

constexpr int32 MaxDigits = 800;

/// Decimal digits of a value: 0.D1D2...Dn * 10^Exponent.
struct Digits
{
    char Text[MaxDigits];
    int32 Count = 0;
    int32 Exponent = 0;
};

/// Sets up value = r / s.
void Setup(const FloatParts &parts, BigInteger &r, BigInteger &s)
{
    r.Set(parts.Mantissa);
    if (parts.Exponent >= 0)
    {
        r.ShiftLeft(static_cast<uint32>(parts.Exponent));
        s.Set(1);
    }
    else
    {
        s.SetPow2(static_cast<uint32>(-parts.Exponent));
    }
}

/// Scales so r / s lies in [0.1, 1) and returns the decimal exponent.
int32 Scale(const FloatParts &parts, BigInteger &r, BigInteger &s)
{
    const int32 highBit = FloorLog2(parts.Mantissa) + parts.Exponent;
    auto k = static_cast<int32>(__builtin_ceil(static_cast<float64>(highBit) * 0.30102999566398114 - 1e-10));
    if (k >= 0)
    {
        s.MultiplyPow10(static_cast<uint32>(k));
    }
    else
    {
        r.MultiplyPow10(static_cast<uint32>(-k));
    }
    while (BigInteger::Compare(r, s) >= 0)
    {
        s.MultiplyAdd(10);
        ++k;
    }
    while (true)
    {
        BigInteger next = r;
        next.MultiplyAdd(10);
        if (BigInteger::Compare(next, s) >= 0)
        {
            break;
        }
        r.MultiplyAdd(10);
        --k;
    }
    return k;
}

/// Adds one unit in the last place, carrying into a new leading digit if every digit was 9.
void RoundUp(Digits &digits)
{
    for (int32 i = digits.Count - 1; i >= 0; --i)
    {
        if (digits.Text[i] != '9')
        {
            ++digits.Text[i];
            digits.Count = i + 1;
            return;
        }
    }
    digits.Text[0] = '1';
    digits.Count = 1;
    ++digits.Exponent;
}

template <typename T> void ShortestDigits(T value, Digits &digits)
{
    const Private::Dragonbox::DecimalFloat decimal = Private::Dragonbox::ToDecimal(value);
    digits.Count = static_cast<int32>(FormatUnsigned(decimal.Significand, digits.Text));
    digits.Exponent = decimal.Exponent + digits.Count;
}

/// Exact digits, cut off after `significant` digits or, when `fixed`, `significant` digits after the point.
/// Ties round away from zero.
template <typename T> void CutoffDigits(T value, int32 count, bool fixed, Digits &digits)
{
    const FloatParts parts = Decompose(value);
    BigInteger r;
    BigInteger s;
    Setup(parts, r, s);
    digits.Exponent = Scale(parts, r, s);
    digits.Count = 0;
    int32 wanted = fixed ? digits.Exponent + count : count;
    wanted = wanted < MaxDigits ? wanted : MaxDigits;
    if (wanted < 0)
    {
        return;
    }
    if (wanted == 0)
    {
        r.MultiplyAdd(10);
        if (r.DivideDigit(s) >= 5)
        {
            digits.Text[0] = '1';
            digits.Count = 1;
            ++digits.Exponent;
        }
        return;
    }
    while (digits.Count < wanted && !r.IsZero())
    {
        r.MultiplyAdd(10);
        digits.Text[digits.Count++] = static_cast<char>('0' + r.DivideDigit(s));
    }
    if (!r.IsZero())
    {
        r.ShiftLeft(1);
        if (BigInteger::Compare(r, s) >= 0)
        {
            RoundUp(digits);
        }
    }
}

void AppendExponent(StringWriter &output, int32 exponent, int32 minimumDigits, bool upperCase)
{
    output.Append(upperCase ? 'E' : 'e');
    output.Append(exponent < 0 ? '-' : '+');
    char buffer[MaxIntegerLength];
    const usize length = FormatUnsigned(static_cast<uint64>(exponent < 0 ? -exponent : exponent), buffer);
    if (static_cast<int32>(length) < minimumDigits)
    {
        output.Append(static_cast<usize>(minimumDigits) - length, '0');
    }
    output.Append(StringView(buffer, length));
}

char DigitAt(const Digits &digits, int32 index)
{
    return index >= 0 && index < digits.Count ? digits.Text[index] : '0';
}

/// Positional notation with `decimals` digits after the point.
void AppendPositional(StringWriter &output, const Digits &digits, int32 decimals)
{
    if (digits.Exponent <= 0)
    {
        output.Append('0');
    }
    else
    {
        for (int32 i = 0; i < digits.Exponent; ++i)
        {
            output.Append(DigitAt(digits, i));
        }
    }
    if (decimals > 0)
    {
        output.Append('.');
        for (int32 i = 0; i < decimals; ++i)
        {
            output.Append(DigitAt(digits, digits.Exponent + i));
        }
    }
}

/// d.ddd with `decimals` digits after the point, then the exponent.
void AppendScientific(StringWriter &output, const Digits &digits, int32 decimals, int32 exponentDigits, bool upperCase)
{
    output.Append(DigitAt(digits, 0));
    if (decimals > 0)
    {
        output.Append('.');
        for (int32 i = 1; i <= decimals; ++i)
        {
            output.Append(DigitAt(digits, i));
        }
    }
    AppendExponent(output, digits.Count == 0 ? 0 : digits.Exponent - 1, exponentDigits, upperCase);
}

/// Shortest and General: positional unless the exponent is below -4 or reaches `limit`, no trailing zeros.
void AppendCompact(StringWriter &output, Digits &digits, int32 limit, bool upperCase)
{
    while (digits.Count > 1 && digits.Text[digits.Count - 1] == '0')
    {
        --digits.Count;
    }
    const int32 exponent = digits.Exponent - 1;
    if (exponent < -4 || exponent >= limit)
    {
        AppendScientific(output, digits, digits.Count - 1, 2, upperCase);
        return;
    }
    const int32 decimals = digits.Count - digits.Exponent;
    AppendPositional(output, digits, decimals > 0 ? decimals : 0);
}

template <typename T>
void AppendFloatImpl(StringWriter &output, T value, FloatStyle style, int32 precision, bool upperCase)
{
    using Traits = FloatTraits<T>;
    const auto bits = static_cast<uint64>(BitCast<typename Traits::Bits>(value));
    const bool negative = (bits >> (Traits::MantissaBits + Traits::ExponentBits)) != 0;
    if (__builtin_isnan(value))
    {
        output.Append("NaN");
        return;
    }
    if (negative)
    {
        output.Append('-');
    }
    if (value == static_cast<T>(__builtin_inf()) || value == static_cast<T>(-__builtin_inf()))
    {
        output.Append("Infinity");
        return;
    }
    precision = precision < 0 ? 0 : precision;
    Digits digits;
    if (value == T{0})
    {
        digits.Count = 0;
        digits.Exponent = 1;
    }
    else
    {
        switch (style)
        {
        case FloatStyle::Shortest:
            ShortestDigits(value, digits);
            break;
        case FloatStyle::Fixed:
            CutoffDigits(value, precision, true, digits);
            break;
        case FloatStyle::Exponent:
            CutoffDigits(value, precision + 1, false, digits);
            break;
        case FloatStyle::General:
            if (precision == 0)
            {
                ShortestDigits(value, digits);
            }
            else
            {
                CutoffDigits(value, precision, false, digits);
            }
            break;
        }
    }
    switch (style)
    {
    case FloatStyle::Shortest:
        AppendCompact(output, digits, 15, upperCase);
        break;
    case FloatStyle::Fixed:
        AppendPositional(output, digits, precision);
        break;
    case FloatStyle::Exponent:
        AppendScientific(output, digits, precision, 3, upperCase);
        break;
    case FloatStyle::General:
        AppendCompact(output, digits, precision == 0 ? 15 : precision, upperCase);
        break;
    }
}

/// Rounds `top` * 2^`exponent2`, with `sticky` marking nonzero bits below, to the nearest T; ties to even.
template <typename T> T RoundToFloat(uint64 top, int32 exponent2, bool sticky, bool negative)
{
    using Traits = FloatTraits<T>;
    const int32 leading = CountLeadingZeros(top);
    top <<= static_cast<uint32>(leading);
    exponent2 -= leading;
    const int32 msbExponent = exponent2 + 63;
    constexpr int32 MinExponent = 1 - Traits::Bias;
    constexpr int32 MaxExponent = Traits::Bias;
    int32 excess = 0;
    if (msbExponent < MinExponent)
    {
        excess = MinExponent - msbExponent;
    }
    const uint32 shift = 63 - Traits::MantissaBits + static_cast<uint32>(excess < 64 ? excess : 64);
    uint64 mantissa = 0;
    if (shift < 64)
    {
        mantissa = top >> shift;
        const uint64 rest = top & ((uint64{1} << shift) - 1);
        const uint64 half = uint64{1} << (shift - 1);
        if (rest > half || (rest == half && (sticky || (mantissa & 1) != 0)))
        {
            ++mantissa;
        }
    }
    else if (shift == 64 && (top > (uint64{1} << 63) || (top == (uint64{1} << 63) && sticky)))
    {
        mantissa = 1;
    }
    const int32 biased = msbExponent < MinExponent ? 0 : msbExponent + Traits::Bias;
    uint64 bits = mantissa;
    if (biased > 0)
    {
        bits += static_cast<uint64>(static_cast<uint32>(biased) - 1) << Traits::MantissaBits;
    }
    const uint64 infinityBits = static_cast<uint64>((1u << Traits::ExponentBits) - 1) << Traits::MantissaBits;
    if (msbExponent > MaxExponent || bits >= infinityBits)
    {
        bits = infinityBits;
    }
    bits |= static_cast<uint64>(negative) << (Traits::MantissaBits + Traits::ExponentBits);
    return BitCast<T>(static_cast<Traits::Bits>(bits));
}

constexpr float64 ExactPow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

bool MatchesIgnoreCase(StringView text, usize index, StringView word)
{
    return text.Substring(index).EqualsIgnoreCase(word);
}

template <typename T> bool TryParseFloat(StringView text, T &value)
{
    usize index = 0;
    bool negative = false;
    if (index < text.Length() && (text[index] == '+' || text[index] == '-'))
    {
        negative = text[index] == '-';
        ++index;
    }
    if (MatchesIgnoreCase(text, index, "inf") || MatchesIgnoreCase(text, index, "infinity"))
    {
        value = negative ? -static_cast<T>(__builtin_inf()) : static_cast<T>(__builtin_inf());
        return true;
    }
    if (MatchesIgnoreCase(text, index, "nan"))
    {
        value = static_cast<T>(__builtin_nan(""));
        return true;
    }

    char kept[MaxDigits];
    int32 keptCount = 0;
    bool truncated = false;
    int32 exponent = 0;
    bool anyDigit = false;
    bool seenPoint = false;
    for (; index < text.Length(); ++index)
    {
        const char c = text[index];
        if (c == '.' && !seenPoint)
        {
            seenPoint = true;
            continue;
        }
        if (!Ascii::IsDigit(c))
        {
            break;
        }
        anyDigit = true;
        if (keptCount == 0 && c == '0')
        {
            exponent -= seenPoint ? 1 : 0;
            continue;
        }
        if (keptCount < MaxDigits)
        {
            kept[keptCount++] = c;
            exponent -= seenPoint ? 1 : 0;
        }
        else
        {
            truncated |= c != '0';
            exponent += seenPoint ? 0 : 1;
        }
    }
    if (!anyDigit)
    {
        return false;
    }
    if (index < text.Length() && (text[index] == 'e' || text[index] == 'E'))
    {
        ++index;
        bool exponentNegative = false;
        if (index < text.Length() && (text[index] == '+' || text[index] == '-'))
        {
            exponentNegative = text[index] == '-';
            ++index;
        }
        if (index == text.Length())
        {
            return false;
        }
        int32 written = 0;
        for (; index < text.Length() && Ascii::IsDigit(text[index]); ++index)
        {
            written = written < 100000 ? written * 10 + (text[index] - '0') : written;
        }
        exponent += exponentNegative ? -written : written;
    }
    if (index != text.Length())
    {
        return false;
    }

    const T zero = negative ? -T{0} : T{0};
    if (keptCount == 0)
    {
        value = zero;
        return true;
    }
    if (exponent + keptCount > 310)
    {
        value = negative ? -static_cast<T>(__builtin_inf()) : static_cast<T>(__builtin_inf());
        return true;
    }
    if (exponent + keptCount < -330)
    {
        value = zero;
        return true;
    }

    if (keptCount <= 15 && !truncated)
    {
        uint64 integer = 0;
        for (int32 i = 0; i < keptCount; ++i)
        {
            integer = integer * 10 + static_cast<uint64>(kept[i] - '0');
        }
        // The digits and the power of ten are exact doubles, so one rounding gives the right double. A float32
        // only takes this path when the double result is exact, so it is rounded once too.
        const bool exact =
            sizeof(T) == 8 ? exponent >= -22 && exponent <= 22 : exponent >= 0 && keptCount + exponent <= 15;
        if (exact)
        {
            const auto digits = static_cast<float64>(integer);
            const float64 result = exponent >= 0 ? digits * ExactPow10[exponent] : digits / ExactPow10[-exponent];
            value = static_cast<T>(negative ? -result : result);
            return true;
        }
    }

    BigInteger number;
    for (int32 i = 0; i < keptCount; i += 9)
    {
        uint32 chunk = 0;
        uint32 scale = 1;
        for (int32 j = i; j < i + 9 && j < keptCount; ++j)
        {
            chunk = chunk * 10 + static_cast<uint32>(kept[j] - '0');
            scale *= 10;
        }
        number.MultiplyAdd(scale, chunk);
    }
    if (exponent >= 0)
    {
        number.MultiplyPow10(static_cast<uint32>(exponent));
        const uint32 length = number.BitLength();
        const uint32 start = length > 64 ? length - 64 : 0;
        bool below = false;
        const uint64 top = number.Bits64(start, below);
        value = RoundToFloat<T>(top, static_cast<int32>(start), below || truncated, negative);
        return true;
    }
    BigInteger divisor(1);
    divisor.MultiplyPow10(static_cast<uint32>(-exponent));
    // Scales so the quotient has 65 or 66 bits.
    const int32 shift = static_cast<int32>(divisor.BitLength()) - static_cast<int32>(number.BitLength()) + 65;
    if (shift > 0)
    {
        number.ShiftLeft(static_cast<uint32>(shift));
    }
    else
    {
        divisor.ShiftLeft(static_cast<uint32>(-shift));
    }
    divisor.ShiftLeft(66);
    uint64 quotient = 0;
    uint32 quotientTop = 0;
    for (int32 bit = 66; bit >= 0; --bit)
    {
        quotientTop = (quotientTop << 1) | static_cast<uint32>(quotient >> 63);
        quotient <<= 1;
        if (BigInteger::Compare(number, divisor) >= 0)
        {
            number.Subtract(divisor);
            quotient |= 1;
        }
        divisor.ShiftRightOne();
    }
    bool sticky = !number.IsZero() || truncated;
    int32 exponent2 = -shift;
    while (quotientTop != 0)
    {
        sticky |= (quotient & 1) != 0;
        quotient = (quotient >> 1) | (static_cast<uint64>(quotientTop & 1) << 63);
        quotientTop >>= 1;
        ++exponent2;
    }
    value = RoundToFloat<T>(quotient, exponent2, sticky, negative);
    return true;
}
} // namespace

void AppendFloat(StringWriter &output, float64 value, FloatStyle style, int32 precision, bool upperCase)
{
    AppendFloatImpl(output, value, style, precision, upperCase);
}

void AppendFloat(StringWriter &output, float32 value, FloatStyle style, int32 precision, bool upperCase)
{
    AppendFloatImpl(output, value, style, precision, upperCase);
}

bool TryParse(StringView text, float64 &value)
{
    return TryParseFloat(text, value);
}

bool TryParse(StringView text, float32 &value)
{
    return TryParseFloat(text, value);
}
} // namespace Marlin
