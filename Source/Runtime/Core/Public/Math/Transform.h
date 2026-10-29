//! A local transform: translation, rotation and scale.
//!
//! A transform applies scale, then rotation, then translation. Composition of non-uniform scale with rotation cannot
//! represent the resulting shear, so `Transform::operator*` approximates it the same way Unreal Engine's `FTransform`
//! does.

#pragma once

#include "CoreTypes.h"
#include "Math/Matrix.h"
#include "Math/Quaternion.h"
#include "Math/Vector.h"
#include "Math/Transform.gen.h"

namespace Marlin
{
/// A translation, rotation and scale, applied as scale, then rotation, then translation.
///
/// The rotation is a unit quaternion. Composition with non-uniform scale and rotation cannot express shear, so
/// `operator*` approximates the true composition. `Inverse` is exact for uniform scale; with non-uniform scale and
/// a non-identity rotation the true inverse is not a `Transform`.
MSTRUCT()
struct Transform
{
    MGENERATED_BODY()

    /// The translation in meters; defaults to zero.
    MPROPERTY()
    Vector3 Translation;
    /// The rotation; defaults to the identity rotation.
    MPROPERTY()
    Quaternion Rotation;
    /// The scale along each axis; defaults to one.
    MPROPERTY()
    Vector3 Scale = Vector3::One;

    /// Creates the identity transform.
    constexpr Transform() = default;

    /// Creates a transform from a translation, a rotation and a scale.
    ///
    /// `rotation` defaults to `Quaternion::Identity` and `scale` defaults to `Vector3::One`.
    constexpr Transform(Vector3 translation, Quaternion rotation = Quaternion::Identity, Vector3 scale = Vector3::One)
        : Translation(translation), Rotation(rotation), Scale(scale)
    {
    }

    /// The identity transform.
    static const Transform Identity;

    /// Returns the point transformed as scale, then rotation, then translation.
    [[nodiscard]] constexpr Vector3 TransformPoint(Vector3 point) const
    {
        return Translation + Rotation.Rotate(point * Scale);
    }

    /// Returns the direction transformed by the scale and rotation, without translation.
    [[nodiscard]] constexpr Vector3 TransformVector(Vector3 vector) const
    {
        return Rotation.Rotate(vector * Scale);
    }

    /// Returns the point mapped back from the transform's space.
    ///
    /// This is the exact inverse of `TransformPoint`, including non-uniform scale.
    ///
    /// # Preconditions
    /// No scale component is zero.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const Transform transform({10.0f, 0.0f, 0.0f}, Quaternion::Identity, {2.0f, 2.0f, 2.0f});
    /// const Vector3 local = transform.InverseTransformPoint({12.0f, 0.0f, 0.0f}); // -> (1.0, 0.0, 0.0)
    /// ```
    [[nodiscard]] constexpr Vector3 InverseTransformPoint(Vector3 point) const
    {
        return Rotation.Conjugate().Rotate(point - Translation) / Scale;
    }

    /// Composes a parent transform with a child.
    ///
    /// `parent * child` places `child` in the parent's space. With non-uniform scale and rotation the result
    /// approximates the true composition because a `Transform` cannot represent the shear it would produce.
    constexpr Transform operator*(const Transform &child) const
    {
        return {TransformPoint(child.Translation), Rotation * child.Rotation, Scale * child.Scale};
    }

    /// Returns the inverse transform.
    ///
    /// Exact for uniform scale. With non-uniform scale and rotation the true inverse is not a `Transform`; use
    /// `InverseTransformPoint` or `ToMatrix().Inverse()` instead.
    ///
    /// # Preconditions
    /// No scale component is zero.
    [[nodiscard]] constexpr Transform Inverse() const
    {
        const Quaternion inverseRotation = Rotation.Conjugate();
        const Vector3 inverseScale(1 / Scale.X, 1 / Scale.Y, 1 / Scale.Z);
        return {inverseRotation.Rotate(-Translation) * inverseScale, inverseRotation, inverseScale};
    }

    /// Converts the transform to an affine matrix.
    [[nodiscard]] constexpr Matrix3x4 ToMatrix() const
    {
        return Matrix3x4::FromTranslationRotationScale(Translation, Rotation, Scale);
    }

    /// Interpolates between two transforms by `t`.
    ///
    /// Lerps translation and scale, and slerps rotation. `t` is not clamped.
    [[nodiscard]] static Transform Blend(const Transform &a, const Transform &b, float32 t)
    {
        return {Vector3::Lerp(a.Translation, b.Translation, t), Quaternion::Slerp(a.Rotation, b.Rotation, t),
                Vector3::Lerp(a.Scale, b.Scale, t)};
    }

    /// Returns whether translation, rotation and scale are within `tolerance` of `other`'s.
    [[nodiscard]] bool IsNearlyEqual(const Transform &other, float32 tolerance = 1e-5f) const
    {
        return Translation.IsNearlyEqual(other.Translation, tolerance) &&
               Rotation.IsNearlyEqual(other.Rotation, tolerance) && Scale.IsNearlyEqual(other.Scale, tolerance);
    }
};

inline constexpr Transform Transform::Identity{};
} // namespace Marlin
