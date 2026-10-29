#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "MarlinText/BinaryFileLink.h"
#include "UIIcon/Icon.h"
#include "IconPipeline/IconConverter.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(ICONPIPELINE_API, LogIconPipeline, Info, Warning);

namespace Marlin
{
/// How an SVG file becomes an Icon.
MSTRUCT()
struct IconSource
{
    MGENERATED_BODY()

    MPROPERTY()
    BinaryFileLink SourceFile;
};

/// Bakes the SVG into the Icon's shapes (ParseSvgIcon): transforms, arcs and basic shapes become plain paths, so
/// drawing only scales the points. What it skips or approximates is logged as a warning.
MCLASS()
class ICONPIPELINE_API IconConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "IconSource";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<Icon>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Convert(ConversionContext &context) override;
};

/// Imports .svg files: one IconSource per file, named after it.
MCLASS()
class ICONPIPELINE_API SvgImporter final : public AssetImporter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanImport(const ImportContext &context) const override;
    bool Import(ImportContext &context) const override;
};
} // namespace Marlin
