#pragma once

#include "Asset/Asset.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "UIPainter/UIGeometry.h"
#include "UIPainter/UIOutline.h"
#include "UIPainter/UIPath.h"
#include "UIIcon/Icon.gen.h"

namespace Marlin
{
class UIPainter;

/// A step of an icon path; each takes the points after the current point: MoveTo and LineTo one, QuadTo two
/// (control, end), CubicTo three (two controls, end), Close none.
MENUM()
enum class IconPathVerb : uint8
{
    MoveTo,
    LineTo,
    QuadTo,
    CubicTo,
    Close,
};

/// Whether a shape fills its path or strokes it.
MENUM()
enum class IconShapeKind : uint8
{
    Fill,
    Stroke,
};

/// One painted path of an icon, in the order the SVG paints them. Its path is VerbCount verbs from FirstVerb and
/// PointCount points from FirstPoint in the icon's arrays.
MSTRUCT()
struct IconShape
{
    MGENERATED_BODY()

    MPROPERTY()
    IconShapeKind Kind = IconShapeKind::Fill;
    /// SVG's currentColor: the color DrawIcon is given, multiplied by Color (white with the shape's opacity).
    /// Otherwise Color is the shape's own straight linear color.
    MPROPERTY()
    bool CurrentColor = true;
    MPROPERTY()
    LinearColor Color = LinearColor::White;
    /// For fills.
    MPROPERTY()
    UIFillRule FillRule = UIFillRule::NonZero;
    /// For strokes, in view box units.
    MPROPERTY()
    float32 StrokeWidth = 1;
    MPROPERTY()
    UILineCap Cap = UILineCap::Butt;
    MPROPERTY()
    UILineJoin Join = UILineJoin::Miter;
    MPROPERTY()
    float32 MiterLimit = 4;
    MPROPERTY()
    uint32 FirstVerb = 0;
    MPROPERTY()
    uint32 VerbCount = 0;
    MPROPERTY()
    uint32 FirstPoint = 0;
    MPROPERTY()
    uint32 PointCount = 0;
};

/// A vector icon (Documentation/Architecture/09 §2) baked from an SVG by IconConverter: the view box's size and the
/// shapes ready for UIPainter, with the SVG's transforms, arcs and basic shapes already turned into paths. Points are
/// in view box units, relative to its top-left corner, Y down.
MCLASS()
class UIICON_API Icon final : public Asset
{
    MGENERATED_BODY()
public:
    /// The view box's width and height.
    MPROPERTY()
    Vector2 Size;
    MPROPERTY()
    FrozenArray<IconShape> Shapes;
    MPROPERTY()
    FrozenArray<IconPathVerb> Verbs;
    MPROPERTY()
    FrozenArray<Vector2> Points;

    Icon() = default;
    ~Icon() override = default;
    MARLIN_NONCOPYABLE(Icon);

    /// Whether every shape's verbs and points lie in the arrays and the verbs take exactly the shape's points.
    [[nodiscard]] bool IsValid() const;
};

/// The points *verb* takes.
[[nodiscard]] constexpr uint32 GetIconVerbPointCount(IconPathVerb verb)
{
    switch (verb)
    {
    case IconPathVerb::MoveTo:
    case IconPathVerb::LineTo:
        return 1;
    case IconPathVerb::QuadTo:
        return 2;
    case IconPathVerb::CubicTo:
        return 3;
    case IconPathVerb::Close:
        return 0;
    }
    return 0;
}

/// Draws *icon* fitted into *rect* (uniform scale, centred). currentColor becomes *color*; *strokeWidth* > 0 replaces
/// every authored stroke width, in painter units (Lucide's "absolute stroke width"); 0 keeps the authored widths
/// scaled.
UIICON_API void DrawIcon(UIPainter &painter, const Icon &icon, const UIRect &rect, const LinearColor &color,
                         float32 strokeWidth = 0);

/// What DrawIcon builds of an icon for one scale and stroke width: its fills encoded and its strokes split into
/// contours, relative to where the icon is placed. Drawing with the same cache again at that size only copies them;
/// another icon, scale or stroke width rebuilds it. Keep one where an icon is drawn every frame.
class UIICON_API IconDrawCache
{
public:
    /// DrawIcon, through the cache.
    void Draw(UIPainter &painter, const Icon &icon, const UIRect &rect, const LinearColor &color,
              float32 strokeWidth = 0);

private:
    struct Shape
    {
        uint32 Index = 0;
        UIEncodedOutline Outline;
        List<UIPathContour> Contours;
        UIStrokeStyle Style;
    };

    void Build(const Icon &icon, float32 scale, float32 strokeWidth);

    const Icon *_icon = nullptr;
    /// The arrays it was built from, so that an icon reloaded in place rebuilds it.
    const IconShape *_iconShapes = nullptr;
    const Vector2 *_iconPoints = nullptr;
    float32 _scale = 0;
    float32 _strokeWidth = 0;
    List<Shape> _shapes;
};
} // namespace Marlin
