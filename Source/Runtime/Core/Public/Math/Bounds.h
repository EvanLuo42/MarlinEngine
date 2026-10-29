//! Bounding volumes and intersection tests: boxes, spheres, planes, frusta and rays.
//!
//! The types use the engine's right-handed, Y-up space. Distances are in the same units as the coordinates.

#pragma once

#include "CoreTypes.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Math/Bounds.gen.h"

namespace Marlin
{
/// An axis-aligned box with minimum and maximum corners.
///
/// A default-constructed box is empty: `Min` is above `Max` on every axis, so adding any point makes it valid.
/// `IsValid` reports whether the corners are ordered.
MSTRUCT()
struct Box
{
    MGENERATED_BODY()

    /// The minimum corner; the default is far above `Max`, so a new box is empty.
    MPROPERTY()
    Vector3 Min{1e30f};
    /// The maximum corner; the default is far below `Min`, so a new box is empty.
    MPROPERTY()
    Vector3 Max{-1e30f};

    /// Creates an empty box with `Min` above `Max` on every axis.
    constexpr Box() = default;

    /// Creates a box from two corners.
    ///
    /// # Preconditions
    /// `min` is at or below `max` on every axis for a valid box. The corners are not checked; `IsValid` reports
    /// whether they are ordered.
    constexpr Box(Vector3 min, Vector3 max) : Min(min), Max(max)
    {
    }

    /// Returns the box around `center` with half-size `extent`.
    ///
    /// A negative component of `extent` produces a box for which `IsValid` is false.
    [[nodiscard]] static constexpr Box FromCenterExtent(Vector3 center, Vector3 extent)
    {
        return {center - extent, center + extent};
    }

    /// Returns whether `Min` is at or below `Max` on every axis.
    [[nodiscard]] constexpr bool IsValid() const
    {
        return Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z;
    }

    /// Returns the center of the box.
    [[nodiscard]] constexpr Vector3 GetCenter() const
    {
        return (Min + Max) * 0.5f;
    }

    /// Returns half the size of the box on every axis.
    [[nodiscard]] constexpr Vector3 GetExtent() const
    {
        return (Max - Min) * 0.5f;
    }

    /// Returns the size of the box on every axis.
    [[nodiscard]] constexpr Vector3 GetSize() const
    {
        return Max - Min;
    }

    /// Extends the box to include `point` and returns it.
    ///
    /// Adding a point to the default box makes it valid.
    constexpr Box &Add(Vector3 point)
    {
        Min = Vector3::Min(Min, point);
        Max = Vector3::Max(Max, point);
        return *this;
    }

    /// Extends the box to include `other` and returns it.
    ///
    /// A default-constructed `other` is ignored.
    constexpr Box &Add(const Box &other)
    {
        Min = Vector3::Min(Min, other.Min);
        Max = Vector3::Max(Max, other.Max);
        return *this;
    }

    /// Returns the box grown by `amount` on every side.
    ///
    /// A negative `amount` shrinks the box and can make it invalid.
    [[nodiscard]] constexpr Box Expanded(float32 amount) const
    {
        return {Min - Vector3(amount), Max + Vector3(amount)};
    }

    /// Returns whether `point` is inside the box or on its surface.
    [[nodiscard]] constexpr bool Contains(Vector3 point) const
    {
        return point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y && point.Z >= Min.Z &&
               point.Z <= Max.Z;
    }

    /// Returns whether the two boxes overlap or touch.
    [[nodiscard]] constexpr bool Intersects(const Box &other) const
    {
        return Min.X <= other.Max.X && Max.X >= other.Min.X && Min.Y <= other.Max.Y && Max.Y >= other.Min.Y &&
               Min.Z <= other.Max.Z && Max.Z >= other.Min.Z;
    }

    /// Returns the point on the box closest to `point`.
    ///
    /// For a point inside the box, the result is `point`.
    [[nodiscard]] constexpr Vector3 ClosestPoint(Vector3 point) const
    {
        return Vector3::Min(Vector3::Max(point, Min), Max);
    }

    /// Returns the axis-aligned box that contains this box after `transform`.
    ///
    /// The center is transformed and the extent is scaled by the absolute values of the three matrix rows
    /// (Arvo's method), so the result is the box around the eight transformed corners.
    [[nodiscard]] constexpr Box Transformed(const Matrix3x4 &transform) const
    {
        const Vector3 center = transform.TransformPoint(GetCenter());
        const Vector3 extent = GetExtent();
        Vector3 newExtent;
        for (uint32 i = 0; i < 3; ++i)
        {
            const Vector4 &row = transform.Rows[i];
            const float32 value =
                Math::Abs(row.X) * extent.X + Math::Abs(row.Y) * extent.Y + Math::Abs(row.Z) * extent.Z;
            (i == 0 ? newExtent.X : (i == 1 ? newExtent.Y : newExtent.Z)) = value;
        }
        return FromCenterExtent(center, newExtent);
    }

    /// Compares the two corners exactly.
    constexpr bool operator==(const Box &) const = default;
};

/// A sphere with a center and radius.
///
/// The default sphere has its center at the origin and radius zero.
MSTRUCT()
struct Sphere
{
    MGENERATED_BODY()

    /// The center in the same units as `Radius`.
    MPROPERTY()
    Vector3 Center;
    /// The radius in the same units as `Center`; defaults to 0.
    MPROPERTY()
    float32 Radius = 0;

    /// Creates a sphere at the origin with radius zero.
    constexpr Sphere() = default;

    /// Creates a sphere with the given center and radius.
    ///
    /// # Preconditions
    /// The radius is expected to be zero or positive. It is not checked.
    constexpr Sphere(Vector3 center, float32 radius) : Center(center), Radius(radius)
    {
    }

    /// Returns whether `point` is inside the sphere or on its surface.
    [[nodiscard]] constexpr bool Contains(Vector3 point) const
    {
        return (point - Center).LengthSquared() <= Radius * Radius;
    }

    /// Returns whether the two spheres overlap or touch.
    [[nodiscard]] constexpr bool Intersects(const Sphere &other) const
    {
        const float32 radii = Radius + other.Radius;
        return (other.Center - Center).LengthSquared() <= radii * radii;
    }

    /// Returns whether the sphere overlaps or touches `box`.
    [[nodiscard]] constexpr bool Intersects(const Box &box) const
    {
        return (box.ClosestPoint(Center) - Center).LengthSquared() <= Radius * Radius;
    }
};

/// A plane defined by a unit normal and a constant offset.
///
/// Points `p` on the plane satisfy `Dot(Normal, p) + Distance = 0`. `Normal` points to the positive side, so a
/// default-constructed plane is `y = 0` with its positive side up.
MSTRUCT()
struct Plane
{
    MGENERATED_BODY()

    /// The unit normal that points to the positive side; defaults to `Vector3::Up`.
    MPROPERTY()
    Vector3 Normal = Vector3::Up;
    /// The offset from the origin; the closest point on the plane is `-Distance * Normal`; defaults to 0.
    MPROPERTY()
    float32 Distance = 0;

    /// Creates the `y = 0` plane with the positive side up.
    constexpr Plane() = default;

    /// Creates a plane from a normal and a constant term.
    ///
    /// # Preconditions
    /// `normal` is unit length. It is not checked.
    constexpr Plane(Vector3 normal, float32 distance) : Normal(normal), Distance(distance)
    {
    }

    /// Returns the plane through `point` with unit `normal`.
    ///
    /// # Preconditions
    /// `normal` is unit length. It is not normalized or checked.
    [[nodiscard]] static constexpr Plane FromPointNormal(Vector3 point, Vector3 normal)
    {
        return {normal, -Vector3::Dot(normal, point)};
    }

    /// Returns the plane through three points, with `a`, `b` and `c` counterclockwise viewed from the positive
    /// side.
    ///
    /// Collinear points produce a plane whose normal is zero.
    [[nodiscard]] static Plane FromPoints(Vector3 a, Vector3 b, Vector3 c)
    {
        return FromPointNormal(a, Vector3::Cross(b - a, c - a).Normalized());
    }

    /// Returns the signed distance from `point` to the plane along the normal.
    ///
    /// The value is positive on the side the normal points to, and is a distance while `Normal` is unit length.
    [[nodiscard]] constexpr float32 SignedDistance(Vector3 point) const
    {
        return Vector3::Dot(Normal, point) + Distance;
    }

    /// Returns the plane from four coefficients, scaled so its normal is unit length.
    ///
    /// `coefficients` holds the `x`, `y` and `z` of the normal and the distance in `w`. The scaling leaves the
    /// plane `Dot(Normal, p) + Distance = 0` unchanged.
    ///
    /// # Preconditions
    /// The `x`, `y` and `z` components are not all zero. The length is not checked.
    [[nodiscard]] static Plane Normalize(Vector4 coefficients)
    {
        const float32 inverse = 1 / coefficients.XYZ().Length();
        return {coefficients.XYZ() * inverse, coefficients.W * inverse};
    }
};

/// A view frustum as six inward-facing planes, for culling.
///
/// `Planes` is indexed by `Left`, `Right`, `Bottom`, `Top`, `Near` and `Far`; a point is inside when it is on
/// the positive side of every plane. `Contains` is exact, while the `Intersects` overloads are conservative: they
/// can accept an object outside the frustum, but never reject one that is inside.
struct Frustum
{
    /// Index of the left plane in `Planes`.
    static constexpr uint32 Left = 0;
    /// Index of the right plane in `Planes`.
    static constexpr uint32 Right = 1;
    /// Index of the bottom plane in `Planes`.
    static constexpr uint32 Bottom = 2;
    /// Index of the top plane in `Planes`.
    static constexpr uint32 Top = 3;
    /// Index of the near plane in `Planes`.
    static constexpr uint32 Near = 4;
    /// Index of the far plane in `Planes`.
    static constexpr uint32 Far = 5;
    /// The number of planes in a frustum.
    static constexpr uint32 PlaneCount = 6;

    /// The six planes, in the order of the index constants.
    Plane Planes[PlaneCount];

    /// Returns the frustum of a view-projection matrix with the engine's reversed depth.
    ///
    /// Near maps to 1 and far to 0. An infinite far plane, whose third row has no `x`, `y` or `z` component,
    /// gives a far plane that never culls.
    ///
    /// # Preconditions
    /// `viewProjection` follows the engine's reversed-depth convention.
    [[nodiscard]] CORE_API static Frustum FromViewProjection(const Matrix4x4 &viewProjection);

    /// Returns whether `point` is inside the frustum or on one of its planes.
    [[nodiscard]] CORE_API bool Contains(Vector3 point) const;
    /// Returns whether the frustum may intersect `sphere`.
    ///
    /// The test is conservative: it returns true for a sphere that touches a plane and may also return true for a
    /// sphere just outside a corner.
    [[nodiscard]] CORE_API bool Intersects(const Sphere &sphere) const;
    /// Returns whether the frustum may intersect `box`.
    ///
    /// The test is conservative: it returns true for a box that touches a plane and may also return true for a box
    /// just outside a corner.
    [[nodiscard]] CORE_API bool Intersects(const Box &box) const;
};

/// A half-line from `Origin` along the unit `Direction`.
///
/// `Direction` is unit length, so `PointAt` and the intersection results are distances. The constructors do not
/// normalize it.
MSTRUCT()
struct Ray
{
    MGENERATED_BODY()

    /// The start of the ray; defaults to the origin.
    MPROPERTY()
    Vector3 Origin;
    /// The unit direction of the ray; defaults to `Vector3::Forward`.
    MPROPERTY()
    Vector3 Direction = Vector3::Forward;

    /// Creates a ray at the origin pointing along -Z.
    constexpr Ray() = default;

    /// Creates a ray from an origin and a direction.
    ///
    /// # Preconditions
    /// `direction` is unit length. It is not normalized or checked.
    constexpr Ray(Vector3 origin, Vector3 direction) : Origin(origin), Direction(direction)
    {
    }

    /// Returns the point at `distance` along the ray.
    [[nodiscard]] constexpr Vector3 PointAt(float32 distance) const
    {
        return Origin + Direction * distance;
    }

    /// Returns the distance to the first intersection with `box`, or `-1` when the ray misses.
    ///
    /// A ray whose origin is inside the box intersects at distance zero.
    ///
    /// # Preconditions
    /// `box` is valid (`IsValid()`). An invalid box is not detected and can return `0` instead of `-1` for a
    /// direction with a nonzero component on every axis.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const Ray ray({-5, 0.5f, 0.5f}, Vector3::Right);
    /// const float32 distance = ray.Intersect(Box({0, 0, 0}, {1, 1, 1})); // -> 5
    /// ```
    [[nodiscard]] CORE_API float32 Intersect(const Box &box) const;
    /// Returns the distance to the first intersection with `sphere`, or `-1` when the ray misses.
    ///
    /// A ray whose origin is inside the sphere intersects at distance zero.
    [[nodiscard]] CORE_API float32 Intersect(const Sphere &sphere) const;
    /// Returns the distance to the plane, or `-1` when it is behind the origin or the ray is parallel to it.
    ///
    /// A ray that lies in the plane counts as parallel and misses.
    [[nodiscard]] CORE_API float32 Intersect(const Plane &plane) const;
};
} // namespace Marlin
