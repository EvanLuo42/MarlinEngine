#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// Texel and vertex formats; component order is memory order, lowest address first.
enum class RhiFormat : uint8
{
    Unknown,

    R8Unorm,
    R8Snorm,
    R8Uint,
    R8Sint,
    RG8Unorm,
    RG8Snorm,
    RG8Uint,
    RG8Sint,
    RGBA8Unorm,
    RGBA8UnormSrgb,
    RGBA8Snorm,
    RGBA8Uint,
    RGBA8Sint,
    BGRA8Unorm,
    BGRA8UnormSrgb,

    R16Unorm,
    R16Snorm,
    R16Uint,
    R16Sint,
    R16Float,
    RG16Unorm,
    RG16Snorm,
    RG16Uint,
    RG16Sint,
    RG16Float,
    RGBA16Unorm,
    RGBA16Snorm,
    RGBA16Uint,
    RGBA16Sint,
    RGBA16Float,

    R32Uint,
    R32Sint,
    R32Float,
    RG32Uint,
    RG32Sint,
    RG32Float,
    RGB32Uint,
    RGB32Sint,
    RGB32Float,
    RGBA32Uint,
    RGBA32Sint,
    RGBA32Float,
    /// For 64-bit image atomics, such as the visibility buffer.
    R64Uint,

    RGB10A2Unorm,
    RGB10A2Uint,
    RG11B10Float,
    RGB9E5Float,

    D16Unorm,
    D32Float,
    D32FloatS8Uint,

    BC1Unorm,
    BC1UnormSrgb,
    BC2Unorm,
    BC2UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC4Unorm,
    BC4Snorm,
    BC5Unorm,
    BC5Snorm,
    BC6HUfloat,
    BC6HSfloat,
    BC7Unorm,
    BC7UnormSrgb,

    Astc4x4Unorm,
    Astc4x4UnormSrgb,
    Astc5x5Unorm,
    Astc5x5UnormSrgb,
    Astc6x6Unorm,
    Astc6x6UnormSrgb,
    Astc8x8Unorm,
    Astc8x8UnormSrgb,

    Count,
};

enum class RhiFormatKind : uint8
{
    Unorm,
    Snorm,
    Uint,
    Sint,
    Float,
    UnormSrgb,
    DepthStencil,
};

struct RhiFormatInfo
{
    const char *Name = "";
    /// Bytes per texel, or per block for block-compressed formats.
    uint8 BlockBytes = 0;
    uint8 BlockWidth = 1;
    uint8 BlockHeight = 1;
    uint8 ComponentCount = 0;
    RhiFormatKind Kind = RhiFormatKind::Unorm;
    bool HasDepth = false;
    bool HasStencil = false;
    bool Compressed = false;
};

[[nodiscard]] RHI_API const RhiFormatInfo &GetFormatInfo(RhiFormat format);

[[nodiscard]] inline bool IsDepthFormat(RhiFormat format)
{
    return GetFormatInfo(format).HasDepth;
}

[[nodiscard]] inline bool IsStencilFormat(RhiFormat format)
{
    return GetFormatInfo(format).HasStencil;
}

/// The bytes of one row of *width* texels, rounded up to whole blocks.
[[nodiscard]] RHI_API uint64 GetFormatRowBytes(RhiFormat format, uint32 width);
/// The rows of blocks that *height* texels take.
[[nodiscard]] RHI_API uint32 GetFormatRowCount(RhiFormat format, uint32 height);
} // namespace Marlin
