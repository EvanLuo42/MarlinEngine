//! ASCII byte classification and case mapping for the string types.
//!
//! Every helper works on one `char`. Bytes at or above 0x80 are never letters, digits or white space, so the
//! helpers are safe on any byte of UTF-8 but do not classify Unicode.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// ASCII classification and case mapping for single bytes.
struct Ascii
{
    /// Returns whether `c` is an ASCII decimal digit, `0` to `9`.
    [[nodiscard]] static constexpr bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    /// Returns whether `c` is an ASCII hexadecimal digit, `0` to `9`, `a` to `f` or `A` to `F`.
    [[nodiscard]] static constexpr bool IsHexDigit(char c)
    {
        return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    /// Returns whether `c` is an ASCII upper-case letter, `A` to `Z`.
    [[nodiscard]] static constexpr bool IsUpper(char c)
    {
        return c >= 'A' && c <= 'Z';
    }

    /// Returns whether `c` is an ASCII lower-case letter, `a` to `z`.
    [[nodiscard]] static constexpr bool IsLower(char c)
    {
        return c >= 'a' && c <= 'z';
    }

    /// Returns whether `c` is an ASCII letter.
    [[nodiscard]] static constexpr bool IsLetter(char c)
    {
        return IsUpper(c) || IsLower(c);
    }

    /// Returns whether `c` is an ASCII letter or decimal digit.
    [[nodiscard]] static constexpr bool IsLetterOrDigit(char c)
    {
        return IsLetter(c) || IsDigit(c);
    }

    /// Returns whether `c` is ASCII white space: space, tab, newline, carriage return, vertical tab or form feed.
    [[nodiscard]] static constexpr bool IsWhiteSpace(char c)
    {
        return c == ' ' || (c >= '\t' && c <= '\r');
    }

    /// Returns `c` in upper case when it is an ASCII letter, else `c` unchanged.
    [[nodiscard]] static constexpr char ToUpper(char c)
    {
        return IsLower(c) ? static_cast<char>(c - ('a' - 'A')) : c;
    }

    /// Returns `c` in lower case when it is an ASCII letter, else `c` unchanged.
    [[nodiscard]] static constexpr char ToLower(char c)
    {
        return IsUpper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    /// Returns the value of `c` as a digit in bases up to 36, or -1 when it is not a digit.
    ///
    /// `0` to `9` give 0 to 9; `a` to `z` and `A` to `Z` give 10 to 35.
    [[nodiscard]] static constexpr int32 DigitValue(char c)
    {
        if (IsDigit(c))
        {
            return c - '0';
        }
        if (IsLower(c))
        {
            return c - 'a' + 10;
        }
        if (IsUpper(c))
        {
            return c - 'A' + 10;
        }
        return -1;
    }
};
} // namespace Marlin
