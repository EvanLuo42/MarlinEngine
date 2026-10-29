#include "Testing/Testing.h"

#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "Config/Config.h"
#include "HAL/PlatformFile.h"
#include "Memory/MemoryOps.h"
#include "PngTestImages.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "TexturePipeline/PngDecoder.h"
#include "TexturePipeline/TextureConverter.h"
#include "TextureTestTypes.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
void Pattern(uint32 x, uint32 y, uint8 (&rgba)[4])
{
    rgba[0] = static_cast<uint8>((x * 17 + y * 5) & 255);
    rgba[1] = static_cast<uint8>((x * 3 + y * 29) & 255);
    rgba[2] = static_cast<uint8>((x * y * 7) & 255);
    rgba[3] = static_cast<uint8>((255 - x * 11 - y) & 255);
}

bool Decode(Span<const uint8> bytes, DecodedImage &image)
{
    StringBuilder<128> error;
    return PngDecoder().Decode(bytes, image, error);
}

/// True if *image* is RGBA8 and holds Pattern, with opaque alpha unless *alpha*.
bool MatchesPattern(const DecodedImage &image, bool alpha)
{
    if (image.Format != DecodedPixelFormat::RGBA8 || image.Pixels.Count() != usize{image.Width} * image.Height * 4)
    {
        return false;
    }
    for (uint32 y = 0; y < image.Height; ++y)
    {
        for (uint32 x = 0; x < image.Width; ++x)
        {
            uint8 expected[4];
            Pattern(x, y, expected);
            const uint8 *pixel = image.Pixels.Data() + (usize{y} * image.Width + x) * 4;
            if (pixel[0] != expected[0] || pixel[1] != expected[1] || pixel[2] != expected[2] ||
                pixel[3] != (alpha ? expected[3] : 255))
            {
                return false;
            }
        }
    }
    return true;
}

/// Serves one conversion result to the asset table.
class LocationStore final : public IAssetStore
{
public:
    /// *path* holds the converted result, as ConversionStore::ExportResult writes it.
    LocationStore(const Guid &guid, StringView path, ConvertedResultLocation location)
        : _guid(guid), _path(path), _location(Move(location))
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        FileRegion file;
        if (guid != _guid || !Vfs::OpenRegion(_path.AsView(), file))
        {
            done(nullptr);
            return;
        }
        AssetLocation location;
        location.Block = file;
        location.Block.Offset = file.Offset + _location.BlockOffset;
        location.Block.Size = _location.BlockSize;
        location.Bulk = file;
        location.Bulk.Offset = file.Offset + _location.BulkOffset;
        location.Bulk.Size = _location.BulkSize;
        done(&location);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "TextureTest";
    }

private:
    Guid _guid;
    String _path;
    ConvertedResultLocation _location;
};

/// A scratch folder at /Temp/TexturePipelineTest mounted as /TextureTest. Everything is created and deleted through
/// the VFS, inside the /Temp mount, so no failed lookup can turn into a native path outside it.
struct TextureFixture
{
    MountId Mount = 0;

    TextureFixture()
    {
        (void)Vfs::Delete("/Temp/TexturePipelineTest");
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/TexturePipelineTest"));
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/TexturePipelineTest/Content"));
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/TexturePipelineTest/Config"));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/TexturePipelineTest", path) && !path.IsEmpty());
        Mount = Vfs::Mount("/TextureTest", MakeUnique<DirectoryMount>(path.AsView(), true), 0);
    }

    ~TextureFixture()
    {
        if (Mount != 0)
        {
            Vfs::Unmount(Mount);
        }
        (void)Vfs::Delete("/Temp/TexturePipelineTest");
    }

    MARLIN_NONCOPYABLE(TextureFixture);
};
} // namespace

MARLIN_TEST(TexturePipeline, PngColorTypes)
{
    DecodedImage image;
    MARLIN_REQUIRE(Decode(Rgba8Png, image));
    MARLIN_EXPECT(image.Width == 13 && image.Height == 9 && image.Format == DecodedPixelFormat::RGBA8);
    MARLIN_EXPECT(MatchesPattern(image, true));
    MARLIN_REQUIRE(Decode(BigPng, image));
    MARLIN_EXPECT(image.Width == 70 && image.Height == 40 && MatchesPattern(image, true));
    MARLIN_REQUIRE(Decode(InterlacedPng, image));
    MARLIN_EXPECT(image.Width == 11 && image.Height == 10 && MatchesPattern(image, false));

    MARLIN_REQUIRE(Decode(Grey16Png, image));
    MARLIN_REQUIRE(image.Format == DecodedPixelFormat::RGBA16 && image.Width == 6 && image.Height == 5);
    bool grey = true;
    for (uint32 y = 0; y < 5; ++y)
    {
        for (uint32 x = 0; x < 6; ++x)
        {
            uint16 texel[4];
            MemCopy(texel, image.Pixels.Data() + (y * 6 + x) * 8, sizeof(texel));
            const auto expected = static_cast<uint16>((x * 9001 + y * 333) & 0xFFFF);
            grey = grey && texel[0] == expected && texel[1] == expected && texel[2] == expected && texel[3] == 0xFFFF;
        }
    }
    MARLIN_EXPECT(grey);

    MARLIN_REQUIRE(Decode(Palette4Png, image));
    MARLIN_REQUIRE(image.Width == 7 && image.Height == 3);
    bool palette = true;
    constexpr uint8 Alpha[5] = {255, 128, 0, 255, 255};
    for (uint32 y = 0; y < 3; ++y)
    {
        for (uint32 x = 0; x < 7; ++x)
        {
            const uint32 index = (x + y) % 5;
            const uint8 *pixel = image.Pixels.Data() + (y * 7 + x) * 4;
            palette = palette && pixel[0] == index * 40 && pixel[1] == 255 - index * 40 && pixel[2] == index * 13 &&
                      pixel[3] == Alpha[index];
        }
    }
    MARLIN_EXPECT(palette);

    List<uint8> corrupt;
    corrupt.AddRange(Span<const uint8>(BigPng));
    corrupt[60] ^= 0x40;
    MARLIN_EXPECT(!Decode(corrupt.AsSpan(), image));
    MARLIN_EXPECT(!Decode(Span<const uint8>(BigPng).Slice(0, 200), image));
    MARLIN_EXPECT(!PngDecoder().CanDecode(Span<const uint8>(Rgba8Png).Slice(1)));
}

MARLIN_TEST(TexturePipeline, SrgbTablesMatchTheCurve)
{
    // Decoding: the tables are the curve evaluated at each code.
    for (uint32 i = 0; i < 256; ++i)
    {
        const float32 exact = TextureColor::SrgbToLinear(static_cast<float32>(i) / 255.0f);
        MARLIN_EXPECT(TextureColor::SrgbToLinear8(static_cast<uint8>(i)) == exact);
    }
    for (uint32 i = 0; i < 65536; i += 97)
    {
        const float32 exact = TextureColor::SrgbToLinear(static_cast<float32>(i) / 65535.0f);
        MARLIN_EXPECT(TextureColor::SrgbToLinear16(static_cast<uint16>(i)) == exact);
    }
    // Encoding: what round(LinearToSrgb(v) * 255) gives, at most one code off right at a rounding threshold, over a
    // dense sweep and every decoded code, which must come back exactly.
    uint32 offByOne = 0;
    for (uint32 i = 0; i <= 1u << 20; ++i)
    {
        const float32 value = static_cast<float32>(i) / static_cast<float32>(1u << 20);
        const float32 encoded = TextureColor::LinearToSrgb(value) * 255.0f;
        const int32 exact = static_cast<int32>(encoded + 0.5f);
        const int32 table = TextureColor::LinearToSrgb8(value);
        MARLIN_REQUIRE(table - exact <= 1 && exact - table <= 1);
        offByOne += table != exact ? 1 : 0;
    }
    MARLIN_EXPECT(offByOne < 64);
    for (uint32 i = 0; i < 256; ++i)
    {
        MARLIN_EXPECT(TextureColor::LinearToSrgb8(TextureColor::SrgbToLinear8(static_cast<uint8>(i))) == i);
    }
    MARLIN_EXPECT(TextureColor::LinearToSrgb8(-1.0f) == 0 && TextureColor::LinearToSrgb8(2.0f) == 255);
    MARLIN_EXPECT(TextureColor::LinearToSrgb8(__builtin_nanf("")) == 0);
}

MARLIN_TEST(TexturePipeline, TextureConversion)
{
    TextureFixture fixture;
    MARLIN_REQUIRE(Vfs::WriteAll("/TextureTest/Content/Big.png",
                                 StringView(reinterpret_cast<const char *>(BigPng), sizeof(BigPng))));
    MARLIN_REQUIRE(
        Vfs::WriteAll("/TextureTest/Config/Engine.mtext",
                      "!Version \"1.0\"\n\nTextures\n{\n    Compression =\n    {\n        Color =\n        {\n"
                      "            Format = BC7\n        }\n    }\n}\n"));
    ConfigDesc desc;
    desc.Directories.Add(String("/TextureTest/Config"));
    desc.Variables.Add({"Platform", "Windows"});
    // RECOVERY-DECISION TM-D9: config re-initialized on the test folder, as ConfigTests does; every format of the
    // loop below is a rewrite of it and a Reload.
    Config::Initialize(desc);
    TextureSettings settings;
    MARLIN_REQUIRE(Config::Apply("Engine.Textures", TypeOf<TextureSettings>(), &settings));
    MARLIN_REQUIRE(settings.Compression.Find("Color") != nullptr);

    AssetIndex index;
    List<String> folders;
    folders.Add("/TextureTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    AssetFileRecord png;
    MARLIN_REQUIRE(index.GetRecord("/TextureTest/Content/Big.png", png));
    StringBuilder<40> guid;
    png.BinaryGuid.FormatTo(guid, {});
    const String text =
        Format("!Version \"1.0\"\n\nTextureSource\n{{\n    !UUID = \"7e570000-0000-0000-0000-000000000001\"\n"
               "    !Name = \"Big\"\n    Compression = \"Color\"\n    MaxSize = 64\n    ColorSpace = Srgb\n"
               "    SourceFile = `source:Big.png#{0}`\n}}\n",
               guid.AsView());
    MARLIN_REQUIRE(Vfs::WriteAll("/TextureTest/Content/Big.masset", text.AsView()));
    scanner.ScanAll(changed);

    IndexSourceResolver resolver(index);
    ConversionStore globalStore(ConversionStoreDesc::InProcess("/TextureTest/GlobalStore"));
    AssetConversion conversion(resolver, globalStore);
    MARLIN_REQUIRE(conversion.GetImageDecoders().Find("png").Count() >= 1);
    ConversionTarget target;
    target.Platform = "Windows";
    const Guid texture = {0x7e570000, 0, 0, 1};
    Assets::Initialize();

    // BC7 first, as written above; then one format of each kind the converter writes itself or hands to an encoder.
    constexpr TextureFormat Formats[] = {TextureFormat::BC7,    TextureFormat::RGBA8,       TextureFormat::RG8,
                                         TextureFormat::BC4,    TextureFormat::RGBA16Float, TextureFormat::BC6H,
                                         TextureFormat::ASTC6x6};
    for (const TextureFormat format : Formats)
    {
        const EnumeratorInfo *name = TypeOf<TextureFormat>().FindEnumerator(static_cast<int64>(format));
        MARLIN_REQUIRE(name != nullptr);
        if (format != TextureFormat::BC7)
        {
            MARLIN_REQUIRE(Vfs::WriteAll(
                "/TextureTest/Config/Engine.mtext",
                Format("!Version \"1.0\"\n\nTextures\n{{\n    Compression =\n    {{\n        Color =\n        {{\n"
                       "            Format = {0}\n        }}\n    }}\n}}\n",
                       name->Name)
                    .AsView()));
            Config::Reload();
        }

        const ConversionResult result = conversion.Convert(texture, target);
        MARLIN_REQUIRE(result.Succeeded);
        bool decoderInput = false;
        bool encoderInput = false;
        for (const DependencyInput &input : result.Inputs)
        {
            decoderInput = decoderInput || (input.Kind == DependencyInputKind::Decoder && input.Section == "png");
            encoderInput = encoderInput || (input.Kind == DependencyInputKind::Converter &&
                                            input.Section == Format("TextureEncoder:{0}", name->Name));
        }
        MARLIN_EXPECT(decoderInput);
        MARLIN_EXPECT(encoderInput == TextureFormats::IsCompressed(format));
        if (format == TextureFormat::BC7)
        {
            const ConversionResult again = conversion.Convert(texture, target);
            MARLIN_EXPECTF(again.Succeeded && again.CacheHit && again.Key == result.Key, "{0} {1} {2}", again.Succeeded,
                           again.CacheHit, again.Errors);
            // Another kind added to the config is no input of a Color texture.
            MARLIN_REQUIRE(Vfs::WriteAll(
                "/TextureTest/Config/Engine.mtext",
                "!Version \"1.0\"\n\nTextures\n{\n    Compression =\n    {\n        Color =\n        {\n"
                "            Format = BC7\n        }\n        Normal =\n        {\n            Format = BC5\n"
                "        }\n    }\n}\n"));
            Config::Reload();
            const ConversionResult other = conversion.Convert(texture, target);
            MARLIN_EXPECTF(other.Succeeded && other.CacheHit && other.Key == result.Key, "{0} {1} {2}", other.Succeeded,
                           other.CacheHit, other.Errors);
        }

        MARLIN_REQUIRE(globalStore.ExportResult(result.Key, "/TextureTest/Result.bin"));
        const AssetStoreId store =
            Assets::AddStore(MakeUnique<LocationStore>(texture, "/TextureTest/Result.bin", result.Location), 0);
        {
            const AssetRef<Texture> loaded = Assets::LoadSync<Texture>(texture);
            MARLIN_REQUIRE(loaded.Get() != nullptr);
            MARLIN_EXPECT(loaded->Width == 35 && loaded->Height == 20 && loaded->Format == format);
            MARLIN_EXPECT(loaded->Name.AsView() == "Big");
            const bool color =
                format == TextureFormat::BC7 || format == TextureFormat::RGBA8 || format == TextureFormat::ASTC6x6;
            MARLIN_EXPECT(loaded->ColorSpace == (color ? TextureColorSpace::Srgb : TextureColorSpace::Linear));
            MARLIN_REQUIRE(loaded->Mips.Count() == 6);
            uint32 rowPitch = 0;
            uint64 size = 0;
            TextureFormats::GetMipLayout(format, 35, 20, rowPitch, size);
            MARLIN_EXPECT(loaded->Mips[0].Size == size && loaded->Mips[0].RowPitch == rowPitch);
            TextureFormats::GetMipLayout(format, 1, 1, rowPitch, size);
            MARLIN_EXPECT(loaded->Mips[5].Width == 1 && loaded->Mips[5].Height == 1 && loaded->Mips[5].Size == size);
            const TextureMip &last = loaded->Mips[5];
            MARLIN_EXPECT(last.Offset + last.Size <= result.Location.BulkSize && last.Offset % 16 == 0);

            if (TextureFormats::IsCompressed(format))
            {
                // The test encoder repeats the first texel of its strip; a 35x20 mip is a single strip.
                List<uint8> file;
                MARLIN_REQUIRE(Vfs::ReadAll("/TextureTest/Result.bin", file));
                MARLIN_REQUIRE(result.Location.BulkOffset + result.Location.BulkSize <= file.Count());
                const uint8 *blocks = file.Data() + result.Location.BulkOffset + loaded->Mips[0].Offset;
                const usize texel = TextureFormats::IsFloat(format) ? 16 : 4;
                bool repeated = true;
                for (usize i = 0; i < loaded->Mips[0].Size; ++i)
                {
                    repeated = repeated && blocks[i] == blocks[i % texel];
                }
                MARLIN_EXPECT(repeated);
            }
        }
        Assets::CollectGarbage();
        Assets::RemoveStore(store);
    }
    Assets::Shutdown();
    Config::Shutdown();
}
} // namespace Marlin::Tests
