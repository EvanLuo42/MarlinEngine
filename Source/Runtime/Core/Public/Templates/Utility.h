//! Core vocabulary utilities: move and forward, address-of, swap and exchange, min/max/clamp, enum conversion,
//! pairs, integer sequences and tag types.

#pragma once

#include "CoreTypes.h"
#include "Templates/TypeTraits.h"

#include <compare>
#include <initializer_list>
#include <new>

namespace Marlin
{
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
/// Casts `value` to an rvalue reference so that it can be moved from.
template <typename T> [[nodiscard]] constexpr RemoveReference<T> &&Move(T &&value) noexcept
{
    return static_cast<RemoveReference<T> &&>(value);
}

/// Forwards `value` as `T&&`, preserving its original value category.
///
/// `T` is normally the template parameter of the enclosing forwarding reference.
template <typename T> [[nodiscard]] constexpr T &&Forward(RemoveReference<T> &value) noexcept
{
    return static_cast<T &&>(value);
}

/// Forwards an rvalue as `T&&`.
///
/// # Preconditions
/// `T` must not be an lvalue reference type; a `static_assert` rejects one.
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
template <typename T> [[nodiscard]] constexpr T &&Forward(RemoveReference<T> &&value) noexcept
{
    static_assert(!IsLvalueReference<T>, "Cannot forward an rvalue as an lvalue");
    return static_cast<T &&>(value);
}

/// Declares an unevaluated value of `T` for use in `decltype` and `sizeof`; never defined.
///
/// `T` may be incomplete or have no default constructor.
template <typename T> AddRvalueReference<T> Declval() noexcept;

/// Returns the address of `value`, even when `T` overloads `operator&`.
template <typename T> [[nodiscard]] constexpr T *AddressOf(T &value) noexcept
{
    return __builtin_addressof(value);
}

/// Exchanges the values of `a` and `b` through a temporary.
///
/// # Preconditions
/// `T` must be move constructible and move assignable.
template <typename T> constexpr void Swap(T &a, T &b) noexcept
{
    T temporary = Move(a);
    a = Move(b);
    b = Move(temporary);
}

/// Replaces `target` with `value` and returns the previous value.
///
/// # Examples
///
/// ```cpp
/// int32 value = 3;
/// int32 previous = Exchange(value, 7); // -> 3
/// int32 current = value;               // -> 7
/// ```
template <typename T, typename TValue = T> constexpr T Exchange(T &target, TValue &&value) noexcept
{
    T previous = Move(target);
    target = Forward<TValue>(value);
    return previous;
}

/// Returns the smaller of `a` and `b`, compared with `operator<`; returns `a` when both compare equal.
template <typename T> [[nodiscard]] constexpr T Min(T a, T b)
{
    return b < a ? b : a;
}

/// Returns the larger of `a` and `b`, compared with `operator<`; returns `a` when both compare equal.
template <typename T> [[nodiscard]] constexpr T Max(T a, T b)
{
    return a < b ? b : a;
}

/// Clamps `value` to the range `[low, high]`.
///
/// # Preconditions
/// `low` must not be greater than `high`.
///
/// # Examples
///
/// ```cpp
/// int32 a = Clamp(3, 0, 10);  // -> 3
/// int32 b = Clamp(-1, 0, 10); // -> 0
/// int32 c = Clamp(12, 0, 10); // -> 10
/// ```
template <typename T> [[nodiscard]] constexpr T Clamp(T value, T low, T high)
{
    if (value < low)
    {
        return low;
    }
    return high < value ? high : value;
}

/// Returns the integer value of the enum `value`, converted to its underlying type.
template <Enum T> [[nodiscard]] constexpr UnderlyingType<T> ToUnderlying(T value) noexcept
{
    return static_cast<UnderlyingType<T>>(value);
}

/// Two values of independent types, with structural equality and ordering.
///
/// # Examples
///
/// ```cpp
/// Pair pair{1, 2.5f};
/// int32 first = pair.First;     // -> 1
/// float32 second = pair.Second; // -> 2.5f
/// ```
template <typename TFirst, typename TSecond> struct Pair
{
    /// The first value.
    TFirst First;
    /// The second value.
    TSecond Second;

    /// Returns whether both members compare equal.
    constexpr bool operator==(const Pair &) const = default;
    /// Orders lexicographically by `First`, then `Second`.
    constexpr auto operator<=>(const Pair &) const = default;
};

/// Deduces the member types from the two values.
template <typename TFirst, typename TSecond> Pair(TFirst, TSecond) -> Pair<TFirst, TSecond>;

/// A compile-time sequence of values of type `T`.
template <typename T, T... TValues> struct IntegerSequence
{
    /// The number of values in the sequence.
    static constexpr usize Count = sizeof...(TValues);
};

/// An [IntegerSequence] of `usize` values.
template <usize... TValues> using IndexSequence = IntegerSequence<usize, TValues...>;
/// The sequence of `TCount` values of type `T`, from 0 to `TCount - 1`.
template <typename T, T TCount> using MakeIntegerSequence = __make_integer_seq<IntegerSequence, T, TCount>;
/// The sequence of `TCount` `usize` values, from 0 to `TCount - 1`.
template <usize TCount> using MakeIndexSequence = MakeIntegerSequence<usize, TCount>;

/// A tag type for a constructor that leaves storage uninitialized, for buffers filled immediately.
struct NoInitTag
{
    explicit NoInitTag() = default;
};
/// The tag value for uninitialized construction.
inline constexpr NoInitTag NoInit{};

/// A tag type that selects a constructor building the value in place from constructor arguments.
struct InPlaceTag
{
    explicit InPlaceTag() = default;
};
/// The tag value for in-place construction.
inline constexpr InPlaceTag InPlace{};
} // namespace Marlin
