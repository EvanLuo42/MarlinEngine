#pragma once

#include "CoreTypes.h"
#include "Math/Color.h"
#include "RHI/RhiShaderTypes.h"
#include "Shader/ShaderDeclaration.h"
#include "UIPainterRenderer/UIPainterShaders.gen.h"

namespace Marlin
{
/// The [Uniform] arguments of Count and Scatter (Shaders/Passes/Ui/UiBin.mshader): buffers are Spans of device
/// addresses, textures and samplers descriptor indices (MarlinLanguage.md §4.4, §4.5).
MSTRUCT()
struct UiBinArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan TileRects;
    MPROPERTY()
    ShaderSpan PairStarts;
    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    ShaderSpan TileCursors;
    MPROPERTY()
    ShaderSpan TileCommands;
    MPROPERTY()
    uint32 CommandCount = 0;
    MPROPERTY()
    uint32 PairCount = 0;
    MPROPERTY()
    uint32 TilesX = 0;
};

MSTRUCT()
struct UiScanArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    ShaderSpan TileOffsets;
    MPROPERTY()
    ShaderSpan TileCursors;
    MPROPERTY()
    uint32 TileCount = 0;
};

/// What both raster entry points read (Shaders/Passes/Ui/UiRaster.mshader).
MSTRUCT()
struct alignas(16) UiRasterInputs
{
    MGENERATED_BODY()

    MPROPERTY()
    LinearColor Clear = LinearColor::Transparent;
    MPROPERTY()
    ShaderSpan Commands;
    MPROPERTY()
    ShaderSpan TileOffsets;
    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    ShaderSpan TileCommands;
    MPROPERTY()
    ShaderSpan Brushes;
    MPROPERTY()
    ShaderSpan Stops;
    MPROPERTY()
    ShaderSpan Transforms;
    MPROPERTY()
    ShaderSpan Clips;
    MPROPERTY()
    ShaderSpan Curves;
    MPROPERTY()
    ShaderSpan OutlineWords;
    MPROPERTY()
    ShaderSpan GlyphCurves;
    MPROPERTY()
    ShaderSpan GlyphWords;
    MPROPERTY()
    ShaderSpan Images;
    MPROPERTY()
    uint32 Atlas = 0;
    MPROPERTY()
    uint32 Background = 0;
    MPROPERTY()
    uint32 LinearSampler = 0;
    MPROPERTY()
    uint32 PointSampler = 0;
    MPROPERTY()
    uint32 TilesX = 0;
    MPROPERTY()
    uint32 FirstCommand = 0;
    MPROPERTY()
    uint32 EndCommand = 0;
    /// Encoding (0 linear, 1 sRGB, 2 PQ) in the low byte; 0x100 when Background holds earlier commands.
    MPROPERTY()
    uint32 Flags = 0;
    MPROPERTY()
    float32 OriginX = 0;
    MPROPERTY()
    float32 OriginY = 0;
    MPROPERTY()
    float32 WhiteNits = 203;
};
static_assert(sizeof(UiRasterInputs) == 272);

/// 288 bytes, more than the arguments passed directly, so ShaderLibrary passes them through upload memory (§9.3).
MSTRUCT()
struct alignas(16) UiRasterArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    UiRasterInputs Inputs;
    MPROPERTY()
    uint32 Output = 0;
};

MSTRUCT()
struct UiGlyphAtlasArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Glyphs;
    MPROPERTY()
    ShaderSpan GlyphCurves;
    MPROPERTY()
    ShaderSpan GlyphWords;
    MPROPERTY()
    uint32 Atlas = 0;
    MPROPERTY()
    uint32 GlyphCount = 0;
};

/// One glyph the atlas pass rasterizes (UiAtlasGlyph in Shaders/Passes/Ui/UiGlyphAtlas.mshader).
struct UiAtlasGlyph
{
    uint32 Record = 0;
    uint32 X = 0;
    uint32 Y = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    float32 Scale = 0;
    float32 OriginX = 0;
    float32 OriginY = 0;
    float32 Gamma = 1;
};
static_assert(sizeof(UiAtlasGlyph) == 36);

MSTRUCT()
struct UiBlurArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Source = 0;
    MPROPERTY()
    uint32 Target = 0;
    MPROPERTY()
    uint32 Horizontal = 0;
    MPROPERTY()
    int32 OffsetX = 0;
    MPROPERTY()
    int32 OffsetY = 0;
    MPROPERTY()
    float32 Sigma = 0;
};

inline constexpr ComputeShader<UiBinArguments> UiCountShader{"Marlin.Passes.Ui.Count"};
inline constexpr ComputeShader<UiScanArguments> UiScanShader{"Marlin.Passes.Ui.Scan"};
inline constexpr ComputeShader<UiBinArguments> UiScatterShader{"Marlin.Passes.Ui.Scatter"};
inline constexpr ComputeShader<UiRasterArguments> UiRasterSurfaceShader{"Marlin.Passes.Ui.RasterSurface"};
inline constexpr ComputeShader<UiRasterArguments> UiRasterLayerShader{"Marlin.Passes.Ui.RasterLayer"};
inline constexpr ComputeShader<UiGlyphAtlasArguments> UiGlyphAtlasShader{"Marlin.Passes.Ui.RasterGlyphs"};
inline constexpr ComputeShader<UiBlurArguments> UiBlurShader{"Marlin.Passes.Ui.Blur"};
} // namespace Marlin
