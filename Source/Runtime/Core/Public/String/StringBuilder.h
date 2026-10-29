//! Growable text buffers for formatting, with optional inline storage.
//!
//! A [`Marlin::StringWriter`] owns its heap buffer; a [`Marlin::StringBuilder`] starts in a fixed buffer stored in
//! the object.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/MemoryOps.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// A growable text buffer that formatting writes into.
///
/// The base class has no inline storage and allocates on the first write; [`Marlin::StringBuilder`] adds a buffer
/// stored in the object. A writer is not copyable, and pointers returned by [`Data`] are invalidated by any write
/// that grows the buffer.
class CORE_API StringWriter
{
public:
    /// Creates an empty writer; the first write allocates.
    StringWriter() = default;
    /// Destroys the writer and frees its heap buffer.
    ~StringWriter();
    MARLIN_NONCOPYABLE(StringWriter);

    /// Returns the text bytes, which are not null-terminated until [`CStr`] is called.
    [[nodiscard]] char *Data()
    {
        return _data;
    }

    /// Returns the text bytes, which are not null-terminated until [`CStr`] is called.
    [[nodiscard]] const char *Data() const
    {
        return _data;
    }

    /// Returns the number of bytes written.
    [[nodiscard]] usize Length() const
    {
        return _length;
    }

    /// Returns whether no bytes were written.
    [[nodiscard]] bool IsEmpty() const
    {
        return _length == 0;
    }

    /// Returns a view of the text, valid until the next write.
    [[nodiscard]] StringView AsView() const
    {
        return {_data, _length};
    }

    /// Converts to a view of the text.
    operator StringView() const
    {
        return AsView();
    }

    /// Returns the text as a null-terminated string, writing the terminator first.
    ///
    /// The result is valid until the next write.
    [[nodiscard]] const char *CStr()
    {
        _data[_length] = '\0';
        return _data;
    }

    /// Returns a copy of the text as a [`Marlin::String`].
    [[nodiscard]] String ToString() const
    {
        return {_data, _length};
    }

    /// Appends `text`.
    ///
    /// # Preconditions
    /// `text` must not point into this writer's own text when the append grows the buffer, because growth reallocates
    /// it.
    StringWriter &Append(StringView text)
    {
        MemCopy(AppendUninitialized(text.Length()), text.Data(), text.Length());
        return *this;
    }

    /// Appends `c`.
    StringWriter &Append(char c)
    {
        if (MARLIN_UNLIKELY(_length == _capacity))
        {
            Grow(1);
        }
        _data[_length++] = c;
        return *this;
    }

    /// Appends `count` copies of `c`.
    StringWriter &Append(usize count, char c)
    {
        MemSet(AppendUninitialized(count), static_cast<uint8>(c), count);
        return *this;
    }

    /// Extends the text by `count` bytes and returns them for the caller to fill.
    ///
    /// The bytes are uninitialized and are part of the text immediately; the caller must write all of them before
    /// the next read or write.
    char *AppendUninitialized(usize count)
    {
        if (MARLIN_UNLIKELY(_capacity - _length < count))
        {
            Grow(count);
        }
        char *result = _data + _length;
        _length += count;
        return result;
    }

    /// Shortens the text to `length`; a larger `length` is ignored and the capacity is unchanged.
    void Truncate(usize length)
    {
        _length = length < _length ? length : _length;
    }

    /// Removes all text, keeping the capacity.
    void Clear()
    {
        _length = 0;
    }

    /// Ensures room for `capacity` bytes; never shrinks.
    void Reserve(usize capacity)
    {
        if (capacity > _capacity)
        {
            Grow(capacity - _length);
        }
    }

protected:
    /// Creates a writer over `buffer`, which holds `size` bytes, one of them reserved for the terminator.
    ///
    /// The writer does not own `buffer`; it allocates a heap buffer only after the inline one is outgrown.
    StringWriter(char *buffer, usize size) : _data(buffer), _capacity(size - 1), _inlineBuffer(buffer)
    {
    }

private:
    void Grow(usize extra);

    char _empty[1] = {};
    char *_data = _empty;
    usize _length = 0;
    usize _capacity = 0;
    char *_inlineBuffer = _empty;
};

/// Builds text in place, starting in a buffer of `TInlineSize` bytes stored in the object.
///
/// Up to `TInlineSize - 1` bytes fit before the writer allocates on the heap. `TInlineSize` must be at least 2 and
/// defaults to 128.
///
/// # Examples
///
/// ```cpp
/// StringBuilder<> builder;
/// builder.Append("Score: ").Append('7');
/// String text = builder.ToString(); // -> "Score: 7"
/// ```
template <usize TInlineSize = 128> class StringBuilder : public StringWriter
{
    static_assert(TInlineSize >= 2);

public:
    /// Creates an empty builder.
    StringBuilder() : StringWriter(_buffer, TInlineSize)
    {
    }

    /// Creates a builder holding `text`.
    explicit StringBuilder(StringView text) : StringBuilder()
    {
        Append(text);
    }

    ~StringBuilder() = default;
    MARLIN_NONCOPYABLE(StringBuilder);

private:
    char _buffer[TInlineSize];
};
} // namespace Marlin
