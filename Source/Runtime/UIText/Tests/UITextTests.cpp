#include "Testing/Testing.h"

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformFile.h"
#include "String/StringBuilder.h"
#include "UIText/UITextSubsystem.h"
#include "Vfs/Vfs.h"

// RECOVERY-DECISION D5: the outline indexes no UIText tests; this smoke test was added during recovery to check
// shaping and outlines against the engine's Inter font.
namespace Marlin::Tests
{
namespace
{
/// Serves Font blocks written under /Temp/UITextTests.
class FontBlockStore final : public IAssetStore
{
public:
    explicit FontBlockStore(HashMap<Guid, String> files) : _files(Move(files))
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const String *path = _files.Find(guid);
        AssetLocation location;
        done(path != nullptr && Vfs::OpenRegion(path->AsView(), location.Block) ? &location : nullptr);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "UIText test blocks";
    }

private:
    HashMap<Guid, String> _files;
};

constexpr Guid InterGuid = {0x5E57F047u, 0, 0, 1};
} // namespace

MARLIN_TEST(UIText, ShapeAndOutline)
{
    List<uint8> bytes;
    MARLIN_REQUIRE(Vfs::ReadAll("/Engine/Content/Fonts/Inter/Inter-Regular.ttf", bytes));

    StringBuilder<512> physical;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/UITextTests", physical));
    const String directory(physical.AsView());
    MARLIN_REQUIRE(directory.Length() > 12 && directory.AsView().EndsWith("UITextTests"));
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
    MARLIN_REQUIRE(PlatformFile::CreateDirectory(directory.CStr()));

    Assets::Initialize();
    {
        Font font;
        font.Name = FrozenString("Inter-Regular");
        font.Data = FrozenArray<uint8>(bytes.AsSpan());
        List<uint8> block;
        MARLIN_REQUIRE(AssetBlock::Write(font, InterGuid, block));
        const StringView blockPath = "/Temp/UITextTests/Inter.mablk";
        MARLIN_REQUIRE(Vfs::WriteAll(blockPath, StringView(reinterpret_cast<const char *>(block.Data()), block.Count())));
        HashMap<Guid, String> files;
        files.FindOrAdd(InterGuid) = blockPath;
        const AssetStoreId store = Assets::AddStore(MakeUnique<FontBlockStore>(Move(files)), 0);

        UITextSubsystem text;
        SubsystemContext context;
        text.Initialize(context);
        const AssetRef<Font> ref = Assets::LoadSync<Font>(InterGuid);
        MARLIN_REQUIRE(ref.Get() != nullptr);
        const UIFontFace face = text.RegisterFont(ref);
        MARLIN_EXPECT(face.IsValid() && text.RegisterFont(ref) == face);
        Assets::ProcessFrame();
        MARLIN_REQUIRE(text.IsReady(face));

        const UIFontFace faces[] = {face};
        UIShapedText shaped;
        text.Shape("Hello, world", {Span<const UIFontFace>(faces), 20}, shaped);
        MARLIN_REQUIRE(shaped.Runs.Count() == 1);
        MARLIN_EXPECT(shaped.Runs[0].Glyphs.Count() == 12);
        MARLIN_EXPECT(shaped.Width > 60 && shaped.Width < 200);
        MARLIN_EXPECT(shaped.Ascent > 10 && shaped.Descent > 0);
        MARLIN_EXPECT(shaped.Runs[0].Glyphs[5].Bounds.IsEmpty() == false);
        MARLIN_EXPECT(shaped.Runs[0].Glyphs[6].Bounds.IsEmpty());

        UIPath outline;
        MARLIN_EXPECT(text.GetGlyphOutline(face, shaped.Runs[0].Glyphs[0].GlyphId, outline) && !outline.IsEmpty());
        MARLIN_EXPECT(!text.GetGlyphOutline(face, shaped.Runs[0].Glyphs[6].GlyphId, outline));
        MARLIN_EXPECT(!text.IsReady({}) && !text.GetGlyphOutline({99}, 1, outline));

        text.Deinitialize();
        Assets::RemoveStore(store);
    }
    Assets::Shutdown();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}
} // namespace Marlin::Tests
