#include "Bc7EncRdoEncoder/Bc7EncRdoEncoder.h"

#include "MarlinBc7EncRdo.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// The MarlinBc7EncRdoFormat of *format*, or 0 if bc7enc_rdo does not write it.
uint32 LibraryFormat(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::BC1:
        return MarlinBc7EncRdo_BC1;
    case TextureFormat::BC3:
        return MarlinBc7EncRdo_BC3;
    case TextureFormat::BC4:
        return MarlinBc7EncRdo_BC4;
    case TextureFormat::BC5:
        return MarlinBc7EncRdo_BC5;
    case TextureFormat::BC7:
        return MarlinBc7EncRdo_BC7;
    default:
        return 0;
    }
}
} // namespace

Span<const TextureFormat> Bc7EncRdoEncoder::GetFormats() const
{
    static constexpr TextureFormat Formats[] = {TextureFormat::BC1, TextureFormat::BC3, TextureFormat::BC4,
                                                TextureFormat::BC5, TextureFormat::BC7};
    return Formats;
}

bool Bc7EncRdoEncoder::Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const
{
    const uint32 format = LibraryFormat(input.Format);
    if (format == 0 || input.Texels.Count() != usize{input.Width} * input.Height * 4)
    {
        error.Append("bc7enc_rdo takes RGBA8 texels of BC1, BC3, BC4, BC5 or BC7");
        return false;
    }
    // The library maps Quality onto its BC7 uber level and BC1 quality level (Build/MarlinBc7EncRdo.cpp).
    MarlinBc7EncRdoInput library{};
    library.Format = format;
    library.Width = input.Width;
    library.Height = input.Height;
    library.Texels = input.Texels.Data();
    library.Quality = input.Quality;
    library.RdoLambda = input.RdoLambda;
    library.Perceptual = input.Srgb && input.RdoLambda <= 0 ? 1 : 0;
    if (MarlinBc7EncRdo_Encode(&library, blocks.Data(), blocks.Count()) == 0)
    {
        error.Append("bc7enc_rdo failed");
        return false;
    }
    return true;
}

bool Bc7EncRdoEncoder::Decode(TextureFormat format, Span<const uint8> blocks, uint32 width, uint32 height,
                              Span<uint8> texels)
{
    const uint32 library = LibraryFormat(format);
    if (library == 0 || texels.Count() != usize{width} * height * 4 ||
        blocks.Count() < usize{(width + 3) / 4} * ((height + 3) / 4) * TextureFormats::GetBlockBytes(format))
    {
        return false;
    }
    return MarlinBc7EncRdo_Decode(library, blocks.Data(), width, height, texels.Data()) != 0;
}
} // namespace Marlin
