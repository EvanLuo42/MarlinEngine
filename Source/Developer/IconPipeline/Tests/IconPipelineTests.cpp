#include "Testing/Testing.h"

#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "IconPipeline/IconConverter.h"
#include "IconPipeline/SvgIcon.h"
#include "Math/MathUtility.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
bool Near(float32 a, float32 b, float32 tolerance = 1e-3f)
{
    return Math::Abs(a - b) <= tolerance;
}

bool Near(Vector2 a, Vector2 b, float32 tolerance = 1e-3f)
{
    return Near(a.X, b.X, tolerance) && Near(a.Y, b.Y, tolerance);
}

/// Parses *body* inside a 24 × 24 svg; requires success.
void Parse(StringView body, Icon &icon, SvgIconDiagnostics &diagnostics)
{
    const String text = Format("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">{0}</svg>", body);
    MARLIN_REQUIREF(ParseSvgIcon(text.AsView(), icon, diagnostics), "{0}", diagnostics.Error);
    MARLIN_REQUIRE(icon.IsValid());
}

bool VerbsAre(const Icon &icon, const IconShape &shape, std::initializer_list<IconPathVerb> verbs)
{
    if (shape.VerbCount != verbs.size())
    {
        return false;
    }
    uint32 index = shape.FirstVerb;
    for (const IconPathVerb verb : verbs)
    {
        if (icon.Verbs[index++] != verb)
        {
            return false;
        }
    }
    return true;
}

Vector2 PointOf(const Icon &icon, const IconShape &shape, uint32 index)
{
    return icon.Points[shape.FirstPoint + index];
}

using enum IconPathVerb;

constexpr StringView LucideIcons[] = {
    "bug",   "chevron-down", "chevron-left", "chevron-right",    "copy",   "folder-open", "minus",
    "music", "plus",         "settings",     "sliders-vertical", "square", "x",
};

class IconLocationStore final : public IAssetStore
{
public:
    /// *path* holds the converted result, as ConversionStore::ExportResult writes it.
    void Add(const Guid &guid, StringView path, const ConvertedResultLocation &location)
    {
        _guids.Add(guid);
        _paths.Add(String(path));
        _locations.Add(location);
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const usize index = _guids.IndexOf(guid);
        FileRegion file;
        if (index == List<Guid>::NotFound || !Vfs::OpenRegion(_paths[index].AsView(), file))
        {
            done(nullptr);
            return;
        }
        AssetLocation location;
        location.Block = file;
        location.Block.Offset = file.Offset + _locations[index].BlockOffset;
        location.Block.Size = _locations[index].BlockSize;
        done(&location);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "IconTest";
    }

private:
    List<Guid> _guids;
    List<String> _paths;
    List<ConvertedResultLocation> _locations;
};

/// A scratch folder at /Temp/IconPipelineTest mounted as /IconTest, created and deleted through the VFS.
struct IconFixture
{
    MountId Mount = 0;

    IconFixture()
    {
        (void)Vfs::Delete("/Temp/IconPipelineTest");
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/IconPipelineTest"));
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/IconPipelineTest/Content"));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/IconPipelineTest", path) && !path.IsEmpty());
        Mount = Vfs::Mount("/IconTest", MakeUnique<DirectoryMount>(path.AsView(), true), 0);
    }

    ~IconFixture()
    {
        if (Mount != 0)
        {
            Vfs::Unmount(Mount);
        }
        (void)Vfs::Delete("/Temp/IconPipelineTest");
    }

    MARLIN_NONCOPYABLE(IconFixture);
};
} // namespace

MARLIN_TEST(IconPipeline, RelativeCommandsAndRepeats)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    // Relative lines, H and V, a close, then a relative move from the subpath's start with an implicit line after it;
    // the second path packs its numbers without separators.
    Parse("<path d=\"M1 2 l3 4 h1 v1 H0 V0 z m1 1 2 2\"/><path d=\"M.5.5-1-1\"/>", icon, diagnostics);
    MARLIN_EXPECT(diagnostics.Warnings.IsEmpty());
    MARLIN_EXPECT(icon.Size == Vector2(24, 24));
    MARLIN_REQUIRE(icon.Shapes.Count() == 2);
    const IconShape &shape = icon.Shapes[0];
    // SVG's initial fill is black, and there is no stroke.
    MARLIN_EXPECT(shape.Kind == IconShapeKind::Fill && !shape.CurrentColor && shape.Color == LinearColor::Black);
    MARLIN_EXPECT(VerbsAre(icon, shape, {MoveTo, LineTo, LineTo, LineTo, LineTo, LineTo, Close, MoveTo, LineTo}));
    const Vector2 expected[] = {{1, 2}, {4, 6}, {5, 6}, {5, 7}, {0, 7}, {0, 0}, {2, 3}, {4, 5}};
    MARLIN_REQUIRE(shape.PointCount == 8);
    for (uint32 i = 0; i < 8; ++i)
    {
        MARLIN_EXPECTF(PointOf(icon, shape, i) == expected[i], "point {0}", i);
    }
    const IconShape &packed = icon.Shapes[1];
    MARLIN_EXPECT(VerbsAre(icon, packed, {MoveTo, LineTo}));
    MARLIN_EXPECT(PointOf(icon, packed, 0) == Vector2(0.5f, 0.5f) && PointOf(icon, packed, 1) == Vector2(-1, -1));
}

MARLIN_TEST(IconPipeline, ArcsBecomeCubics)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    // A half circle over the top (sweep 1 is clockwise on screen), and one whose radii are too small, so they grow
    // until the arc fits; then a relative arc in Lucide's style and a zero-radius arc, which is a line.
    Parse("<path d=\"M0 12 A12 12 0 0 1 24 12\"/><path d=\"M0 0 A1 1 0 0 1 10 0\"/>"
          "<path d=\"M14 7a4 4 0 0 1 4 4\"/><path d=\"M0 0 A0 5 0 0 1 3 4\"/>",
          icon, diagnostics);
    MARLIN_REQUIRE(icon.Shapes.Count() == 4);
    const IconShape &half = icon.Shapes[0];
    MARLIN_EXPECT(VerbsAre(icon, half, {MoveTo, CubicTo, CubicTo}));
    MARLIN_EXPECT(Near(PointOf(icon, half, 3), {12, 0}));
    MARLIN_EXPECT(PointOf(icon, half, 6) == Vector2(24, 12));
    // The first control point leaves the start straight up, a quarter circle's handle length away.
    MARLIN_EXPECT(Near(PointOf(icon, half, 1), {0, 12 - 12 * 0.5522847f}, 1e-2f));

    const IconShape &grown = icon.Shapes[1];
    MARLIN_EXPECT(VerbsAre(icon, grown, {MoveTo, CubicTo, CubicTo}));
    MARLIN_EXPECT(Near(PointOf(icon, grown, 3), {5, -5}));

    // A quarter turn of radius 4 around (14, 11).
    const IconShape &quarter = icon.Shapes[2];
    MARLIN_EXPECT(VerbsAre(icon, quarter, {MoveTo, CubicTo}));
    MARLIN_EXPECT(Near(PointOf(icon, quarter, 3), {18, 11}));
    MARLIN_EXPECT(Near(PointOf(icon, quarter, 1), {14 + 4 * 0.5522847f, 7}, 1e-2f));

    MARLIN_EXPECT(VerbsAre(icon, icon.Shapes[3], {MoveTo, LineTo}));
}

MARLIN_TEST(IconPipeline, SmoothCurves)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    Parse("<path d=\"M0 0 C0 10 10 10 10 0 S20 -10 20 0\"/><path d=\"M0 0 Q5 10 10 0 T20 0 t10 0\"/>"
          "<path d=\"M0 0 S5 5 10 0\"/>",
          icon, diagnostics);
    MARLIN_REQUIRE(icon.Shapes.Count() == 3);
    const IconShape &cubic = icon.Shapes[0];
    MARLIN_EXPECT(VerbsAre(icon, cubic, {MoveTo, CubicTo, CubicTo}));
    // S reflects the previous second control point about the current point.
    MARLIN_EXPECT(PointOf(icon, cubic, 4) == Vector2(10, -10) && PointOf(icon, cubic, 5) == Vector2(20, -10));

    const IconShape &quad = icon.Shapes[1];
    MARLIN_EXPECT(VerbsAre(icon, quad, {MoveTo, QuadTo, QuadTo, QuadTo}));
    MARLIN_EXPECT(PointOf(icon, quad, 3) == Vector2(15, -10) && PointOf(icon, quad, 4) == Vector2(20, 0));
    MARLIN_EXPECT(PointOf(icon, quad, 5) == Vector2(25, 10) && PointOf(icon, quad, 6) == Vector2(30, 0));

    // Without a cubic before it, S's first control point is the current point.
    MARLIN_EXPECT(PointOf(icon, icon.Shapes[2], 1) == Vector2(0, 0));
}

MARLIN_TEST(IconPipeline, BasicShapes)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    Parse("<circle cx=\"12\" cy=\"12\" r=\"3\"/><rect x=\"3\" y=\"3\" width=\"18\" height=\"10\" rx=\"2\"/>"
          "<rect width=\"4\" height=\"4\"/><line x1=\"1\" y1=\"2\" x2=\"3\" y2=\"4\" stroke=\"#000\"/>"
          "<polyline points=\"1,1 2,2 3,1\" fill=\"none\" stroke=\"#000\"/><polygon points=\"1 1 2 2 3 1\"/>"
          "<ellipse cx=\"5\" cy=\"6\" rx=\"2\" ry=\"1\"/><circle r=\"0\"/>",
          icon, diagnostics);
    MARLIN_EXPECT(diagnostics.Warnings.IsEmpty());
    // The zero-radius circle draws nothing; the line is only stroked.
    MARLIN_REQUIRE(icon.Shapes.Count() == 7);
    const IconShape &circle = icon.Shapes[0];
    MARLIN_EXPECT(VerbsAre(icon, circle, {MoveTo, CubicTo, CubicTo, CubicTo, CubicTo, Close}));
    MARLIN_EXPECT(PointOf(icon, circle, 0) == Vector2(15, 12) && PointOf(icon, circle, 3) == Vector2(12, 15));

    // rx alone sets ry too: four straight sides and four corners.
    const IconShape &rounded = icon.Shapes[1];
    MARLIN_EXPECT(
        VerbsAre(icon, rounded, {MoveTo, LineTo, CubicTo, LineTo, CubicTo, LineTo, CubicTo, LineTo, CubicTo, Close}));
    MARLIN_EXPECT(PointOf(icon, rounded, 0) == Vector2(5, 3) && PointOf(icon, rounded, 4) == Vector2(21, 5));

    MARLIN_EXPECT(VerbsAre(icon, icon.Shapes[2], {MoveTo, LineTo, LineTo, LineTo, Close}));
    MARLIN_EXPECT(icon.Shapes[3].Kind == IconShapeKind::Stroke && VerbsAre(icon, icon.Shapes[3], {MoveTo, LineTo}));
    MARLIN_EXPECT(icon.Shapes[4].Kind == IconShapeKind::Stroke &&
                  VerbsAre(icon, icon.Shapes[4], {MoveTo, LineTo, LineTo}));
    MARLIN_EXPECT(VerbsAre(icon, icon.Shapes[5], {MoveTo, LineTo, LineTo, Close}));
    MARLIN_EXPECT(PointOf(icon, icon.Shapes[6], 0) == Vector2(7, 6));
}

MARLIN_TEST(IconPipeline, ColorsAndInheritance)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    Parse("<g fill=\"none\" stroke=\"currentColor\" stroke-width=\"3\" stroke-linecap=\"round\" "
          "stroke-linejoin=\"bevel\" stroke-opacity=\"0.5\">"
          "<path d=\"M0 0 L1 1\"/><path d=\"M0 0 L1 1\" stroke=\"#f00\" style=\"stroke-width: 1; opacity: 0.5\"/></g>"
          "<path d=\"M0 0 L1 1 L0 1z\" fill=\"#808080\" fill-rule=\"evenodd\"/>"
          "<path d=\"M0 0 L1 1 L0 1z\" fill=\"rgb(255, 0, 0)\"/><path d=\"M0 0 L1 1 L0 1z\" fill=\"White\"/>"
          "<path d=\"M0 0 L1 1 L0 1z\" fill=\"#ff000080\"/>",
          icon, diagnostics);
    MARLIN_EXPECT(diagnostics.Warnings.IsEmpty());
    MARLIN_REQUIRE(icon.Shapes.Count() == 6);
    const IconShape &current = icon.Shapes[0];
    MARLIN_EXPECT(current.Kind == IconShapeKind::Stroke && current.CurrentColor);
    MARLIN_EXPECT(current.StrokeWidth == 3 && current.Cap == UILineCap::Round && current.Join == UILineJoin::Bevel);
    MARLIN_EXPECT(current.Color == LinearColor(1, 1, 1, 0.5f));

    // style wins over the attribute; opacity multiplies stroke-opacity.
    const IconShape &red = icon.Shapes[1];
    MARLIN_EXPECT(!red.CurrentColor && red.StrokeWidth == 1 && red.Cap == UILineCap::Round);
    MARLIN_EXPECT(red.Color.R == 1 && red.Color.G == 0 && Near(red.Color.A, 0.25f));

    // sRGB #808080 is about 0.2159 linear.
    const IconShape &gray = icon.Shapes[2];
    MARLIN_EXPECT(gray.Kind == IconShapeKind::Fill && gray.FillRule == UIFillRule::EvenOdd);
    MARLIN_EXPECT(Near(gray.Color.R, 0.2159f) && Near(gray.Color.B, 0.2159f) && gray.Color.A == 1);
    MARLIN_EXPECT(icon.Shapes[3].Color == LinearColor(1, 0, 0, 1));
    MARLIN_EXPECT(icon.Shapes[4].Color == LinearColor::White);
    MARLIN_EXPECT(Near(icon.Shapes[5].Color.A, 128.0f / 255));
}

MARLIN_TEST(IconPipeline, TransformsAndViewBox)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    const StringView text =
        "<?xml version=\"1.0\"?><!DOCTYPE svg><!-- comment -->"
        "<svg viewBox=\"-12 -12 24 24\" width=\"48\" height=\"48\"><path d=\"M0 0 L1 0\" stroke=\"#000\"/>"
        "<g transform=\"translate(10 0) scale(2)\"><circle cx=\"1\" cy=\"1\" r=\"1\" stroke=\"#000\"/></g>"
        "<path d=\"M1 0 L2 0\" transform=\"rotate(90)\" fill=\"none\" stroke=\"#000\"/></svg>";
    MARLIN_REQUIREF(ParseSvgIcon(text, icon, diagnostics), "{0}", diagnostics.Error);
    MARLIN_EXPECT(diagnostics.Warnings.IsEmpty());
    MARLIN_EXPECT(icon.Size == Vector2(24, 24));
    // The view box's corner is the origin.
    MARLIN_REQUIRE(icon.Shapes.Count() == 5);
    MARLIN_EXPECT(PointOf(icon, icon.Shapes[0], 0) == Vector2(12, 12));
    // translate(10 0) scale(2): the circle's (2, 1) lands on (14, 2), and its stroke doubles.
    const IconShape &stroke = icon.Shapes[3];
    MARLIN_EXPECT(stroke.Kind == IconShapeKind::Stroke && Near(stroke.StrokeWidth, 2));
    MARLIN_EXPECT(Near(PointOf(icon, stroke, 0), {12 + 14, 12 + 2}));
    // rotate(90) turns +x into +y (clockwise on screen).
    MARLIN_EXPECT(Near(PointOf(icon, icon.Shapes[4], 1), {12, 14}));

    // width and height stand in for a missing viewBox.
    MARLIN_REQUIRE(ParseSvgIcon("<svg width=\"16px\" height=\"20\"/>", icon, diagnostics));
    MARLIN_EXPECT(icon.Size == Vector2(16, 20) && icon.Shapes.IsEmpty());
}

MARLIN_TEST(IconPipeline, ErrorsAndWarnings)
{
    Icon icon;
    SvgIconDiagnostics diagnostics;
    MARLIN_EXPECT(!ParseSvgIcon("<html></html>", icon, diagnostics) && !diagnostics.Error.IsEmpty());
    MARLIN_EXPECT(!ParseSvgIcon("not xml at all", icon, diagnostics) && !diagnostics.Error.IsEmpty());
    MARLIN_EXPECT(!ParseSvgIcon("<svg><path d=\"M0 0\"/></svg>", icon, diagnostics));
    MARLIN_EXPECT(!ParseSvgIcon("<svg viewBox=\"0 0 24\"></svg>", icon, diagnostics));
    MARLIN_EXPECT(!ParseSvgIcon("<svg viewBox=\"0 0 24 24\"><path d=\"M0 0 L1 1\"", icon, diagnostics));
    MARLIN_EXPECT(!ParseSvgIcon("", icon, diagnostics));

    // Malformed path data draws up to the error; unsupported content is skipped with a warning each.
    Parse("<path d=\"M0 0 L1 1 L2 x 3 3\"/><text>Hi</text><use href=\"#a\"/>"
          "<defs><linearGradient id=\"g\"/></defs><path d=\"M0 0 L1 1\" fill=\"url(#g)\" stroke=\"#000\" "
          "stroke-dasharray=\"2 2\"/><path d=\"L1 1\"/>",
          icon, diagnostics);
    MARLIN_EXPECT(diagnostics.Warnings.Count() == 5);
    MARLIN_REQUIRE(icon.Shapes.Count() == 2);
    MARLIN_EXPECT(VerbsAre(icon, icon.Shapes[0], {MoveTo, LineTo}));
    // The gradient fill without a fallback paints nothing; the stroke stays.
    MARLIN_EXPECT(icon.Shapes[1].Kind == IconShapeKind::Stroke);

    // Arbitrary bytes never crash.
    const char noise[] = "<svg viewBox='0 0 1 1'><path d='M1e 2.e-+ A 1 1 0 2 1 3 3 Q'/><g><<g/></svg";
    (void)ParseSvgIcon(StringView(noise, sizeof(noise) - 1), icon, diagnostics);
}

MARLIN_TEST(IconPipeline, LucideIcons)
{
    for (const StringView name : LucideIcons)
    {
        List<uint8> bytes;
        const String path = Format("/Engine/Content/Icons/Lucide/{0}.svg", name);
        MARLIN_REQUIREF(Vfs::ReadAll(path.AsView(), bytes), "{0}", path);
        Icon icon;
        SvgIconDiagnostics diagnostics;
        const StringView text(reinterpret_cast<const char *>(bytes.Data()), bytes.Count());
        MARLIN_REQUIREF(ParseSvgIcon(text, icon, diagnostics), "{0}: {1}", name, diagnostics.Error);
        MARLIN_EXPECTF(diagnostics.Warnings.IsEmpty(), "{0}: {1}", name,
                       diagnostics.Warnings.IsEmpty() ? StringView() : diagnostics.Warnings[0].AsView());
        MARLIN_EXPECT(icon.Size == Vector2(24, 24) && !icon.Shapes.IsEmpty() && icon.IsValid());
        for (const IconShape &shape : icon.Shapes)
        {
            MARLIN_EXPECTF(shape.Kind == IconShapeKind::Stroke && shape.CurrentColor && shape.StrokeWidth == 2 &&
                               shape.Cap == UILineCap::Round && shape.Join == UILineJoin::Round,
                           "{0}", name);
            for (uint32 i = 0; i < shape.PointCount; ++i)
            {
                const Vector2 point = PointOf(icon, shape, i);
                MARLIN_EXPECTF(point.X >= -1 && point.X <= 25 && point.Y >= -1 && point.Y <= 25, "{0}", name);
            }
        }
    }
}

MARLIN_TEST(IconPipeline, ImportAndConvert)
{
    IconFixture fixture;
    List<uint8> bytes;
    MARLIN_REQUIRE(Vfs::ReadAll("/Engine/Content/Icons/Lucide/x.svg", bytes));
    MARLIN_REQUIRE(Vfs::WriteAll("/IconTest/Content/x.svg",
                                 StringView(reinterpret_cast<const char *>(bytes.Data()), bytes.Count())));
    AssetIndex index;
    List<String> folders;
    folders.Add("/IconTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    AssetFileRecord record;
    MARLIN_REQUIRE(index.GetRecord("/IconTest/Content/x.svg", record) && record.BinaryGuid.IsValid());

    AssetImport importer;
    const ImportResult imported = importer.Import("/IconTest/Content/x.svg", record.BinaryGuid);
    MARLIN_REQUIREF(imported.Succeeded, "{0}", imported.Errors);
    MARLIN_EXPECT(imported.AssetPath == "/IconTest/Content/x.masset" && imported.Added == 1);
    scanner.ScanAll(changed);

    IndexSourceResolver resolver(index);
    ConversionStore globalStore(ConversionStoreDesc::InProcess("/IconTest/GlobalStore"));
    AssetConversion conversion(resolver, globalStore);
    ConversionTarget target;
    target.Platform = "Windows";
    const Guid guid = ImportContext::MakeUuid(TypeOf<SvgImporter>(), record.BinaryGuid, {});
    const ConversionResult result = conversion.Convert(guid, target);
    MARLIN_REQUIREF(result.Succeeded, "{0}", result.Errors);
    auto store = MakeUnique<IconLocationStore>();
    MARLIN_REQUIRE(globalStore.ExportResult(result.Key, "/IconTest/Result.bin"));
    store->Add(guid, "/IconTest/Result.bin", result.Location);

    Assets::Initialize();
    const AssetStoreId storeId = Assets::AddStore(Move(store), 0);
    {
        const AssetRef<Icon> icon = Assets::LoadSync<Icon>(guid);
        MARLIN_REQUIRE(icon.Get() != nullptr);
        MARLIN_EXPECT(icon->Name.AsView() == "x" && icon->Size == Vector2(24, 24) && icon->IsValid());
        MARLIN_REQUIRE(icon->Shapes.Count() == 2);
        MARLIN_EXPECT(icon->Shapes[0].Kind == IconShapeKind::Stroke && icon->Shapes[0].CurrentColor);
        MARLIN_EXPECT(icon->Points[icon->Shapes[0].FirstPoint] == Vector2(18, 6));
    }
    Assets::CollectGarbage();
    Assets::RemoveStore(storeId);
    Assets::Shutdown();
}
} // namespace Marlin::Tests
