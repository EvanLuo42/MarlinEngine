#include "UIPainter/UIPath.h"

namespace Marlin
{
namespace
{
constexpr float32 Tolerance = 0.25f;

Vector2 Lerp(Vector2 a, Vector2 b, float32 t)
{
    return a + (b - a) * t;
}
} // namespace

void UIPath::EnsureContour()
{
    if (!_open)
    {
        MoveTo(_current);
    }
}

void UIPath::MoveTo(Vector2 point)
{
    if (!_verbs.IsEmpty() && _verbs.Last() == Verb::Move)
    {
        _points.Last() = point;
    }
    else
    {
        _verbs.Add(Verb::Move);
        _points.Add(point);
    }
    _start = point;
    _current = point;
    _open = true;
}

void UIPath::LineTo(Vector2 point)
{
    EnsureContour();
    _verbs.Add(Verb::Line);
    _points.Add(point);
    _current = point;
}

void UIPath::QuadTo(Vector2 control, Vector2 point)
{
    EnsureContour();
    _verbs.Add(Verb::Quad);
    _points.Add(control);
    _points.Add(point);
    _current = point;
}

void UIPath::CubicTo(Vector2 control1, Vector2 control2, Vector2 point)
{
    EnsureContour();
    const Vector2 p0 = _current;
    const Vector2 error = point - control2 * 3 + control1 * 3 - p0;
    const float32 distance = error.Length() * (Math::Sqrt(3.0f) / 36.0f);
    const auto count =
        static_cast<uint32>(Math::Clamp(Math::Ceil(Math::Pow(distance / Tolerance, 1.0f / 3.0f)), 1.0f, 16.0f));
    for (uint32 index = 0; index < count; ++index)
    {
        const float32 t0 = static_cast<float32>(index) / static_cast<float32>(count);
        const float32 t1 = static_cast<float32>(index + 1) / static_cast<float32>(count);
        const auto at = [&](float32 t) {
            const Vector2 a = Lerp(p0, control1, t);
            const Vector2 b = Lerp(control1, control2, t);
            const Vector2 c = Lerp(control2, point, t);
            return Lerp(Lerp(a, b, t), Lerp(b, c, t), t);
        };
        const auto derivative = [&](float32 t) {
            const float32 u = 1 - t;
            return (control1 - p0) * (3 * u * u) + (control2 - control1) * (6 * u * t) +
                   (point - control2) * (3 * t * t);
        };
        const Vector2 start = at(t0);
        const Vector2 end = at(t1);
        const float32 span = t1 - t0;
        const Vector2 c1 = start + derivative(t0) * (span / 3);
        const Vector2 c2 = end - derivative(t1) * (span / 3);
        QuadTo((c1 * 3 + c2 * 3 - start - end) * 0.25f, end);
    }
}

void UIPath::ArcTo(Vector2 center, Vector2 radii, float32 startAngle, float32 sweepAngle)
{
    const Vector2 start = center + Vector2(Math::Cos(startAngle) * radii.X, Math::Sin(startAngle) * radii.Y);
    if (_open)
    {
        LineTo(start);
    }
    else
    {
        MoveTo(start);
    }
    const auto count =
        static_cast<uint32>(Math::Clamp(Math::Ceil(Math::Abs(sweepAngle) / (Math::Pi / 4)), 1.0f, 64.0f));
    const float32 step = sweepAngle / static_cast<float32>(count);
    const float32 stretch = 1 / Math::Cos(step / 2);
    for (uint32 index = 0; index < count; ++index)
    {
        const float32 middle = startAngle + step * (static_cast<float32>(index) + 0.5f);
        const float32 end = startAngle + step * static_cast<float32>(index + 1);
        QuadTo(center + Vector2(Math::Cos(middle) * radii.X * stretch, Math::Sin(middle) * radii.Y * stretch),
               center + Vector2(Math::Cos(end) * radii.X, Math::Sin(end) * radii.Y));
    }
}

void UIPath::Close()
{
    if (_open)
    {
        _verbs.Add(Verb::Close);
        _current = _start;
        _open = false;
    }
}

void UIPath::AddRect(const UIRect &rect)
{
    MoveTo(rect.GetMin());
    LineTo({rect.X + rect.Width, rect.Y});
    LineTo(rect.GetMax());
    LineTo({rect.X, rect.Y + rect.Height});
    Close();
}

void UIPath::AddRoundedRect(const UIRect &rect, const UICornerRadii &radii)
{
    if (radii.IsZero())
    {
        AddRect(rect);
        return;
    }
    const float32 limit = Min(rect.Width, rect.Height) * 0.5f;
    const float32 tl = Math::Clamp(radii.TopLeft, 0.0f, limit);
    const float32 tr = Math::Clamp(radii.TopRight, 0.0f, limit);
    const float32 br = Math::Clamp(radii.BottomRight, 0.0f, limit);
    const float32 bl = Math::Clamp(radii.BottomLeft, 0.0f, limit);
    const float32 x0 = rect.X;
    const float32 y0 = rect.Y;
    const float32 x1 = rect.X + rect.Width;
    const float32 y1 = rect.Y + rect.Height;
    _open = false;
    MoveTo({x0 + tl, y0});
    LineTo({x1 - tr, y0});
    if (tr > 0)
    {
        ArcTo({x1 - tr, y0 + tr}, Vector2(tr), -Math::HalfPi, Math::HalfPi);
    }
    LineTo({x1, y1 - br});
    if (br > 0)
    {
        ArcTo({x1 - br, y1 - br}, Vector2(br), 0, Math::HalfPi);
    }
    LineTo({x0 + bl, y1});
    if (bl > 0)
    {
        ArcTo({x0 + bl, y1 - bl}, Vector2(bl), Math::HalfPi, Math::HalfPi);
    }
    LineTo({x0, y0 + tl});
    if (tl > 0)
    {
        ArcTo({x0 + tl, y0 + tl}, Vector2(tl), Math::Pi, Math::HalfPi);
    }
    Close();
}

void UIPath::AddEllipse(Vector2 center, Vector2 radii)
{
    _open = false;
    ArcTo(center, radii, 0, Math::TwoPi);
    Close();
}

void UIPath::AddPolyline(Span<const Vector2> points, bool close)
{
    if (points.IsEmpty())
    {
        return;
    }
    MoveTo(points[0]);
    for (usize index = 1; index < points.Count(); ++index)
    {
        LineTo(points[index]);
    }
    if (close)
    {
        Close();
    }
}

void UIPath::Clear()
{
    _verbs.Clear();
    _points.Clear();
    _start = {};
    _current = {};
    _open = false;
}

UIRect UIPath::GetBounds() const
{
    if (_points.IsEmpty())
    {
        return {};
    }
    Vector2 min = _points[0];
    Vector2 max = _points[0];
    for (const Vector2 &point : _points)
    {
        min = {Min(min.X, point.X), Min(min.Y, point.Y)};
        max = {Max(max.X, point.X), Max(max.Y, point.Y)};
    }
    return UIRect::FromCorners(min, max);
}
} // namespace Marlin
