//! A non-owning view of contiguous elements, for passing ranges regardless of the container and allocator.
//!
//! A span does not own its elements or extend their lifetime.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// A non-owning view of a contiguous range of `T`.
///
/// Copies of a span refer to the same elements, and the view is valid only while the storage it was created from is
/// alive. `T` may be `const` to make the elements read-only.
///
/// # Thread safety
/// Reading the span itself is safe from any thread. Accessing its elements follows the rules of the storage it views.
///
/// # Examples
///
/// ```cpp
/// int32 values[] = {3, 1, 4};
/// Span<int32> span = values;
/// Span<int32> tail = span.Slice(1);
/// int32 second = tail[0]; // -> 1
/// ```
template <typename T> class Span
{
public:
    /// Creates an empty span.
    constexpr Span() = default;

    /// Creates a span of `count` elements starting at `data`.
    ///
    /// # Preconditions
    /// `data` points to at least `count` elements, or `count` is zero. The range is not checked.
    constexpr Span(T *data, usize count) : _data(data), _count(count)
    {
    }

    /// Creates a span over the elements of a C array.
    template <usize TCount> constexpr Span(T (&array)[TCount]) : _data(array), _count(TCount)
    {
    }

    /// Creates a span over a container with `Data()` and `Count()`, such as [`List`], [`Array`] or [`String`].
    ///
    /// The container must not reallocate or be destroyed while the span is used.
    template <typename TContainer>
        requires(requires(TContainer &container) {
                    { container.Data() } -> ConvertibleTo<T *>;
                    { container.Count() } -> ConvertibleTo<usize>;
                } && !IsSame<RemoveCvRef<TContainer>, Span>)
    constexpr Span(TContainer &&container) : _data(container.Data()), _count(container.Count())
    {
    }

    /// Creates a span over a brace-enclosed initializer list.
    ///
    /// # Safety
    /// The initializer list's backing array lives only until the end of the full expression, so the span must not
    /// outlive it.
    constexpr Span(std::initializer_list<RemoveConst<T>> values)
        requires(IsConst<T>)
        : _data(values.begin()), _count(values.size())
    {
    }

    /// Creates a span of const elements from a span of mutable elements.
    template <typename TOther>
        requires(IsConvertible<TOther (*)[], T (*)[]>)
    constexpr Span(const Span<TOther> &other) : _data(other.Data()), _count(other.Count())
    {
    }

    /// Returns the stored pointer.
    ///
    /// It is null for a default-constructed span and otherwise points to the first element.
    [[nodiscard]] constexpr T *Data() const
    {
        return _data;
    }

    /// Returns the number of elements.
    [[nodiscard]] constexpr usize Count() const
    {
        return _count;
    }

    /// Returns the size of the viewed elements in bytes.
    [[nodiscard]] constexpr usize SizeInBytes() const
    {
        return _count * sizeof(T);
    }

    /// Returns whether the span has no elements.
    [[nodiscard]] constexpr bool IsEmpty() const
    {
        return _count == 0;
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`. The index is checked by an assertion in builds with assertions; out of range is undefined
    /// behavior otherwise.
    [[nodiscard]] constexpr T &operator[](usize index) const
    {
        MARLIN_ASSERT(index < _count);
        return _data[index];
    }

    /// Returns the first element.
    ///
    /// # Preconditions
    /// The span is not empty.
    [[nodiscard]] constexpr T &First() const
    {
        MARLIN_ASSERT(_count > 0);
        return _data[0];
    }

    /// Returns the last element.
    ///
    /// # Preconditions
    /// The span is not empty.
    [[nodiscard]] constexpr T &Last() const
    {
        MARLIN_ASSERT(_count > 0);
        return _data[_count - 1];
    }

    /// Returns `count` elements starting at `start`, or every element from `start` when `count` is omitted.
    ///
    /// `count` is clamped to the elements that remain.
    ///
    /// # Preconditions
    /// `start <= Count()`. The start is checked by an assertion in builds with assertions; out of range is undefined
    /// behavior otherwise.
    [[nodiscard]] constexpr Span Slice(usize start, usize count = ~usize{0}) const
    {
        MARLIN_ASSERT(start <= _count);
        const usize available = _count - start;
        return {_data + start, count < available ? count : available};
    }

    /// Returns the first `count` elements, or the whole span when it is shorter.
    [[nodiscard]] constexpr Span Left(usize count) const
    {
        return Slice(0, count);
    }

    /// Returns the last `count` elements, or the whole span when it is shorter.
    [[nodiscard]] constexpr Span Right(usize count) const
    {
        return count >= _count ? *this : Slice(_count - count);
    }

    /// Returns the first element, for range-based `for`.
    [[nodiscard]] constexpr T *begin() const
    {
        return _data;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] constexpr T *end() const
    {
        return _data + _count;
    }

private:
    T *_data = nullptr;
    usize _count = 0;
};

/// Deduces the element type of a span from a C array.
template <typename T, usize TCount> Span(T (&)[TCount]) -> Span<T>;

/// Views the bytes of `span` as read-only `uint8` values.
///
/// The returned span has `span.SizeInBytes()` elements. No copy is made.
template <typename T> [[nodiscard]] Span<const uint8> AsBytes(Span<T> span)
{
    return {reinterpret_cast<const uint8 *>(span.Data()), span.SizeInBytes()};
}

/// Views the bytes of `span` as writable `uint8` values.
///
/// The returned span has `span.SizeInBytes()` elements. No copy is made. `T` must not be `const`.
template <typename T> [[nodiscard]] Span<uint8> AsWritableBytes(Span<T> span)
{
    return {reinterpret_cast<uint8 *>(span.Data()), span.SizeInBytes()};
}
} // namespace Marlin
