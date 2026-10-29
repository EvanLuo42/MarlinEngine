#pragma once

#include "CoreTypes.h"
#include "RHI/RhiShaderTypes.h"
#include "Shader/ShaderDeclaration.h"
#include "SceneRenderer/SceneShaders.gen.h"

namespace Marlin
{
// The [Uniform] arguments of the scene passes (Language/Shaders/Passes/Scene/), field for field; ShaderLibrary checks
// them against the entry points when they load. Buffers are Spans of device addresses, textures bindless indices.

MSTRUCT()
struct SceneScatterArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Updates;
    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    uint32 UpdateCount = 0;
};

MSTRUCT()
struct SceneLodInstanceArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Meshes;
    MPROPERTY()
    ShaderSpan Views;
    MPROPERTY()
    ShaderSpan Queue;
    MPROPERTY()
    ShaderSpan State;
    MPROPERTY()
    ShaderSpan Occluded;
    MPROPERTY()
    uint32 Hzb = 0;
    MPROPERTY()
    uint32 InstanceCount = 0;
    MPROPERTY()
    uint32 QueueCapacity = 0;
    MPROPERTY()
    uint32 OccludedCapacity = 0;
    MPROPERTY()
    uint32 HzbMips = 0;
    MPROPERTY()
    uint32 Flags = 0;
};

MSTRUCT()
struct SceneTraverseArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Meshes;
    MPROPERTY()
    ShaderSpan Nodes;
    MPROPERTY()
    ShaderSpan Groups;
    MPROPERTY()
    ShaderSpan PageTable;
    MPROPERTY()
    ShaderSpan PageRequested;
    MPROPERTY()
    ShaderSpan Requests;
    MPROPERTY()
    ShaderSpan Views;
    MPROPERTY()
    ShaderSpan Queue;
    MPROPERTY()
    ShaderSpan State;
    MPROPERTY()
    ShaderSpan VisibleGroups;
    MPROPERTY()
    ShaderSpan Occluded;
    MPROPERTY()
    uint32 Hzb = 0;
    MPROPERTY()
    uint32 QueueCapacity = 0;
    MPROPERTY()
    uint32 MaxGroups = 0;
    MPROPERTY()
    uint32 MaxRequests = 0;
    MPROPERTY()
    float32 LodThreshold = 0;
    MPROPERTY()
    uint32 Frame = 0;
    MPROPERTY()
    uint32 OccludedCapacity = 0;
    MPROPERTY()
    uint32 HzbMips = 0;
    /// SceneCullHzb: test nodes against the HZB.
    MPROPERTY()
    uint32 Flags = 0;
    MPROPERTY()
    uint32 Phase = 0;
    MPROPERTY()
    uint32 ThreadCount = 0;
};

MSTRUCT()
struct SceneGroupArgumentsArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan State;
    MPROPERTY()
    ShaderSpan Requests;
    MPROPERTY()
    ShaderSpan Arguments;
    MPROPERTY()
    uint32 MaxGroups = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
    MPROPERTY()
    uint32 OccludedCapacity = 0;
    MPROPERTY()
    uint32 Phase = 0;
};

MSTRUCT()
struct SceneCullGroupsArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Meshes;
    MPROPERTY()
    ShaderSpan Groups;
    MPROPERTY()
    ShaderSpan Clusters;
    MPROPERTY()
    ShaderSpan PageTable;
    MPROPERTY()
    ShaderSpan SlotUsed;
    MPROPERTY()
    ShaderSpan Views;
    MPROPERTY()
    uint32 Hzb = 0;
    MPROPERTY()
    ShaderSpan VisibleGroups;
    MPROPERTY()
    ShaderSpan State;
    MPROPERTY()
    ShaderSpan Visible;
    MPROPERTY()
    ShaderSpan Candidates;
    MPROPERTY()
    ShaderSpan Hardware;
    MPROPERTY()
    ShaderSpan Software;
    MPROPERTY()
    ShaderSpan Counts;
    MPROPERTY()
    uint32 HzbMips = 0;
    MPROPERTY()
    uint32 MaxVisible = 0;
    MPROPERTY()
    uint32 MaxGroups = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
    MPROPERTY()
    float32 LodThreshold = 0;
    MPROPERTY()
    uint32 Frame = 0;
    /// SceneCullHzb: test against the HZB.
    MPROPERTY()
    uint32 Flags = 0;
    /// Clusters whose triangle edges are shorter than this many pixels on screen go to the software raster.
    MPROPERTY()
    float32 SoftwarePixels = 0;
    MPROPERTY()
    uint32 Phase = 0;
};

/// SceneCullGroupsArguments::Flags.
inline constexpr uint32 SceneCullHzb = 1;

MSTRUCT()
struct SceneCandidateArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Clusters;
    MPROPERTY()
    ShaderSpan Views;
    MPROPERTY()
    uint32 Hzb = 0;
    MPROPERTY()
    ShaderSpan Candidates;
    MPROPERTY()
    ShaderSpan Visible;
    MPROPERTY()
    ShaderSpan Hardware;
    MPROPERTY()
    ShaderSpan Software;
    MPROPERTY()
    ShaderSpan Counts;
    MPROPERTY()
    uint32 HzbMips = 0;
    MPROPERTY()
    uint32 MaxVisible = 0;
    MPROPERTY()
    float32 SoftwarePixels = 0;
};

MSTRUCT()
struct SceneRasterArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan Counts;
    MPROPERTY()
    ShaderSpan Arguments;
    MPROPERTY()
    ShaderSpan SoftwareArguments;
    MPROPERTY()
    ShaderSpan CandidateArguments;
    MPROPERTY()
    uint32 MaxVisible = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
    MPROPERTY()
    uint32 Phase = 0;
};

MSTRUCT()
struct SceneHzbFromVisBufferArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan VisBuffer;
    MPROPERTY()
    uint32 Output = 0;
    MPROPERTY()
    uint32 Width = 0;
    MPROPERTY()
    uint32 Height = 0;
};

MSTRUCT()
struct SceneHzbMipArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Source = 0;
    MPROPERTY()
    uint32 Output = 0;
};

MSTRUCT()
struct SceneVisBufferArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan View;
    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Clusters;
    MPROPERTY()
    uint64 Geometry = 0;
    MPROPERTY()
    ShaderSpan Visible;
    /// The visible clusters the raster draws, by their index in Visible.
    MPROPERTY()
    ShaderSpan Listed;
    MPROPERTY()
    ShaderSpan Count;
    MPROPERTY()
    ShaderSpan VisBuffer;
    /// With SceneRasterOverdraw, a uint per pixel counting the fragments rastered there.
    MPROPERTY()
    ShaderSpan Overdraw;
    MPROPERTY()
    uint32 MaxVisible = 0;
    MPROPERTY()
    uint32 GroupsX = 0;
    MPROPERTY()
    uint32 Phase = 0;
    /// SceneRasterOverdraw: count fragments in Overdraw.
    MPROPERTY()
    uint32 Flags = 0;
};

/// SceneVisBufferArguments::Flags.
inline constexpr uint32 SceneRasterOverdraw = 1;

MSTRUCT()
struct SceneVisualizeArguments
{
    MGENERATED_BODY()

    MPROPERTY()
    ShaderSpan View;
    MPROPERTY()
    ShaderSpan Instances;
    MPROPERTY()
    ShaderSpan Clusters;
    MPROPERTY()
    uint64 Geometry = 0;
    MPROPERTY()
    ShaderSpan Visible;
    MPROPERTY()
    ShaderSpan VisBuffer;
    MPROPERTY()
    ShaderSpan Meshes;
    MPROPERTY()
    ShaderSpan Groups;
    MPROPERTY()
    ShaderSpan Overdraw;
    MPROPERTY()
    ShaderSpan Headers;
    MPROPERTY()
    ShaderSpan Plane;
    MPROPERTY()
    ShaderSpan Records;
    MPROPERTY()
    ShaderSpan TileFlags;
    MPROPERTY()
    uint32 Output = 0;
    MPROPERTY()
    uint32 Mode = 0;
    MPROPERTY()
    float32 SoftwarePixels = 0;
    MPROPERTY()
    uint32 TilesX = 0;
};

inline constexpr ComputeShader<SceneScatterArguments> SceneScatterShader{"Marlin.Passes.Scene.ScatterInstances"};
inline constexpr ComputeShader<SceneLodInstanceArguments> SceneLodInstanceShader{"Marlin.Passes.Scene.CullInstances"};
inline constexpr ComputeShader<SceneTraverseArguments> SceneTraverseShader{"Marlin.Passes.Scene.Traverse"};
inline constexpr ComputeShader<SceneGroupArgumentsArguments> SceneGroupArgumentsShader{
    "Marlin.Passes.Scene.BuildGroupArguments"};
inline constexpr ComputeShader<SceneCullGroupsArguments> SceneCullGroupsShader{"Marlin.Passes.Scene.CullGroups"};
inline constexpr ComputeShader<SceneCandidateArguments> SceneCandidateShader{"Marlin.Passes.Scene.CullCandidates"};
inline constexpr ComputeShader<SceneHzbFromVisBufferArguments> SceneHzbFromVisBufferShader{
    "Marlin.Passes.Scene.BuildHzbFromVisBuffer"};
inline constexpr ComputeShader<SceneHzbMipArguments> SceneHzbMipShader{"Marlin.Passes.Scene.BuildHzbMip"};
inline constexpr ComputeShader<SceneRasterArguments> SceneRasterArgumentsShader{
    "Marlin.Passes.Scene.BuildRasterArguments"};
inline constexpr MeshShader<SceneVisBufferArguments> SceneVisBufferMeshShader{"Marlin.Passes.Scene.VisBufferMesh"};
inline constexpr PixelShader<SceneVisBufferArguments> SceneVisBufferPixelShader{"Marlin.Passes.Scene.VisBufferPixel"};
inline constexpr ComputeShader<SceneVisBufferArguments> SceneSoftwareRasterShader{
    "Marlin.Passes.Scene.RasterizeSoftware"};
inline constexpr ComputeShader<SceneVisualizeArguments> SceneVisualizeShader{"Marlin.Passes.Scene.VisualizeVisBuffer"};

/// The visibility raster: no attachments, both faces (the pixel shader keeps the nearest with an atomic).
[[nodiscard]] inline ShaderGraphicsPipelineDesc GetSceneVisBufferPipeline()
{
    ShaderGraphicsPipelineDesc desc;
    desc.Mesh = &SceneVisBufferMeshShader;
    desc.Pixel = &SceneVisBufferPixelShader;
    desc.Raster.Cull = RhiCullMode::None;
    return desc;
}
} // namespace Marlin
