//! An inline optional value, for values that may be absent without a separate pointer or sentinel.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/MemoryOps.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// A tag type that selects the empty state of [Optional].
struct NullOptionalTag
{
    /// Creates the tag; the argument is ignored.
    explicit constexpr NullOptionalTag(int32 /*unused*/)
    {
    }
};
/// The empty value for [Optional]; `Optional<T> value = NullOptional;` creates an empty optional.
inline constexpr NullOptionalTag NullOptional{0};

/// A value of `T` or nothing, stored inline.
///
/// An `Optional` never allocates for its own storage. It either holds a constructed `T` or is empty; destroying
/// the optional destroys a contained value. Use `HasValue()` or `operator bool` to test the state before
/// calling `Value()`.
///
/// # Examples
///
/// ```cpp
/// Optional<int32> value;
/// bool empty = !value; // -> true
/// value.Emplace(7);
/// int32 seven = value.Value(); // -> 7
/// ```
template <typename T> class Optional
{
public:
    /// Creates an empty optional.
    constexpr Optional() : _empty()
    {
    }

    /// Creates an empty optional from [NullOptional].
    constexpr Optional(NullOptionalTag /*tag*/) : _empty()
    {
    }

    /// Creates an optional that holds a copy of `value`.
    constexpr Optional(const T &value) : _value(value), _hasValue(true)
    {
    }

    /// Creates an optional that holds `value` moved in.
    constexpr Optional(T &&value) : _value(Move(value)), _hasValue(true)
    {
    }

    /// Creates an optional that holds a `T` constructed in place from `args`.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// Optional<int32> value(InPlace, 7);
    /// int32 seven = value.Value(); // -> 7
    /// ```
    template <typename... TArgs>
    constexpr explicit Optional(InPlaceTag /*tag*/, TArgs &&...args) : _value(Forward<TArgs>(args)...), _hasValue(true)
    {
    }

    /// Creates an optional that holds a copy of the value in `other`.
    constexpr Optional(const Optional &other) : _empty()
    {
        if (other._hasValue)
        {
            ConstructAt(&_value, other._value);
            _hasValue = true;
        }
    }

    /// Creates an optional that moves the value out of `other`; `other` keeps its state and a moved-from
    /// value.
    constexpr Optional(Optional &&other) noexcept : _empty()
    {
        if (other._hasValue)
        {
            ConstructAt(&_value, Move(other._value));
            _hasValue = true;
        }
    }

    /// Destroys the contained value, if any.
    constexpr ~Optional()
    {
        Reset();
    }

    /// Replaces the state and value with a copy of `other`.
    constexpr Optional &operator=(const Optional &other)
    {
        if (this != &other)
        {
            if (other._hasValue)
            {
                Emplace(other._value);
            }
            else
            {
                Reset();
            }
        }
        return *this;
    }

    /// Replaces the state and value by moving from `other`; `other` keeps its state and a moved-from value.
    constexpr Optional &operator=(Optional &&other) noexcept
    {
        if (this != &other)
        {
            if (other._hasValue)
            {
                Emplace(Move(other._value));
            }
            else
            {
                Reset();
            }
        }
        return *this;
    }

    /// Destroys the current value, constructs a `T` from `args` in place, and returns a reference to it.
    template <typename... TArgs> constexpr T &Emplace(TArgs &&...args)
    {
        Reset();
        ConstructAt(&_value, Forward<TArgs>(args)...);
        _hasValue = true;
        return _value;
    }

    /// Destroys the contained value, if any, and leaves the optional empty.
    constexpr void Reset()
    {
        if (_hasValue)
        {
            DestroyAt(&_value);
            _hasValue = false;
        }
    }

    /// Returns whether a value is stored.
    [[nodiscard]] constexpr bool HasValue() const
    {
        return _hasValue;
    }

    /// Returns whether a value is stored; the same as `HasValue()`.
    constexpr explicit operator bool() const
    {
        return _hasValue;
    }

    /// Returns the stored value as an lvalue.
    ///
    /// # Preconditions
    /// A value must be stored. The check is `MARLIN_ASSERT`, so accessing an empty optional is undefined
    /// behavior in Profile and Shipping.
    [[nodiscard]] constexpr T &Value() &
    {
        MARLIN_ASSERT(_hasValue);
        return _value;
    }

    /// Returns the stored value as a const lvalue.
    [[nodiscard]] constexpr const T &Value() const &
    {
        MARLIN_ASSERT(_hasValue);
        return _value;
    }

    /// Returns the stored value as an rvalue reference; the optional still holds it.
    [[nodiscard]] constexpr T &&Value() &&
    {
        MARLIN_ASSERT(_hasValue);
        return Move(_value);
    }

    /// Returns the stored value, or `fallback` when the optional is empty.
    [[nodiscard]] constexpr T ValueOr(const T &fallback) const &
    {
        return _hasValue ? _value : fallback;
    }

    /// Returns a pointer to the stored value, or null when the optional is empty.
    [[nodiscard]] constexpr T *TryGet()
    {
        return _hasValue ? &_value : nullptr;
    }

    /// Returns a const pointer to the stored value, or null when the optional is empty.
    [[nodiscard]] constexpr const T *TryGet() const
    {
        return _hasValue ? &_value : nullptr;
    }

    /// Returns the stored value; equivalent to `Value()`.
    [[nodiscard]] constexpr T &operator*() &
    {
        return Value();
    }

    /// Returns the stored value as a const lvalue; equivalent to `Value()`.
    [[nodiscard]] constexpr const T &operator*() const &
    {
        return Value();
    }

    /// Returns a pointer to the stored value, for member access.
    ///
    /// The precondition of `Value()` applies.
    [[nodiscard]] constexpr T *operator->()
    {
        return &Value();
    }

    /// Returns a const pointer to the stored value, for member access.
    ///
    /// The precondition of `Value()` applies.
    [[nodiscard]] constexpr const T *operator->() const
    {
        return &Value();
    }

    /// Returns whether both optionals are empty or hold equal values.
    constexpr bool operator==(const Optional &other) const
        requires(EqualityComparable<T>)
    {
        return _hasValue == other._hasValue && (!_hasValue || _value == other._value);
    }

    /// Returns whether the optional holds a value equal to `value`.
    constexpr bool operator==(const T &value) const
        requires(EqualityComparable<T>)
    {
        return _hasValue && _value == value;
    }

private:
    struct Empty
    {
    };

    union {
        Empty _empty;
        T _value;
    };
    bool _hasValue = false;
};

/// Deduces `T` from the constructor argument.
template <typename T> Optional(T) -> Optional<T>;
} // namespace Marlin
