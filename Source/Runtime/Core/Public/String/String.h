//! Owned, null-terminated text with a small-buffer optimization.
//!
//! Up to `String::InlineCapacity` bytes live inside the object; longer text moves to a heap buffer that grows
//! geometrically. The bytes are copied, compared and searched byte-wise.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/MemoryOps.h"
#include "String/StringView.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

#include <compare>

namespace Marlin
{
/// Owned, null-terminated UTF-8 text.
///
/// Up to `InlineCapacity` bytes are stored inside the object; longer text uses a heap buffer that grows
/// geometrically. A null terminator always follows the bytes and is not counted by `Length()`.
///
/// # Thread safety
/// A string is not synchronized. Concurrent reads are safe only while no thread modifies the string.
///
/// # Examples
///
/// ```cpp
/// String path("Content/");
/// path += "Level.marlin";
/// bool isLevel = path.EndsWith(".marlin"); // -> true
/// ```
class CORE_API String
{
public:
    /// The result of a failed search, shared with `StringView`.
    static constexpr usize NotFound = StringView::NotFound;
    /// The bytes stored inside the object, not counting the terminator.
    static constexpr usize InlineCapacity = 3 * sizeof(void *) - 1;

    /// Creates an empty string.
    String()
    {
        InitInline();
    }

    /// Creates a copy of `text`.
    String(StringView text)
    {
        InitFrom(text.Data(), text.Length());
    }

    /// Creates a copy of a null-terminated string.
    String(const char *text) : String(StringView(text))
    {
    }

    /// Creates a copy of `length` bytes starting at `data`.
    ///
    /// # Preconditions
    /// `data` points to at least `length` bytes. The range is not checked.
    String(const char *data, usize length)
    {
        InitFrom(data, length);
    }

    /// Deleted: a null pointer is not a valid string.
    String(decltype(nullptr)) = delete;

    /// Creates a string of `count` copies of `c`.
    String(usize count, char c);

    /// Creates a copy of `other`.
    String(const String &other)
    {
        InitFrom(other.Data(), other.Length());
    }

    /// Moves the bytes out of `other`, which becomes empty.
    String(String &&other) noexcept
    {
        MemCopy(_bytes, other._bytes, sizeof(_bytes));
        other.InitInline();
    }

    /// Destroys the string and frees its heap buffer.
    ~String()
    {
        if (IsHeap())
        {
            FreeHeap();
        }
    }

    /// Replaces the text with a copy of `other`.
    String &operator=(const String &other)
    {
        if (this != &other)
        {
            Assign(other);
        }
        return *this;
    }

    /// Moves `other` into this string, leaving `other` empty.
    String &operator=(String &&other) noexcept
    {
        if (this != &other)
        {
            if (IsHeap())
            {
                FreeHeap();
            }
            MemCopy(_bytes, other._bytes, sizeof(_bytes));
            other.InitInline();
        }
        return *this;
    }

    /// Replaces the text with a copy of `text`.
    String &operator=(StringView text)
    {
        Assign(text);
        return *this;
    }

    /// Replaces the text with a copy of a null-terminated string.
    String &operator=(const char *text)
    {
        Assign(StringView(text));
        return *this;
    }

    /// Returns a pointer to the bytes; the byte at `Length()` is `'\0'`.
    ///
    /// The bytes may be modified in place, but that does not change the length.
    [[nodiscard]] char *Data()
    {
        return IsHeap() ? HeapData() : _bytes;
    }

    /// Returns a pointer to the bytes; the byte at `Length()` is `'\0'`.
    [[nodiscard]] const char *Data() const
    {
        return IsHeap() ? HeapData() : _bytes;
    }

    /// Returns the text as a null-terminated string.
    [[nodiscard]] const char *CStr() const
    {
        return Data();
    }

    /// Returns the length in bytes, not counting the terminator.
    [[nodiscard]] usize Length() const
    {
        return IsHeap() ? HeapLength() : InlineCapacity - static_cast<uint8>(_bytes[InlineCapacity]);
    }

    /// Returns the length in bytes.
    ///
    /// This alias lets a `String` convert to a `Span<char>`.
    [[nodiscard]] usize Count() const
    {
        return Length();
    }

    /// Returns the bytes that fit without reallocating, not counting the terminator.
    [[nodiscard]] usize Capacity() const
    {
        return IsHeap() ? HeapCapacity() : InlineCapacity;
    }

    /// Returns whether the string has no bytes.
    [[nodiscard]] bool IsEmpty() const
    {
        return Length() == 0;
    }

    /// Returns a reference to the byte at `index`.
    ///
    /// # Preconditions
    /// `index < Length()`. Checked by `MARLIN_CHECK_SLOW` in Debug builds; out of range is undefined behavior
    /// otherwise.
    [[nodiscard]] char &operator[](usize index)
    {
        MARLIN_CHECK_SLOW(index < Length());
        return Data()[index];
    }

    /// Returns the byte at `index`.
    ///
    /// # Preconditions
    /// `index < Length()`. Checked by `MARLIN_CHECK_SLOW` in Debug builds; out of range is undefined behavior
    /// otherwise.
    [[nodiscard]] char operator[](usize index) const
    {
        MARLIN_CHECK_SLOW(index < Length());
        return Data()[index];
    }

    /// Returns a view of the text, valid until the string is modified or destroyed.
    [[nodiscard]] StringView AsView() const
    {
        return {Data(), Length()};
    }

    /// Converts to a view of the text.
    operator StringView() const
    {
        return AsView();
    }

    /// Appends `text` and returns this string.
    ///
    /// `text` may refer into this string; it stays valid when the buffer reallocates.
    String &Append(StringView text)
    {
        return Append(text.Data(), text.Length());
    }

    /// Appends `length` bytes starting at `data` and returns this string.
    ///
    /// `data` may refer into this string; it stays valid when the buffer reallocates.
    String &Append(const char *data, usize length);

    /// Appends `c` and returns this string.
    String &Append(char c)
    {
        const usize length = Length();
        if (MARLIN_UNLIKELY(length == Capacity()))
        {
            Grow(length + 1);
        }
        Data()[length] = c;
        SetLength(length + 1);
        return *this;
    }

    /// Appends `count` copies of `c` and returns this string.
    String &Append(usize count, char c);

    /// Appends `text`.
    String &operator+=(StringView text)
    {
        return Append(text);
    }

    /// Appends `c`.
    String &operator+=(char c)
    {
        return Append(c);
    }

    /// Inserts `text` at `index`, shifting the bytes at and after it.
    ///
    /// An empty `text` leaves the string unchanged. `text` may refer into this string.
    ///
    /// # Preconditions
    /// `index <= Length()`. Checked by `MARLIN_ASSERT` when assertions are on; out of range is undefined behavior
    /// otherwise.
    void Insert(usize index, StringView text);

    /// Removes up to `count` bytes starting at `index`.
    ///
    /// `count` is clamped to the bytes that remain, so the default removes through the end.
    ///
    /// # Preconditions
    /// `index <= Length()`. Checked by `MARLIN_ASSERT` when assertions are on; out of range is undefined behavior
    /// otherwise.
    void Remove(usize index, usize count = NotFound);

    /// Replaces every occurrence of `from` with `to` and returns how many there were.
    ///
    /// Both views may refer into this string.
    ///
    /// # Preconditions
    /// `from` is not empty. Checked by `MARLIN_ASSERT` when assertions are on; an empty `from` does not terminate
    /// otherwise.
    usize Replace(StringView from, StringView to);

    /// Replaces every `from` byte with `to`, in place.
    void Replace(char from, char to);

    /// Removes all text, keeping the capacity.
    void Clear()
    {
        SetLength(0);
    }

    /// Ensures capacity for `capacity` bytes; never shrinks.
    void Reserve(usize capacity)
    {
        if (capacity > Capacity())
        {
            Grow(capacity);
        }
    }

    /// Sets the length to `length`.
    ///
    /// New bytes are `fill`; bytes past `length` are discarded. The null terminator is maintained.
    void Resize(usize length, char fill = '\0');

    /// Sets the length to `length` and returns the data for the caller to fill.
    ///
    /// New bytes are uninitialized, and the terminator is already written at `length`. The caller must write the new
    /// bytes before reading them.
    char *ResizeUninitialized(usize length);

    /// Releases unused capacity when that reduces it, possibly moving the text back into the inline buffer.
    void ShrinkToFit();

    /// Converts every ASCII letter to upper case; other bytes are unchanged.
    void ToUpper();
    /// Converts every ASCII letter to lower case; other bytes are unchanged.
    void ToLower();

    /// Removes leading ASCII white space.
    void TrimStart();
    /// Removes trailing ASCII white space.
    void TrimEnd();
    /// Removes leading and trailing ASCII white space.
    void Trim();

    /// Returns the index of the first `c` at or after `start`, or `NotFound`.
    [[nodiscard]] usize IndexOf(char c, usize start = 0) const
    {
        return AsView().IndexOf(c, start);
    }

    /// Returns the index of the first occurrence of `text` at or after `start`, or `NotFound`.
    [[nodiscard]] usize IndexOf(StringView text, usize start = 0) const
    {
        return AsView().IndexOf(text, start);
    }

    /// Returns the index of the last `c`, or `NotFound`.
    [[nodiscard]] usize LastIndexOf(char c) const
    {
        return AsView().LastIndexOf(c);
    }

    /// Returns the index of the last occurrence of `text`, or `NotFound`.
    [[nodiscard]] usize LastIndexOf(StringView text) const
    {
        return AsView().LastIndexOf(text);
    }

    /// Returns whether `c` occurs in the string.
    [[nodiscard]] bool Contains(char c) const
    {
        return AsView().Contains(c);
    }

    /// Returns whether `text` occurs in the string.
    [[nodiscard]] bool Contains(StringView text) const
    {
        return AsView().Contains(text);
    }

    /// Returns whether the string starts with `prefix`.
    [[nodiscard]] bool StartsWith(StringView prefix) const
    {
        return AsView().StartsWith(prefix);
    }

    /// Returns whether the string ends with `suffix`.
    [[nodiscard]] bool EndsWith(StringView suffix) const
    {
        return AsView().EndsWith(suffix);
    }

    /// Returns whether `other` equals the text ignoring ASCII case.
    [[nodiscard]] bool EqualsIgnoreCase(StringView other) const
    {
        return AsView().EqualsIgnoreCase(other);
    }

    /// Returns a 64-bit hash of the bytes; equal strings hash equally.
    [[nodiscard]] uint64 GetHash() const
    {
        return AsView().GetHash();
    }

    /// Returns a pointer to the first byte, for range-based `for`.
    [[nodiscard]] char *begin()
    {
        return Data();
    }

    /// Returns one past the last byte, for range-based `for`.
    [[nodiscard]] char *end()
    {
        return Data() + Length();
    }

    /// Returns a pointer to the first byte, for range-based `for`.
    [[nodiscard]] const char *begin() const
    {
        return Data();
    }

    /// Returns one past the last byte, for range-based `for`.
    [[nodiscard]] const char *end() const
    {
        return Data() + Length();
    }

    /// Returns whether both strings have the same bytes.
    friend bool operator==(const String &a, const String &b)
    {
        return a.AsView() == b.AsView();
    }

    /// Returns whether the bytes equal `b`.
    friend bool operator==(const String &a, StringView b)
    {
        return a.AsView() == b;
    }

    /// Returns whether the bytes equal the null-terminated `b`.
    friend bool operator==(const String &a, const char *b)
    {
        return a.AsView() == StringView(b);
    }

    /// Orders strings lexicographically by their bytes.
    friend std::strong_ordering operator<=>(const String &a, const String &b)
    {
        return a.AsView() <=> b.AsView();
    }

    /// Orders the bytes of `a` and `b` lexicographically.
    friend std::strong_ordering operator<=>(const String &a, StringView b)
    {
        return a.AsView() <=> b;
    }

    /// Orders the bytes of `a` and the null-terminated `b` lexicographically.
    friend std::strong_ordering operator<=>(const String &a, const char *b)
    {
        return a.AsView() <=> StringView(b);
    }

    /// Concatenates `a` and `b` into a new string.
    friend String operator+(const String &a, StringView b)
    {
        String result;
        result.Reserve(a.Length() + b.Length());
        result.Append(a).Append(b);
        return result;
    }

    /// Appends `b` to `a` and returns `a`, reusing its buffer.
    friend String operator+(String &&a, StringView b)
    {
        a.Append(b);
        return Move(a);
    }

    /// Concatenates `a` and `b` into a new string.
    friend String operator+(StringView a, const String &b)
    {
        String result;
        result.Reserve(a.Length() + b.Length());
        result.Append(a).Append(b);
        return result;
    }

    /// Concatenates `a` and the null-terminated `b` into a new string.
    friend String operator+(const String &a, const char *b)
    {
        return a + StringView(b);
    }

    /// Appends the null-terminated `b` to `a` and returns `a`, reusing its buffer.
    friend String operator+(String &&a, const char *b)
    {
        return Move(a) + StringView(b);
    }

    /// Concatenates two strings into a new string.
    friend String operator+(const String &a, const String &b)
    {
        return a + b.AsView();
    }

    /// Appends `b` to `a` and returns `a`, reusing its buffer.
    friend String operator+(String &&a, const String &b)
    {
        return Move(a) + b.AsView();
    }

private:
    static constexpr uint8 HeapFlag = 0x80;
    static constexpr usize CapacityMask = ~usize{0} >> 8;

    [[nodiscard]] bool IsHeap() const
    {
        return (static_cast<uint8>(_bytes[InlineCapacity]) & HeapFlag) != 0;
    }

    [[nodiscard]] char *HeapData() const
    {
        char *data = nullptr;
        MemCopy(static_cast<void *>(&data), _bytes, sizeof(data));
        return data;
    }

    [[nodiscard]] usize HeapLength() const
    {
        usize length = 0;
        MemCopy(&length, _bytes + sizeof(void *), sizeof(length));
        return length;
    }

    [[nodiscard]] usize HeapCapacity() const
    {
        usize capacity = 0;
        MemCopy(&capacity, _bytes + 2 * sizeof(void *), sizeof(capacity));
        return capacity & CapacityMask;
    }

    void InitInline()
    {
        MemZero(_bytes, sizeof(_bytes));
        _bytes[InlineCapacity] = static_cast<char>(InlineCapacity);
    }

    void InitFrom(const char *data, usize length);
    void SetHeap(char *data, usize length, usize capacity);
    void SetLength(usize length);
    void Assign(StringView text);
    void Grow(usize required);
    void FreeHeap();

    alignas(void *) char _bytes[3 * sizeof(void *)];
};

static_assert(sizeof(String) == 3 * sizeof(void *));
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "String keeps its heap flag in the capacity's top byte");

/// Marks `String` as trivially relocatable: moving it is equivalent to copying its bytes and discarding the source.
template <> inline constexpr bool IsTriviallyRelocatable<String> = true;
} // namespace Marlin
