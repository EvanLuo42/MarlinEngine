//! A 16-byte SIMD vector for batched float math.
//!
//! `Vector4Simd` carries four `float32` lanes between the storage types `Vector3` and `Vector4`. Load storage
//! vectors, run the batch operations, then store the result back.

#pragma once

#include "CoreTypes.h"
#include "Math/Simd.h"
#include "Math/Vector.h"

namespace Marlin
{
/// The SIMD compute type: a 16-byte vector of four `float32` lanes.
///
/// Load storage vectors into it, run batches of math, then store the result back. Lanes that a three-component
/// operation ignores are noted per function. The size and alignment match a hardware four-lane float vector.
struct alignas(16) Vector4Simd
{
    /// The four lanes, as a `Simd::Float4`.
    Simd::Float4 Value;

    /// Creates a zero vector.
    Vector4Simd() : Value(0)
    {
    }

    /// Creates a vector that holds `value`.
    Vector4Simd(Simd::Float4 value) : Value(value)
    {
    }

    /// Creates a vector from four lane values.
    Vector4Simd(float32 x, float32 y, float32 z, float32 w) : Value{x, y, z, w}
    {
    }

    /// Creates a vector with every lane set to `value`.
    explicit Vector4Simd(float32 value) : Value(value)
    {
    }

    /// Creates a vector from `vector` and sets the fourth lane to `w`.
    explicit Vector4Simd(Vector3 vector, float32 w = 0) : Value{vector.X, vector.Y, vector.Z, w}
    {
    }

    /// Creates a vector from the components of `vector`.
    explicit Vector4Simd(Vector4 vector) : Value(Simd::Load(&vector.X))
    {
    }

    /// Returns the `x`, `y` and `z` lanes as a `Vector3`.
    [[nodiscard]] Vector3 ToVector3() const
    {
        return {Value.x, Value.y, Value.z};
    }

    /// Returns all four lanes as a `Vector4`.
    [[nodiscard]] Vector4 ToVector4() const
    {
        Vector4 result;
        Simd::Store(&result.X, Value);
        return result;
    }

    /// Returns the `x` lane.
    [[nodiscard]] float32 X() const
    {
        return Value.x;
    }

    /// Returns the `y` lane.
    [[nodiscard]] float32 Y() const
    {
        return Value.y;
    }

    /// Returns the `z` lane.
    [[nodiscard]] float32 Z() const
    {
        return Value.z;
    }

    /// Returns the `w` lane.
    [[nodiscard]] float32 W() const
    {
        return Value.w;
    }

    /// Returns the vector with every lane negated.
    Vector4Simd operator-() const
    {
        return -Value;
    }

    /// Returns the componentwise sum of the two vectors.
    Vector4Simd operator+(Vector4Simd other) const
    {
        return Value + other.Value;
    }

    /// Returns the componentwise difference of the two vectors.
    Vector4Simd operator-(Vector4Simd other) const
    {
        return Value - other.Value;
    }

    /// Returns the componentwise product of the two vectors.
    Vector4Simd operator*(Vector4Simd other) const
    {
        return Value * other.Value;
    }

    /// Returns the componentwise quotient of the two vectors.
    Vector4Simd operator/(Vector4Simd other) const
    {
        return Value / other.Value;
    }

    /// Returns every lane multiplied by `scale`.
    Vector4Simd operator*(float32 scale) const
    {
        return Value * scale;
    }

    /// Adds `other` to this vector and returns it.
    Vector4Simd &operator+=(Vector4Simd other)
    {
        Value += other.Value;
        return *this;
    }

    /// Subtracts `other` from this vector and returns it.
    Vector4Simd &operator-=(Vector4Simd other)
    {
        Value -= other.Value;
        return *this;
    }

    /// Multiplies this vector by `other` componentwise and returns it.
    Vector4Simd &operator*=(Vector4Simd other)
    {
        Value *= other.Value;
        return *this;
    }

    /// Returns `a * b + c` in every lane with a single rounding.
    [[nodiscard]] static Vector4Simd MultiplyAdd(Vector4Simd a, Vector4Simd b, Vector4Simd c)
    {
        return Simd::MultiplyAdd(a.Value, b.Value, c.Value);
    }

    /// Returns the dot product of the `x`, `y` and `z` lanes in every lane.
    ///
    /// The `w` lanes are ignored.
    [[nodiscard]] static Vector4Simd Dot3(Vector4Simd a, Vector4Simd b)
    {
        return Simd::Dot3(a.Value, b.Value);
    }

    /// Returns the four-lane dot product in every lane.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const Vector4Simd a(1, 2, 3, 4);
    /// const Vector4Simd b(5, 6, 7, 8);
    /// const float32 dot = Vector4Simd::Dot4(a, b).X(); // -> 70
    /// ```
    [[nodiscard]] static Vector4Simd Dot4(Vector4Simd a, Vector4Simd b)
    {
        return Simd::Dot4(a.Value, b.Value);
    }

    /// Returns the cross product of the `x`, `y` and `z` lanes.
    ///
    /// The `w` lane of the result is zero.
    [[nodiscard]] static Vector4Simd Cross3(Vector4Simd a, Vector4Simd b)
    {
        return Simd::Cross3(a.Value, b.Value);
    }

    /// Returns the componentwise minimum of `a` and `b`.
    ///
    /// A lane where either input is NaN takes the value from `b`.
    [[nodiscard]] static Vector4Simd Min(Vector4Simd a, Vector4Simd b)
    {
        return Simd::Min(a.Value, b.Value);
    }

    /// Returns the componentwise maximum of `a` and `b`.
    ///
    /// A lane where either input is NaN takes the value from `b`.
    [[nodiscard]] static Vector4Simd Max(Vector4Simd a, Vector4Simd b)
    {
        return Simd::Max(a.Value, b.Value);
    }

    /// Returns the absolute value of every lane.
    [[nodiscard]] static Vector4Simd Abs(Vector4Simd value)
    {
        return Simd::Abs(value.Value);
    }

    /// Returns the square root of every lane.
    [[nodiscard]] static Vector4Simd Sqrt(Vector4Simd value)
    {
        return Simd::Sqrt(value.Value);
    }

    /// Returns the linear interpolation from `a` to `b` by `t` in every lane.
    ///
    /// `t` is a scalar and is not clamped; the multiply and add use one rounding.
    [[nodiscard]] static Vector4Simd Lerp(Vector4Simd a, Vector4Simd b, float32 t)
    {
        return Simd::MultiplyAdd(b.Value - a.Value, Simd::Splat(t), a.Value);
    }

    /// Returns the vector with `x`, `y` and `z` scaled to unit length; `w` is scaled by the same factor.
    ///
    /// The result uses the refined reciprocal square root and has about 23 bits of precision. When the length
    /// of `x`, `y` and `z` is at most `Math::SmallNumber`, the result is all zeros.
    [[nodiscard]] Vector4Simd Normalized3() const
    {
        const Simd::Float4 lengthSquared = Simd::Dot3(Value, Value);
        const Simd::Mask4 valid = lengthSquared > Simd::Splat(Math::SmallNumber * Math::SmallNumber);
        return Simd::Select(valid, Value * Simd::ReciprocalSqrt(lengthSquared), Simd::Splat(0));
    }

    /// Returns the length of the `x`, `y` and `z` lanes.
    [[nodiscard]] float32 Length3() const
    {
        return Math::Sqrt(Simd::Dot3(Value, Value).x);
    }

    /// Returns a vector whose lanes are selected from this vector by `TX`, `TY`, `TZ` and `TW`.
    ///
    /// Indices 0 to 3 select `x`, `y`, `z` and `w`; lanes may repeat.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const Vector4Simd value(1, 2, 3, 4);
    /// const Vector4Simd reversed = value.Swizzle<3, 2, 1, 0>(); // -> (4, 3, 2, 1)
    /// ```
    template <int32 TX, int32 TY, int32 TZ, int32 TW> [[nodiscard]] Vector4Simd Swizzle() const
    {
        return Simd::Swizzle<TX, TY, TZ, TW>(Value);
    }

    /// Returns whether every lane is within `tolerance` of `other`.
    ///
    /// The default tolerance is `Math::SmallNumber`.
    [[nodiscard]] bool IsNearlyEqual(Vector4Simd other, float32 tolerance = Math::SmallNumber) const
    {
        return Simd::AllTrue(Simd::Abs(Value - other.Value) <= Simd::Splat(tolerance));
    }
};

static_assert(sizeof(Vector4Simd) == 16);
} // namespace Marlin
