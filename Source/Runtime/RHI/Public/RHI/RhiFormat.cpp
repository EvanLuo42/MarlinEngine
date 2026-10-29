#include "RHI/RhiFormat.h"

#include "Diagnostics/Assert.h"

namespace Marlin
{
namespace
{
using enum RhiFormatKind;

constexpr RhiFormatInfo Plain(const char *name, uint8 bytes, uint8 components, RhiFormatKind kind)
{
    RhiFormatInfo info;
    info.Name = name;
    info.BlockBytes = bytes;
    info.ComponentCount = components;
    info.Kind = kind;
    return info;
}

constexpr RhiFormatInfo Depth(const char *name, uint8 bytes, bool stencil)
{
    RhiFormatInfo info = Plain(name, bytes, stencil ? 2 : 1, DepthStencil);
    info.HasDepth = true;
    info.HasStencil = stencil;
    return info;
}

constexpr RhiFormatInfo Block(const char *name, uint8 bytes, uint8 width, uint8 height, uint8 components,
                              RhiFormatKind kind)
{
    RhiFormatInfo info = Plain(name, bytes, components, kind);
    info.BlockWidth = width;
    info.BlockHeight = height;
    info.Compressed = true;
    return info;
}

constexpr RhiFormatInfo FormatTable[] = {
    Plain("Unknown", 0, 0, Unorm),

    Plain("R8Unorm", 1, 1, Unorm),
    Plain("R8Snorm", 1, 1, Snorm),
    Plain("R8Uint", 1, 1, Uint),
    Plain("R8Sint", 1, 1, Sint),
    Plain("RG8Unorm", 2, 2, Unorm),
    Plain("RG8Snorm", 2, 2, Snorm),
    Plain("RG8Uint", 2, 2, Uint),
    Plain("RG8Sint", 2, 2, Sint),
    Plain("RGBA8Unorm", 4, 4, Unorm),
    Plain("RGBA8UnormSrgb", 4, 4, UnormSrgb),
    Plain("RGBA8Snorm", 4, 4, Snorm),
    Plain("RGBA8Uint", 4, 4, Uint),
    Plain("RGBA8Sint", 4, 4, Sint),
    Plain("BGRA8Unorm", 4, 4, Unorm),
    Plain("BGRA8UnormSrgb", 4, 4, UnormSrgb),

    Plain("R16Unorm", 2, 1, Unorm),
    Plain("R16Snorm", 2, 1, Snorm),
    Plain("R16Uint", 2, 1, Uint),
    Plain("R16Sint", 2, 1, Sint),
    Plain("R16Float", 2, 1, Float),
    Plain("RG16Unorm", 4, 2, Unorm),
    Plain("RG16Snorm", 4, 2, Snorm),
    Plain("RG16Uint", 4, 2, Uint),
    Plain("RG16Sint", 4, 2, Sint),
    Plain("RG16Float", 4, 2, Float),
    Plain("RGBA16Unorm", 8, 4, Unorm),
    Plain("RGBA16Snorm", 8, 4, Snorm),
    Plain("RGBA16Uint", 8, 4, Uint),
    Plain("RGBA16Sint", 8, 4, Sint),
    Plain("RGBA16Float", 8, 4, Float),

    Plain("R32Uint", 4, 1, Uint),
    Plain("R32Sint", 4, 1, Sint),
    Plain("R32Float", 4, 1, Float),
    Plain("RG32Uint", 8, 2, Uint),
    Plain("RG32Sint", 8, 2, Sint),
    Plain("RG32Float", 8, 2, Float),
    Plain("RGB32Uint", 12, 3, Uint),
    Plain("RGB32Sint", 12, 3, Sint),
    Plain("RGB32Float", 12, 3, Float),
    Plain("RGBA32Uint", 16, 4, Uint),
    Plain("RGBA32Sint", 16, 4, Sint),
    Plain("RGBA32Float", 16, 4, Float),
    Plain("R64Uint", 8, 1, Uint),

    Plain("RGB10A2Unorm", 4, 4, Unorm),
    Plain("RGB10A2Uint", 4, 4, Uint),
    Plain("RG11B10Float", 4, 3, Float),
    Plain("RGB9E5Float", 4, 3, Float),

    Depth("D16Unorm", 2, false),
    Depth("D32Float", 4, false),
    Depth("D32FloatS8Uint", 8, true),

    Block("BC1Unorm", 8, 4, 4, 4, Unorm),
    Block("BC1UnormSrgb", 8, 4, 4, 4, UnormSrgb),
    Block("BC2Unorm", 16, 4, 4, 4, Unorm),
    Block("BC2UnormSrgb", 16, 4, 4, 4, UnormSrgb),
    Block("BC3Unorm", 16, 4, 4, 4, Unorm),
    Block("BC3UnormSrgb", 16, 4, 4, 4, UnormSrgb),
    Block("BC4Unorm", 8, 4, 4, 1, Unorm),
    Block("BC4Snorm", 8, 4, 4, 1, Snorm),
    Block("BC5Unorm", 16, 4, 4, 2, Unorm),
    Block("BC5Snorm", 16, 4, 4, 2, Snorm),
    Block("BC6HUfloat", 16, 4, 4, 3, Float),
    Block("BC6HSfloat", 16, 4, 4, 3, Float),
    Block("BC7Unorm", 16, 4, 4, 4, Unorm),
    Block("BC7UnormSrgb", 16, 4, 4, 4, UnormSrgb),

    Block("Astc4x4Unorm", 16, 4, 4, 4, Unorm),
    Block("Astc4x4UnormSrgb", 16, 4, 4, 4, UnormSrgb),
    Block("Astc5x5Unorm", 16, 5, 5, 4, Unorm),
    Block("Astc5x5UnormSrgb", 16, 5, 5, 4, UnormSrgb),
    Block("Astc6x6Unorm", 16, 6, 6, 4, Unorm),
    Block("Astc6x6UnormSrgb", 16, 6, 6, 4, UnormSrgb),
    Block("Astc8x8Unorm", 16, 8, 8, 4, Unorm),
    Block("Astc8x8UnormSrgb", 16, 8, 8, 4, UnormSrgb),
};
static_assert(sizeof(FormatTable) / sizeof(FormatTable[0]) == static_cast<usize>(RhiFormat::Count));
} // namespace

const RhiFormatInfo &GetFormatInfo(RhiFormat format)
{
    MARLIN_ASSERT(format < RhiFormat::Count);
    return FormatTable[static_cast<usize>(format)];
}

uint64 GetFormatRowBytes(RhiFormat format, uint32 width)
{
    const RhiFormatInfo &info = GetFormatInfo(format);
    return uint64{(width + info.BlockWidth - 1) / info.BlockWidth} * info.BlockBytes;
}

uint32 GetFormatRowCount(RhiFormat format, uint32 height)
{
    const RhiFormatInfo &info = GetFormatInfo(format);
    return (height + info.BlockHeight - 1) / info.BlockHeight;
}
} // namespace Marlin
