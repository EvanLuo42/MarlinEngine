#pragma once

#include "CoreTypes.h"
#include "TexturePipeline/TextureEncoder.h"
#include "Bc7EncRdoEncoder/Bc7EncRdoEncoder.gen.h"

namespace Marlin
{
/// BC1, BC3, BC4, BC5 and BC7 through bc7enc_rdo: rgbcx for BC1-5, bc7e for BC7, and ert's rate-distortion pass when
/// RdoLambda is set. sRGB color is encoded with perceptual weights, unless RDO is on.
MCLASS()
class BC7ENCRDOENCODER_API Bc7EncRdoEncoder final : public TextureEncoder
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Span<const TextureFormat> GetFormats() const override;
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const override;

    /// Decodes a *width* by *height* mip of *blocks* into RGBA8 *texels*; for tests and previews. False if bc7enc_rdo
    /// does not write the format or the sizes do not match.
    static bool Decode(TextureFormat format, Span<const uint8> blocks, uint32 width, uint32 height, Span<uint8> texels);
};
} // namespace Marlin
