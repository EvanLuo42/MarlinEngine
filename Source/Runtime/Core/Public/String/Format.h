//! Type-safe composite formatting into text buffers and strings.
//!
//! A format string is checked at compile time when it is a literal, and validated while it is used when it comes
//! from run-time text such as a localization table.

#pragma once

#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringBuilder.h"
#include "String/StringView.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// The formatting hook for a type used with [`Format`].
///
/// Specialize it with `static void Format(StringWriter &output, const T &value, StringView spec)`, or give `T` a
/// member `void FormatTo(StringWriter &output, StringView spec) const`. The `spec` is the text after `:` in the
/// placeholder and may be empty.
template <typename T> struct Formatter;

/// A format string known only at run time, such as localized text.
///
/// The text is validated while it is used: a malformed placeholder, or one that names an argument that was not
/// passed, is copied to the output literally.
struct RuntimeFormat
{
    /// The format text; it must outlive every use of this value.
    StringView Text;

    /// Wraps `text` as a run-time format string.
    constexpr explicit RuntimeFormat(StringView text) : Text(text)
    {
    }
};

namespace Private
{
/// The kind of value stored in a [`FormatArg`].
enum class FormatArgType : uint8
{
    Bool,
    Char,
    Signed,
    Unsigned,
    Float32,
    Float64,
    String,
    Pointer,
    Custom,
};

/// A type-erased format argument; [`MakeFormatArg`] builds one.
struct FormatArg
{
    /// The pointer and length of a string argument.
    struct StringValue
    {
        const char *Data;
        usize Length;
    };

    /// An object and the function that writes it.
    struct CustomValue
    {
        const void *Object;
        void (*Write)(StringWriter &output, const void *object, StringView spec);
    };

    union {
        bool Bool;
        char Char;
        int64 Signed;
        uint64 Unsigned;
        float32 Float32;
        float64 Float64;
        StringValue Text;
        const void *Pointer;
        CustomValue Custom;
    };
    /// The union member that is active.
    FormatArgType Type;
    /// The size in bytes of an integer argument; `X` and `B` mask negative values to this width.
    uint8 Size;
};

/// Whether `T` has a `Formatter` specialization.
template <typename T>
concept HasFormatter =
    requires(StringWriter &output, const T &value, StringView spec) { Formatter<T>::Format(output, value, spec); };

/// Whether `T` has a `FormatTo(StringWriter &, StringView)` member.
template <typename T>
concept HasFormatTo = requires(const T &value, StringWriter &output, StringView spec) { value.FormatTo(output, spec); };

/// A dependent false, for a `static_assert` in a discarded `if constexpr` branch.
template <typename T> inline constexpr bool AlwaysFalse = false;

/// Erases `value` into a [`FormatArg`].
///
/// Static-asserts when the type has no formatter, unless it is a bool, char, enum, integer, float,
/// string-convertible or pointer.
template <typename T> FormatArg MakeFormatArg(const T &value)
{
    using TValue = RemoveCv<T>;
    FormatArg arg; // NOLINT(cppcoreguidelines-pro-type-member-init)
    arg.Size = 0;
    if constexpr (HasFormatter<TValue>)
    {
        arg.Type = FormatArgType::Custom;
        arg.Custom = {
            &value,
            [](StringWriter &output, const void *object, StringView spec) {
                Formatter<TValue>::Format(output, *static_cast<const TValue *>(object), spec);
            },
        };
    }
    else if constexpr (HasFormatTo<TValue>)
    {
        arg.Type = FormatArgType::Custom;
        arg.Custom = {
            &value,
            [](StringWriter &output, const void *object, StringView spec) {
                static_cast<const TValue *>(object)->FormatTo(output, spec);
            },
        };
    }
    else if constexpr (IsSame<TValue, bool>)
    {
        arg.Type = FormatArgType::Bool;
        arg.Bool = value;
    }
    else if constexpr (IsSame<TValue, char>)
    {
        arg.Type = FormatArgType::Char;
        arg.Char = value;
    }
    else if constexpr (IsEnum<TValue>)
    {
        return MakeFormatArg(static_cast<UnderlyingType<TValue>>(value));
    }
    else if constexpr (IsIntegral<TValue> && IsSigned<TValue>)
    {
        arg.Type = FormatArgType::Signed;
        arg.Signed = value;
        arg.Size = sizeof(TValue);
    }
    else if constexpr (IsIntegral<TValue>)
    {
        arg.Type = FormatArgType::Unsigned;
        arg.Unsigned = value;
        arg.Size = sizeof(TValue);
    }
    else if constexpr (IsSame<TValue, float32>)
    {
        arg.Type = FormatArgType::Float32;
        arg.Float32 = value;
    }
    else if constexpr (IsSame<TValue, float64>)
    {
        arg.Type = FormatArgType::Float64;
        arg.Float64 = value;
    }
    else if constexpr (IsConvertible<const TValue &, StringView>)
    {
        const StringView text = value;
        arg.Type = FormatArgType::String;
        arg.Text = {text.Data(), text.Length()};
    }
    else if constexpr (IsPointer<TValue> || IsSame<TValue, decltype(nullptr)>)
    {
        arg.Type = FormatArgType::Pointer;
        arg.Pointer = static_cast<const void *>(value);
    }
    else
    {
        static_assert(AlwaysFalse<TValue>, "No Formatter<T> specialization or FormatTo member for this type");
    }
    return arg;
}

/// Writes `format` with `count` arguments from `args` to `output`.
///
/// A placeholder that is malformed or names an argument that was not passed is written literally.
CORE_API void FormatArgs(StringWriter &output, StringView format, const FormatArg *args, usize count);

/// Not defined: calling it while checking a format string at compile time stops the build with `reason`.
void InvalidFormatString(const char *reason);

/// Checks `text` against `count` arguments at compile time.
///
/// Stops the build when a brace is unmatched, a placeholder does not start with an in-range argument index, an
/// alignment lacks digits, a placeholder is not closed, or an argument is not used by any placeholder. Arguments at
/// index 64 and above are not tracked by the unused-argument check.
consteval void CheckFormat(StringView text, usize count)
{
    uint64 used = 0;
    for (usize i = 0; i < text.Length(); ++i)
    {
        const char c = text[i];
        if (c == '}')
        {
            if (i + 1 >= text.Length() || text[i + 1] != '}')
            {
                InvalidFormatString("unmatched '}'; write '}}' for a literal brace");
            }
            ++i;
            continue;
        }
        if (c != '{')
        {
            continue;
        }
        if (i + 1 < text.Length() && text[i + 1] == '{')
        {
            ++i;
            continue;
        }
        ++i;
        usize index = 0;
        const usize digitsStart = i;
        while (i < text.Length() && Ascii::IsDigit(text[i]))
        {
            index = index * 10 + static_cast<usize>(text[i] - '0');
            ++i;
        }
        if (i == digitsStart)
        {
            InvalidFormatString("a placeholder must start with an argument index, as in {0}");
        }
        if (index >= count)
        {
            InvalidFormatString("a placeholder index is not below the argument count");
        }
        used |= index < 64 ? uint64{1} << index : 0;
        if (i < text.Length() && text[i] == ',')
        {
            ++i;
            if (i < text.Length() && text[i] == '-')
            {
                ++i;
            }
            const usize alignmentStart = i;
            while (i < text.Length() && Ascii::IsDigit(text[i]))
            {
                ++i;
            }
            if (i == alignmentStart)
            {
                InvalidFormatString("an alignment after ',' needs digits, as in {0,8}");
            }
        }
        if (i < text.Length() && text[i] == ':')
        {
            while (i < text.Length() && text[i] != '}' && text[i] != '{')
            {
                ++i;
            }
        }
        if (i >= text.Length() || text[i] != '}')
        {
            InvalidFormatString("a placeholder is not closed with '}'");
        }
    }
    const uint64 all = count >= 64 ? ~uint64{0} : (uint64{1} << count) - 1;
    if ((used & all) != all)
    {
        InvalidFormatString("an argument is not used by any placeholder");
    }
}
} // namespace Private

/// A format string checked against its argument types at compile time.
///
/// A literal is checked by the constructor, which requires every placeholder to name an argument and every argument
/// up to index 63 to be used; the unused-argument check tracks only the first 64 arguments, so an argument at index
/// 64 and above may go unused. A [`Marlin::RuntimeFormat`] bypasses that check and is validated while formatting.
template <typename... TArgs> class FormatString
{
public:
    /// Checks `text` at compile time and stores it.
    ///
    /// A violation stops the build with a message naming the problem.
    template <typename TText>
        requires(IsConvertible<const TText &, StringView>)
    consteval FormatString(const TText &text) : _text(text)
    {
        Private::CheckFormat(_text, sizeof...(TArgs));
    }

    /// Stores the text of `format` without a compile-time check.
    constexpr FormatString(RuntimeFormat format) : _text(format.Text)
    {
    }

    /// Returns the format text.
    [[nodiscard]] constexpr StringView Get() const
    {
        return _text;
    }

private:
    StringView _text;
};

/// Appends C#-style composite formatting to `output`.
///
/// A placeholder has the form `{index[,alignment][:spec]}`, and `{{` and `}}` write literal braces. The index is
/// decimal and required. A positive `alignment` pads on the left with spaces and a negative one pads on the right.
///
/// Integer specs:
///
/// - `D` writes decimal digits; a following number is the minimum digit count.
/// - `X` writes hexadecimal digits, upper case for an upper-case `X`; a following number is the minimum digit
///   count, and negative values are masked to the width of the argument type.
/// - `B` writes binary digits with the same minimum count and masking.
/// - `F` writes decimal digits and appends `.` followed by the given number of zeros, 2 by default; with a count of
///   0 nothing is appended.
/// - `N` is `F` with the integer digits grouped in threes by commas.
/// - `E` formats the value as a float in scientific notation.
/// - `P` formats the value times 100 as a float and appends `%`.
///
/// Float specs:
///
/// - `F` is fixed with 2 decimals by default.
/// - `E` is scientific with 6 decimals by default; an upper-case `E` gives an upper-case exponent letter.
/// - `G` is general with the given significant digits; 0 or no number gives the shortest round-trip form.
/// - `N` is fixed with 2 decimals by default and groups the integer digits in threes.
/// - `P` multiplies by 100, is fixed with 2 decimals by default, and appends `%`.
///
/// An integer with any other spec writes decimal digits. A float with an empty or unrecognized spec, including `R`,
/// gives the shortest round-trip form. Bools are written as `true` and `false`,
/// characters as themselves, strings as their bytes, and pointers as `0x` followed by upper-case hexadecimal
/// padded to the width of a pointer. A custom type's `Formatter` or `FormatTo` receives the spec text unchanged.
///
/// # Examples
///
/// ```cpp
/// StringBuilder<> builder;
/// FormatTo(builder, "{0}-{1}", "abc", 12);
/// String text = builder.ToString(); // -> "abc-12"
/// ```
template <typename... TArgs>
void FormatTo(StringWriter &output, FormatString<TypeIdentity<TArgs>...> format, const TArgs &...args)
{
    if constexpr (sizeof...(TArgs) == 0)
    {
        Private::FormatArgs(output, format.Get(), nullptr, 0);
    }
    else
    {
        const Private::FormatArg list[] = {Private::MakeFormatArg(args)...};
        Private::FormatArgs(output, format.Get(), list, sizeof...(TArgs));
    }
}

/// Formats `args` into a new `String`.
///
/// The format string is checked at compile time; [`FormatTo`] describes the placeholder syntax.
///
/// # Examples
///
/// ```cpp
/// String text = Format("{0} {1:F2}", "pi", 3.14159); // -> "pi 3.14"
/// ```
template <typename... TArgs>
[[nodiscard]] String Format(FormatString<TypeIdentity<TArgs>...> format, const TArgs &...args)
{
    StringBuilder<256> builder;
    FormatTo(builder, format, args...);
    return builder.ToString();
}
} // namespace Marlin
