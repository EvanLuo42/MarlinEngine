//! Fixed-size vectors of float and integer coordinates.
//!
//! `Vector2`, `Vector3` and `Vector4` hold `float32` components; `IntVector2` and `IntVector3` hold `int32`
//! coordinates. The float vectors use the engine's right-handed, Y-up space, where a camera looks along -Z.
//!
//! All five types format as `(x, y, ...)` through `FormatTo`.

#pragma once

#include "CoreTypes.h"
#include "Hash/Hash.h"
#include "Math/MathUtility.h"
#include "String/StringView.h"
#include "Math/Vector.gen.h"

namespace Marlin
{
class StringWriter;

/// Appends `count` components to `output` as `(a, b, ...)`, applying the format `spec` to each value.
///
/// The vector types use this from their `FormatTo` members. `spec` is the text after the colon in a format item,
/// such as `F1`; an empty `spec` uses the default format.
///
/// # Preconditions
/// `components` points to at least `count` values. The count is not checked.
CORE_API void FormatComponents(StringWriter &output, const float32 *components, uint32 count, StringView spec);
/// Appends `count` `float64` components to `output` in the same `(a, b, ...)` form.
CORE_API void FormatComponents(StringWriter &output, const float64 *components, uint32 count, StringView spec);
/// Appends `count` `int32` components to `output` in the same `(a, b, ...)` form.
CORE_API void FormatComponents(StringWriter &output, const int32 *components, uint32 count, StringView spec);

/// A vector of two `float32` components.
///
/// Serializers write the components as an ordered tuple. Arithmetic operators apply componentwise, and
/// `FormatTo` writes `(x, y)`.
MSTRUCT(Tuple)
struct Vector2
{
    MGENERATED_BODY()

    /// The X component.
    MPROPERTY()
    float32 X = 0;
    /// The Y component.
    MPROPERTY()
    float32 Y = 0;

    /// Creates a zero vector.
    constexpr Vector2() = default;

    /// Creates a vector from `x` and `y`.
    constexpr Vector2(float32 x, float32 y) : X(x), Y(y)
    {
    }

    /// Creates a vector with both components set to `value`.
    constexpr explicit Vector2(float32 value) : X(value), Y(value)
    {
    }

    /// The zero vector, `(0, 0)`.
    static const Vector2 Zero;
    /// The one vector, `(1, 1)`.
    static const Vector2 One;

    /// Returns the vector with both components negated.
    constexpr Vector2 operator-() const
    {
        return {-X, -Y};
    }

    /// Returns the componentwise sum of the two vectors.
    constexpr Vector2 operator+(Vector2 other) const
    {
        return {X + other.X, Y + other.Y};
    }

    /// Returns the componentwise difference of the two vectors.
    constexpr Vector2 operator-(Vector2 other) const
    {
        return {X - other.X, Y - other.Y};
    }

    /// Returns the componentwise product of the two vectors.
    constexpr Vector2 operator*(Vector2 other) const
    {
        return {X * other.X, Y * other.Y};
    }

    /// Returns the componentwise quotient of the two vectors.
    constexpr Vector2 operator/(Vector2 other) const
    {
        return {X / other.X, Y / other.Y};
    }

    /// Returns both components multiplied by `scale`.
    constexpr Vector2 operator*(float32 scale) const
    {
        return {X * scale, Y * scale};
    }

    /// Returns both components divided by `divisor`.
    constexpr Vector2 operator/(float32 divisor) const
    {
        return *this * (1 / divisor);
    }

    /// Returns `vector` scaled by `scale`.
    friend constexpr Vector2 operator*(float32 scale, Vector2 vector)
    {
        return vector * scale;
    }

    /// Adds `other` to this vector and returns it.
    constexpr Vector2 &operator+=(Vector2 other)
    {
        return *this = *this + other;
    }

    /// Subtracts `other` from this vector and returns it.
    constexpr Vector2 &operator-=(Vector2 other)
    {
        return *this = *this - other;
    }

    /// Multiplies both components by `scale` and returns this vector.
    constexpr Vector2 &operator*=(float32 scale)
    {
        return *this = *this * scale;
    }

    /// Compares both components exactly.
    constexpr bool operator==(const Vector2 &) const = default;

    /// Returns the dot product of `a` and `b`.
    [[nodiscard]] static constexpr float32 Dot(Vector2 a, Vector2 b)
    {
        return a.X * b.X + a.Y * b.Y;
    }

    /// Returns the z component of the 3D cross product of `a` and `b`.
    ///
    /// The result is positive when `b` is counterclockwise from `a`.
    [[nodiscard]] static constexpr float32 Cross(Vector2 a, Vector2 b)
    {
        return a.X * b.Y - a.Y * b.X;
    }

    /// Returns the squared length of the vector.
    [[nodiscard]] constexpr float32 LengthSquared() const
    {
        return Dot(*this, *this);
    }

    /// Returns the length of the vector.
    [[nodiscard]] float32 Length() const
    {
        return Math::Sqrt(LengthSquared());
    }

    /// Returns the unit vector in the same direction, or zero when the vector is too short to normalize.
    ///
    /// A vector whose length is at most `Math::SmallNumber` returns zero.
    [[nodiscard]] Vector2 Normalized() const
    {
        const float32 lengthSquared = LengthSquared();
        return lengthSquared > Math::SmallNumber * Math::SmallNumber ? *this * Math::InverseSqrt(lengthSquared)
                                                                     : Vector2();
    }

    /// Returns the componentwise minimum of `a` and `b`.
    [[nodiscard]] static constexpr Vector2 Min(Vector2 a, Vector2 b)
    {
        return {a.X < b.X ? a.X : b.X, a.Y < b.Y ? a.Y : b.Y};
    }

    /// Returns the componentwise maximum of `a` and `b`.
    [[nodiscard]] static constexpr Vector2 Max(Vector2 a, Vector2 b)
    {
        return {a.X > b.X ? a.X : b.X, a.Y > b.Y ? a.Y : b.Y};
    }

    /// Returns the linear interpolation from `a` to `b` by `t`.
    ///
    /// `t` is not clamped, so values outside [0, 1] extrapolate.
    [[nodiscard]] static constexpr Vector2 Lerp(Vector2 a, Vector2 b, float32 t)
    {
        return a + (b - a) * t;
    }

    /// Returns whether both components are within `tolerance` of `other`.
    ///
    /// The default tolerance is `Math::SmallNumber`.
    [[nodiscard]] constexpr bool IsNearlyEqual(Vector2 other, float32 tolerance = Math::SmallNumber) const
    {
        return Math::IsNearlyEqual(X, other.X, tolerance) && Math::IsNearlyEqual(Y, other.Y, tolerance);
    }

    /// Writes `(x, y)` to `output`, applying `spec` to each component.
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &X, 2, spec);
    }
};

inline constexpr Vector2 Vector2::Zero{0, 0};
inline constexpr Vector2 Vector2::One{1, 1};

/// A vector of three `float32` components.
///
/// The engine space is right-handed and Y is up, with a camera looking along -Z. Serializers write the components
/// as an ordered tuple, and `FormatTo` writes `(x, y, z)`.
MSTRUCT(Tuple)
struct Vector3
{
    MGENERATED_BODY()

    /// The X component.
    MPROPERTY()
    float32 X = 0;
    /// The Y component.
    MPROPERTY()
    float32 Y = 0;
    /// The Z component.
    MPROPERTY()
    float32 Z = 0;

    /// Creates a zero vector.
    constexpr Vector3() = default;

    /// Creates a vector from `x`, `y` and `z`.
    constexpr Vector3(float32 x, float32 y, float32 z) : X(x), Y(y), Z(z)
    {
    }

    /// Creates a vector with all three components set to `value`.
    constexpr explicit Vector3(float32 value) : X(value), Y(value), Z(value)
    {
    }

    /// Creates a vector from `xy` and `z`.
    constexpr Vector3(Vector2 xy, float32 z) : X(xy.X), Y(xy.Y), Z(z)
    {
    }

    /// The zero vector, `(0, 0, 0)`.
    static const Vector3 Zero;
    /// The one vector, `(1, 1, 1)`.
    static const Vector3 One;
    /// The +X axis, `(1, 0, 0)`.
    static const Vector3 Right;
    /// The +Y axis, `(0, 1, 0)`.
    static const Vector3 Up;
    /// The +Z axis, `(0, 0, 1)`; it points toward the viewer.
    static const Vector3 Backward;
    /// The -Z axis, `(0, 0, -1)`; it is the direction a camera looks along.
    static const Vector3 Forward;

    /// Returns the `x` and `y` components.
    [[nodiscard]] constexpr Vector2 XY() const
    {
        return {X, Y};
    }

    /// Returns the component at `index`.
    ///
    /// 0, 1 and 2 select `X`, `Y` and `Z`; a larger index returns `Z`. The index is not checked.
    constexpr float32 operator[](uint32 index) const
    {
        return index == 0 ? X : (index == 1 ? Y : Z);
    }

    /// Returns the vector with all three components negated.
    constexpr Vector3 operator-() const
    {
        return {-X, -Y, -Z};
    }

    /// Returns the componentwise sum of the two vectors.
    constexpr Vector3 operator+(Vector3 other) const
    {
        return {X + other.X, Y + other.Y, Z + other.Z};
    }

    /// Returns the componentwise difference of the two vectors.
    constexpr Vector3 operator-(Vector3 other) const
    {
        return {X - other.X, Y - other.Y, Z - other.Z};
    }

    /// Returns the componentwise product of the two vectors.
    constexpr Vector3 operator*(Vector3 other) const
    {
        return {X * other.X, Y * other.Y, Z * other.Z};
    }

    /// Returns the componentwise quotient of the two vectors.
    constexpr Vector3 operator/(Vector3 other) const
    {
        return {X / other.X, Y / other.Y, Z / other.Z};
    }

    /// Returns all three components multiplied by `scale`.
    constexpr Vector3 operator*(float32 scale) const
    {
        return {X * scale, Y * scale, Z * scale};
    }

    /// Returns all three components divided by `divisor`.
    constexpr Vector3 operator/(float32 divisor) const
    {
        return *this * (1 / divisor);
    }

    /// Returns `vector` scaled by `scale`.
    friend constexpr Vector3 operator*(float32 scale, Vector3 vector)
    {
        return vector * scale;
    }

    /// Adds `other` to this vector and returns it.
    constexpr Vector3 &operator+=(Vector3 other)
    {
        return *this = *this + other;
    }

    /// Subtracts `other` from this vector and returns it.
    constexpr Vector3 &operator-=(Vector3 other)
    {
        return *this = *this - other;
    }

    /// Multiplies all three components by `scale` and returns this vector.
    constexpr Vector3 &operator*=(float32 scale)
    {
        return *this = *this * scale;
    }

    /// Multiplies this vector by `other` componentwise and returns it.
    constexpr Vector3 &operator*=(Vector3 other)
    {
        return *this = *this * other;
    }

    /// Compares all three components exactly.
    constexpr bool operator==(const Vector3 &) const = default;

    /// Returns the dot product of `a` and `b`.
    [[nodiscard]] static constexpr float32 Dot(Vector3 a, Vector3 b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    /// Returns the cross product of `a` and `b`.
    [[nodiscard]] static constexpr Vector3 Cross(Vector3 a, Vector3 b)
    {
        return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
    }

    /// Returns the squared length of the vector.
    [[nodiscard]] constexpr float32 LengthSquared() const
    {
        return Dot(*this, *this);
    }

    /// Returns the length of the vector.
    [[nodiscard]] float32 Length() const
    {
        return Math::Sqrt(LengthSquared());
    }

    /// Returns the distance between `a` and `b`.
    [[nodiscard]] static float32 Distance(Vector3 a, Vector3 b)
    {
        return (a - b).Length();
    }

    /// Returns the unit vector in the same direction, or zero when the vector is too short to normalize.
    ///
    /// A vector whose length is at most `Math::SmallNumber` returns zero.
    [[nodiscard]] Vector3 Normalized() const
    {
        const float32 lengthSquared = LengthSquared();
        return lengthSquared > Math::SmallNumber * Math::SmallNumber ? *this * Math::InverseSqrt(lengthSquared)
                                                                     : Vector3();
    }

    /// Returns whether every component is within `tolerance` of zero.
    ///
    /// The default tolerance is `Math::SmallNumber`.
    [[nodiscard]] constexpr bool IsNearlyZero(float32 tolerance = Math::SmallNumber) const
    {
        return Math::IsNearlyZero(X, tolerance) && Math::IsNearlyZero(Y, tolerance) && Math::IsNearlyZero(Z, tolerance);
    }

    /// Returns whether every component is within `tolerance` of `other`.
    ///
    /// The default tolerance is `Math::SmallNumber`.
    [[nodiscard]] constexpr bool IsNearlyEqual(Vector3 other, float32 tolerance = Math::SmallNumber) const
    {
        return (*this - other).IsNearlyZero(tolerance);
    }

    /// Returns the componentwise minimum of `a` and `b`.
    [[nodiscard]] static constexpr Vector3 Min(Vector3 a, Vector3 b)
    {
        return {a.X < b.X ? a.X : b.X, a.Y < b.Y ? a.Y : b.Y, a.Z < b.Z ? a.Z : b.Z};
    }

    /// Returns the componentwise maximum of `a` and `b`.
    [[nodiscard]] static constexpr Vector3 Max(Vector3 a, Vector3 b)
    {
        return {a.X > b.X ? a.X : b.X, a.Y > b.Y ? a.Y : b.Y, a.Z > b.Z ? a.Z : b.Z};
    }

    /// Returns the componentwise absolute value of `value`.
    [[nodiscard]] static constexpr Vector3 Abs(Vector3 value)
    {
        return {Math::Abs(value.X), Math::Abs(value.Y), Math::Abs(value.Z)};
    }

    /// Returns the linear interpolation from `a` to `b` by `t`.
    ///
    /// `t` is not clamped, so values outside [0, 1] extrapolate.
    [[nodiscard]] static constexpr Vector3 Lerp(Vector3 a, Vector3 b, float32 t)
    {
        return a + (b - a) * t;
    }

    /// Returns `vector` mirrored about the plane through the origin that has unit `normal`.
    [[nodiscard]] static constexpr Vector3 Reflect(Vector3 vector, Vector3 normal)
    {
        return vector - normal * (2 * Dot(vector, normal));
    }

    /// Returns the part of `vector` along the unit `direction`.
    [[nodiscard]] static constexpr Vector3 Project(Vector3 vector, Vector3 direction)
    {
        return direction * Dot(vector, direction);
    }

    /// Returns the largest of `X`, `Y` and `Z`.
    [[nodiscard]] constexpr float32 MaxComponent() const
    {
        const float32 xy = X > Y ? X : Y;
        return xy > Z ? xy : Z;
    }

    /// Returns the smallest of `X`, `Y` and `Z`.
    [[nodiscard]] constexpr float32 MinComponent() const
    {
        const float32 xy = X < Y ? X : Y;
        return xy < Z ? xy : Z;
    }

    /// Writes `(x, y, z)` to `output`, applying `spec` to each component.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const String text = Format("{0:F1}", Vector3(1, 2.5f, -3)); // -> "(1.0, 2.5, -3.0)"
    /// ```
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &X, 3, spec);
    }
};

inline constexpr Vector3 Vector3::Zero{0, 0, 0};
inline constexpr Vector3 Vector3::One{1, 1, 1};
inline constexpr Vector3 Vector3::Right{1, 0, 0};
inline constexpr Vector3 Vector3::Up{0, 1, 0};
inline constexpr Vector3 Vector3::Backward{0, 0, 1};
inline constexpr Vector3 Vector3::Forward{0, 0, -1};

/// A vector of four `float32` components.
///
/// The fourth component is `W`. Serializers write the components as an ordered tuple, and `FormatTo` writes
/// `(x, y, z, w)`.
MSTRUCT(Tuple)
struct Vector4
{
    MGENERATED_BODY()

    /// The X component.
    MPROPERTY()
    float32 X = 0;
    /// The Y component.
    MPROPERTY()
    float32 Y = 0;
    /// The Z component.
    MPROPERTY()
    float32 Z = 0;
    /// The W component.
    MPROPERTY()
    float32 W = 0;

    /// Creates a zero vector.
    constexpr Vector4() = default;

    /// Creates a vector from `x`, `y`, `z` and `w`.
    constexpr Vector4(float32 x, float32 y, float32 z, float32 w) : X(x), Y(y), Z(z), W(w)
    {
    }

    /// Creates a vector with all four components set to `value`.
    constexpr explicit Vector4(float32 value) : X(value), Y(value), Z(value), W(value)
    {
    }

    /// Creates a vector from `xyz` and `w`.
    constexpr Vector4(Vector3 xyz, float32 w) : X(xyz.X), Y(xyz.Y), Z(xyz.Z), W(w)
    {
    }

    /// Returns the `x`, `y` and `z` components.
    [[nodiscard]] constexpr Vector3 XYZ() const
    {
        return {X, Y, Z};
    }

    /// Returns the component at `index`.
    ///
    /// 0, 1, 2 and 3 select `X`, `Y`, `Z` and `W`; a larger index returns `W`. The index is not checked.
    constexpr float32 operator[](uint32 index) const
    {
        return index == 0 ? X : (index == 1 ? Y : (index == 2 ? Z : W));
    }

    /// Returns the vector with all four components negated.
    constexpr Vector4 operator-() const
    {
        return {-X, -Y, -Z, -W};
    }

    /// Returns the componentwise sum of the two vectors.
    constexpr Vector4 operator+(Vector4 other) const
    {
        return {X + other.X, Y + other.Y, Z + other.Z, W + other.W};
    }

    /// Returns the componentwise difference of the two vectors.
    constexpr Vector4 operator-(Vector4 other) const
    {
        return {X - other.X, Y - other.Y, Z - other.Z, W - other.W};
    }

    /// Returns the componentwise product of the two vectors.
    constexpr Vector4 operator*(Vector4 other) const
    {
        return {X * other.X, Y * other.Y, Z * other.Z, W * other.W};
    }

    /// Returns all four components multiplied by `scale`.
    constexpr Vector4 operator*(float32 scale) const
    {
        return {X * scale, Y * scale, Z * scale, W * scale};
    }

    /// Returns all four components divided by `divisor`.
    constexpr Vector4 operator/(float32 divisor) const
    {
        return *this * (1 / divisor);
    }

    /// Returns `vector` scaled by `scale`.
    friend constexpr Vector4 operator*(float32 scale, Vector4 vector)
    {
        return vector * scale;
    }

    /// Adds `other` to this vector and returns it.
    constexpr Vector4 &operator+=(Vector4 other)
    {
        return *this = *this + other;
    }

    /// Compares all four components exactly.
    constexpr bool operator==(const Vector4 &) const = default;

    /// Returns the dot product of `a` and `b`.
    [[nodiscard]] static constexpr float32 Dot(Vector4 a, Vector4 b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    }

    /// Returns the squared length of the vector.
    [[nodiscard]] constexpr float32 LengthSquared() const
    {
        return Dot(*this, *this);
    }

    /// Returns the length of the vector.
    [[nodiscard]] float32 Length() const
    {
        return Math::Sqrt(LengthSquared());
    }

    /// Returns the linear interpolation from `a` to `b` by `t`.
    ///
    /// `t` is not clamped, so values outside [0, 1] extrapolate.
    [[nodiscard]] static constexpr Vector4 Lerp(Vector4 a, Vector4 b, float32 t)
    {
        return a + (b - a) * t;
    }

    /// Returns whether every component is within `tolerance` of `other`.
    ///
    /// The default tolerance is `Math::SmallNumber`.
    [[nodiscard]] constexpr bool IsNearlyEqual(Vector4 other, float32 tolerance = Math::SmallNumber) const
    {
        return Math::IsNearlyEqual(X, other.X, tolerance) && Math::IsNearlyEqual(Y, other.Y, tolerance) &&
               Math::IsNearlyEqual(Z, other.Z, tolerance) && Math::IsNearlyEqual(W, other.W, tolerance);
    }

    /// Writes `(x, y, z, w)` to `output`, applying `spec` to each component.
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &X, 4, spec);
    }
};

/// Integer coordinates for 2D grids: texels, grid cells and screen pixels.
///
/// Serializers write the coordinates as an ordered tuple, and `FormatTo` writes `(x, y)`.
MSTRUCT(Tuple)
struct IntVector2
{
    MGENERATED_BODY()

    /// The X coordinate.
    MPROPERTY()
    int32 X = 0;
    /// The Y coordinate.
    MPROPERTY()
    int32 Y = 0;

    /// Creates a zero vector.
    constexpr IntVector2() = default;

    /// Creates a vector from `x` and `y`.
    constexpr IntVector2(int32 x, int32 y) : X(x), Y(y)
    {
    }

    /// Returns the componentwise sum of the two vectors.
    constexpr IntVector2 operator+(IntVector2 other) const
    {
        return {X + other.X, Y + other.Y};
    }

    /// Returns the componentwise difference of the two vectors.
    constexpr IntVector2 operator-(IntVector2 other) const
    {
        return {X - other.X, Y - other.Y};
    }

    /// Returns both coordinates multiplied by `scale`.
    constexpr IntVector2 operator*(int32 scale) const
    {
        return {X * scale, Y * scale};
    }

    /// Compares both coordinates exactly.
    constexpr bool operator==(const IntVector2 &) const = default;

    /// Converts the coordinates to `float32` as a `Vector2`.
    [[nodiscard]] constexpr explicit operator Vector2() const
    {
        return {static_cast<float32>(X), static_cast<float32>(Y)};
    }

    /// Returns a hash of the coordinates.
    ///
    /// Equal coordinates have equal hashes, so the type can key a hash container.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashInteger((static_cast<uint64>(static_cast<uint32>(X)) << 32) | static_cast<uint32>(Y));
    }

    /// Writes `(x, y)` to `output`, applying `spec` to each coordinate.
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &X, 2, spec);
    }
};

/// Integer coordinates for 3D grids.
///
/// Serializers write the coordinates as an ordered tuple, and `FormatTo` writes `(x, y, z)`.
MSTRUCT(Tuple)
struct IntVector3
{
    MGENERATED_BODY()

    /// The X coordinate.
    MPROPERTY()
    int32 X = 0;
    /// The Y coordinate.
    MPROPERTY()
    int32 Y = 0;
    /// The Z coordinate.
    MPROPERTY()
    int32 Z = 0;

    /// Creates a zero vector.
    constexpr IntVector3() = default;

    /// Creates a vector from `x`, `y` and `z`.
    constexpr IntVector3(int32 x, int32 y, int32 z) : X(x), Y(y), Z(z)
    {
    }

    /// Returns the componentwise sum of the two vectors.
    constexpr IntVector3 operator+(IntVector3 other) const
    {
        return {X + other.X, Y + other.Y, Z + other.Z};
    }

    /// Returns the componentwise difference of the two vectors.
    constexpr IntVector3 operator-(IntVector3 other) const
    {
        return {X - other.X, Y - other.Y, Z - other.Z};
    }

    /// Returns all three coordinates multiplied by `scale`.
    constexpr IntVector3 operator*(int32 scale) const
    {
        return {X * scale, Y * scale, Z * scale};
    }

    /// Compares all three coordinates exactly.
    constexpr bool operator==(const IntVector3 &) const = default;

    /// Converts the coordinates to `float32` as a `Vector3`.
    [[nodiscard]] constexpr explicit operator Vector3() const
    {
        return {static_cast<float32>(X), static_cast<float32>(Y), static_cast<float32>(Z)};
    }

    /// Returns a hash of the coordinates.
    ///
    /// Equal coordinates have equal hashes, so the type can key a hash container.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashCombine(HashInteger((static_cast<uint64>(static_cast<uint32>(X)) << 32) | static_cast<uint32>(Y)),
                           HashInteger(static_cast<uint32>(Z)));
    }

    /// Writes `(x, y, z)` to `output`, applying `spec` to each coordinate.
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &X, 3, spec);
    }
};
} // namespace Marlin
