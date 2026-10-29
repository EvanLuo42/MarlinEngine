#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "UIPainter/UIGeometry.h"
#include "UIPainter/UIPath.gen.h"

namespace Marlin
{
MENUM()
enum class UIFillRule : uint8
{
    NonZero,
    EvenOdd,
};

MENUM()
enum class UILineJoin : uint8
{
    /// Sharp corners, beveled past the miter limit.
    Miter,
    Round,
    Bevel,
};

MENUM()
enum class UILineCap : uint8
{
    Butt,
    Round,
    /// Extends past the end by half the width.
    Square,
};

struct UIStrokeStyle
{
    float32 Width = 1;
    UILineJoin Join = UILineJoin::Miter;
    UILineCap Cap = UILineCap::Butt;
    /// The longest miter, as a multiple of the width, before a Miter join bevels instead (SVG's stroke-miterlimit).
    float32 MiterLimit = 4;
};

/// A path of contours made of lines and quadratic curves, which is what the tile rasterizer evaluates (09 §2): cubic
/// curves and arcs are converted into quadratics as they are added, to within a quarter of a unit of the
/// painter's units. Build it once and draw it many times; it does not reference the painter.
class UIPAINTER_API UIPath
{
public:
    enum class Verb : uint8
    {
        Move,
        Line,
        Quad,
        Close,
    };

    void MoveTo(Vector2 point);
    void LineTo(Vector2 point);
    void QuadTo(Vector2 control, Vector2 point);
    void CubicTo(Vector2 control1, Vector2 control2, Vector2 point);
    /// An arc of the ellipse around *center* with *radii*, from *startAngle* sweeping *sweepAngle* radians (clockwise
    /// on screen when positive), joined to the current point by a line unless it starts a contour.
    void ArcTo(Vector2 center, Vector2 radii, float32 startAngle, float32 sweepAngle);
    void Close();

    void AddRect(const UIRect &rect);
    void AddRoundedRect(const UIRect &rect, const UICornerRadii &radii);
    void AddEllipse(Vector2 center, Vector2 radii);
    /// Straight segments through *points*, closed when *close*.
    void AddPolyline(Span<const Vector2> points, bool close = false);

    void Clear();

    [[nodiscard]] bool IsEmpty() const
    {
        return _verbs.IsEmpty();
    }

    [[nodiscard]] Span<const Verb> GetVerbs() const
    {
        return _verbs.AsSpan();
    }

    /// One point per Move and Line, two per Quad (control, end), none per Close.
    [[nodiscard]] Span<const Vector2> GetPoints() const
    {
        return _points.AsSpan();
    }

    /// The bounds of the points, control points included.
    [[nodiscard]] UIRect GetBounds() const;

private:
    void EnsureContour();

    List<Verb> _verbs;
    List<Vector2> _points;
    Vector2 _start;
    Vector2 _current;
    bool _open = false;
};
} // namespace Marlin
