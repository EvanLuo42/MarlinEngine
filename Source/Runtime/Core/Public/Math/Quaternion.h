//! Unit quaternions and the rotations they represent.
//!
//! A quaternion stores the vector part in `X`, `Y` and `Z` and the scalar part in `W`. Multiplication composes
//! rotations, and `Transform` uses one for its rotation.

#pragma once

#include "CoreTypes.h"
#include "Math/MathUtility.h"
#include "Math/Vector.h"
#include "Math/Quaternion.gen.h"

namespace Marlin
{
/// A rotation stored as a unit quaternion.
///
/// The vector part is `(X, Y, Z)` and `W` is the scalar part. `a * b` applies the rotation of `b` first, then `a`.
/// The identity rotation is `(0, 0, 0, 1)`. The four-component constructor does not normalize, so call `Normalized`
/// when building a rotation from arbitrary components.
MSTRUCT(Tuple)
struct Quaternion
{
    MGENERATED_BODY()

    /// The X component of the vector part; defaults to 0.
    MPROPERTY()
    float32 X = 0;
    /// The Y component of the vector part; defaults to 0.
    MPROPERTY()
    float32 Y = 0;
    /// The Z component of the vector part; defaults to 0.
    MPROPERTY()
    float32 Z = 0;
    /// The scalar part; defaults to 1.
    MPROPERTY()
    float32 W = 1;

    /// Creates the identity rotation.
    constexpr Quaternion() = default;

    /// Creates a quaternion from its four components; the result is not normalized.
    constexpr Quaternion(float32 x, float32 y, float32 z, float32 w) : X(x), Y(y), Z(z), W(w)
    {
    }

    /// The identity rotation.
    static const Quaternion Identity;

    /// Creates a rotation of `radians` about `axis`.
    ///
    /// Positive angles turn counterclockwise when viewed from the positive end of the axis, looking toward the
    /// origin. `axis` must be a unit vector.
    ///
    /// # Preconditions
    /// `axis` is unit length.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const Quaternion turn = Quaternion::FromAxisAngle(Vector3::Up, Math::HalfPi);
    /// const Vector3 forward = turn.Rotate(Vector3::Right); // -> (0.0, 0.0, -1.0)
    /// ```
    [[nodiscard]] static Quaternion FromAxisAngle(Vector3 axis, float32 radians)
    {
        float32 sine = 0;
        float32 cosine = 0;
        Math::SinCos(radians * 0.5f, sine, cosine);
        return {axis.X * sine, axis.Y * sine, axis.Z * sine, cosine};
    }

    /// Creates a rotation from pitch, yaw and roll angles in radians.
    ///
    /// Applies roll about Z, then pitch about X, then yaw about Y.
    [[nodiscard]] static Quaternion FromEuler(float32 pitch, float32 yaw, float32 roll)
    {
        return FromAxisAngle(Vector3::Up, yaw) * FromAxisAngle(Vector3::Right, pitch) *
               FromAxisAngle(Vector3::Backward, roll);
    }

    /// Returns pitch, yaw and roll in radians, in the order `FromEuler` takes them.
    ///
    /// At gimbal lock (`|sin pitch| > 0.99999`) the roll is returned as zero and the yaw absorbs the remaining
    /// rotation.
    [[nodiscard]] Vector3 ToEuler() const
    {
        const float32 sinPitch = 2 * (W * X - Y * Z);
        const float32 pitch = Math::Asin(sinPitch);
        if (Math::Abs(sinPitch) > 0.99999f)
        {
            return {pitch, Math::Atan2(-2 * (X * Z - W * Y), 1 - 2 * (Y * Y + Z * Z)), 0};
        }
        const float32 yaw = Math::Atan2(2 * (X * Z + W * Y), 1 - 2 * (X * X + Y * Y));
        const float32 roll = Math::Atan2(2 * (X * Y + W * Z), 1 - 2 * (X * X + Z * Z));
        return {pitch, yaw, roll};
    }

    /// Returns the shortest rotation that takes unit `from` onto unit `to`.
    ///
    /// For opposite vectors the axis is chosen perpendicular to `from`; any perpendicular axis works, so the result
    /// is not unique.
    ///
    /// # Preconditions
    /// `from` and `to` are unit vectors.
    [[nodiscard]] static Quaternion FromTo(Vector3 from, Vector3 to)
    {
        const float32 dot = Vector3::Dot(from, to);
        if (dot < -0.999999f)
        {
            Vector3 axis = Vector3::Cross(Vector3::Right, from);
            axis = axis.LengthSquared() < 1e-6f ? Vector3::Cross(Vector3::Up, from) : axis;
            return FromAxisAngle(axis.Normalized(), Math::Pi);
        }
        const Vector3 cross = Vector3::Cross(from, to);
        return Quaternion(cross.X, cross.Y, cross.Z, 1 + dot).Normalized();
    }

    /// Multiplies two quaternions; `a * b` applies `b` first, then `a`.
    constexpr Quaternion operator*(Quaternion b) const
    {
        return {W * b.X + X * b.W + Y * b.Z - Z * b.Y, W * b.Y - X * b.Z + Y * b.W + Z * b.X,
                W * b.Z + X * b.Y - Y * b.X + Z * b.W, W * b.W - X * b.X - Y * b.Y - Z * b.Z};
    }

    /// Multiplies this quaternion by `other` in place.
    constexpr Quaternion &operator*=(Quaternion other)
    {
        return *this = *this * other;
    }

    /// Compares all four components exactly.
    ///
    /// `q` and `-q` represent the same rotation but compare unequal.
    constexpr bool operator==(const Quaternion &) const = default;

    /// Returns the conjugate, which negates the vector part.
    ///
    /// For a unit quaternion the conjugate is the inverse rotation.
    [[nodiscard]] constexpr Quaternion Conjugate() const
    {
        return {-X, -Y, -Z, W};
    }

    /// Returns the multiplicative inverse.
    ///
    /// For a unit quaternion this equals `Conjugate`.
    ///
    /// # Preconditions
    /// The quaternion is not zero.
    [[nodiscard]] constexpr Quaternion Inverse() const
    {
        const float32 lengthSquared = X * X + Y * Y + Z * Z + W * W;
        const Quaternion conjugate = Conjugate();
        return {conjugate.X / lengthSquared, conjugate.Y / lengthSquared, conjugate.Z / lengthSquared,
                conjugate.W / lengthSquared};
    }

    /// Returns the dot product of `a` and `b`.
    [[nodiscard]] static constexpr float32 Dot(Quaternion a, Quaternion b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    }

    /// Returns the quaternion scaled to unit length.
    ///
    /// Returns `Identity` when the squared length is below `1e-12`.
    [[nodiscard]] Quaternion Normalized() const
    {
        const float32 lengthSquared = Dot(*this, *this);
        if (lengthSquared < 1e-12f)
        {
            return Identity;
        }
        const float32 scale = Math::InverseSqrt(lengthSquared);
        return {X * scale, Y * scale, Z * scale, W * scale};
    }

    /// Rotates `vector` by this quaternion.
    ///
    /// The quaternion must be unit length for a pure rotation.
    [[nodiscard]] constexpr Vector3 Rotate(Vector3 vector) const
    {
        const Vector3 axis(X, Y, Z);
        const Vector3 t = Vector3::Cross(axis, vector) * 2;
        return vector + t * W + Vector3::Cross(axis, t);
    }

    /// Returns the rotated +X axis.
    [[nodiscard]] constexpr Vector3 GetRight() const
    {
        return Rotate(Vector3::Right);
    }

    /// Returns the rotated +Y axis.
    [[nodiscard]] constexpr Vector3 GetUp() const
    {
        return Rotate(Vector3::Up);
    }

    /// Returns the rotated forward axis, [Vector3::Forward].
    [[nodiscard]] constexpr Vector3 GetForward() const
    {
        return Rotate(Vector3::Forward);
    }

    /// Returns the angle between two rotations, in radians in [0, Pi].
    ///
    /// `a` and `b` are unit quaternions. The result is the shortest unsigned angle, so `q` and `-q` have angle zero.
    [[nodiscard]] static float32 AngleBetween(Quaternion a, Quaternion b)
    {
        return 2 * Math::Acos(Math::Abs(Dot(a, b)));
    }

    /// Returns the normalized linear interpolation from `a` to `b` by `t`.
    ///
    /// Follows the shorter arc. It is cheaper than `Slerp` and close to it for small angles, but its angular speed is
    /// not constant. `t` is not clamped.
    [[nodiscard]] static Quaternion Nlerp(Quaternion a, Quaternion b, float32 t)
    {
        const float32 sign = Dot(a, b) < 0 ? -1.0f : 1.0f;
        return Quaternion(a.X + (b.X * sign - a.X) * t, a.Y + (b.Y * sign - a.Y) * t, a.Z + (b.Z * sign - a.Z) * t,
                          a.W + (b.W * sign - a.W) * t)
            .Normalized();
    }

    /// Returns the spherical interpolation from `a` to `b` by `t`.
    ///
    /// Follows the shorter arc at constant angular speed. Falls back to `Nlerp` when the rotations are nearly equal.
    /// `t` is not clamped.
    [[nodiscard]] static Quaternion Slerp(Quaternion a, Quaternion b, float32 t)
    {
        float32 cosine = Dot(a, b);
        const float32 sign = cosine < 0 ? -1.0f : 1.0f;
        cosine *= sign;
        if (cosine > 0.9995f)
        {
            return Nlerp(a, b, t);
        }
        const float32 angle = Math::Acos(cosine);
        const float32 inverseSine = 1 / Math::Sin(angle);
        const float32 weightA = Math::Sin((1 - t) * angle) * inverseSine;
        const float32 weightB = Math::Sin(t * angle) * inverseSine * sign;
        return {a.X * weightA + b.X * weightB, a.Y * weightA + b.Y * weightB, a.Z * weightA + b.Z * weightB,
                a.W * weightA + b.W * weightB};
    }

    /// Returns whether two rotations differ by at most `tolerance`.
    ///
    /// Compares `1 - |Dot(a, b)|` with `tolerance`, so `q` and `-q` are equal.
    [[nodiscard]] bool IsNearlyEqual(Quaternion other, float32 tolerance = 1e-5f) const
    {
        return Math::Abs(Dot(*this, other)) >= 1 - tolerance;
    }
};

inline constexpr Quaternion Quaternion::Identity{0, 0, 0, 1};
} // namespace Marlin
