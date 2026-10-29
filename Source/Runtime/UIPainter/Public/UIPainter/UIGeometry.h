#pragma once

#include "CoreTypes.h"
#include "Math/MathUtility.h"
#include "Math/Vector.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// An axis-aligned rectangle in painter units: Min is the top-left corner, Y grows downwards.
struct UIRect
{
    float32 X = 0;
    float32 Y = 0;
    float32 Width = 0;
    float32 Height = 0;

    constexpr UIRect() = default;

    constexpr UIRect(float32 x, float32 y, float32 width, float32 height) : X(x), Y(y), Width(width), Height(height)
    {
    }

    [[nodiscard]] static constexpr UIRect FromCorners(Vector2 min, Vector2 max)
    {
        return {min.X, min.Y, max.X - min.X, max.Y - min.Y};
    }

    [[nodiscard]] constexpr Vector2 GetMin() const
    {
        return {X, Y};
    }

    [[nodiscard]] constexpr Vector2 GetMax() const
    {
        return {X + Width, Y + Height};
    }

    [[nodiscard]] constexpr Vector2 GetCenter() const
    {
        return {X + Width * 0.5f, Y + Height * 0.5f};
    }

    [[nodiscard]] constexpr bool IsEmpty() const
    {
        return !(Width > 0 && Height > 0);
    }

    /// Grown by *amount* on every side; negative shrinks.
    [[nodiscard]] constexpr UIRect Inflate(float32 amount) const
    {
        return {X - amount, Y - amount, Width + amount * 2, Height + amount * 2};
    }

    [[nodiscard]] constexpr UIRect Intersect(const UIRect &other) const
    {
        const float32 x0 = Max(X, other.X);
        const float32 y0 = Max(Y, other.Y);
        const float32 x1 = Min(X + Width, other.X + other.Width);
        const float32 y1 = Min(Y + Height, other.Y + other.Height);
        return {x0, y0, Max(x1 - x0, 0.0f), Max(y1 - y0, 0.0f)};
    }

    [[nodiscard]] constexpr UIRect Union(const UIRect &other) const
    {
        if (IsEmpty())
        {
            return other;
        }
        if (other.IsEmpty())
        {
            return *this;
        }
        return FromCorners({Min(X, other.X), Min(Y, other.Y)},
                           {Max(X + Width, other.X + other.Width), Max(Y + Height, other.Y + other.Height)});
    }

    constexpr bool operator==(const UIRect &) const = default;
};

/// Corner radii, clockwise from the top left; each is clamped to half the shorter side when drawn.
struct UICornerRadii
{
    float32 TopLeft = 0;
    float32 TopRight = 0;
    float32 BottomRight = 0;
    float32 BottomLeft = 0;

    constexpr UICornerRadii() = default;

    constexpr UICornerRadii(float32 all) : TopLeft(all), TopRight(all), BottomRight(all), BottomLeft(all)
    {
    }

    constexpr UICornerRadii(float32 topLeft, float32 topRight, float32 bottomRight, float32 bottomLeft)
        : TopLeft(topLeft), TopRight(topRight), BottomRight(bottomRight), BottomLeft(bottomLeft)
    {
    }

    [[nodiscard]] constexpr bool IsZero() const
    {
        return TopLeft <= 0 && TopRight <= 0 && BottomRight <= 0 && BottomLeft <= 0;
    }

    constexpr bool operator==(const UICornerRadii &) const = default;
};

/// A 2D affine transform: (x, y) maps to (M11 x + M21 y + Dx, M12 x + M22 y + Dy).
struct UIPAINTER_API UITransform
{
    float32 M11 = 1;
    float32 M12 = 0;
    float32 M21 = 0;
    float32 M22 = 1;
    float32 Dx = 0;
    float32 Dy = 0;

    [[nodiscard]] static constexpr UITransform Translation(float32 x, float32 y)
    {
        return {1, 0, 0, 1, x, y};
    }

    [[nodiscard]] static constexpr UITransform Scale(float32 x, float32 y)
    {
        return {x, 0, 0, y, 0, 0};
    }

    /// Clockwise on screen by *radians*.
    [[nodiscard]] static UITransform Rotation(float32 radians);

    /// *first*, then this.
    [[nodiscard]] constexpr UITransform Then(const UITransform &second) const
    {
        return {M11 * second.M11 + M12 * second.M21,           M11 * second.M12 + M12 * second.M22,
                M21 * second.M11 + M22 * second.M21,           M21 * second.M12 + M22 * second.M22,
                Dx * second.M11 + Dy * second.M21 + second.Dx, Dx * second.M12 + Dy * second.M22 + second.Dy};
    }

    [[nodiscard]] constexpr Vector2 Apply(Vector2 point) const
    {
        return {M11 * point.X + M21 * point.Y + Dx, M12 * point.X + M22 * point.Y + Dy};
    }

    [[nodiscard]] constexpr Vector2 ApplyVector(Vector2 vector) const
    {
        return {M11 * vector.X + M21 * vector.Y, M12 * vector.X + M22 * vector.Y};
    }

    [[nodiscard]] constexpr float32 GetDeterminant() const
    {
        return M11 * M22 - M12 * M21;
    }

    /// The inverse; the identity for a singular transform.
    [[nodiscard]] UITransform Inverse() const;

    /// True if axes stay axes with no flip: a scale and translation only.
    [[nodiscard]] constexpr bool IsAxisAligned() const
    {
        return M12 == 0 && M21 == 0 && M11 > 0 && M22 > 0;
    }

    /// The bounds of *rect* once transformed.
    [[nodiscard]] UIRect TransformBounds(const UIRect &rect) const;

    constexpr bool operator==(const UITransform &) const = default;
};
} // namespace Marlin
