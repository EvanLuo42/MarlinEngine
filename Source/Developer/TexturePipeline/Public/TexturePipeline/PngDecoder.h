#pragma once

#include "AssetPipeline/SourceDecoder.h"
#include "CoreTypes.h"
#include "TexturePipeline/PngDecoder.gen.h"

namespace Marlin
{
/// PNG (ISO/IEC 15948): every color type and bit depth, transparency chunks and Adam7 interlacing. 16-bit images
/// decode to RGBA16, everything else to RGBA8. Gamma and color profile chunks are ignored: textures are authored in
/// the color space their TextureSource names.
MCLASS()
class TEXTUREPIPELINE_API PngDecoder final : public ImageDecoder
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Span<const StringView> GetExtensions() const override;
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    [[nodiscard]] bool CanDecode(Span<const uint8> header) const override;
    bool Decode(Span<const uint8> bytes, DecodedImage &image, StringWriter &error) const override;
};
} // namespace Marlin
