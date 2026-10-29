//! Matrix types for affine transforms, projections and view transforms.
//!
//! Matrices store rows and vectors are columns, so a point transforms as `p' = M * p`. `Matrix3x4` holds an affine
//! transform; `Matrix4x4` adds a fourth row for projections and view transforms.

#pragma once

#include "CoreTypes.h"
#include "Math/Quaternion.h"
#include "Math/Vector.h"

namespace Marlin
{
/// An affine transform stored as three rows.
///
/// Points transform as column vectors, `p' = M * (p, 1)`. Row `i` holds the coefficients of output component `i`, and
/// the fourth column is the translation.
///
/// # Examples
///
/// ```cpp
/// const Matrix3x4 transform = Matrix3x4::FromTranslationRotationScale(
///     {1.0f, 0.0f, 0.0f}, Quaternion::Identity, {2.0f, 2.0f, 2.0f});
/// const Vector3 point = transform.TransformPoint({1.0f, 0.0f, 0.0f}); // -> (3.0, 0.0, 0.0)
/// ```
struct Matrix3x4
{
    /// The three rows; row `i` holds the coefficients of output component `i`.
    Vector4 Rows[3] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    /// Creates the identity transform.
    constexpr Matrix3x4() = default;

    /// Creates a transform from three rows.
    constexpr Matrix3x4(Vector4 row0, Vector4 row1, Vector4 row2) : Rows{row0, row1, row2}
    {
    }

    /// The identity transform.
    static const Matrix3x4 Identity;

    /// Creates a transform that scales, then rotates, then translates.
    ///
    /// `rotation` must be a unit quaternion.
    [[nodiscard]] static constexpr Matrix3x4 FromTranslationRotationScale(Vector3 translation, Quaternion rotation,
                                                                          Vector3 scale)
    {
        const float32 x = rotation.X;
        const float32 y = rotation.Y;
        const float32 z = rotation.Z;
        const float32 w = rotation.W;
        return {{(1 - 2 * (y * y + z * z)) * scale.X, 2 * (x * y - w * z) * scale.Y, 2 * (x * z + w * y) * scale.Z,
                 translation.X},
                {2 * (x * y + w * z) * scale.X, (1 - 2 * (x * x + z * z)) * scale.Y, 2 * (y * z - w * x) * scale.Z,
                 translation.Y},
                {2 * (x * z - w * y) * scale.X, 2 * (y * z + w * x) * scale.Y, (1 - 2 * (x * x + y * y)) * scale.Z,
                 translation.Z}};
    }

    /// Creates a transform that only translates.
    [[nodiscard]] static constexpr Matrix3x4 FromTranslation(Vector3 translation)
    {
        return {{1, 0, 0, translation.X}, {0, 1, 0, translation.Y}, {0, 0, 1, translation.Z}};
    }

    /// Returns the affine image of `point`, including translation.
    [[nodiscard]] constexpr Vector3 TransformPoint(Vector3 point) const
    {
        return {Vector4::Dot(Rows[0], Vector4(point, 1)), Vector4::Dot(Rows[1], Vector4(point, 1)),
                Vector4::Dot(Rows[2], Vector4(point, 1))};
    }

    /// Returns the image of `vector` as a direction, without translation.
    [[nodiscard]] constexpr Vector3 TransformVector(Vector3 vector) const
    {
        return {Vector4::Dot(Rows[0], Vector4(vector, 0)), Vector4::Dot(Rows[1], Vector4(vector, 0)),
                Vector4::Dot(Rows[2], Vector4(vector, 0))};
    }

    /// Returns the translation, stored in the fourth column.
    [[nodiscard]] constexpr Vector3 GetTranslation() const
    {
        return {Rows[0].W, Rows[1].W, Rows[2].W};
    }

    /// Composes two transforms; `a * b` applies `b` first, then `a`.
    constexpr Matrix3x4 operator*(const Matrix3x4 &b) const
    {
        Matrix3x4 result;
        for (uint32 i = 0; i < 3; ++i)
        {
            const Vector4 &row = Rows[i];
            result.Rows[i] = Vector4(row.X * b.Rows[0].X + row.Y * b.Rows[1].X + row.Z * b.Rows[2].X,
                                     row.X * b.Rows[0].Y + row.Y * b.Rows[1].Y + row.Z * b.Rows[2].Y,
                                     row.X * b.Rows[0].Z + row.Y * b.Rows[1].Z + row.Z * b.Rows[2].Z,
                                     row.X * b.Rows[0].W + row.Y * b.Rows[1].W + row.Z * b.Rows[2].W + row.W);
        }
        return result;
    }

    /// Compares the three rows exactly.
    constexpr bool operator==(const Matrix3x4 &) const = default;

    /// Returns the determinant of the 3x3 linear part.
    ///
    /// The translation does not affect the result. A value near zero means the transform is singular.
    [[nodiscard]] CORE_API float32 Determinant() const;
    /// Returns the inverse transform, or the identity when the matrix is singular.
    ///
    /// A determinant whose magnitude is below `1e-12` is treated as zero.
    [[nodiscard]] CORE_API Matrix3x4 Inverse() const;
    /// Returns whether every row is within `tolerance` of the corresponding row of `other`.
    [[nodiscard]] CORE_API bool IsNearlyEqual(const Matrix3x4 &other, float32 tolerance = 1e-5f) const;
};

inline constexpr Matrix3x4 Matrix3x4::Identity{};

/// A general 4x4 matrix stored as four rows, used for projections and view transforms.
///
/// Vectors are columns, `v' = M * v`. Projections follow the engine's right-handed view space, where the camera looks
/// along -Z, and reversed depth: the near plane maps to 1 and the far plane to 0.
struct Matrix4x4
{
    /// The four rows; row `i` holds the coefficients of output component `i`.
    Vector4 Rows[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    /// Creates the identity matrix.
    constexpr Matrix4x4() = default;

    /// Creates a matrix from four rows.
    constexpr Matrix4x4(Vector4 row0, Vector4 row1, Vector4 row2, Vector4 row3) : Rows{row0, row1, row2, row3}
    {
    }

    /// Creates a 4x4 matrix from an affine transform; the last row is `(0, 0, 0, 1)`.
    constexpr explicit Matrix4x4(const Matrix3x4 &affine)
        : Rows{affine.Rows[0], affine.Rows[1], affine.Rows[2], {0, 0, 0, 1}}
    {
    }

    /// The identity matrix.
    static const Matrix4x4 Identity;

    /// Creates a perspective projection.
    ///
    /// `verticalFov` is the vertical field of view in radians and `aspect` is width over height. `nearZ` and `farZ`
    /// are finite positive distances in front of the camera. The near plane maps to 1 and the far plane to 0.
    ///
    /// # Preconditions
    /// `0 < verticalFov < Pi`, `aspect > 0`, and `0 < nearZ < farZ`. The values are not checked.
    [[nodiscard]] CORE_API static Matrix4x4 Perspective(float32 verticalFov, float32 aspect, float32 nearZ,
                                                        float32 farZ);
    /// Creates a perspective projection with the far plane at infinity.
    ///
    /// `verticalFov` is in radians and `aspect` is width over height. `nearZ` is a finite positive distance in front
    /// of the camera. The near plane maps to 1; the far plane is the limit at 0.
    ///
    /// # Preconditions
    /// `0 < verticalFov < Pi`, `aspect > 0`, and `nearZ > 0`. The values are not checked.
    [[nodiscard]] CORE_API static Matrix4x4 PerspectiveInfinite(float32 verticalFov, float32 aspect, float32 nearZ);
    /// Creates an orthographic projection of a `width` by `height` view volume.
    ///
    /// The volume extends from `nearZ` to `farZ`, both finite positive distances in front of the camera. The near
    /// plane maps to 1 and the far plane to 0.
    ///
    /// # Preconditions
    /// `width > 0`, `height > 0`, and `0 < nearZ < farZ`. The values are not checked.
    [[nodiscard]] CORE_API static Matrix4x4 Orthographic(float32 width, float32 height, float32 nearZ, float32 farZ);
    /// Creates a view matrix for a camera at `eye` looking at `target`.
    ///
    /// `up` orients the camera. The view space looks along -Z, so `target` lies on -Z in view space.
    ///
    /// # Preconditions
    /// `target` is not `eye`, and `up` is non-zero and not parallel to `target - eye`. The vectors are not checked.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const Matrix4x4 view = Matrix4x4::LookAt({0.0f, 0.0f, 10.0f}, Vector3::Zero, Vector3::Up);
    /// const Vector3 targetInView = view.TransformPoint(Vector3::Zero); // -> (0.0, 0.0, -10.0)
    /// ```
    [[nodiscard]] CORE_API static Matrix4x4 LookAt(Vector3 eye, Vector3 target, Vector3 up);

    /// Returns the product of the matrix and a 4D vector.
    [[nodiscard]] constexpr Vector4 Transform(Vector4 vector) const
    {
        return {Vector4::Dot(Rows[0], vector), Vector4::Dot(Rows[1], vector), Vector4::Dot(Rows[2], vector),
                Vector4::Dot(Rows[3], vector)};
    }

    /// Returns the perspective divide of `M * (x, y, z, 1)`.
    ///
    /// The result is divided by the transformed `w`.
    ///
    /// # Preconditions
    /// The transformed `w` is non-zero; the division is not checked.
    [[nodiscard]] constexpr Vector3 TransformPoint(Vector3 point) const
    {
        const Vector4 result = Transform(Vector4(point, 1));
        return result.XYZ() / result.W;
    }

    /// Composes two matrices; `a * b` applies `b` first, then `a`.
    constexpr Matrix4x4 operator*(const Matrix4x4 &b) const
    {
        Matrix4x4 result;
        for (uint32 i = 0; i < 4; ++i)
        {
            const Vector4 &row = Rows[i];
            result.Rows[i] = b.Rows[0] * row.X + b.Rows[1] * row.Y + b.Rows[2] * row.Z + b.Rows[3] * row.W;
        }
        return result;
    }

    /// Compares all four rows exactly.
    constexpr bool operator==(const Matrix4x4 &) const = default;

    /// Returns the transpose.
    [[nodiscard]] constexpr Matrix4x4 Transposed() const
    {
        return {{Rows[0].X, Rows[1].X, Rows[2].X, Rows[3].X},
                {Rows[0].Y, Rows[1].Y, Rows[2].Y, Rows[3].Y},
                {Rows[0].Z, Rows[1].Z, Rows[2].Z, Rows[3].Z},
                {Rows[0].W, Rows[1].W, Rows[2].W, Rows[3].W}};
    }

    /// Returns the determinant of the full 4x4 matrix.
    [[nodiscard]] CORE_API float32 Determinant() const;
    /// Returns the inverse matrix, or the identity when the matrix is singular.
    ///
    /// A determinant whose magnitude is below `1e-20` is treated as zero.
    [[nodiscard]] CORE_API Matrix4x4 Inverse() const;
    /// Returns whether every row is within `tolerance` of the corresponding row of `other`.
    [[nodiscard]] CORE_API bool IsNearlyEqual(const Matrix4x4 &other, float32 tolerance = 1e-5f) const;
};

inline constexpr Matrix4x4 Matrix4x4::Identity{};
} // namespace Marlin
