#include "UIPainter/UIGeometry.h"

namespace Marlin
{
UITransform UITransform::Rotation(float32 radians)
{
    const float32 c = Math::Cos(radians);
    const float32 s = Math::Sin(radians);
    return {c, s, -s, c, 0, 0};
}

UITransform UITransform::Inverse() const
{
    const float32 determinant = GetDeterminant();
    if (Math::Abs(determinant) < 1e-12f)
    {
        return {};
    }
    const float32 inverse = 1 / determinant;
    UITransform result;
    result.M11 = M22 * inverse;
    result.M12 = -M12 * inverse;
    result.M21 = -M21 * inverse;
    result.M22 = M11 * inverse;
    result.Dx = -(Dx * result.M11 + Dy * result.M21);
    result.Dy = -(Dx * result.M12 + Dy * result.M22);
    return result;
}

UIRect UITransform::TransformBounds(const UIRect &rect) const
{
    const Vector2 corners[4] = {Apply(rect.GetMin()), Apply({rect.X + rect.Width, rect.Y}),
                                Apply({rect.X, rect.Y + rect.Height}), Apply(rect.GetMax())};
    Vector2 min = corners[0];
    Vector2 max = corners[0];
    for (const Vector2 &corner : corners)
    {
        min = {Min(min.X, corner.X), Min(min.Y, corner.Y)};
        max = {Max(max.X, corner.X), Max(max.Y, corner.Y)};
    }
    return UIRect::FromCorners(min, max);
}
} // namespace Marlin
