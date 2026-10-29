//! Containers that live in a loaded memory image: frozen arrays and frozen strings.
//!
//! A frozen container is a pointer, a 32-bit count and a 32-bit owned flag, so a loaded asset block can be used in
//! place on any platform. Borrowed elements belong to the block; copying, resizing or removing detaches them to
//! the heap.

#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "String/StringView.h"
#include "Templates/Utility.h"

namespace Marlin
{
class StringWriter;

/// Identifies how a frozen container is laid out in a memory image.
///
/// The reflected type of a frozen container reports one of these through `TypeInfo::GetExtension`; image writers
/// and loaders use the tag to tell frozen containers apart from containers that own heap memory.
enum class FrozenLayout : uint8
{
    /// A `FrozenArray`: a data pointer or offset, an element count and an owned flag.
    Array,
    /// A `FrozenString`: a data pointer or offset, a character count and an owned flag.
    String,
};

namespace Private
{
/// The layout that every frozen container shares.
///
/// In a memory image, `Data` holds an offset from the start of the block. The loader replaces it with the address of
/// the data inside the block (the block's address plus that offset) and leaves `Owned` zero. A container with `Owned`
/// set owns `Data` and frees it on destruction. `Count` is the element count for an array and the character count for
/// a string.
struct FrozenStorage
{
    void *Data;
    uint32 Count;
    uint32 Owned;
};

/// The layout tag of [`FrozenArray`], reported by its reflected type.
CORE_API extern const FrozenLayout FrozenArrayLayout;
/// The layout tag of [`FrozenString`], reported by its reflected type.
CORE_API extern const FrozenLayout FrozenStringLayout;
} // namespace Private

/// An array that either owns heap elements or borrows them from a loaded memory image.
///
/// In a memory image the array is a pointer and a count with a fixed layout on every platform, so a loaded block is
/// used in place. [`Resize`], [`RemoveAt`] and assignment detach a borrowed array into owned storage; writing
/// through `operator[]` does not detach and modifies the block. `T` must be copy-constructible; growing also
/// default-constructs elements, and [`RemoveAt`] copy-assigns them.
///
/// # Thread safety
/// A single array is not synchronized. Access to a borrowed array must be synchronized with every writer of the
/// block.
///
/// # Examples
///
/// ```cpp
/// const int32 source[] = {1, 2, 3};
/// FrozenArray<int32> values{Span<const int32>(source)};
/// values.Resize(5);
/// usize count = values.Count(); // -> 5
/// int32 last = values[4];       // -> 0
/// ```
template <typename T> class FrozenArray
{
public:
    /// Creates an empty array.
    FrozenArray() = default;

    /// Copies `values` into owned storage.
    FrozenArray(Span<const T> values)
    {
        Assign(values);
    }

    /// Copies the elements of `other` into owned storage.
    FrozenArray(const FrozenArray &other)
    {
        Assign(other.AsSpan());
    }

    /// Takes the elements of `other` and leaves it empty.
    FrozenArray(FrozenArray &&other) noexcept : _data(other._data), _count(other._count), _owned(other._owned)
    {
        other._data = nullptr;
        other._count = 0;
        other._owned = 0;
    }

    /// Frees owned elements.
    ~FrozenArray()
    {
        Reset();
    }

    /// Replaces the elements with a copy of `other`, owning them.
    FrozenArray &operator=(const FrozenArray &other)
    {
        if (this != &other)
        {
            Assign(other.AsSpan());
        }
        return *this;
    }

    /// Frees owned elements, then takes the elements of `other` and leaves it empty.
    FrozenArray &operator=(FrozenArray &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            _data = other._data;
            _count = other._count;
            _owned = other._owned;
            other._data = nullptr;
            other._count = 0;
            other._owned = 0;
        }
        return *this;
    }

    /// Returns the number of elements.
    [[nodiscard]] usize Count() const
    {
        return _count;
    }

    /// Returns whether there are no elements.
    [[nodiscard]] bool IsEmpty() const
    {
        return _count == 0;
    }

    /// Returns whether the array owns its elements rather than borrowing them from a loaded block.
    [[nodiscard]] bool IsOwned() const
    {
        return _owned != 0;
    }

    /// Returns a pointer to the first element.
    [[nodiscard]] T *Data()
    {
        return _data;
    }

    /// Returns a pointer to the first element.
    [[nodiscard]] const T *Data() const
    {
        return _data;
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] T &operator[](usize index)
    {
        MARLIN_ASSERT(index < _count);
        return _data[index];
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] const T &operator[](usize index) const
    {
        MARLIN_ASSERT(index < _count);
        return _data[index];
    }

    /// Returns a view of the elements.
    ///
    /// The view is invalidated when the storage changes.
    [[nodiscard]] Span<T> AsSpan()
    {
        return {_data, _count};
    }

    /// Returns a view of the elements.
    ///
    /// The view is invalidated when the storage changes.
    [[nodiscard]] Span<const T> AsSpan() const
    {
        return {_data, _count};
    }

    /// Returns a pointer to the first element, for range-based `for`.
    [[nodiscard]] T *begin()
    {
        return _data;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] T *end()
    {
        return _data + _count;
    }

    /// Returns a pointer to the first element, for range-based `for`.
    [[nodiscard]] const T *begin() const
    {
        return _data;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] const T *end() const
    {
        return _data + _count;
    }

    /// Resizes to `count` elements, keeping the first ones and default-constructing the rest.
    ///
    /// Borrowed elements are copied into newly allocated owned storage, so the array owns its elements afterwards.
    ///
    /// # Preconditions
    /// `count <= 0xFFFFFFFF`. The limit is checked by an assertion in builds with assertions.
    ///
    /// # Performance
    /// Allocates a new buffer and moves or copies the elements that are kept.
    void Resize(usize count)
    {
        MARLIN_ASSERT(count <= 0xFFFFFFFFu);
        T *data = count != 0 ? static_cast<T *>(Memory::Allocate(count * sizeof(T), alignof(T))) : nullptr;
        const usize kept = count < _count ? count : _count;
        if (_owned != 0)
        {
            RelocateRange(data, _data, kept);
            DestroyRange(_data + kept, _count - kept);
            Memory::Free(_data);
        }
        else
        {
            CopyConstructRange(data, _data, kept);
        }
        DefaultConstructRange(data + kept, count - kept);
        _data = data;
        _count = static_cast<uint32>(count);
        _owned = 1;
    }

    /// Removes the element at `index`, keeping the order of the remaining elements.
    ///
    /// A borrowed array becomes owned, because the remaining elements are copied into new storage.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    ///
    /// # Performance
    /// Allocates a new buffer and copies every remaining element.
    void RemoveAt(usize index)
    {
        MARLIN_ASSERT(index < _count);
        FrozenArray copy;
        copy.Resize(_count - 1);
        for (usize i = 0, j = 0; i < _count; ++i)
        {
            if (i != index)
            {
                copy._data[j++] = _data[i];
            }
        }
        *this = Move(copy);
    }

    /// Frees owned elements and leaves the array empty.
    ///
    /// Borrowed elements are not freed.
    void Reset()
    {
        if (_owned != 0)
        {
            DestroyRange(_data, _count);
            Memory::Free(_data);
        }
        _data = nullptr;
        _count = 0;
        _owned = 0;
    }

    /// Returns whether both arrays hold equal elements, in order.
    ///
    /// Available only when `T` supports `==`.
    bool operator==(const FrozenArray &other) const
        requires requires(const T &value) { value == value; }
    {
        if (_count != other._count)
        {
            return false;
        }
        for (usize i = 0; i < _count; ++i)
        {
            if (!(_data[i] == other._data[i]))
            {
                return false;
            }
        }
        return true;
    }

private:
    void Assign(Span<const T> values)
    {
        Reset();
        if (!values.IsEmpty())
        {
            MARLIN_ASSERT(values.Count() <= 0xFFFFFFFFu);
            _data = static_cast<T *>(Memory::Allocate(values.Count() * sizeof(T), alignof(T)));
            CopyConstructRange(_data, values.Data(), values.Count());
            _count = static_cast<uint32>(values.Count());
            _owned = 1;
        }
    }

    T *_data = nullptr;
    uint32 _count = 0;
    uint32 _owned = 0;
};

/// A string with the same memory-image layout as `FrozenArray<char>`, kept null-terminated.
///
/// The length excludes the terminating null. A string either owns a heap copy or borrows characters from a loaded
/// block; copying, assignment and reset never free borrowed characters.
///
/// # Thread safety
/// A single string is not synchronized. Access to a borrowed string must be synchronized with every writer of the
/// block.
///
/// # Examples
///
/// ```cpp
/// FrozenString name(StringView("Marlin"));
/// usize length = name.Length();    // -> 6
/// const char *text = name.CStr();  // -> "Marlin"
/// ```
class CORE_API FrozenString
{
public:
    /// Creates an empty string.
    FrozenString() = default;
    /// Copies `text` into owned storage, appending a terminating null.
    ///
    /// # Preconditions
    /// `text.Length() < 0xFFFFFFFF`. The limit is checked by an assertion in builds with assertions.
    FrozenString(StringView text);
    /// Copies `other` into owned storage.
    FrozenString(const FrozenString &other);
    /// Takes the characters of `other` and leaves it empty.
    FrozenString(FrozenString &&other) noexcept;
    /// Frees owned characters.
    ~FrozenString();
    /// Replaces the contents with a copy of `other`.
    FrozenString &operator=(const FrozenString &other);
    /// Frees owned characters, then takes the characters of `other` and leaves it empty.
    FrozenString &operator=(FrozenString &&other) noexcept;

    /// Returns the number of characters, excluding the terminating null.
    [[nodiscard]] usize Length() const
    {
        return _length;
    }

    /// Returns whether there are no characters.
    [[nodiscard]] bool IsEmpty() const
    {
        return _length == 0;
    }

    /// Returns a view of the characters, without the terminating null.
    [[nodiscard]] StringView AsView() const
    {
        return {_data, _length};
    }

    /// Returns a null-terminated pointer, also when empty.
    [[nodiscard]] const char *CStr() const
    {
        return _data != nullptr ? _data : "";
    }

    /// Frees owned characters and leaves the string empty.
    void Reset();

    /// Appends the characters to `output`.
    ///
    /// `spec` is accepted for the formatter interface and ignored.
    void FormatTo(StringWriter &output, StringView spec) const;
    /// Replaces `value` with `text`; returns true.
    ///
    /// Every string is valid, so parsing never fails. The characters are copied.
    static bool TryParse(StringView text, FrozenString &value);

    /// Returns whether both strings hold the same characters.
    bool operator==(const FrozenString &other) const
    {
        return AsView() == other.AsView();
    }

private:
    char *_data = nullptr;
    uint32 _length = 0;
    uint32 _owned = 0;
};

static_assert(sizeof(FrozenArray<uint32>) == sizeof(Private::FrozenStorage));
static_assert(sizeof(FrozenString) == sizeof(Private::FrozenStorage));
} // namespace Marlin
