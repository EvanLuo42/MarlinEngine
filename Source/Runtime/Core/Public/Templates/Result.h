//! A success-or-error return type for fallible functions, since the engine builds without exceptions.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/MemoryOps.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// The error half of a [Result], used to return an error unambiguously.
///
/// A `Result` can be constructed from a `Failure`; the deduction guide deduces `TError` from the value.
///
/// # Examples
///
/// ```cpp
/// #include "HAL/PlatformSocket.h"
///
/// Result<int32, SocketError> result = Failure(SocketError::ConnectionReset);
/// SocketError error = result.Error(); // -> SocketError::ConnectionReset
/// ```
template <typename TError> struct Failure
{
    /// The error value.
    TError Error;

    /// Creates a failure that holds `error`.
    constexpr explicit Failure(TError error) : Error(Move(error))
    {
    }
};

/// Deduces `TError` from the error value.
template <typename TError> Failure(TError) -> Failure<TError>;

/// A value of type `T` or an error of type `TError`.
///
/// The engine builds without exceptions, so fallible functions return a `Result` and the caller checks
/// `IsOk()` before calling `Value()` or `Error()`. Constructing from a [Failure] selects the error state; any
/// other value selects success. The type is `[[nodiscard]]`, so ignoring a returned result warns.
///
/// # Examples
///
/// ```cpp
/// #include "HAL/PlatformSocket.h"
///
/// Result<int32, SocketError> success = 3;
/// bool ok = success.IsOk(); // -> true
/// int32 value = success.Value(); // -> 3
///
/// Result<int32, SocketError> failure = Failure(SocketError::TimedOut);
/// SocketError error = failure.Error(); // -> SocketError::TimedOut
/// ```
template <typename T, typename TError> class [[nodiscard]] Result
{
public:
    /// Creates a successful result that holds a copy of `value`.
    constexpr Result(const T &value) : _value(value), _isOk(true)
    {
    }

    /// Creates a successful result that holds `value` moved in.
    constexpr Result(T &&value) : _value(Move(value)), _isOk(true)
    {
    }

    /// Creates a failed result from `failure`.
    ///
    /// `TError` must be constructible from `TOther &&`, which lets a function return an error type that wraps
    /// or converts the source error.
    template <typename TOther>
        requires(IsConstructible<TError, TOther &&>)
    constexpr Result(Failure<TOther> &&failure) : _error(Move(failure.Error)), _isOk(false)
    {
    }

    /// Creates a result that copies the value or error of `other`.
    constexpr Result(const Result &other) : _isOk(other._isOk)
    {
        if (_isOk)
        {
            ConstructAt(&_value, other._value);
        }
        else
        {
            ConstructAt(&_error, other._error);
        }
    }

    /// Creates a result that moves the value or error out of `other`; `other` keeps its state and a moved-from
    /// value.
    constexpr Result(Result &&other) noexcept : _isOk(other._isOk)
    {
        if (_isOk)
        {
            ConstructAt(&_value, Move(other._value));
        }
        else
        {
            ConstructAt(&_error, Move(other._error));
        }
    }

    /// Destroys the held value or error.
    constexpr ~Result()
    {
        Destroy();
    }

    /// Replaces the contents with a copy of `other`.
    constexpr Result &operator=(const Result &other)
    {
        if (this != &other)
        {
            Destroy();
            ConstructAt(this, other);
        }
        return *this;
    }

    /// Replaces the contents by moving from `other`; `other` keeps its state and a moved-from value.
    constexpr Result &operator=(Result &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            ConstructAt(this, Move(other));
        }
        return *this;
    }

    /// Returns whether the result holds a value.
    [[nodiscard]] constexpr bool IsOk() const
    {
        return _isOk;
    }

    /// Returns whether the result holds an error.
    [[nodiscard]] constexpr bool IsError() const
    {
        return !_isOk;
    }

    /// Returns whether the result holds a value; the same as `IsOk()`.
    constexpr explicit operator bool() const
    {
        return _isOk;
    }

    /// Returns the stored value as an lvalue.
    ///
    /// # Preconditions
    /// The result must hold a value. The check is `MARLIN_ASSERT`, so calling this on an error is undefined
    /// behavior in Profile and Shipping.
    [[nodiscard]] constexpr T &Value() &
    {
        MARLIN_ASSERT(_isOk);
        return _value;
    }

    /// Returns the stored value as a const lvalue.
    [[nodiscard]] constexpr const T &Value() const &
    {
        MARLIN_ASSERT(_isOk);
        return _value;
    }

    /// Returns the stored value as an rvalue reference; the result still holds it.
    [[nodiscard]] constexpr T &&Value() &&
    {
        MARLIN_ASSERT(_isOk);
        return Move(_value);
    }

    /// Returns the stored value, or `fallback` when the result holds an error.
    [[nodiscard]] constexpr T ValueOr(const T &fallback) const &
    {
        return _isOk ? _value : fallback;
    }

    /// Returns the stored error.
    ///
    /// # Preconditions
    /// The result must hold an error. The check is `MARLIN_ASSERT`, so calling this on a value is undefined
    /// behavior in Profile and Shipping.
    [[nodiscard]] constexpr const TError &Error() const
    {
        MARLIN_ASSERT(!_isOk);
        return _error;
    }

private:
    constexpr void Destroy()
    {
        if (_isOk)
        {
            DestroyAt(&_value);
        }
        else
        {
            DestroyAt(&_error);
        }
    }

    union {
        T _value;
        TError _error;
    };
    bool _isOk;
};

/// A successful `void` result or an error of type `TError`.
///
/// Default construction is success; constructing from a [Failure] selects the error state. Used by functions
/// that report failure but return no value.
///
/// # Examples
///
/// ```cpp
/// #include "HAL/PlatformSocket.h"
///
/// Result<void, SocketError> success;
/// bool ok = success.IsOk(); // -> true
///
/// Result<void, SocketError> failure = Failure(SocketError::NotConnected);
/// SocketError error = failure.Error(); // -> SocketError::NotConnected
/// ```
template <typename TError> class [[nodiscard]] Result<void, TError>
{
public:
    /// Creates a successful result.
    constexpr Result() : _isOk(true)
    {
    }

    /// Creates a failed result from `failure`.
    ///
    /// `TError` must be constructible from `TOther &&`.
    template <typename TOther>
        requires(IsConstructible<TError, TOther &&>)
    constexpr Result(Failure<TOther> &&failure) : _error(Move(failure.Error)), _isOk(false)
    {
    }

    /// Creates a result that copies the error of `other`, if any.
    constexpr Result(const Result &other) : _isOk(other._isOk)
    {
        if (!_isOk)
        {
            ConstructAt(&_error, other._error);
        }
    }

    /// Creates a result that moves the error out of `other`, if any.
    constexpr Result(Result &&other) noexcept : _isOk(other._isOk)
    {
        if (!_isOk)
        {
            ConstructAt(&_error, Move(other._error));
        }
    }

    /// Destroys the held error, if any.
    constexpr ~Result()
    {
        if (!_isOk)
        {
            DestroyAt(&_error);
        }
    }

    /// Replaces the contents with a copy of `other`.
    constexpr Result &operator=(const Result &other)
    {
        if (this != &other)
        {
            this->~Result();
            ConstructAt(this, other);
        }
        return *this;
    }

    /// Replaces the contents by moving from `other`.
    constexpr Result &operator=(Result &&other) noexcept
    {
        if (this != &other)
        {
            this->~Result();
            ConstructAt(this, Move(other));
        }
        return *this;
    }

    /// Returns whether the result is a success.
    [[nodiscard]] constexpr bool IsOk() const
    {
        return _isOk;
    }

    /// Returns whether the result holds an error.
    [[nodiscard]] constexpr bool IsError() const
    {
        return !_isOk;
    }

    /// Returns whether the result is a success; the same as `IsOk()`.
    constexpr explicit operator bool() const
    {
        return _isOk;
    }

    /// Returns the stored error.
    ///
    /// # Preconditions
    /// The result must hold an error. The check is `MARLIN_ASSERT`, so calling this on a success is undefined
    /// behavior in Profile and Shipping.
    [[nodiscard]] constexpr const TError &Error() const
    {
        MARLIN_ASSERT(!_isOk);
        return _error;
    }

private:
    union {
        TError _error;
    };
    bool _isOk;
};
} // namespace Marlin
