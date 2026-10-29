#include "UIIcon/Icon.h"

#include "Templates/Utility.h"
#include "UIPainter/UIPainter.h"

namespace Marlin
{
namespace
{
bool IsShapeValid(const Icon &icon, const IconShape &shape)
{
    const uint64 verbEnd = static_cast<uint64>(shape.FirstVerb) + shape.VerbCount;
    const uint64 pointEnd = static_cast<uint64>(shape.FirstPoint) + shape.PointCount;
    if (verbEnd > icon.Verbs.Count() || pointEnd > icon.Points.Count())
    {
        return false;
    }
    uint64 points = 0;
    for (uint32 v = shape.FirstVerb; v < verbEnd; ++v)
    {
        points += GetIconVerbPointCount(icon.Verbs[v]);
    }
    return points == shape.PointCount;
}
} // namespace

bool Icon::IsValid() const
{
    for (const IconShape &shape : Shapes)
    {
        if (!IsShapeValid(*this, shape))
        {
            return false;
        }
    }
    return true;
}

void DrawIcon(UIPainter &painter, const Icon &icon, const UIRect &rect, const LinearColor &color, float32 strokeWidth)
{
    IconDrawCache cache;
    cache.Draw(painter, icon, rect, color, strokeWidth);
}

void IconDrawCache::Build(const Icon &icon, float32 scale, float32 strokeWidth)
{
    _icon = &icon;
    _iconShapes = icon.Shapes.Data();
    _iconPoints = icon.Points.Data();
    _scale = scale;
    _strokeWidth = strokeWidth;
    _shapes.Clear();
    // The path is built in painter units, so that UIPath turns curves into quadratics to its tolerance whatever the
    // icon's size, and authored stroke widths scale the same as the points. It is relative to the icon's top-left
    // corner, so that only the scale and stroke width key the cache.
    UIPath path;
    for (uint32 index = 0; index < icon.Shapes.Count(); ++index)
    {
        const IconShape &shape = icon.Shapes[index];
        if (!IsShapeValid(icon, shape))
        {
            continue;
        }
        path.Clear();
        const Vector2 *point = icon.Points.Data() + shape.FirstPoint;
        for (uint32 v = shape.FirstVerb; v < shape.FirstVerb + shape.VerbCount; ++v)
        {
            switch (icon.Verbs[v])
            {
            case IconPathVerb::MoveTo:
                path.MoveTo(point[0] * scale);
                break;
            case IconPathVerb::LineTo:
                path.LineTo(point[0] * scale);
                break;
            case IconPathVerb::QuadTo:
                path.QuadTo(point[0] * scale, point[1] * scale);
                break;
            case IconPathVerb::CubicTo:
                path.CubicTo(point[0] * scale, point[1] * scale, point[2] * scale);
                break;
            case IconPathVerb::Close:
                path.Close();
                break;
            }
            point += GetIconVerbPointCount(icon.Verbs[v]);
        }
        if (path.IsEmpty())
        {
            continue;
        }
        Shape &cached = _shapes.AddDefaulted();
        cached.Index = index;
        if (shape.Kind == IconShapeKind::Fill)
        {
            if (!cached.Outline.Encode(path))
            {
                _shapes.Pop();
            }
            continue;
        }
        cached.Style.Width = strokeWidth > 0 ? strokeWidth : shape.StrokeWidth * scale;
        cached.Style.Cap = shape.Cap;
        cached.Style.Join = shape.Join;
        cached.Style.MiterLimit = shape.MiterLimit;
        const usize contours = GetPathContours(path, cached.Contours);
        if (cached.Style.Width <= 0 || contours == 0)
        {
            _shapes.Pop();
            continue;
        }
        cached.Contours.Resize(contours);
    }
}

void IconDrawCache::Draw(UIPainter &painter, const Icon &icon, const UIRect &rect, const LinearColor &color,
                         float32 strokeWidth)
{
    if (rect.IsEmpty() || !(icon.Size.X > 0 && icon.Size.Y > 0) || icon.Shapes.IsEmpty())
    {
        return;
    }
    const float32 scale = Min(rect.Width / icon.Size.X, rect.Height / icon.Size.Y);
    const Vector2 origin = rect.GetCenter() - icon.Size * (scale * 0.5f);
    if (_icon != &icon || _iconShapes != icon.Shapes.Data() || _iconPoints != icon.Points.Data() || _scale != scale ||
        _strokeWidth != strokeWidth)
    {
        Build(icon, scale, strokeWidth);
    }
    for (const Shape &cached : _shapes)
    {
        const IconShape &shape = icon.Shapes[cached.Index];
        const LinearColor paint = shape.CurrentColor ? color * shape.Color : shape.Color;
        if (shape.Kind == IconShapeKind::Fill)
        {
            painter.FillOutline(cached.Outline, origin, 1, paint, shape.FillRule);
        }
        else
        {
            painter.StrokeContours(cached.Contours, origin, cached.Style, paint);
        }
    }
}
} // namespace Marlin
