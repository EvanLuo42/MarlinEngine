#pragma once

#include "CoreTypes.h"
#include "TexturePipeline/TextureEncoder.h"
#include "IspcTextureEncoder/IspcTextureEncoder.gen.h"

namespace Marlin
{
/// BC6H, unsigned, through Intel's ISPC Texture Compressor, from linear float texels: negative values clamp to zero
/// and values beyond the half-float range to its largest.
MCLASS()
class ISPCTEXTUREENCODER_API IspcTextureEncoder final : public TextureEncoder
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Span<const TextureFormat> GetFormats() const override;
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const override;
};
} // namespace Marlin
