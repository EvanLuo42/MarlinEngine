//! A non-owning view of UTF-8 text, plus a splitter that iterates separators.
//!
//! Views are cheap to copy and never allocate. They do not own their bytes and are valid only while the bytes they
//! were created from are alive.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Hash/RapidHash.h"
#include "String/Ascii.h"

#include <compare>

namespace Marlin
{
class StringSplit;

/// A non-owning view of UTF-8 text: a pointer and a byte length.
///
/// The view is not null-terminated and does not own its bytes, so it is valid only while the storage it was
/// created from outlives it and is not modified. A `String` and a C string convert implicitly.
///
/// # Thread safety
/// Reading a view is safe from any thread. The bytes it refers to must not be written while another thread reads
/// them.
class StringView
{
public:
    /// The result of a failed search.
    static constexpr usize NotFound = ~usize{0};

    /// Creates an empty view.
    constexpr StringView() = default;

    /// Creates a view of `length` bytes starting at `data`.
    ///
    /// # Preconditions
    /// `data` points to at least `length` bytes, or `length` is 0. The range is not checked.
    constexpr StringView(const char *data, usize length) : _data(data), _length(length)
    {
    }

    /// Creates a view of a null-terminated string; a null pointer gives an empty view.
    constexpr StringView(const char *text) : _data(text), _length(text != nullptr ? __builtin_strlen(text) : 0)
    {
    }

    /// Deleted: pass a null-terminated string or `StringView()` instead of a null pointer.
    StringView(decltype(nullptr)) = delete;

    /// Returns the pointer to the first byte; the bytes are not null-terminated.
    [[nodiscard]] constexpr const char *Data() const
    {
        return _data;
    }

    /// Returns the length in bytes.
    [[nodiscard]] constexpr usize Length() const
    {
        return _length;
    }

    /// Returns the length in bytes.
    ///
    /// This alias lets a `StringView` convert to a `Span<const char>`.
    [[nodiscard]] constexpr usize Count() const
    {
        return _length;
    }

    /// Returns whether the view has no bytes.
    [[nodiscard]] constexpr bool IsEmpty() const
    {
        return _length == 0;
    }

    /// Returns the byte at `index`.
    ///
    /// # Preconditions
    /// `index < Length()`. Checked by `MARLIN_CHECK_SLOW` in Debug builds; out of range is undefined behavior
    /// otherwise.
    [[nodiscard]] constexpr char operator[](usize index) const
    {
        MARLIN_CHECK_SLOW(index < _length);
        return _data[index];
    }

    /// Returns the first byte.
    ///
    /// # Preconditions
    /// The view is not empty; checked by `MARLIN_ASSERT` when assertions are on.
    [[nodiscard]] constexpr char First() const
    {
        MARLIN_ASSERT(_length != 0);
        return _data[0];
    }

    /// Returns the last byte.
    ///
    /// # Preconditions
    /// The view is not empty; checked by `MARLIN_ASSERT` when assertions are on.
    [[nodiscard]] constexpr char Last() const
    {
        MARLIN_ASSERT(_length != 0);
        return _data[_length - 1];
    }

    /// Returns up to `length` bytes starting at `start`.
    ///
    /// `start` and `length` are clamped to the view, so the result never extends past the end.
    [[nodiscard]] constexpr StringView Substring(usize start, usize length = NotFound) const
    {
        start = start < _length ? start : _length;
        const usize available = _length - start;
        return {_data + start, length < available ? length : available};
    }

    /// Returns the first `length` bytes, or the whole view when it is shorter.
    [[nodiscard]] constexpr StringView Left(usize length) const
    {
        return {_data, length < _length ? length : _length};
    }

    /// Returns the last `length` bytes, or the whole view when it is shorter.
    [[nodiscard]] constexpr StringView Right(usize length) const
    {
        length = length < _length ? length : _length;
        return {_data + _length - length, length};
    }

    /// Returns the view without its first `count` bytes; a larger `count` gives an empty view.
    [[nodiscard]] constexpr StringView RemoveStart(usize count) const
    {
        return Substring(count);
    }

    /// Returns the view without its last `count` bytes; a larger `count` gives an empty view.
    [[nodiscard]] constexpr StringView RemoveEnd(usize count) const
    {
        return Left(count < _length ? _length - count : 0);
    }

    /// Returns the view without leading ASCII white space.
    [[nodiscard]] constexpr StringView TrimStart() const
    {
        usize start = 0;
        while (start < _length && Ascii::IsWhiteSpace(_data[start]))
        {
            ++start;
        }
        return {_data + start, _length - start};
    }

    /// Returns the view without trailing ASCII white space.
    [[nodiscard]] constexpr StringView TrimEnd() const
    {
        usize length = _length;
        while (length != 0 && Ascii::IsWhiteSpace(_data[length - 1]))
        {
            --length;
        }
        return {_data, length};
    }

    /// Returns the view without leading or trailing ASCII white space.
    [[nodiscard]] constexpr StringView Trim() const
    {
        return TrimStart().TrimEnd();
    }

    /// Returns the index of the first `c` at or after `start`, or `NotFound`.
    ///
    /// A `start` past the end gives `NotFound`.
    [[nodiscard]] constexpr usize IndexOf(char c, usize start = 0) const
    {
        if (start >= _length)
        {
            return NotFound;
        }
        const char *found = __builtin_char_memchr(_data + start, c, _length - start);
        return found != nullptr ? static_cast<usize>(found - _data) : NotFound;
    }

    /// Returns the index of the first occurrence of `text` at or after `start`, or `NotFound`.
    ///
    /// An empty `text` is found at `start` when `start` is not past the end.
    [[nodiscard]] constexpr usize IndexOf(StringView text, usize start = 0) const
    {
        if (text._length == 0)
        {
            return start <= _length ? start : NotFound;
        }
        while (start + text._length <= _length)
        {
            const usize index = IndexOf(text._data[0], start);
            if (index == NotFound || index + text._length > _length)
            {
                return NotFound;
            }
            if (__builtin_memcmp(_data + index, text._data, text._length) == 0)
            {
                return index;
            }
            start = index + 1;
        }
        return NotFound;
    }

    /// Returns the index of the last `c`, or `NotFound`.
    [[nodiscard]] constexpr usize LastIndexOf(char c) const
    {
        for (usize i = _length; i != 0; --i)
        {
            if (_data[i - 1] == c)
            {
                return i - 1;
            }
        }
        return NotFound;
    }

    /// Returns the index of the last occurrence of `text`, or `NotFound`.
    ///
    /// An empty `text` matches at the end of the view.
    [[nodiscard]] constexpr usize LastIndexOf(StringView text) const
    {
        if (text._length > _length)
        {
            return NotFound;
        }
        for (usize i = _length - text._length + 1; i != 0; --i)
        {
            if (__builtin_memcmp(_data + i - 1, text._data, text._length) == 0)
            {
                return i - 1;
            }
        }
        return NotFound;
    }

    /// Returns the index of the first byte at or after `start` that occurs in `bytes`, or `NotFound`.
    ///
    /// `bytes` is a set of bytes, not a sequence to match.
    [[nodiscard]] constexpr usize IndexOfAny(StringView bytes, usize start = 0) const
    {
        for (usize i = start; i < _length; ++i)
        {
            if (bytes.Contains(_data[i]))
            {
                return i;
            }
        }
        return NotFound;
    }

    /// Returns whether `c` occurs in the view.
    [[nodiscard]] constexpr bool Contains(char c) const
    {
        return IndexOf(c) != NotFound;
    }

    /// Returns whether `text` occurs in the view.
    [[nodiscard]] constexpr bool Contains(StringView text) const
    {
        return IndexOf(text) != NotFound;
    }

    /// Returns whether the view starts with `prefix`; an empty `prefix` matches.
    [[nodiscard]] constexpr bool StartsWith(StringView prefix) const
    {
        return prefix._length <= _length && __builtin_memcmp(_data, prefix._data, prefix._length) == 0;
    }

    /// Returns whether the first byte is `c`.
    [[nodiscard]] constexpr bool StartsWith(char c) const
    {
        return _length != 0 && _data[0] == c;
    }

    /// Returns whether the view ends with `suffix`; an empty `suffix` matches.
    [[nodiscard]] constexpr bool EndsWith(StringView suffix) const
    {
        return suffix._length <= _length &&
               __builtin_memcmp(_data + _length - suffix._length, suffix._data, suffix._length) == 0;
    }

    /// Returns whether the last byte is `c`.
    [[nodiscard]] constexpr bool EndsWith(char c) const
    {
        return _length != 0 && _data[_length - 1] == c;
    }

    /// Splits at the first `separator`.
    ///
    /// Returns whether a separator was found. When there is none, `left` becomes the whole view and `right` is
    /// empty; otherwise `right` is the text after the separator.
    constexpr bool SplitFirst(char separator, StringView &left, StringView &right) const
    {
        const usize index = IndexOf(separator);
        if (index == NotFound)
        {
            left = *this;
            right = {};
            return false;
        }
        left = Left(index);
        right = Substring(index + 1);
        return true;
    }

    /// Splits at the last `separator`.
    ///
    /// Returns whether a separator was found. When there is none, `left` becomes the whole view and `right` is
    /// empty; otherwise `right` is the text after the separator.
    constexpr bool SplitLast(char separator, StringView &left, StringView &right) const
    {
        const usize index = LastIndexOf(separator);
        if (index == NotFound)
        {
            left = *this;
            right = {};
            return false;
        }
        left = Left(index);
        right = Substring(index + 1);
        return true;
    }

    /// Returns the parts of the view between occurrences of `separator`.
    ///
    /// Empty parts are included, so there is always at least one part.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// usize parts = 0;
    /// for (StringView part : StringView("a,,b,").Split(','))
    /// {
    ///     ++parts;
    /// }
    /// // -> parts == 4
    /// ```
    [[nodiscard]] constexpr StringSplit Split(char separator) const;

    /// Compares the bytes lexicographically.
    ///
    /// Returns a negative value when this view sorts before `other`, zero when they are equal, and a positive value
    /// otherwise. The order is byte-wise, not Unicode collation.
    [[nodiscard]] constexpr int32 CompareTo(StringView other) const
    {
        const usize common = _length < other._length ? _length : other._length;
        const int32 result = common != 0 ? __builtin_memcmp(_data, other._data, common) : 0;
        if (result != 0)
        {
            return result;
        }
        if (_length == other._length)
        {
            return 0;
        }
        return _length < other._length ? -1 : 1;
    }

    /// Compares the bytes ignoring ASCII case.
    ///
    /// Returns a negative value, zero or a positive value as [`CompareTo`] does.
    [[nodiscard]] constexpr int32 CompareToIgnoreCase(StringView other) const
    {
        const usize common = _length < other._length ? _length : other._length;
        for (usize i = 0; i < common; ++i)
        {
            const char a = Ascii::ToLower(_data[i]);
            const char b = Ascii::ToLower(other._data[i]);
            if (a != b)
            {
                return static_cast<uint8>(a) < static_cast<uint8>(b) ? -1 : 1;
            }
        }
        if (_length == other._length)
        {
            return 0;
        }
        return _length < other._length ? -1 : 1;
    }

    /// Returns whether both views have the same length and equal bytes ignoring ASCII case.
    [[nodiscard]] constexpr bool EqualsIgnoreCase(StringView other) const
    {
        return _length == other._length && CompareToIgnoreCase(other) == 0;
    }

    /// Returns whether the whole view matches `pattern`, where `*` matches any run of bytes and `?` any one byte.
    ///
    /// Matching is byte-wise and case-sensitive, and neither wildcard can be escaped.
    [[nodiscard]] constexpr bool MatchesWildcard(StringView pattern) const
    {
        usize text = 0;
        usize at = 0;
        usize star = NotFound;
        usize resume = 0;
        while (text < _length)
        {
            if (at < pattern._length && (pattern._data[at] == '?' || pattern._data[at] == _data[text]))
            {
                ++text;
                ++at;
            }
            else if (at < pattern._length && pattern._data[at] == '*')
            {
                star = at++;
                resume = text;
            }
            else if (star != NotFound)
            {
                at = star + 1;
                text = ++resume;
            }
            else
            {
                return false;
            }
        }
        while (at < pattern._length && pattern._data[at] == '*')
        {
            ++at;
        }
        return at == pattern._length;
    }

    /// Returns a 64-bit hash of the bytes; equal views hash equally.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return RapidHash(_data, _length);
    }

    /// Returns the pointer to the first byte, for range-based `for`.
    [[nodiscard]] constexpr const char *begin() const
    {
        return _data;
    }

    /// Returns one past the last byte, for range-based `for`.
    [[nodiscard]] constexpr const char *end() const
    {
        return _data + _length;
    }

    /// Returns whether both views have the same length and bytes.
    friend constexpr bool operator==(StringView a, StringView b)
    {
        return a._length == b._length && (a._length == 0 || __builtin_memcmp(a._data, b._data, a._length) == 0);
    }

    /// Orders views lexicographically by their bytes.
    friend constexpr std::strong_ordering operator<=>(StringView a, StringView b)
    {
        return a.CompareTo(b) <=> 0;
    }

private:
    const char *_data = "";
    usize _length = 0;
};

/// The parts of a [`Marlin::StringView`] between separators, for range-based `for`.
class StringSplit
{
public:
    /// Iterates the parts of a [`Marlin::StringSplit`].
    class Iterator
    {
    public:
        /// Creates an iterator over `rest`; `done` starts it at the end.
        constexpr Iterator(StringView rest, char separator, bool done) : _rest(rest), _separator(separator), _done(done)
        {
            Advance();
        }

        /// Returns the current part.
        constexpr StringView operator*() const
        {
            return _current;
        }

        /// Advances to the next part.
        constexpr Iterator &operator++()
        {
            Advance();
            return *this;
        }

        /// Returns whether both iterators are at the same position.
        constexpr bool operator==(const Iterator &other) const
        {
            return _finished == other._finished && (_finished || _rest.Data() == other._rest.Data());
        }

    private:
        constexpr void Advance()
        {
            if (_done)
            {
                _finished = true;
                return;
            }
            _done = !_rest.SplitFirst(_separator, _current, _rest);
        }

        StringView _rest;
        StringView _current;
        char _separator;
        bool _done;
        bool _finished = false;
    };

    /// Creates a splitter over `text` with `separator`.
    constexpr StringSplit(StringView text, char separator) : _text(text), _separator(separator)
    {
    }

    /// Returns an iterator at the first part.
    [[nodiscard]] constexpr Iterator begin() const
    {
        return {_text, _separator, false};
    }

    /// Returns the past-the-end iterator.
    [[nodiscard]] constexpr Iterator end() const
    {
        return {_text, _separator, true};
    }

private:
    StringView _text;
    char _separator;
};

constexpr StringSplit StringView::Split(char separator) const
{
    return {*this, separator};
}
} // namespace Marlin
