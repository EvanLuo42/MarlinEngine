#pragma once

#include "CoreTypes.h"
#include "RHI/RhiShaderTypes.h"
#include "Shader/ShaderDeclaration.h"
#include "SceneRenderer/SceneMaterialShaders.gen.h"

namespace Marlin
{
// The [Uniform] arguments of Language/Shaders/Passes/{Material,Lighting,Post}, laid out as the passes declare them
// (MarlinLanguage.md §10).

MSTRUCT()
struct SceneClassifyArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan View;
    MPROPERTY()
    ShaderSpan VisBuffer;
    MPROPERTY()
    ShaderSpan Visible;
    MPROPERTY()
    ShaderSpan Clusters;
    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Meshes;
    MPROPERTY()
    ShaderSpan Sections;
    MPROPERTY()
    ShaderSpan MaterialBindings;
    MPROPERTY()
    ShaderSpan MaterialSlots;
    MPROPERTY()
    ShaderSpan Bins;
    MPROPERTY()
    ShaderSpan TileLists;
    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    uint32 TileCapacity = 0;
    MPROPERTY()
    uint32 TilesX = 0;
    MPROPERTY()
    uint32 PermutationCount = 0;
};

MSTRUCT()
struct SceneResolveArgumentsArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    ShaderSpan Arguments;
    MPROPERTY()
    uint32 TileCapacity = 0;
    MPROPERTY()
    uint32 PermutationCount = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
};

MSTRUCT()
struct SceneResolveArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan View;
    MPROPERTY()
    ShaderSpan VisBuffer;
    MPROPERTY()
    ShaderSpan Visible;
    MPROPERTY()
    ShaderSpan Clusters;
    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Meshes;
    MPROPERTY()
    ShaderSpan Sections;
    MPROPERTY()
    ShaderSpan MaterialBindings;
    MPROPERTY()
    ShaderSpan MaterialSlots;
    MPROPERTY()
    uint64 Geometry = 0;
    MPROPERTY()
    ShaderSpan Bins;
    MPROPERTY()
    ShaderSpan TileLists;
    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    ShaderSpan Headers;
    MPROPERTY()
    ShaderSpan Plane;
    MPROPERTY()
    ShaderSpan Records;
    MPROPERTY()
    ShaderSpan RecordCount;
    MPROPERTY()
    ShaderSpan TileFlags;
    MPROPERTY()
    uint32 RecordCapacity = 0;
    MPROPERTY()
    uint32 TileCapacity = 0;
    MPROPERTY()
    uint32 TilesX = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
    MPROPERTY()
    uint32 Permutation = 0;
    /// The permutation's material table.
    MPROPERTY()
    ShaderSpan Materials;
};

MSTRUCT()
struct SceneTileListArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan TileFlags;
    MPROPERTY()
    ShaderSpan Lists;
    MPROPERTY()
    ShaderSpan Counts;
    MPROPERTY()
    uint32 TileCount = 0;
    MPROPERTY()
    uint32 TileCapacity = 0;
    MPROPERTY()
    uint32 Classify = 1;
};

MSTRUCT()
struct SceneLightArgumentsArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Counts;
    MPROPERTY()
    ShaderSpan Arguments;
    MPROPERTY()
    uint32 GroupsX = 0;
};

MSTRUCT()
struct SceneLightArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan View;
    MPROPERTY()
    ShaderSpan Lights;
    MPROPERTY()
    ShaderSpan Headers;
    MPROPERTY()
    ShaderSpan Plane;
    MPROPERTY()
    ShaderSpan Records;
    MPROPERTY()
    ShaderSpan TileLists;
    MPROPERTY()
    ShaderSpan TileCounts;
    MPROPERTY()
    uint32 Color = 0;
    MPROPERTY()
    uint32 TileCapacity = 0;
    MPROPERTY()
    uint32 TilesX = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
};

MSTRUCT()
struct SceneTonemapArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Radiance = 0;
    MPROPERTY()
    uint32 Output = 0;
    MPROPERTY()
    uint32 Width = 0;
    MPROPERTY()
    uint32 Height = 0;
    MPROPERTY()
    float32 Exposure = 0;
};

/// Words of Closures.mshader: a Slab in the plane, an overflow record.
inline constexpr uint32 SceneSlabWords = 6;
inline constexpr uint32 SceneClosureRecordWords = 12;
/// Tile classes the lighting passes take: Simple, Single, Complex.
inline constexpr uint32 SceneTileClasses = 3;

inline constexpr ComputeShader<SceneClassifyArguments> SceneClassifyShader{"Marlin.Passes.Material.Classify"};
inline constexpr ComputeShader<SceneResolveArgumentsArguments> SceneResolveArgumentsShader{
    "Marlin.Passes.Material.BuildResolveArguments"};
/// A material pass: dispatched through ShaderLibrary::GetMaterialShader for each permutation.
inline constexpr ComputeShader<SceneResolveArguments> SceneResolveShader{"Marlin.Passes.Material.Resolve"};
inline constexpr ComputeShader<SceneTileListArguments> SceneTileListShader{"Marlin.Passes.Material.BuildTileLists"};
inline constexpr ComputeShader<SceneLightArgumentsArguments> SceneLightArgumentsShader{
    "Marlin.Passes.Material.BuildLightArguments"};
inline constexpr ComputeShader<SceneLightArguments> SceneLightShaders[SceneTileClasses] = {
    ComputeShader<SceneLightArguments>{"Marlin.Passes.Lighting.LightSimple"},
    ComputeShader<SceneLightArguments>{"Marlin.Passes.Lighting.LightSingle"},
    ComputeShader<SceneLightArguments>{"Marlin.Passes.Lighting.LightComplex"},
};
/// A custom BSDF's lighting: dispatched through ShaderLibrary::GetMaterialShader for each custom BSDF in use.
inline constexpr ComputeShader<SceneLightArguments> SceneLightCustomShader{"Marlin.Passes.Lighting.LightCustom"};
inline constexpr ComputeShader<SceneTonemapArguments> SceneTonemapShader{"Marlin.Passes.Post.Tonemap"};
} // namespace Marlin
