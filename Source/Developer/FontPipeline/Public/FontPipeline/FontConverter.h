#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "CoreTypes.h"
#include "MarlinText/BinaryFileLink.h"
#include "UIText/Font.h"
#include "FontPipeline/FontConverter.gen.h"

namespace Marlin
{
/// How a font file becomes a Font: which file, and which face of a collection.
MSTRUCT()
struct FontSource
{
    MGENERATED_BODY()

    MPROPERTY()
    BinaryFileLink SourceFile;
    /// The face within a .ttc collection; 0 for a single font.
    MPROPERTY()
    uint32 FaceIndex = 0;
};

/// Checks the sfnt header (and the collection's face count) and keeps the file whole: HarfBuzz reads its tables at
/// run time, so there is nothing to bake.
MCLASS()
class FONTPIPELINE_API FontConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "FontSource";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<Font>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Convert(ConversionContext &context) override;
};

/// Imports .ttf, .otf and .ttc files: one FontSource per face, named after the file (and the face index in a
/// collection).
MCLASS()
class FONTPIPELINE_API FontImporter final : public AssetImporter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanImport(const ImportContext &context) const override;
    bool Import(ImportContext &context) const override;
};

/// The faces an sfnt file holds: 1 for a font, the count for a collection, 0 if it is neither.
[[nodiscard]] FONTPIPELINE_API uint32 GetSfntFaceCount(Span<const uint8> data);
} // namespace Marlin
