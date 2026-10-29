//! Fixed-size arrays whose element type and count are part of the type.
//!
//! The elements are stored directly in the object with no indirection, and the count never changes.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"

#include <compare>

namespace Marlin
{
/// A fixed-size array with its element count as a template parameter.
///
/// `T` is the element type and `TCount` is the element count. The type is an aggregate, so a value can be
/// brace-initialized.
///
/// # Examples
///
/// ```cpp
/// Array<int32, 3> values = {1, 2, 3};
/// values.Fill(0);
/// usize count = values.Count(); // -> 3
/// int32 first = values[0];      // -> 0
/// ```
template <typename T, usize TCount> struct Array
{
    /// The `TCount` elements, stored contiguously in declaration order.
    ///
    /// The member is public so that the type stays an aggregate.
    T Elements[TCount];

    /// Returns the number of elements, which is `TCount`.
    [[nodiscard]] static constexpr usize Count()
    {
        return TCount;
    }

    /// Returns a pointer to the first element.
    [[nodiscard]] constexpr T *Data()
    {
        return Elements;
    }

    /// Returns a pointer to the first element.
    [[nodiscard]] constexpr const T *Data() const
    {
        return Elements;
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`. The index is checked by an assertion in builds with assertions; out of range is undefined
    /// behavior otherwise.
    [[nodiscard]] constexpr T &operator[](usize index)
    {
        MARLIN_ASSERT(index < TCount);
        return Elements[index];
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`. The index is checked by an assertion in builds with assertions; out of range is undefined
    /// behavior otherwise.
    [[nodiscard]] constexpr const T &operator[](usize index) const
    {
        MARLIN_ASSERT(index < TCount);
        return Elements[index];
    }

    /// Assigns `value` to every element.
    constexpr void Fill(const T &value)
    {
        for (T &element : Elements)
        {
            element = value;
        }
    }

    /// Returns a pointer to the first element, for range-based `for`.
    [[nodiscard]] constexpr T *begin()
    {
        return Elements;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] constexpr T *end()
    {
        return Elements + TCount;
    }

    /// Returns a pointer to the first element, for range-based `for`.
    [[nodiscard]] constexpr const T *begin() const
    {
        return Elements;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] constexpr const T *end() const
    {
        return Elements + TCount;
    }

    /// Returns whether both arrays hold equal elements, in order.
    ///
    /// The comparison is defaulted, so it requires `T` to support `==`.
    constexpr bool operator==(const Array &) const = default;
    /// Compares both arrays lexicographically.
    ///
    /// The comparison is defaulted, so it requires `T` to support `<=>`.
    constexpr auto operator<=>(const Array &) const = default;
};

/// Deduces the element type and count from the initializer values.
template <typename T, typename... TRest> Array(T, TRest...) -> Array<T, 1 + sizeof...(TRest)>;
} // namespace Marlin
