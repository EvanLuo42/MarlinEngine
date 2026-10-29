#include "IspcTextureEncoder/IspcTextureEncoder.h"

#include "Math/Float16.h"
#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"
#include "ispc_texcomp.h"

namespace Marlin
{
Span<const TextureFormat> IspcTextureEncoder::GetFormats() const
{
    // RECOVERY-DECISION ENC-D3: BC6H only, as the class text says; rgbcx and bc7e (Bc7EncRdoEncoder) write the LDR BC
    // formats and astcenc the ASTC ones, so no format has two encoders and no Priority is needed.
    static constexpr TextureFormat Formats[] = {TextureFormat::BC6H};
    return Formats;
}

bool IspcTextureEncoder::Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const
{
    if (input.Format != TextureFormat::BC6H || input.Texels.Count() != usize{input.Width} * input.Height * 16)
    {
        error.Append("the ISPC encoder takes RGBA32Float texels of BC6H");
        return false;
    }
    const uint32 blocksX = (input.Width + 3) / 4;
    const uint32 blocksY = (input.Height + 3) / 4;
    if (input.Width == 0 || input.Height == 0 || blocks.Count() != usize{blocksX} * blocksY * 16)
    {
        error.Append("the block buffer does not fit the mip");
        return false;
    }

    // The kernels take whole blocks of half floats: pad the right and bottom edges by repeating the last texel.
    const uint32 width = blocksX * 4;
    const uint32 height = blocksY * 4;
    List<uint16> halves;
    halves.ResizeUninitialized(usize{width} * height * 4);
    for (uint32 y = 0; y < height; ++y)
    {
        const usize sourceRow = usize{Min(y, input.Height - 1)} * input.Width;
        for (uint32 x = 0; x < width; ++x)
        {
            const uint8 *source = input.Texels.Data() + (sourceRow + Min(x, input.Width - 1)) * 16;
            for (uint32 c = 0; c < 4; ++c)
            {
                float32 value = 0;
                MemCopy(&value, source + c * 4, sizeof(value));
                // NaN fails both comparisons and becomes zero.
                value = value > 0.0f ? Math::Clamp(value, 0.0f, Float16::Max) : 0.0f;
                halves[(usize{y} * width + x) * 4 + c] = Float16(value).Bits;
            }
        }
    }

    // RECOVERY-DECISION ENC-D4: Quality 0-1 picks one of the library's five BC6H profiles in equal steps, veryfast to
    // veryslow, as Build/MarlinBc7EncRdo.cpp spreads it over bc7e's uber levels.
    bc6h_enc_settings settings;
    const float32 quality = Math::Clamp(input.Quality, 0.0f, 1.0f);
    switch (static_cast<uint32>(quality * 4.0f + 0.5f))
    {
    case 0:
        GetProfile_bc6h_veryfast(&settings);
        break;
    case 1:
        GetProfile_bc6h_fast(&settings);
        break;
    case 2:
        GetProfile_bc6h_basic(&settings);
        break;
    case 3:
        GetProfile_bc6h_slow(&settings);
        break;
    default:
        GetProfile_bc6h_veryslow(&settings);
        break;
    }
    rgba_surface surface;
    surface.ptr = reinterpret_cast<uint8_t *>(halves.Data());
    surface.width = static_cast<int32_t>(width);
    surface.height = static_cast<int32_t>(height);
    surface.stride = static_cast<int32_t>(width * 8);
    CompressBlocksBC6H(&surface, blocks.Data(), &settings);
    return true;
}
} // namespace Marlin
