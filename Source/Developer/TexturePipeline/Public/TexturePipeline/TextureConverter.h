#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "Containers/HashMap.h"
#include "CoreTypes.h"
#include "MarlinText/BinaryFileLink.h"
#include "String/String.h"
#include "Texture/Texture.h"
#include "TexturePipeline/TextureEncoder.h"
#include "TexturePipeline/TextureConverter.gen.h"

namespace Marlin
{
/// How an image file becomes a Texture (Documentation/Architecture/06 §3.4).
MSTRUCT()
struct TextureSource
{
    MGENERATED_BODY()

    /// What kind of texture this is, such as Color, Normal or Hdr: a key of Engine.Textures.Compression, where each
    /// platform names the format it encodes that kind to.
    MPROPERTY()
    String Compression = "Color";
    /// A full chain down to 1x1 when true; the top level only otherwise.
    MPROPERTY()
    bool MipMaps = true;
    /// The largest side kept; bigger images are halved until they fit. Zero keeps every size.
    MPROPERTY()
    uint32 MaxSize = 0;
    MPROPERTY()
    TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
    MPROPERTY()
    BinaryFileLink SourceFile;
};

/// One entry of Engine.Textures.Compression: the format a kind of texture is encoded to, and how hard to try.
MSTRUCT()
struct TextureCompression
{
    MGENERATED_BODY()

    MPROPERTY()
    TextureFormat Format = TextureFormat::RGBA8;
    /// 0 for the fastest encoding, 1 for the best.
    MPROPERTY()
    float32 Quality = 0.5f; // RECOVERY-DECISION TM-D2: a balanced default; platforms raise it for shipping data.
    /// Rate-distortion lambda for encoders that support it; 0 is off.
    MPROPERTY()
    float32 RdoLambda = 0;
};

/// The Engine.Textures config section (06 §3.4); platform layers override it.
MSTRUCT(Config = "Engine.Textures")
struct TextureSettings
{
    MGENERATED_BODY()

    /// Kinds of texture, as TextureSource.Compression names them, to the format each platform encodes them to.
    MPROPERTY()
    HashMap<String, TextureCompression> Compression;
};

/// Turns a TextureSource into a Texture: decodes the file through ImageDecoder, filters mips in linear light (so sRGB
/// color darkens correctly as it shrinks), and writes every mip into the bulk data as the GPU reads it.
/// Block-compressed formats go through the first TextureEncoder of the format; the encoders of it are a DDC input.
MCLASS()
class TEXTUREPIPELINE_API TextureConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    TextureConverter();

    [[nodiscard]] StringView GetSourceType() const override
    {
        return "TextureSource";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<Texture>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 3;
    }
    bool Convert(ConversionContext &context) override;
    bool WriteInput(StringView name, StringWriter &text) const override;

    [[nodiscard]] const TextureEncoderSet &GetEncoders() const
    {
        return _encoders;
    }

private:
    TextureEncoderSet _encoders;
};

/// The sRGB transfer function as the converter applies it: tables instead of a pow per channel.
namespace TextureColor
{
/// The exact curve, which the tables are built from.
[[nodiscard]] TEXTUREPIPELINE_API float32 SrgbToLinear(float32 value);
[[nodiscard]] TEXTUREPIPELINE_API float32 LinearToSrgb(float32 value);
/// Linear light of an 8-bit or 16-bit sRGB-encoded channel.
[[nodiscard]] TEXTUREPIPELINE_API float32 SrgbToLinear8(uint8 value);
[[nodiscard]] TEXTUREPIPELINE_API float32 SrgbToLinear16(uint16 value);
/// The 8-bit sRGB encoding of linear *value*, clamped to [0, 1]: round(LinearToSrgb(value) * 255), found by
/// comparing against the curve's 255 rounding thresholds.
[[nodiscard]] TEXTUREPIPELINE_API uint8 LinearToSrgb8(float32 value);
} // namespace TextureColor
} // namespace Marlin
