#include "Testing/Testing.h"

#include "Math/MathUtility.h"
#include "Templates/Bit.h"
#include "UIPainter/UIOutline.h"
#include "UIPainter/UIPainter.h"

namespace Marlin::Tests
{
namespace
{
bool Near(float32 a, float32 b, float32 tolerance = 1e-4f)
{
    return Math::Abs(a - b) <= tolerance;
}

uint32 AsWord(float32 value)
{
    return BitCast<uint32>(value);
}

const UIGpuCommand &RootCommand(const UICommandList &list, usize index)
{
    return list.Canvases[0].Commands[index];
}
} // namespace

MARLIN_TEST(UIPainter, RecordsRootTables)
{
    UICommandList list;
    UIPainter painter;
    MARLIN_EXPECT(!painter.IsRecording());
    painter.Begin(list, 200, 100, 2);
    MARLIN_EXPECT(painter.IsRecording());
    MARLIN_EXPECT(painter.GetSize() == Vector2(100, 50));
    MARLIN_EXPECT(painter.GetClipBounds() == UIRect(0, 0, 200, 100));
    painter.End();
    MARLIN_EXPECT(!painter.IsRecording());

    // The root canvas, the identity transform and the "no clip" entry always exist.
    MARLIN_REQUIRE(list.Canvases.Count() == 1);
    MARLIN_EXPECT(list.Canvases[0].Bounds == UIRect(0, 0, 200, 100));
    MARLIN_EXPECT(list.Transforms.Count() == 1 && list.Clips.Count() == 1);
    MARLIN_EXPECT(list.GetCommandCount() == 0);

    // Calls outside Begin and End record nothing.
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    MARLIN_EXPECT(list.GetCommandCount() == 0);
}

MARLIN_TEST(UIPainter, FillRoundedRect)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 200, 100, 2);
    painter.FillRoundedRect({10, 10, 20, 8}, UICornerRadii(100), LinearColor(1, 0, 0, 0.5f));
    painter.End();

    MARLIN_REQUIRE(list.GetCommandCount() == 1);
    const UIGpuCommand &command = RootCommand(list, 0);
    MARLIN_EXPECT(command.GetKind() == UICommandKind::RoundedRect);
    MARLIN_EXPECT(command.GetFlags() == UICommandFlags::None);
    MARLIN_EXPECT(command.Clip == 0 && command.Group == 0);
    // Local data stays in painter units; radii clamp to half the shorter side; 0 width fills.
    MARLIN_EXPECT(command.Data[0] == 10 && command.Data[1] == 10 && command.Data[2] == 30 && command.Data[3] == 18);
    MARLIN_EXPECT(command.Data[4] == 4 && command.Data[7] == 4 && command.Data[8] == 0);
    // Bounds are in device pixels (scale 2) with the antialiasing margin.
    MARLIN_EXPECT(Near(command.Bounds[0], 18.5f) && Near(command.Bounds[1], 18.5f));
    MARLIN_EXPECT(Near(command.Bounds[2], 61.5f) && Near(command.Bounds[3], 37.5f));
    // The brush is premultiplied.
    MARLIN_REQUIRE(command.Brush < list.Brushes.Count());
    const UIGpuBrush &brush = list.Brushes[command.Brush];
    MARLIN_EXPECT(brush.Kind == static_cast<uint32>(UIGpuBrushKind::Solid));
    MARLIN_EXPECT(Near(brush.Color[0], 0.5f) && Near(brush.Color[3], 0.5f));
    // The device-to-local transform undoes the scale.
    const UIGpuTransform &transform = list.Transforms[command.Transform];
    MARLIN_EXPECT(Near(transform.M11, 0.5f) && Near(transform.M22, 0.5f) && Near(transform.PixelSize, 0.5f));
}

MARLIN_TEST(UIPainter, ReusesRepeatedSolidBrushes)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    painter.FillRect({20, 0, 10, 10}, LinearColor::White);
    painter.FillRect({40, 0, 10, 10}, LinearColor(0, 0, 1, 1));
    painter.End();
    MARLIN_REQUIRE(list.GetCommandCount() == 3);
    MARLIN_EXPECT(list.Brushes.Count() == 2);
    MARLIN_EXPECT(RootCommand(list, 0).Brush == RootCommand(list, 1).Brush);
    MARLIN_EXPECT(RootCommand(list, 1).Brush != RootCommand(list, 2).Brush);
}

MARLIN_TEST(UIPainter, GradientBrushes)
{
    const UIGradientStop stops[] = {{0, LinearColor::White}, {1, LinearColor(0, 0, 0, 0)}};
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.FillRect({0, 0, 50, 50}, UIBrush::LinearGradient({0, 0}, {50, 0}, stops));
    painter.FillRect({0, 0, 50, 50}, UIBrush::RadialGradient({25, 25}, 25, {}));
    painter.End();
    MARLIN_REQUIRE(list.Brushes.Count() == 2 && list.Stops.Count() == 2);
    const UIGpuBrush &linear = list.Brushes[RootCommand(list, 0).Brush];
    MARLIN_EXPECT(linear.Kind == static_cast<uint32>(UIGpuBrushKind::Linear));
    MARLIN_EXPECT(linear.StopOffset == 0 && linear.StopCount == 2);
    MARLIN_EXPECT(linear.Geometry[2] == 50);
    MARLIN_EXPECT(list.Stops[1].Offset == 1 && list.Stops[1].Color[3] == 0);
    // A gradient without stops falls back to its solid color.
    MARLIN_EXPECT(list.Brushes[RootCommand(list, 1).Brush].Kind == static_cast<uint32>(UIGpuBrushKind::Solid));
}

MARLIN_TEST(UIPainter, ClipsCullAndChain)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.PushClip({10, 10, 30, 30});
    MARLIN_EXPECT(painter.GetClipBounds() == UIRect(10, 10, 30, 30));
    painter.FillRect({60, 60, 10, 10}, LinearColor::White);
    painter.FillRect({0, 0, 20, 20}, LinearColor::White);
    painter.PushClip({20, 20, 10, 10}, UICornerRadii(2));
    painter.FillRect({0, 0, 100, 100}, LinearColor::White);
    painter.PopClip();
    painter.PopClip();
    painter.FillRect({60, 60, 10, 10}, LinearColor::White);
    painter.End();

    // The rectangle outside the clip records nothing.
    MARLIN_REQUIRE(list.GetCommandCount() == 3);
    MARLIN_REQUIRE(list.Clips.Count() == 3);
    const UIGpuCommand &clipped = RootCommand(list, 0);
    MARLIN_EXPECT(clipped.Clip == 1);
    MARLIN_EXPECT(clipped.Bounds[0] == 10 && clipped.Bounds[1] == 10);
    MARLIN_EXPECT(Near(clipped.Bounds[2], 21.5f) && Near(clipped.Bounds[3], 21.5f));
    // An axis-aligned rectangle needs no per-pixel test; a rounded one does, and chains to the nearest shaped clip.
    MARLIN_EXPECT(list.Clips[1].Shaped == 0 && list.Clips[2].Shaped == 1 && list.Clips[2].Parent == 0);
    MARLIN_EXPECT(RootCommand(list, 1).Clip == 2);
    MARLIN_EXPECT(RootCommand(list, 1).Bounds[2] == 30);
    MARLIN_EXPECT(RootCommand(list, 2).Clip == 0);
}

MARLIN_TEST(UIPainter, TransformsAreSharedUntilChanged)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    painter.PushTransform(UITransform::Translation(20, 30));
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    painter.FillRect({10, 0, 10, 10}, LinearColor::White);
    painter.PopTransform();
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    painter.End();

    MARLIN_REQUIRE(list.GetCommandCount() == 4);
    MARLIN_EXPECT(list.Transforms.Count() == 3);
    MARLIN_EXPECT(RootCommand(list, 1).Transform == RootCommand(list, 2).Transform);
    MARLIN_EXPECT(RootCommand(list, 0).Transform == RootCommand(list, 3).Transform);
    MARLIN_EXPECT(RootCommand(list, 0).Transform != RootCommand(list, 1).Transform);
    const UIGpuTransform &moved = list.Transforms[RootCommand(list, 1).Transform];
    MARLIN_EXPECT(moved.Dx == -20 && moved.Dy == -30);
    MARLIN_EXPECT(Near(RootCommand(list, 1).Bounds[0], 18.5f) && Near(RootCommand(list, 1).Bounds[1], 28.5f));
}

MARLIN_TEST(UIPainter, StrokePiecesShareAGroup)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.FillRect({0, 0, 1, 1}, LinearColor::White);
    UIPath path;
    const Vector2 points[] = {{10, 10}, {50, 10}, {50, 50}};
    path.AddPolyline(points);
    UIStrokeStyle style;
    style.Width = 4;
    style.Join = UILineJoin::Round;
    painter.StrokePath(path, style, LinearColor::White);
    painter.End();

    // Two straight pieces and a round join, grouped behind the first piece.
    MARLIN_REQUIRE(list.GetCommandCount() == 4);
    MARLIN_EXPECT(RootCommand(list, 1).GetKind() == UICommandKind::StrokeLine);
    MARLIN_EXPECT(RootCommand(list, 2).GetKind() == UICommandKind::StrokeLine);
    MARLIN_EXPECT(RootCommand(list, 3).GetKind() == UICommandKind::Disk);
    for (usize index = 1; index < 4; ++index)
    {
        MARLIN_EXPECT(RootCommand(list, index).Group == 1);
    }
    MARLIN_EXPECT(RootCommand(list, 1).Data[4] == 2);
    // Butt caps leave the outer ends free; the ends at the join meet another piece.
    MARLIN_EXPECT(AsWord(RootCommand(list, 1).Data[5]) == 2u);
    MARLIN_EXPECT(AsWord(RootCommand(list, 2).Data[5]) == 1u);
}

MARLIN_TEST(UIPainter, FillPathWritesAnOutlineRecord)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    UIPath path;
    path.AddRect({10, 20, 30, 40});
    painter.FillPath(path, LinearColor::White, UIFillRule::EvenOdd);
    painter.End();

    MARLIN_REQUIRE(list.GetCommandCount() == 1);
    const UIGpuCommand &command = RootCommand(list, 0);
    MARLIN_EXPECT(command.GetKind() == UICommandKind::Outline);
    MARLIN_EXPECT(command.GetFlags() == UICommandFlags::EvenOdd);
    MARLIN_EXPECT(command.Data[3] == 1);
    const uint32 record = AsWord(command.Data[0]);
    MARLIN_REQUIRE(record == 0 && list.OutlineWords.Count() > 8);
    MARLIN_EXPECT(list.Curves.Count() == 4);
    MARLIN_EXPECT(list.OutlineWords[0] == 0 && list.OutlineWords[1] == 4);
    MARLIN_EXPECT(BitCast<float32>(list.OutlineWords[4]) == 10 && BitCast<float32>(list.OutlineWords[7]) == 60);
}

MARLIN_TEST(UIPainter, OutlineCurvesAreMonotonic)
{
    UIPath path;
    path.MoveTo({0, 0});
    path.QuadTo({50, 100}, {100, 0});
    path.QuadTo({120, -20}, {0, 0});
    List<UIGpuCurve> curves;
    List<uint32> words;
    MARLIN_REQUIRE(AppendOutline(path, curves, words) == 0);
    MARLIN_EXPECT(curves.Count() >= 3);
    for (const UIGpuCurve &curve : curves)
    {
        for (uint32 axis = 0; axis < 2; ++axis)
        {
            const float32 a = curve.Points[axis];
            const float32 b = curve.Points[2 + axis];
            const float32 c = curve.Points[4 + axis];
            MARLIN_EXPECT(Min(a, c) - 1e-3f <= b && b <= Max(a, c) + 1e-3f);
        }
    }
    // Every band range points into the words that follow the header.
    const UIGpuOutlineHeader &header = *reinterpret_cast<const UIGpuOutlineHeader *>(words.Data());
    for (uint32 band = 0; band < header.BandsX + header.BandsY; ++band)
    {
        const uint32 first = words[8 + band * 2];
        const uint32 count = words[8 + band * 2 + 1];
        MARLIN_EXPECT(first + count <= words.Count());
    }

    UIPath empty;
    empty.MoveTo({1, 1});
    MARLIN_EXPECT(AppendOutline(empty, curves, words) == ~0u);
}

MARLIN_TEST(UIPainter, LayersRecordCanvases)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.PushLayer(UILayer{0.5f, 0, 2});
    painter.FillRect({10, 10, 20, 20}, LinearColor::White);
    painter.PopLayer();
    painter.End();

    MARLIN_REQUIRE(list.Canvases.Count() == 2);
    const UICanvas &layer = list.Canvases[1];
    MARLIN_EXPECT(layer.Parent == 0 && layer.Opacity == 0.5f && layer.BackdropBlur == 2);
    MARLIN_EXPECT(layer.Commands.Count() == 1);
    // The parent gets a backdrop and a canvas command, patched with the layer's whole-pixel bounds on pop.
    MARLIN_REQUIRE(list.Canvases[0].Commands.Count() == 2);
    const UIGpuCommand &backdrop = RootCommand(list, 0);
    const UIGpuCommand &canvas = RootCommand(list, 1);
    MARLIN_EXPECT(backdrop.GetKind() == UICommandKind::Backdrop && canvas.GetKind() == UICommandKind::Canvas);
    MARLIN_EXPECT(AsWord(canvas.Data[4]) == 1 && AsWord(backdrop.Data[4]) == 1);
    MARLIN_EXPECT(layer.Bounds == UIRect(8, 8, 24, 24));
    MARLIN_EXPECT(canvas.Bounds[0] == 8 && canvas.Bounds[2] == 32);
}

MARLIN_TEST(UIPainter, DrawTextAndImage)
{
    UIShapedRun run;
    run.Face = {7};
    run.Size = 16;
    run.Glyphs.Add({3, {0, 0}, {1, -10, 8, 10}});
    run.Glyphs.Add({4, {9, 0}, {}});
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.DrawText(run, {10, 50}, LinearColor::White);
    UIImageDraw draw;
    draw.Sampling = UISampling::Point;
    painter.DrawImage(UIImage::FromFrameSlot(5), {0, 0, 10, 10}, draw);
    painter.DrawImage(UIImage(), {0, 0, 10, 10});
    painter.End();

    // Glyphs without ink are skipped; an invalid image draws nothing.
    MARLIN_REQUIRE(list.GetCommandCount() == 2);
    const UIGpuCommand &glyph = RootCommand(list, 0);
    MARLIN_EXPECT(glyph.GetKind() == UICommandKind::Glyph);
    MARLIN_EXPECT(AsWord(glyph.Data[0]) == 7 && AsWord(glyph.Data[1]) == 3);
    MARLIN_EXPECT(glyph.Data[2] == 10 && glyph.Data[3] == 50 && glyph.Data[4] == 16);
    const UIGpuCommand &image = RootCommand(list, 1);
    MARLIN_EXPECT(image.GetKind() == UICommandKind::Image);
    MARLIN_EXPECT(image.GetFlags() == (UICommandFlags::FrameSlot | UICommandFlags::PointSampling));
    MARLIN_EXPECT(AsWord(image.Data[9]) == 5);
}

MARLIN_TEST(UIPainter, AppendMovesIndices)
{
    UICommandList first;
    UICommandList second;
    UIPainter painter;
    painter.Begin(first, 100, 100, 1);
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    UIPath path;
    path.AddRect({0, 0, 5, 5});
    painter.FillPath(path, LinearColor::White);
    painter.End();
    painter.Begin(second, 100, 100, 1);
    painter.PushClip({10, 10, 50, 50});
    painter.PushTransform(UITransform::Translation(5, 5));
    painter.FillPath(path, LinearColor(1, 0, 0, 1));
    painter.PopTransform();
    painter.PopClip();
    painter.PushLayer(0.5f);
    painter.FillRect({20, 20, 10, 10}, LinearColor::White);
    painter.PopLayer();
    painter.End();

    const usize firstWords = first.OutlineWords.Count();
    const usize firstCurves = first.Curves.Count();
    first.Append(second);
    MARLIN_REQUIRE(first.Canvases.Count() == 2);
    MARLIN_REQUIRE(first.Canvases[0].Commands.Count() == 4);
    const UIGpuCommand &outline = RootCommand(first, 2);
    MARLIN_EXPECT(outline.GetKind() == UICommandKind::Outline);
    MARLIN_EXPECT(AsWord(outline.Data[0]) == firstWords);
    MARLIN_EXPECT(first.OutlineWords[firstWords] == firstCurves);
    MARLIN_EXPECT(outline.Clip == 1 && first.Clips.Count() == 2);
    MARLIN_EXPECT(outline.Transform == 2 && first.Transforms[2].Dx == -5);
    MARLIN_EXPECT(first.Brushes[outline.Brush].Color[1] == 0);
    MARLIN_EXPECT(outline.Group == 2);
    const UIGpuCommand &canvas = RootCommand(first, 3);
    MARLIN_EXPECT(canvas.GetKind() == UICommandKind::Canvas && AsWord(canvas.Data[4]) == 1);
    MARLIN_EXPECT(first.Canvases[1].Parent == 0 && first.Canvases[1].Commands.Count() == 1);
}
MARLIN_TEST(UIPainter, EncodedOutlinesDrawLikePaths)
{
    UIPath path;
    path.AddRect({4, 6, 12, 8});
    UIEncodedOutline outline;
    MARLIN_REQUIRE(outline.Encode(path));

    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 200, 100, 1);
    painter.FillPath(path, LinearColor::White);
    // The same outline, twice its size, with its origin at (50, 20).
    painter.FillOutline(outline, {50, 20}, 2, LinearColor::White);
    // Wholly outside the target: nothing is appended.
    painter.FillOutline(outline, {500, 20}, 1, LinearColor::White);
    painter.End();

    MARLIN_REQUIRE(list.GetCommandCount() == 2);
    const UIGpuCommand &first = RootCommand(list, 0);
    const UIGpuCommand &second = RootCommand(list, 1);
    MARLIN_EXPECT(second.GetKind() == UICommandKind::Outline);
    MARLIN_EXPECT(Near(second.Data[1], 50) && Near(second.Data[2], 20) && Near(second.Data[3], 2));
    MARLIN_EXPECT(Near(second.Bounds[0], 50 + 4 * 2 - 1.5f) && Near(second.Bounds[3], 20 + 14 * 2 + 1.5f));
    // The copy's record points at its own curves and band lists, which hold what the path's record holds.
    const uint32 a = AsWord(first.Data[0]);
    const uint32 b = AsWord(second.Data[0]);
    MARLIN_REQUIRE(list.Curves.Count() == outline.Curves.Count() * 2);
    MARLIN_EXPECT(list.OutlineWords[b] == list.OutlineWords[a] + outline.Curves.Count());
    bool same = true;
    const usize words = outline.Words.Count();
    for (usize index = 1; index < words; ++index)
    {
        const bool range =
            index >= 8 && index < 8 + (list.OutlineWords[a + 2] + list.OutlineWords[a + 3]) * 2 && (index - 8) % 2 == 0;
        same = same && list.OutlineWords[b + index] == list.OutlineWords[a + index] + (range ? b - a : 0);
    }
    MARLIN_EXPECT(same);
}

MARLIN_TEST(UIPainter, DrawLineMatchesStrokePath)
{
    UIStrokeStyle style;
    style.Width = 3;
    style.Cap = UILineCap::Round;
    UIPath path;
    path.MoveTo({10, 10});
    path.LineTo({40, 30});

    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.StrokePath(path, style, LinearColor::White);
    painter.DrawLine({10, 10}, {40, 30}, style, LinearColor::White);
    // A point with round caps is a dot.
    painter.DrawLine({60, 60}, {60, 60}, style, LinearColor::White);
    painter.End();

    const UICanvas &root = list.Canvases[0];
    MARLIN_REQUIRE(root.Commands.Count() == 7);
    bool same = true;
    for (usize index = 0; index < 3; ++index)
    {
        const UIGpuCommand &a = root.Commands[index];
        const UIGpuCommand &b = root.Commands[index + 3];
        same = same && a.Header == b.Header && b.Group == 3;
        for (uint32 data = 0; data < UIGpuCommand::DataCount; ++data)
        {
            same = same && AsWord(a.Data[data]) == AsWord(b.Data[data]);
        }
    }
    MARLIN_EXPECT(same);
    MARLIN_EXPECT(root.Commands[6].GetKind() == UICommandKind::Disk && Near(root.Commands[6].Data[2], 1.5f));
}

MARLIN_TEST(UIPainter, ClippedOutAndReset)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    painter.PushClip({10, 10, 20, 20});
    MARLIN_EXPECT(!painter.IsClippedOut({25, 25, 30, 30}));
    MARLIN_EXPECT(painter.IsClippedOut({40, 40, 10, 10}));
    painter.PopClip();
    painter.PushLayer(0.5f);
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    painter.PopLayer();
    painter.End();
    MARLIN_REQUIRE(list.Canvases.Count() == 2 && list.GetCommandCount() == 2);

    // The next recording starts from an empty list.
    painter.Begin(list, 100, 100, 1);
    MARLIN_EXPECT(list.Canvases.Count() == 1 && list.GetCommandCount() == 0);
    painter.FillRect({0, 0, 10, 10}, LinearColor::White);
    painter.End();
    MARLIN_EXPECT(list.GetCommandCount() == 1);
    MARLIN_EXPECT(painter.IsClippedOut({0, 0, 10, 10}));
}
} // namespace Marlin::Tests
