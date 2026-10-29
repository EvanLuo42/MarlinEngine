//! Platform-independent transcendental functions for lockstep simulation and scripts.
//!
//! Results are bit-identical on every platform, compiler and CPU. The implementation uses only IEEE 754 operations
//! whose results are exactly specified, with fused multiply-add contraction disabled.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// Transcendental functions with bit-identical results on every platform, compiler and CPU.
///
/// The C runtime library's functions may differ between implementations; these use only exactly specified IEEE 754
/// operations (`+`, `-`, `*`, `/`, `sqrt`, `fmod`, `trunc`) with fused multiply-add contraction disabled, so lockstep
/// simulation and scripts get the same bits everywhere. The `float64` results are within a few ulps of the correctly
/// rounded value, and each `float32` overload rounds the corresponding `float64` result.
///
/// # Thread safety
/// The functions are pure and keep no state, so they are safe to call from any thread.
struct CORE_API DeterministicMath
{
    /// Returns the sine of `x`, in radians.
    ///
    /// Returns NaN for an infinite or NaN input.
    [[nodiscard]] static float64 Sin(float64 x);
    /// Returns the cosine of `x`, in radians.
    ///
    /// Returns NaN for an infinite or NaN input.
    [[nodiscard]] static float64 Cos(float64 x);
    /// Returns the tangent of `x`, in radians.
    ///
    /// Returns NaN for an infinite or NaN input.
    [[nodiscard]] static float64 Tan(float64 x);
    /// Returns the arcsine of `x`, in radians in [-Pi/2, Pi/2].
    ///
    /// Returns NaN when `x` is outside [-1, 1] or is NaN.
    [[nodiscard]] static float64 Asin(float64 x);
    /// Returns the arccosine of `x`, in radians in [0, Pi].
    ///
    /// Returns NaN when `x` is outside [-1, 1] or is NaN.
    [[nodiscard]] static float64 Acos(float64 x);
    /// Returns the arctangent of `x`, in radians in [-Pi/2, Pi/2].
    ///
    /// Returns plus or minus Pi/2 for infinite input, and NaN for NaN input.
    [[nodiscard]] static float64 Atan(float64 x);
    /// Returns the angle in radians of the point `(x, y)`, in [-Pi, Pi].
    ///
    /// Uses the signs of both arguments to choose the quadrant. The zero, infinite and NaN cases follow the C
    /// library's `atan2`.
    [[nodiscard]] static float64 Atan2(float64 y, float64 x);
    /// Returns the hyperbolic sine of `x`.
    ///
    /// Returns plus or minus infinity for infinite input, and NaN for NaN input.
    [[nodiscard]] static float64 Sinh(float64 x);
    /// Returns the hyperbolic cosine of `x`.
    ///
    /// Returns infinity for infinite input, and NaN for NaN input.
    [[nodiscard]] static float64 Cosh(float64 x);
    /// Returns the hyperbolic tangent of `x`, in [-1, 1].
    ///
    /// Returns plus or minus 1 for infinite or large input, and NaN for NaN input.
    [[nodiscard]] static float64 Tanh(float64 x);
    /// Returns e raised to `x`.
    ///
    /// Returns infinity above the finite range, zero below it, and NaN for NaN input.
    [[nodiscard]] static float64 Exp(float64 x);
    /// Returns 2 raised to `x`.
    ///
    /// Returns infinity above the finite range, zero below it, and NaN for NaN input.
    [[nodiscard]] static float64 Exp2(float64 x);
    /// Returns the natural logarithm of `x`.
    ///
    /// Returns negative infinity for zero, NaN for negative or NaN input, and positive infinity for positive
    /// infinity.
    [[nodiscard]] static float64 Log(float64 x);
    /// Returns the base-2 logarithm of `x`.
    ///
    /// Returns negative infinity for zero, NaN for negative or NaN input, and positive infinity for positive
    /// infinity.
    [[nodiscard]] static float64 Log2(float64 x);
    /// Returns `x` raised to the power `y`.
    ///
    /// Follows the usual `pow` rules: it returns 1 when `y == 0` or `x == 1`, NaN when `x` is negative and `y` is a
    /// finite non-integer, keeps the sign when `x` is negative and `y` is an odd integer, and returns infinity when
    /// `x` is zero and `y` is negative.
    [[nodiscard]] static float64 Pow(float64 x, float64 y);

    /// Returns the sine of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Sin(float32 x)
    {
        return static_cast<float32>(Sin(static_cast<float64>(x)));
    }

    /// Returns the cosine of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Cos(float32 x)
    {
        return static_cast<float32>(Cos(static_cast<float64>(x)));
    }

    /// Returns the tangent of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Tan(float32 x)
    {
        return static_cast<float32>(Tan(static_cast<float64>(x)));
    }

    /// Returns the arcsine of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Asin(float32 x)
    {
        return static_cast<float32>(Asin(static_cast<float64>(x)));
    }

    /// Returns the arccosine of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Acos(float32 x)
    {
        return static_cast<float32>(Acos(static_cast<float64>(x)));
    }

    /// Returns the arctangent of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Atan(float32 x)
    {
        return static_cast<float32>(Atan(static_cast<float64>(x)));
    }

    /// Returns the angle of `(x, y)` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Atan2(float32 y, float32 x)
    {
        return static_cast<float32>(Atan2(static_cast<float64>(y), static_cast<float64>(x)));
    }

    /// Returns the hyperbolic sine of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Sinh(float32 x)
    {
        return static_cast<float32>(Sinh(static_cast<float64>(x)));
    }

    /// Returns the hyperbolic cosine of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Cosh(float32 x)
    {
        return static_cast<float32>(Cosh(static_cast<float64>(x)));
    }

    /// Returns the hyperbolic tangent of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Tanh(float32 x)
    {
        return static_cast<float32>(Tanh(static_cast<float64>(x)));
    }

    /// Returns e raised to `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Exp(float32 x)
    {
        return static_cast<float32>(Exp(static_cast<float64>(x)));
    }

    /// Returns 2 raised to `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Exp2(float32 x)
    {
        return static_cast<float32>(Exp2(static_cast<float64>(x)));
    }

    /// Returns the natural logarithm of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Log(float32 x)
    {
        return static_cast<float32>(Log(static_cast<float64>(x)));
    }

    /// Returns the base-2 logarithm of `x` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Log2(float32 x)
    {
        return static_cast<float32>(Log2(static_cast<float64>(x)));
    }

    /// Returns `x` raised to `y` as `float32`, rounded from the `float64` result.
    [[nodiscard]] static float32 Pow(float32 x, float32 y)
    {
        return static_cast<float32>(Pow(static_cast<float64>(x), static_cast<float64>(y)));
    }

    /// Returns `x * 2^n`; the result is exact when it is normal and correctly rounded when it underflows into the
    /// subnormal range.
    ///
    /// The result has gradual underflow: intermediate scaling keeps the low bits for subnormal results. Equivalent
    /// to the C library's `ldexp`.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const float64 scaled = DeterministicMath::Scale(1.5, 2); // -> 6.0
    /// ```
    [[nodiscard]] static float64 Scale(float64 x, int32 n);
};
} // namespace Marlin
