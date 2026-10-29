//! Scalar math constants and functions shared by the engine.
//!
//! The transcendental functions go through clang builtins, so they compile to target instructions where the target
//! has them and to the C runtime otherwise.

#pragma once

#include "CoreTypes.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// Scalar math constants and functions.
///
/// The transcendental functions go through clang builtins, so they compile to target instructions where the target
/// has them and to the C runtime otherwise. Every member is a constant or a function without state.
struct Math
{
    /// The ratio of a circle's circumference to its diameter.
    static constexpr float32 Pi = 3.14159265358979323846f;
    /// Two times `Pi`, one full turn in radians.
    static constexpr float32 TwoPi = 2 * Pi;
    /// Half of `Pi`, a quarter turn in radians.
    static constexpr float32 HalfPi = Pi / 2;
    /// The factor that converts an angle in degrees to radians.
    static constexpr float32 DegreesToRadians = Pi / 180;
    /// The factor that converts an angle in radians to degrees.
    static constexpr float32 RadiansToDegrees = 180 / Pi;
    /// The default tolerance of `IsNearlyEqual` and `IsNearlyZero`.
    static constexpr float32 SmallNumber = 1e-6f;

    /// Returns the absolute value of `value`.
    template <Arithmetic T> [[nodiscard]] static constexpr T Abs(T value)
    {
        return value < T{0} ? -value : value;
    }

    /// Returns `-1`, `0` or `1` according to the sign of `value`.
    ///
    /// A NaN returns `0` because both comparisons are false.
    template <Arithmetic T> [[nodiscard]] static constexpr T Sign(T value)
    {
        return value > T{0} ? T{1} : (value < T{0} ? T{-1} : T{0});
    }

    /// Returns `value` multiplied by itself.
    template <Arithmetic T> [[nodiscard]] static constexpr T Square(T value)
    {
        return value * value;
    }

    /// Returns `value` limited to the range from `low` to `high`.
    ///
    /// # Preconditions
    /// `low` is less than or equal to `high`. The bounds are not checked.
    template <Arithmetic T> [[nodiscard]] static constexpr T Clamp(T value, T low, T high)
    {
        return value < low ? low : (high < value ? high : value);
    }

    /// Returns `value` limited to the range [0, 1].
    template <FloatingPoint T> [[nodiscard]] static constexpr T Saturate(T value)
    {
        return Clamp(value, T{0}, T{1});
    }

    /// Returns the linear interpolation from `a` to `b` by `t`.
    ///
    /// The result is `a + (b - a) * t`; `t` is not clamped, so values outside [0, 1] extrapolate.
    template <FloatingPoint T> [[nodiscard]] static constexpr T Lerp(T a, T b, T t)
    {
        return a + (b - a) * t;
    }

    /// Returns where `value` lies between `a` and `b`, unclamped.
    ///
    /// The result is `(value - a) / (b - a)`, so it is 0 at `a`, 1 at `b`, and outside [0, 1] for values outside
    /// the range. Equal `a` and `b` divide by zero and give an infinite or NaN result.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const float32 fraction = Math::InverseLerp(10.0f, 20.0f, 15.0f); // -> 0.5
    /// ```
    template <FloatingPoint T> [[nodiscard]] static constexpr T InverseLerp(T a, T b, T value)
    {
        return (value - a) / (b - a);
    }

    /// Returns smooth Hermite interpolation from 0 to 1 as `value` moves from `edge0` to `edge1`.
    ///
    /// The result is clamped to [0, 1] and has zero slope at both edges. When `edge0` equals `edge1`, the quotient
    /// is infinite or NaN: `value` equal to the edge gives NaN, and other values give 0 or 1.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const float32 value = Math::SmoothStep(0.0f, 1.0f, 0.5f); // -> 0.5
    /// ```
    template <FloatingPoint T> [[nodiscard]] static constexpr T SmoothStep(T edge0, T edge1, T value)
    {
        const T t = Saturate((value - edge0) / (edge1 - edge0));
        return t * t * (T{3} - T{2} * t);
    }

    /// Returns whether `a` and `b` differ by at most `tolerance`.
    ///
    /// The default tolerance is `SmallNumber`.
    template <FloatingPoint T>
    [[nodiscard]] static constexpr bool IsNearlyEqual(T a, T b, T tolerance = static_cast<T>(SmallNumber))
    {
        return Abs(a - b) <= tolerance;
    }

    /// Returns whether the absolute value of `value` is at most `tolerance`.
    ///
    /// The default tolerance is `SmallNumber`.
    template <FloatingPoint T> [[nodiscard]] static constexpr bool IsNearlyZero(T value, T tolerance = SmallNumber)
    {
        return Abs(value) <= tolerance;
    }

    /// Returns whether `value` is neither infinite nor NaN.
    template <FloatingPoint T> [[nodiscard]] static constexpr bool IsFinite(T value)
    {
        return __builtin_isfinite(value);
    }

    /// Returns whether `value` is NaN.
    template <FloatingPoint T> [[nodiscard]] static constexpr bool IsNaN(T value)
    {
        return __builtin_isnan(value);
    }

    /// Returns the square root of `value`.
    [[nodiscard]] static float32 Sqrt(float32 value)
    {
        return __builtin_sqrtf(value);
    }

    /// Returns the square root of `value` as a `float64`.
    [[nodiscard]] static float64 Sqrt(float64 value)
    {
        return __builtin_sqrt(value);
    }

    /// Returns the reciprocal of the square root of `value`.
    [[nodiscard]] static float32 InverseSqrt(float32 value)
    {
        return 1.0f / __builtin_sqrtf(value);
    }

    /// Returns `value` rounded down to the nearest integer.
    [[nodiscard]] static float32 Floor(float32 value)
    {
        return __builtin_floorf(value);
    }

    /// Returns `value` rounded down to the nearest integer, as a `float64`.
    [[nodiscard]] static float64 Floor(float64 value)
    {
        return __builtin_floor(value);
    }

    /// Returns `value` rounded up to the nearest integer.
    [[nodiscard]] static float32 Ceil(float32 value)
    {
        return __builtin_ceilf(value);
    }

    /// Returns `value` rounded up to the nearest integer, as a `float64`.
    [[nodiscard]] static float64 Ceil(float64 value)
    {
        return __builtin_ceil(value);
    }

    /// Returns `value` rounded to the nearest integer, with halfway cases rounded to even.
    [[nodiscard]] static float32 Round(float32 value)
    {
        return __builtin_roundevenf(value);
    }

    /// Returns `value` rounded to the nearest integer, with halfway cases rounded to even, as a `float64`.
    [[nodiscard]] static float64 Round(float64 value)
    {
        return __builtin_roundeven(value);
    }

    /// Returns `value` rounded toward zero.
    [[nodiscard]] static float32 Truncate(float32 value)
    {
        return __builtin_truncf(value);
    }

    /// Returns the fractional part of `value`, in [0, 1) for finite values.
    ///
    /// The result is `value - Floor(value)`, so negative inputs get a positive fraction.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const float32 value = Math::Fractional(-1.25f); // -> 0.75
    /// ```
    [[nodiscard]] static float32 Fractional(float32 value)
    {
        return value - __builtin_floorf(value);
    }

    /// Returns the remainder of `value` divided by `divisor`, with the sign of `value`.
    ///
    /// A zero `divisor` gives NaN.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const float32 value = Math::Mod(-5.5f, 2.0f); // -> -1.5
    /// ```
    [[nodiscard]] static float32 Mod(float32 value, float32 divisor)
    {
        return __builtin_fmodf(value, divisor);
    }

    /// Returns the sine of `radians`.
    [[nodiscard]] static float32 Sin(float32 radians)
    {
        return __builtin_sinf(radians);
    }

    /// Returns the cosine of `radians`.
    [[nodiscard]] static float32 Cos(float32 radians)
    {
        return __builtin_cosf(radians);
    }

    /// Computes the sine and cosine of `radians` and writes them through `sine` and `cosine`.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// float32 sine = 0;
    /// float32 cosine = 0;
    /// Math::SinCos(0.0f, sine, cosine); // -> sine 0, cosine 1
    /// ```
    static void SinCos(float32 radians, float32 &sine, float32 &cosine)
    {
        sine = __builtin_sinf(radians);
        cosine = __builtin_cosf(radians);
    }

    /// Returns the tangent of `radians`.
    [[nodiscard]] static float32 Tan(float32 radians)
    {
        return __builtin_tanf(radians);
    }

    /// Returns the arcsine of `value`, in radians.
    ///
    /// `value` is clamped to [-1, 1] before the call.
    [[nodiscard]] static float32 Asin(float32 value)
    {
        return __builtin_asinf(Clamp(value, -1.0f, 1.0f));
    }

    /// Returns the arccosine of `value`, in radians.
    ///
    /// `value` is clamped to [-1, 1] before the call.
    [[nodiscard]] static float32 Acos(float32 value)
    {
        return __builtin_acosf(Clamp(value, -1.0f, 1.0f));
    }

    /// Returns the arctangent of `value`, in radians.
    [[nodiscard]] static float32 Atan(float32 value)
    {
        return __builtin_atanf(value);
    }

    /// Returns the arctangent of `y / x`, in radians.
    ///
    /// The signs of both arguments place the result in [-Pi, Pi]. The first parameter is `y`, not `x`.
    [[nodiscard]] static float32 Atan2(float32 y, float32 x)
    {
        return __builtin_atan2f(y, x);
    }

    /// Returns e raised to `value`.
    [[nodiscard]] static float32 Exp(float32 value)
    {
        return __builtin_expf(value);
    }

    /// Returns 2 raised to `value`.
    [[nodiscard]] static float32 Exp2(float32 value)
    {
        return __builtin_exp2f(value);
    }

    /// Returns the natural logarithm of `value`.
    ///
    /// `value` must be positive; zero and negative values produce a non-finite result.
    [[nodiscard]] static float32 Log(float32 value)
    {
        return __builtin_logf(value);
    }

    /// Returns the base-2 logarithm of `value`.
    ///
    /// `value` must be positive; zero and negative values produce a non-finite result.
    [[nodiscard]] static float32 Log2(float32 value)
    {
        return __builtin_log2f(value);
    }

    /// Returns `base` raised to `exponent`.
    [[nodiscard]] static float32 Pow(float32 base, float32 exponent)
    {
        return __builtin_powf(base, exponent);
    }

    /// Returns `radians` wrapped to the range (-Pi, Pi].
    ///
    /// For finite `radians`, the result is congruent to the input modulo `TwoPi`; an angle that is already in range
    /// is returned unchanged.
    [[nodiscard]] static float32 WrapAngle(float32 radians)
    {
        radians = __builtin_remainderf(radians, TwoPi);
        return radians <= -Pi ? radians + TwoPi : radians;
    }
};
} // namespace Marlin
