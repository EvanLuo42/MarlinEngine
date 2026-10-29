#pragma once

#include "Asset/Asset.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Texture/Texture.gen.h"

namespace Marlin
{
/// How texels are stored. Block-compressed formats keep 4x4 texels per block.
MENUM()
enum class TextureFormat : uint8
{
    RGBA8,
    R8,
    RG8,
    RGBA16Float,
    /// RGB with 1-bit alpha, 8 bytes per block.
    BC1,
    /// RGBA, 16 bytes per block.
    BC3,
    /// One channel, 8 bytes per block: masks, roughness.
    BC4,
    /// Two channels, 16 bytes per block: normal maps.
    BC5,
    /// RGBA at high quality, 16 bytes per block: the default for color.
    BC7,
    /// RGB half floats, unsigned, 16 bytes per block: HDR color such as skies and light maps.
    BC6H,
    /// ASTC LDR RGBA, 16 bytes per block of 4x4, 6x6 or 8x8 texels: 8, 3.56 or 2 bits per texel.
    ASTC4x4,
    ASTC6x6,
    ASTC8x8,
};

MENUM()
enum class TextureColorSpace : uint8
{
    /// Data such as normals and masks.
    Linear,
    /// Color the GPU decodes from sRGB when it samples.
    Srgb,
};

/// One mip level: its size in texels and where its texels lie in the asset's bulk data.
MSTRUCT()
struct TextureMip
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Width = 0;
    MPROPERTY()
    uint32 Height = 0;
    /// Bytes from one row of texels, or of blocks, to the next.
    MPROPERTY()
    uint32 RowPitch = 0;
    MPROPERTY()
    uint64 Offset = 0;
    MPROPERTY()
    uint64 Size = 0;
};

/// A 2D texture (Documentation/Architecture/06 §4.1): the block holds the description, the texels of every mip are
/// in the bulk data, largest first, so the renderer streams the mips it needs straight to the GPU.
MCLASS(PackageClass = "GpuTexture")
class TEXTURE_API Texture final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    TextureFormat Format = TextureFormat::RGBA8;
    MPROPERTY()
    TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
    MPROPERTY()
    uint32 Width = 0;
    MPROPERTY()
    uint32 Height = 0;
    MPROPERTY()
    FrozenArray<TextureMip> Mips;

    Texture() = default;
    ~Texture() override = default;
    MARLIN_NONCOPYABLE(Texture);
};

namespace TextureFormats
{
/// Texels along each side of a block: 4 for BC, 4 to 8 for ASTC, 1 for uncompressed formats.
[[nodiscard]] TEXTURE_API uint32 GetBlockSize(TextureFormat format);
/// Bytes per block, or per texel for uncompressed formats.
[[nodiscard]] TEXTURE_API uint32 GetBlockBytes(TextureFormat format);
[[nodiscard]] TEXTURE_API bool IsCompressed(TextureFormat format);
/// True for formats of floating-point texels, which converters encode from linear float data.
[[nodiscard]] TEXTURE_API bool IsFloat(TextureFormat format);
/// The layout of a *width* by *height* mip: bytes per row of blocks and in all.
TEXTURE_API void GetMipLayout(TextureFormat format, uint32 width, uint32 height, uint32 &rowPitch, uint64 &size);
} // namespace TextureFormats
} // namespace Marlin
