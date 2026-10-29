//! Double-precision world positions for large worlds.
//!
//! Rendering and other float math use a float offset from a nearby origin, such as the camera, so precision stays
//! high far from the world origin.

#pragma once

#include "CoreTypes.h"
#include "Math/Vector.h"
#include "Math/WorldPosition.gen.h"

namespace Marlin
{
/// A position in a large world, stored in double precision.
///
/// The coordinates are world-space meters, with Y up. Converting to a float offset from a nearby origin keeps the
/// precision of float math high near that origin.
///
/// # Examples
///
/// ```cpp
/// const WorldPosition origin(1e9, 0.0, 0.0);
/// const WorldPosition point(1e9 + 2.0, 1.0, 0.0);
/// const Vector3 offset = point.ToRelative(origin); // -> (2.0, 1.0, 0.0)
/// ```
MSTRUCT(Tuple)
struct WorldPosition
{
    MGENERATED_BODY()

    /// The X coordinate, in world meters; defaults to 0.
    MPROPERTY()
    float64 X = 0;
    /// The Y coordinate, in world meters; defaults to 0.
    MPROPERTY()
    float64 Y = 0;
    /// The Z coordinate, in world meters; defaults to 0.
    MPROPERTY()
    float64 Z = 0;

    /// Creates the world origin.
    constexpr WorldPosition() = default;

    /// Creates a position from `x`, `y` and `z` in world meters.
    constexpr WorldPosition(float64 x, float64 y, float64 z) : X(x), Y(y), Z(z)
    {
    }

    /// Converts a `Vector3` position to a world position, widening each coordinate.
    constexpr explicit WorldPosition(Vector3 position) : X(position.X), Y(position.Y), Z(position.Z)
    {
    }

    /// Returns the offset from `origin` as a `Vector3`.
    ///
    /// The subtraction is done in double precision and the result is cast to `float32`, so the offset is precise when
    /// this position is close to `origin`.
    [[nodiscard]] constexpr Vector3 ToRelative(const WorldPosition &origin) const
    {
        return {static_cast<float32>(X - origin.X), static_cast<float32>(Y - origin.Y),
                static_cast<float32>(Z - origin.Z)};
    }

    /// Returns the position that is `offset` away from `origin`.
    [[nodiscard]] static constexpr WorldPosition FromRelative(const WorldPosition &origin, Vector3 offset)
    {
        return {origin.X + offset.X, origin.Y + offset.Y, origin.Z + offset.Z};
    }

    /// Returns this position moved by `offset`.
    constexpr WorldPosition operator+(Vector3 offset) const
    {
        return FromRelative(*this, offset);
    }

    /// Returns this position moved by the negation of `offset`.
    constexpr WorldPosition operator-(Vector3 offset) const
    {
        return {X - offset.X, Y - offset.Y, Z - offset.Z};
    }

    /// Moves this position by `offset`.
    constexpr WorldPosition &operator+=(Vector3 offset)
    {
        return *this = *this + offset;
    }

    /// Compares the three coordinates exactly.
    constexpr bool operator==(const WorldPosition &) const = default;

    /// Returns the Euclidean distance between `a` and `b`, in meters.
    [[nodiscard]] static float64 Distance(const WorldPosition &a, const WorldPosition &b)
    {
        const float64 x = a.X - b.X;
        const float64 y = a.Y - b.Y;
        const float64 z = a.Z - b.Z;
        return Math::Sqrt(x * x + y * y + z * z);
    }

    /// Writes the position as `(x, y, z)` to `output`, applying `spec` to each coordinate.
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &X, 3, spec);
    }
};
} // namespace Marlin
