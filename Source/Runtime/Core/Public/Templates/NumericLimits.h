//! Minimum, maximum and floating-point special values for the engine's arithmetic types.

#pragma once

#include "CoreTypes.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// The limits of an arithmetic type.
///
/// Specialized for integral types, `float32` and `float64`; there is no definition for other types, so using it
/// with one is a compile error.
template <typename T> struct NumericLimits;

/// The limits of the integral type `T`.
template <Integral T> struct NumericLimits<T>
{
    /// The width of `T` in bits, minus one for signed types.
    static constexpr int32 Digits = static_cast<int32>(sizeof(T) * 8) - (IsSigned<T> ? 1 : 0);

    /// Returns the lowest representable value: zero for unsigned types, `-Max() - 1` for signed ones.
    [[nodiscard]] static constexpr T Min()
    {
        if constexpr (IsSigned<T>)
        {
            return static_cast<T>(-Max() - 1);
        }
        else
        {
            return 0;
        }
    }

    /// Returns the highest representable value.
    [[nodiscard]] static constexpr T Max()
    {
        if constexpr (IsSigned<T>)
        {
            return static_cast<T>(static_cast<MakeUnsigned<T>>(~MakeUnsigned<T>{0}) >> 1);
        }
        else
        {
            return static_cast<T>(~static_cast<T>(0));
        }
    }
};

/// The limits of `float32`.
template <> struct NumericLimits<float32>
{
    /// The number of mantissa bits that can be represented without change, including the implicit leading bit.
    static constexpr int32 Digits = __FLT_MANT_DIG__;

    /// Returns the lowest finite value, which is negative.
    ///
    /// For floating-point types this is `-Max()`, not the smallest positive value; use `SmallestNormal()` for
    /// that.
    [[nodiscard]] static constexpr float32 Min()
    {
        return -__FLT_MAX__;
    }

    /// Returns the highest finite value.
    [[nodiscard]] static constexpr float32 Max()
    {
        return __FLT_MAX__;
    }

    /// Returns the smallest positive normal value; subnormal values are smaller.
    [[nodiscard]] static constexpr float32 SmallestNormal()
    {
        return __FLT_MIN__;
    }

    /// Returns the difference between 1 and the next representable value above 1.
    [[nodiscard]] static constexpr float32 Epsilon()
    {
        return __FLT_EPSILON__;
    }

    /// Returns positive infinity.
    [[nodiscard]] static constexpr float32 Infinity()
    {
        return __builtin_huge_valf();
    }

    /// Returns a quiet NaN.
    [[nodiscard]] static constexpr float32 QuietNaN()
    {
        return __builtin_nanf("");
    }
};

/// The limits of `float64`.
template <> struct NumericLimits<float64>
{
    /// The number of mantissa bits that can be represented without change, including the implicit leading bit.
    static constexpr int32 Digits = __DBL_MANT_DIG__;

    /// Returns the lowest finite value, which is negative.
    ///
    /// For floating-point types this is `-Max()`, not the smallest positive value; use `SmallestNormal()` for
    /// that.
    [[nodiscard]] static constexpr float64 Min()
    {
        return -__DBL_MAX__;
    }

    /// Returns the highest finite value.
    [[nodiscard]] static constexpr float64 Max()
    {
        return __DBL_MAX__;
    }

    /// Returns the smallest positive normal value; subnormal values are smaller.
    [[nodiscard]] static constexpr float64 SmallestNormal()
    {
        return __DBL_MIN__;
    }

    /// Returns the difference between 1 and the next representable value above 1.
    [[nodiscard]] static constexpr float64 Epsilon()
    {
        return __DBL_EPSILON__;
    }

    /// Returns positive infinity.
    [[nodiscard]] static constexpr float64 Infinity()
    {
        return __builtin_huge_val();
    }

    /// Returns a quiet NaN.
    [[nodiscard]] static constexpr float64 QuietNaN()
    {
        return __builtin_nan("");
    }
};
} // namespace Marlin
