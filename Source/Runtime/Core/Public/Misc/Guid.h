//! 128-bit GUIDs: random and name-based creation, text forms and hashing.
//!
//! A GUID is four 32-bit words, written in the text form as big-endian hexadecimal with hyphens.

#pragma once

#include "CoreTypes.h"
#include "Hash/Hash.h"
#include "String/Ascii.h"
#include "String/StringView.h"

#include <compare>

#include "Misc/Guid.gen.h"

namespace Marlin
{
class StringWriter;

/// A 128-bit globally unique identifier; all-zero is the invalid Guid.
///
/// Text forms follow the layouts .NET uses: the empty spec writes `01234567-89ab-cdef-0123-456789abcdef`, `n` is
/// the same without hyphens, and an upper-case spec letter (`D`, `N`) writes upper-case digits. Parsing accepts
/// either layout, optionally wrapped in braces, in either case.
MSTRUCT()
struct CORE_API Guid
{
    MGENERATED_BODY()

    /// The first 32 bits, written as the first 8 hexadecimal digits.
    uint32 A = 0;
    /// The second 32 bits, written as the next 8 hexadecimal digits (the second and third groups).
    uint32 B = 0;
    /// The third 32 bits, written as the next 8 hexadecimal digits (the fourth group and the first four digits of the
    /// last group).
    uint32 C = 0;
    /// The last 32 bits, written as the last 8 hexadecimal digits (the end of the last group).
    uint32 D = 0;

    /// Returns a random version 4 GUID from the OS random source.
    [[nodiscard]] static Guid NewGuid();
    /// Returns the version 5 (SHA-1) GUID of `name` in `space` as RFC 9562 defines it.
    ///
    /// The same space and name give the same GUID on every run.
    [[nodiscard]] static Guid FromName(const Guid &space, StringView name);

    /// Parses the `D` or `N` form, optionally wrapped in braces, in either case.
    ///
    /// `D` has 36 characters with hyphens at indices 8, 13, 18 and 23; `N` is the 32 digits without hyphens.
    /// Returns false, leaving `guid` unchanged, when the text is neither form.
    [[nodiscard]] static constexpr bool TryParse(StringView text, Guid &guid)
    {
        if (text.Length() >= 2 && text.First() == '{' && text.Last() == '}')
        {
            text = text.Substring(1, text.Length() - 2);
        }
        const bool hyphens = text.Length() == 36;
        if (!hyphens && text.Length() != 32)
        {
            return false;
        }
        uint32 words[4] = {};
        usize digit = 0;
        for (usize i = 0; i < text.Length(); ++i)
        {
            if (hyphens && (i == 8 || i == 13 || i == 18 || i == 23))
            {
                if (text[i] != '-')
                {
                    return false;
                }
                continue;
            }
            const int32 value = Ascii::IsHexDigit(text[i]) ? Ascii::DigitValue(text[i]) : -1;
            if (value < 0)
            {
                return false;
            }
            words[digit / 8] = (words[digit / 8] << 4) | static_cast<uint32>(value);
            ++digit;
        }
        guid = {words[0], words[1], words[2], words[3]};
        return true;
    }

    /// Returns whether any bit is set; the all-zero GUID is invalid.
    [[nodiscard]] constexpr bool IsValid() const
    {
        return (A | B | C | D) != 0;
    }

    /// Returns a hash of the four words; equal GUIDs hash equally.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashCombine((static_cast<uint64>(A) << 32) | B, (static_cast<uint64>(C) << 32) | D);
    }

    /// Writes the text form selected by the first character of `spec`.
    ///
    /// `N` or `n` omits the hyphens; an upper-case letter writes upper-case digits; an empty spec gives the
    /// lower-case hyphenated form. Remaining characters are ignored.
    void FormatTo(StringWriter &output, StringView spec) const;

    /// Compares the four words.
    constexpr bool operator==(const Guid &) const = default;
    /// Orders by `A`, then `B`, then `C`, then `D`, each compared as a `uint32`.
    constexpr std::strong_ordering operator<=>(const Guid &) const = default;
};
} // namespace Marlin
