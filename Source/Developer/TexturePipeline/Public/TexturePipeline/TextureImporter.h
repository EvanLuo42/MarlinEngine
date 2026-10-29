#pragma once

#include "AssetPipeline/AssetImporter.h"
#include "CoreTypes.h"
#include "TexturePipeline/TextureImporter.gen.h"

namespace Marlin
{
/// Imports every image format an ImageDecoder reads as one TextureSource. The kind follows the file name's suffix:
/// _N or _Normal is Normal, _M, _Mask or _ORM is Mask, _H or _Height is Grayscale, all three linear; .hdr and .exr
/// are Hdr; anything else is Color in sRGB.
MCLASS()
class TEXTUREPIPELINE_API TextureImporter final : public AssetImporter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanImport(const ImportContext &context) const override;
    bool Import(ImportContext &context) const override;
};
} // namespace Marlin
