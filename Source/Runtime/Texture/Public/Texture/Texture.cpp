#include "Texture/Texture.h"

namespace Marlin::TextureFormats
{
uint32 GetBlockSize(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::ASTC6x6:
        return 6;
    case TextureFormat::ASTC8x8:
        return 8;
    default:
        return IsCompressed(format) ? 4 : 1;
    }
}

uint32 GetBlockBytes(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8:
        return 4;
    case TextureFormat::R8:
        return 1;
    case TextureFormat::RG8:
        return 2;
    case TextureFormat::RGBA16Float:
        return 8;
    case TextureFormat::BC1:
    case TextureFormat::BC4:
        return 8;
    case TextureFormat::BC3:
    case TextureFormat::BC5:
    case TextureFormat::BC7:
    case TextureFormat::BC6H:
    case TextureFormat::ASTC4x4:
    case TextureFormat::ASTC6x6:
    case TextureFormat::ASTC8x8:
        return 16;
    }
    return 0;
}

bool IsCompressed(TextureFormat format)
{
    return format >= TextureFormat::BC1;
}

bool IsFloat(TextureFormat format)
{
    return format == TextureFormat::RGBA16Float || format == TextureFormat::BC6H;
}

void GetMipLayout(TextureFormat format, uint32 width, uint32 height, uint32 &rowPitch, uint64 &size)
{
    const uint32 block = GetBlockSize(format);
    const uint32 columns = (width + block - 1) / block;
    const uint32 rows = (height + block - 1) / block;
    rowPitch = columns * GetBlockBytes(format);
    size = uint64{rowPitch} * rows;
}
} // namespace Marlin::TextureFormats
