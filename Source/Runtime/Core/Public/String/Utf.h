//! UTF-8 decoding, encoding and conversion to and from UTF-16.
//!
//! The byte-buffer helpers never allocate and never fail; malformed UTF-8 decodes to the replacement character. The
//! `FromUtf16` overload that returns a `String` allocates the result.

#pragma once

#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// UTF-8 codec and UTF-16 conversion for platform APIs.
///
/// Decoding and encoding work one code point at a time on byte buffers. Malformed input is replaced, not rejected,
/// except by [`IsValid`].
struct Utf8
{
    /// The code point that malformed input decodes to, U+FFFD.
    static constexpr char32_t ReplacementCharacter = 0xFFFD;
    /// The largest Unicode code point, U+10FFFF.
    static constexpr char32_t MaxCodePoint = 0x10FFFF;

    /// Decodes one code point from `cursor` and advances it past the bytes that were consumed.
    ///
    /// Malformed bytes yield [`ReplacementCharacter`] and consume one byte, so decoding can continue at the next
    /// byte. Overlong forms, surrogates and values above [`MaxCodePoint`] are malformed.
    ///
    /// # Preconditions
    /// `cursor < end`: the caller guarantees that at least one byte remains.
    [[nodiscard]] static constexpr char32_t Decode(const char *&cursor, const char *end)
    {
        const auto lead = static_cast<uint8>(*cursor++);
        if (lead < 0x80)
        {
            return lead;
        }
        usize extra = 0;
        char32_t codePoint = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0)
        {
            extra = 1;
            codePoint = lead & 0x1Fu;
            minimum = 0x80;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            extra = 2;
            codePoint = lead & 0x0Fu;
            minimum = 0x800;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            extra = 3;
            codePoint = lead & 0x07u;
            minimum = 0x10000;
        }
        else
        {
            return ReplacementCharacter;
        }
        if (static_cast<usize>(end - cursor) < extra)
        {
            return ReplacementCharacter;
        }
        for (usize i = 0; i < extra; ++i)
        {
            const auto next = static_cast<uint8>(cursor[i]);
            if ((next & 0xC0) != 0x80)
            {
                return ReplacementCharacter;
            }
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        if (codePoint < minimum || codePoint > MaxCodePoint || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            return ReplacementCharacter;
        }
        cursor += extra;
        return codePoint;
    }

    /// Returns the UTF-8 length of `codePoint` by its value: 1 to 4 bytes.
    ///
    /// Values above [`MaxCodePoint`] report 4 even though [`Encode`] substitutes [`ReplacementCharacter`], which is
    /// 3 bytes.
    [[nodiscard]] static constexpr usize EncodedLength(char32_t codePoint)
    {
        if (codePoint < 0x80)
        {
            return 1;
        }
        if (codePoint < 0x800)
        {
            return 2;
        }
        return codePoint < 0x10000 ? 3 : 4;
    }

    /// Writes the UTF-8 form of `codePoint` to `output` and returns the number of bytes written, 1 to 4.
    ///
    /// Surrogates and values above [`MaxCodePoint`] are written as [`ReplacementCharacter`]. The bytes are not
    /// null-terminated.
    ///
    /// # Preconditions
    /// `output` has room for 4 bytes.
    static constexpr usize Encode(char32_t codePoint, char *output)
    {
        if (codePoint > MaxCodePoint || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            codePoint = ReplacementCharacter;
        }
        if (codePoint < 0x80)
        {
            output[0] = static_cast<char>(codePoint);
            return 1;
        }
        if (codePoint < 0x800)
        {
            output[0] = static_cast<char>(0xC0 | (codePoint >> 6));
            output[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
            return 2;
        }
        if (codePoint < 0x10000)
        {
            output[0] = static_cast<char>(0xE0 | (codePoint >> 12));
            output[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            output[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
            return 3;
        }
        output[0] = static_cast<char>(0xF0 | (codePoint >> 18));
        output[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        output[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 4;
    }

    /// Returns whether `text` is well-formed UTF-8.
    ///
    /// A literal U+FFFD encoded as `EF BF BD` is valid; every other malformed sequence makes the result false.
    [[nodiscard]] static constexpr bool IsValid(StringView text)
    {
        const char *cursor = text.begin();
        while (cursor != text.end())
        {
            const char *start = cursor;
            if (Decode(cursor, text.end()) != ReplacementCharacter)
            {
                continue;
            }
            const bool encodedReplacement = cursor - start == 3 && static_cast<uint8>(start[0]) == 0xEF &&
                                            static_cast<uint8>(start[1]) == 0xBF &&
                                            static_cast<uint8>(start[2]) == 0xBD;
            if (!encodedReplacement)
            {
                return false;
            }
        }
        return true;
    }

    /// Returns the number of code points in `text`.
    ///
    /// The count is byte-based: every byte that is not a continuation byte counts as one code point, so malformed
    /// input is counted rather than rejected.
    [[nodiscard]] static constexpr usize CountCodePoints(StringView text)
    {
        usize count = 0;
        for (const char c : text)
        {
            count += (static_cast<uint8>(c) & 0xC0) != 0x80;
        }
        return count;
    }

    /// Converts `text` to UTF-16, writing at most `capacity` units, and returns the units the whole text needs.
    ///
    /// When the whole text does not fit, the units that do not fit are left out and the return value is still the
    /// needed length, so it can be used to size a buffer first. A surrogate pair is written only when both units
    /// fit.
    ///
    /// # Preconditions
    /// `buffer` points to `capacity` writable units, or `capacity` is 0.
    static constexpr usize ToUtf16(StringView text, char16_t *buffer, usize capacity)
    {
        usize length = 0;
        const char *cursor = text.begin();
        while (cursor != text.end())
        {
            const char32_t codePoint = Decode(cursor, text.end());
            if (codePoint >= 0x10000)
            {
                if (length + 2 <= capacity)
                {
                    buffer[length] = static_cast<char16_t>(0xD800 + ((codePoint - 0x10000) >> 10));
                    buffer[length + 1] = static_cast<char16_t>(0xDC00 + ((codePoint - 0x10000) & 0x3FF));
                }
                length += 2;
            }
            else
            {
                if (length < capacity)
                {
                    buffer[length] = static_cast<char16_t>(codePoint);
                }
                ++length;
            }
        }
        return length;
    }

    /// Converts `length` UTF-16 units starting at `text` to UTF-8, writing at most `capacity` bytes, and returns the
    /// bytes the whole text needs.
    ///
    /// Unpaired surrogates become [`ReplacementCharacter`]. When the whole text does not fit, the bytes that do not
    /// fit are left out and the return value is still the needed length.
    ///
    /// # Preconditions
    /// `text` points to `length` units. `buffer` points to `capacity` writable bytes, or `capacity` is 0.
    static constexpr usize FromUtf16(const char16_t *text, usize length, char *buffer, usize capacity)
    {
        usize written = 0;
        for (usize i = 0; i < length; ++i)
        {
            char32_t codePoint = text[i];
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF && i + 1 < length && text[i + 1] >= 0xDC00 &&
                text[i + 1] <= 0xDFFF)
            {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (text[i + 1] - 0xDC00);
                ++i;
            }
            char encoded[4] = {};
            const usize size = Encode(codePoint, encoded);
            if (written + size <= capacity)
            {
                for (usize j = 0; j < size; ++j)
                {
                    buffer[written + j] = encoded[j];
                }
            }
            written += size;
        }
        return written;
    }

    /// Creates a `String` from `length` UTF-16 units starting at `text`; unpaired surrogates become replacement
    /// characters.
    [[nodiscard]] static String FromUtf16(const char16_t *text, usize length)
    {
        String result;
        const usize size = FromUtf16(text, length, nullptr, 0);
        FromUtf16(text, length, result.ResizeUninitialized(size), size);
        return result;
    }

    /// A range of the code points in a [`Marlin::StringView`], for range-based `for`.
    ///
    /// Decoding is lazy and yields [`ReplacementCharacter`] for malformed bytes.
    class CodePoints
    {
    public:
        /// Iterates the code points of a [`Marlin::Utf8::CodePoints`] range.
        class Iterator
        {
        public:
            /// Creates an iterator at `cursor`; `end` bounds decoding.
            constexpr Iterator(const char *cursor, const char *end) : _cursor(cursor), _end(end)
            {
            }

            /// Returns the code point at the current position.
            constexpr char32_t operator*() const
            {
                const char *cursor = _cursor;
                return Decode(cursor, _end);
            }

            /// Advances past the current code point.
            constexpr Iterator &operator++()
            {
                (void)Decode(_cursor, _end);
                return *this;
            }

            /// Returns whether both iterators are at the same byte.
            constexpr bool operator==(const Iterator &other) const
            {
                return _cursor == other._cursor;
            }

        private:
            const char *_cursor;
            const char *_end;
        };

        /// Creates a range over the code points of `text`.
        constexpr explicit CodePoints(StringView text) : _text(text)
        {
        }

        /// Returns an iterator at the first code point.
        [[nodiscard]] constexpr Iterator begin() const
        {
            return {_text.begin(), _text.end()};
        }

        /// Returns the past-the-end iterator.
        [[nodiscard]] constexpr Iterator end() const
        {
            return {_text.end(), _text.end()};
        }

    private:
        StringView _text;
    };
};
} // namespace Marlin
