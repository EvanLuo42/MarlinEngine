#include "Testing/Testing.h"

#include "Asset/AssetBlock.h"
#include "Math/MathUtility.h"
#include "Templates/Bit.h"
#include "UIIcon/Icon.h"
#include "UIPainter/UIPainter.h"

namespace Marlin::Tests
{
namespace
{
bool Near(float32 a, float32 b, float32 tolerance = 1e-4f)
{
    return Math::Abs(a - b) <= tolerance;
}

/// A 24 × 24 icon: a horizontal stroke in currentColor from (5, 12) to (19, 12), then a red 4 × 4 square filled at
/// (2, 2).
void MakeIcon(Icon &icon)
{
    icon.Size = {24, 24};
    const IconPathVerb verbs[] = {IconPathVerb::MoveTo, IconPathVerb::LineTo, IconPathVerb::MoveTo,
                                  IconPathVerb::LineTo, IconPathVerb::LineTo, IconPathVerb::LineTo,
                                  IconPathVerb::Close};
    const Vector2 points[] = {{5, 12}, {19, 12}, {2, 2}, {6, 2}, {6, 6}, {2, 6}};
    icon.Verbs = FrozenArray<IconPathVerb>(Span<const IconPathVerb>(verbs));
    icon.Points = FrozenArray<Vector2>(Span<const Vector2>(points));
    IconShape shapes[2];
    shapes[0].Kind = IconShapeKind::Stroke;
    shapes[0].StrokeWidth = 2;
    shapes[0].Cap = UILineCap::Butt;
    shapes[0].VerbCount = 2;
    shapes[0].PointCount = 2;
    shapes[1].CurrentColor = false;
    shapes[1].Color = LinearColor(1, 0, 0, 1);
    shapes[1].FirstVerb = 2;
    shapes[1].VerbCount = 5;
    shapes[1].FirstPoint = 2;
    shapes[1].PointCount = 4;
    icon.Shapes = FrozenArray<IconShape>(Span<const IconShape>(shapes));
}

const UIGpuCommand &RootCommand(const UICommandList &list, usize index)
{
    return list.Canvases[0].Commands[index];
}
} // namespace

MARLIN_TEST(UIIcon, IconIsImageSafe)
{
    MARLIN_EXPECT(AssetBlock::CheckImageSafe(TypeOf<Icon>()));
    Icon icon;
    MakeIcon(icon);
    MARLIN_EXPECT(icon.IsValid());
    List<uint8> block;
    MARLIN_EXPECT(AssetBlock::Write(icon, Guid{1, 2, 3, 4}, block) && !block.IsEmpty());

    // A shape whose verbs take more points than it has is invalid.
    Icon broken;
    MakeIcon(broken);
    IconShape shape = broken.Shapes[0];
    shape.PointCount = 1;
    broken.Shapes = FrozenArray<IconShape>(Span<const IconShape>(&shape, 1));
    MARLIN_EXPECT(!broken.IsValid());
}

MARLIN_TEST(UIIcon, DrawFitsAndScales)
{
    Icon icon;
    MakeIcon(icon);

    // A 48 × 96 rectangle: scale 2, centred vertically, so the icon spans y 24 to 72.
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    DrawIcon(painter, icon, {0, 0, 48, 96}, LinearColor(0, 1, 0, 0.5f));
    painter.End();

    MARLIN_REQUIRE(list.GetCommandCount() == 2);
    const UIGpuCommand &line = RootCommand(list, 0);
    MARLIN_EXPECT(line.GetKind() == UICommandKind::StrokeLine);
    MARLIN_EXPECT(Near(line.Data[0], 10) && Near(line.Data[1], 48) && Near(line.Data[2], 38) && Near(line.Data[3], 48));
    // The authored width scales with the icon: 2 × 2, half of it stored.
    MARLIN_EXPECT(Near(line.Data[4], 2));
    // currentColor takes the given color (premultiplied in the brush).
    const UIGpuBrush &brush = list.Brushes[line.Brush];
    MARLIN_EXPECT(Near(brush.Color[0], 0) && Near(brush.Color[1], 0.5f) && Near(brush.Color[3], 0.5f));

    const UIGpuCommand &square = RootCommand(list, 1);
    MARLIN_EXPECT(square.GetKind() == UICommandKind::Outline);
    const UIGpuBrush &red = list.Brushes[square.Brush];
    MARLIN_EXPECT(Near(red.Color[0], 1) && Near(red.Color[1], 0) && Near(red.Color[3], 1));
}

MARLIN_TEST(UIIcon, AbsoluteStrokeWidth)
{
    Icon icon;
    MakeIcon(icon);
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    DrawIcon(painter, icon, {0, 0, 96, 96}, LinearColor::White, 1.5f);
    // Nothing is drawn into an empty rectangle.
    DrawIcon(painter, icon, {0, 0, 0, 10}, LinearColor::White);
    painter.End();

    MARLIN_REQUIRE(list.GetCommandCount() == 2);
    MARLIN_EXPECT(RootCommand(list, 0).GetKind() == UICommandKind::StrokeLine);
    MARLIN_EXPECT(Near(RootCommand(list, 0).Data[4], 0.75f));
}
MARLIN_TEST(UIIcon, CachedDrawsMatchUncached)
{
    Icon icon;
    MakeIcon(icon);
    UICommandList uncached;
    UICommandList cached;
    UIPainter painter;
    painter.Begin(uncached, 100, 100, 1);
    DrawIcon(painter, icon, {0, 0, 48, 96}, LinearColor::White);
    DrawIcon(painter, icon, {50, 0, 24, 24}, LinearColor::White);
    painter.End();

    // One cache drawn at two places of one size and then at another size, which rebuilds it.
    IconDrawCache cache;
    painter.Begin(cached, 100, 100, 1);
    cache.Draw(painter, icon, {0, 0, 48, 96}, LinearColor::White);
    painter.End();
    painter.Begin(cached, 100, 100, 1);
    cache.Draw(painter, icon, {0, 0, 48, 96}, LinearColor::White);
    cache.Draw(painter, icon, {50, 0, 24, 24}, LinearColor::White);
    painter.End();

    MARLIN_REQUIRE(cached.GetCommandCount() == uncached.GetCommandCount() && cached.GetCommandCount() == 4);
    bool same = true;
    for (usize index = 0; index < 4; ++index)
    {
        const UIGpuCommand &a = RootCommand(uncached, index);
        const UIGpuCommand &b = RootCommand(cached, index);
        same = same && a.Header == b.Header;
        for (uint32 bound = 0; bound < 4; ++bound)
        {
            same = same && Near(a.Bounds[bound], b.Bounds[bound]);
        }
        if (a.GetKind() == UICommandKind::StrokeLine)
        {
            for (uint32 data = 0; data < 5; ++data)
            {
                same = same && Near(a.Data[data], b.Data[data]);
            }
        }
    }
    MARLIN_EXPECT(same);
    MARLIN_EXPECT(cached.Curves.Count() == uncached.Curves.Count());
}
} // namespace Marlin::Tests
