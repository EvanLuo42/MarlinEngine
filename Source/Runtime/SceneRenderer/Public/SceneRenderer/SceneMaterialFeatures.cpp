#include "SceneRenderer/SceneMaterialFeatures.h"

#include "Math/MathUtility.h"
#include "RHI/RhiDevice.h"
#include "Reflection/TypeInfo.h"
#include "SceneRenderer/SceneFeatureHelpers.h"
#include "SceneRenderer/SceneGpuTypes.h"
#include "SceneRenderer/SceneMaterialShaders.h"
#include "SceneRenderer/SceneRendererSubsystem.h"
#include "SceneRenderer/SceneVisBufferFeatures.h"
#include "Shader/ShaderLibrary.h"

namespace Marlin
{
namespace
{
using namespace SceneFeatureHelpers;

/// Whether the view shows its visibility buffer (a Buffer mode) rather than its lit materials.
bool ShowsBuffer(const SceneViewContext &context)
{
    const SceneViewMode *mode = context.View.Mode;
    return mode != nullptr && mode->GetType().IsA(TypeOf<SceneVisBufferViewMode>());
}

/// Whether the view needs the material stage's closures: lit, or a Buffer mode that shows them.
bool NeedsClosures(const SceneViewContext &context)
{
    return !ShowsBuffer(context) || static_cast<const SceneVisBufferViewMode *>(context.View.Mode)->ShowsClosures();
}
} // namespace

void SceneMaterialFeature::AddPasses(SceneViewContext &context)
{
    SceneViewTextures &textures = context.Textures;
    const SceneGpuResources &scene = context.Scene;
    if (!textures.VisBuffer.IsValid() || !NeedsClosures(context) || !scene.MaterialSlots.IsValid())
    {
        return;
    }
    RenderGraphBuilder &graph = context.Graph;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 width = context.View.Width;
    const uint32 height = context.View.Height;
    const uint32 pixels = width * height;
    const uint32 tilesX = (width + 7) / 8;
    const uint32 tileCount = tilesX * ((height + 7) / 8);
    const auto permutations = static_cast<uint32>(scene.Materials.Count());
    textures.TilesX = tilesX;
    textures.TileCount = tileCount;
    // Room for every pixel to have one closure in the records besides its plane's: most have none.
    textures.RecordCapacity = Max(pixels, 4096u);
    textures.MaterialBins = CreateBuffer(graph, "Scene Material Bins", uint64{pixels} * sizeof(uint32));
    textures.ClosureHeaders = CreateBuffer(graph, "Scene Closure Headers", uint64{pixels} * sizeof(uint32));
    textures.ClosurePlane = CreateBuffer(graph, "Scene Closure Plane", uint64{pixels} * SceneSlabWords * sizeof(uint32));
    textures.ClosureRecords = CreateBuffer(graph, "Scene Closure Records",
                                           uint64{textures.RecordCapacity} * SceneClosureRecordWords * sizeof(uint32));
    textures.ClosureRecordCount = CreateBuffer(graph, "Scene Closure Record Count", sizeof(uint32));
    textures.TileFlags = CreateBuffer(graph, "Scene Tile Flags", uint64{tileCount} * sizeof(uint32));
    AddClear(graph, "Scene Clear Closure Headers", textures.ClosureHeaders, uint64{pixels} * sizeof(uint32));
    AddClear(graph, "Scene Clear Closure Record Count", textures.ClosureRecordCount, sizeof(uint32));
    AddClear(graph, "Scene Clear Tile Flags", textures.TileFlags, uint64{tileCount} * sizeof(uint32));
    const uint32 lists = Max(permutations, 1u);
    const RenderGraphBuffer tileLists =
        CreateBuffer(graph, "Scene Material Tile Lists", uint64{lists} * tileCount * sizeof(uint32));
    const RenderGraphBuffer tileCounts = CreateBuffer(graph, "Scene Material Tile Counts", uint64{lists} * sizeof(uint32));
    const RenderGraphBuffer resolveArguments =
        CreateBuffer(graph, "Scene Resolve Arguments", uint64{lists} * 3 * sizeof(uint32));
    AddClear(graph, "Scene Clear Material Tile Counts", tileCounts, uint64{lists} * sizeof(uint32));
    AddClear(graph, "Scene Clear Resolve Arguments", resolveArguments, uint64{lists} * 3 * sizeof(uint32));

    struct ClassifyData
    {
        RenderGraphView View;
        RenderGraphView VisBuffer;
        RenderGraphView Visible;
        RenderGraphView Clusters;
        RenderGraphView Instances;
        RenderGraphView Meshes;
        RenderGraphView Sections;
        RenderGraphView MaterialBindings;
        RenderGraphView MaterialSlots;
        RenderGraphView Bins;
        RenderGraphView TileLists;
        RenderGraphView TileCounts;
    };
    graph.AddPass<ClassifyData>(
        "Scene Classify Materials", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, ClassifyData &data) {
            data.View = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            data.VisBuffer = pass.ReadBuffer(textures.VisBuffer, Structured(sizeof(uint64)));
            data.Visible = pass.ReadBuffer(textures.VisibleClusters, Structured(sizeof(SceneGpuVisibleCluster)));
            data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Meshes = pass.ReadBuffer(scene.Meshes, Structured(sizeof(SceneGpuMesh)));
            data.Sections = pass.ReadBuffer(scene.Sections, Structured(sizeof(uint32)));
            data.MaterialBindings = pass.ReadBuffer(scene.MaterialBindings, Structured(sizeof(uint32)));
            data.MaterialSlots = pass.ReadBuffer(scene.MaterialSlots, Structured(sizeof(uint32)));
            data.Bins = pass.OverwriteBuffer(textures.MaterialBins, Structured(sizeof(uint32)));
            data.TileLists = pass.OverwriteBuffer(tileLists, Structured(sizeof(uint32)));
            data.TileCounts = pass.WriteBuffer(tileCounts, Structured(sizeof(uint32)));
        },
        [shaders, tileCount, tilesX, permutations, width, height](const ClassifyData &data, RenderGraphContext &pass) {
            SceneClassifyArguments arguments;
            arguments.View = pass.GetSpan(data.View);
            arguments.VisBuffer = pass.GetSpan(data.VisBuffer);
            arguments.Visible = pass.GetSpan(data.Visible);
            arguments.Clusters = pass.GetSpan(data.Clusters);
            arguments.Instances = pass.GetSpan(data.Instances);
            arguments.Meshes = pass.GetSpan(data.Meshes);
            arguments.Sections = pass.GetSpan(data.Sections);
            arguments.MaterialBindings = pass.GetSpan(data.MaterialBindings);
            arguments.MaterialSlots = pass.GetSpan(data.MaterialSlots);
            arguments.Bins = pass.GetSpan(data.Bins);
            arguments.TileLists = pass.GetSpan(data.TileLists);
            arguments.TileCounts = pass.GetSpan(data.TileCounts);
            arguments.TileCapacity = tileCount;
            arguments.TilesX = tilesX;
            arguments.PermutationCount = permutations;
            (void)shaders->DispatchThreads(pass.List, SceneClassifyShader, arguments, width, height);
        });

    struct ArgumentsData
    {
        RenderGraphView TileCounts;
        RenderGraphView Arguments;
    };
    graph.AddPass<ArgumentsData>(
        "Scene Resolve Arguments", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, ArgumentsData &data) {
            data.TileCounts = pass.ReadBuffer(tileCounts, Structured(sizeof(uint32)));
            data.Arguments = pass.WriteBuffer(resolveArguments, Structured(sizeof(uint32)));
        },
        [shaders, tileCount, permutations](const ArgumentsData &data, RenderGraphContext &pass) {
            SceneResolveArgumentsArguments arguments;
            arguments.TileCounts = pass.GetSpan(data.TileCounts);
            arguments.Arguments = pass.GetSpan(data.Arguments);
            arguments.TileCapacity = tileCount;
            arguments.PermutationCount = permutations;
            arguments.GroupsX = GroupsPerRow;
            (void)shaders->DispatchThreads(pass.List, SceneResolveArgumentsShader, arguments, Max(permutations, 1u));
        });

    // A material pass per permutation, each over its own tiles.
    struct ResolveData
    {
        RenderGraphView View;
        RenderGraphView VisBuffer;
        RenderGraphView Visible;
        RenderGraphView Clusters;
        RenderGraphView Instances;
        RenderGraphView Meshes;
        RenderGraphView Sections;
        RenderGraphView MaterialBindings;
        RenderGraphView MaterialSlots;
        RenderGraphView Geometry;
        RenderGraphView Bins;
        RenderGraphView TileLists;
        RenderGraphView TileCounts;
        RenderGraphView Headers;
        RenderGraphView Plane;
        RenderGraphView Records;
        RenderGraphView RecordCount;
        RenderGraphView TileFlags;
        RenderGraphView Materials;
    };
    for (uint32 permutation = 0; permutation < permutations; ++permutation)
    {
        const SceneGpuResources::MaterialPermutation &table = scene.Materials[permutation];
        const ComputeShader<SceneResolveArguments> *shader =
            &context.Shaders.GetMaterialShader(SceneResolveShader, table.Shader, table.Variant);
        const uint32 recordCapacity = textures.RecordCapacity;
        graph.AddPass<ResolveData>(
            "Scene Resolve Material", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, ResolveData &data) {
                data.View = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
                data.VisBuffer = pass.ReadBuffer(textures.VisBuffer, Structured(sizeof(uint64)));
                data.Visible = pass.ReadBuffer(textures.VisibleClusters, Structured(sizeof(SceneGpuVisibleCluster)));
                data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
                data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
                data.Meshes = pass.ReadBuffer(scene.Meshes, Structured(sizeof(SceneGpuMesh)));
                data.Sections = pass.ReadBuffer(scene.Sections, Structured(sizeof(uint32)));
                data.MaterialBindings = pass.ReadBuffer(scene.MaterialBindings, Structured(sizeof(uint32)));
                data.MaterialSlots = pass.ReadBuffer(scene.MaterialSlots, Structured(sizeof(uint32)));
                data.Geometry = pass.ReadBuffer(scene.Geometry);
                data.Bins = pass.ReadBuffer(textures.MaterialBins, Structured(sizeof(uint32)));
                data.TileLists = pass.ReadBuffer(tileLists, Structured(sizeof(uint32)));
                data.TileCounts = pass.ReadBuffer(tileCounts, Structured(sizeof(uint32)));
                data.Headers = pass.WriteBuffer(textures.ClosureHeaders, Structured(sizeof(uint32)));
                data.Plane = permutation == 0 ? pass.OverwriteBuffer(textures.ClosurePlane, Structured(sizeof(uint32)))
                                              : pass.WriteBuffer(textures.ClosurePlane, Structured(sizeof(uint32)));
                data.Records = permutation == 0
                                   ? pass.OverwriteBuffer(textures.ClosureRecords, Structured(sizeof(uint32)))
                                   : pass.WriteBuffer(textures.ClosureRecords, Structured(sizeof(uint32)));
                data.RecordCount = pass.WriteBuffer(textures.ClosureRecordCount, Structured(sizeof(uint32)));
                data.TileFlags = pass.WriteBuffer(textures.TileFlags, Structured(sizeof(uint32)));
                data.Materials = pass.ReadBuffer(table.Parameters, Structured(Max(table.Stride, 4u)));
                (void)pass.Buffer(resolveArguments, RhiAccess::IndirectArguments);
            },
            [shaders, shader, tileCount, tilesX, permutation, recordCapacity, resolveArguments,
             count = table.Count](const ResolveData &data, RenderGraphContext &pass) {
                SceneResolveArguments arguments;
                arguments.View = pass.GetSpan(data.View);
                arguments.VisBuffer = pass.GetSpan(data.VisBuffer);
                arguments.Visible = pass.GetSpan(data.Visible);
                arguments.Clusters = pass.GetSpan(data.Clusters);
                arguments.Instances = pass.GetSpan(data.Instances);
                arguments.Meshes = pass.GetSpan(data.Meshes);
                arguments.Sections = pass.GetSpan(data.Sections);
                arguments.MaterialBindings = pass.GetSpan(data.MaterialBindings);
                arguments.MaterialSlots = pass.GetSpan(data.MaterialSlots);
                arguments.Geometry = pass.GetAddress(data.Geometry);
                arguments.Bins = pass.GetSpan(data.Bins);
                arguments.TileLists = pass.GetSpan(data.TileLists);
                arguments.TileCounts = pass.GetSpan(data.TileCounts);
                arguments.Headers = pass.GetSpan(data.Headers);
                arguments.Plane = pass.GetSpan(data.Plane);
                arguments.Records = pass.GetSpan(data.Records);
                arguments.RecordCount = pass.GetSpan(data.RecordCount);
                arguments.TileFlags = pass.GetSpan(data.TileFlags);
                arguments.RecordCapacity = recordCapacity;
                arguments.TileCapacity = tileCount;
                arguments.TilesX = tilesX;
                arguments.GroupsX = GroupsPerRow;
                arguments.Permutation = permutation;
                arguments.Materials = pass.GetSpan(data.Materials);
                arguments.Materials.Length = count;
                (void)shaders->DispatchIndirect(pass.List, *shader, arguments, pass.GetBuffer(resolveArguments),
                                                uint64{permutation} * 3 * sizeof(uint32));
            });
    }
}

void SceneLightingFeature::AddPasses(SceneViewContext &context)
{
    SceneViewTextures &textures = context.Textures;
    const SceneGpuResources &scene = context.Scene;
    if (!textures.ClosureHeaders.IsValid() || ShowsBuffer(context))
    {
        return;
    }
    RenderGraphBuilder &graph = context.Graph;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 width = context.View.Width;
    const uint32 height = context.View.Height;
    const uint32 tileCount = textures.TileCount;
    const uint32 tilesX = textures.TilesX;
    RhiTextureDesc desc;
    desc.Format = RhiFormat::RGBA16Float;
    desc.Width = width;
    desc.Height = height;
    desc.DebugName = "Scene Radiance";
    textures.Radiance = graph.CreateTexture("Scene Radiance", desc);
    const RenderGraphBuffer lights = scene.Lights;
    const RenderGraphBuffer classLists =
        CreateBuffer(graph, "Scene Tile Class Lists", uint64{tileCount} * SceneTileClasses * sizeof(uint32));
    const RenderGraphBuffer classCounts = CreateBuffer(graph, "Scene Tile Class Counts", SceneTileClasses * sizeof(uint32));
    const RenderGraphBuffer lightArguments =
        CreateBuffer(graph, "Scene Light Arguments", SceneTileClasses * 3 * sizeof(uint32));
    AddClear(graph, "Scene Clear Tile Class Counts", classCounts, SceneTileClasses * sizeof(uint32));
    AddClear(graph, "Scene Clear Light Arguments", lightArguments, SceneTileClasses * 3 * sizeof(uint32));

    struct ListData
    {
        RenderGraphView TileFlags;
        RenderGraphView Lists;
        RenderGraphView Counts;
    };
    graph.AddPass<ListData>(
        "Scene Tile Classes", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, ListData &data) {
            data.TileFlags = pass.ReadBuffer(textures.TileFlags, Structured(sizeof(uint32)));
            data.Lists = pass.OverwriteBuffer(classLists, Structured(sizeof(uint32)));
            data.Counts = pass.WriteBuffer(classCounts, Structured(sizeof(uint32)));
        },
        [shaders, tileCount, classify = SceneRendererSubsystem::MaterialTileClasses](const ListData &data,
                                                                                  RenderGraphContext &pass) {
            SceneTileListArguments arguments;
            arguments.TileFlags = pass.GetSpan(data.TileFlags);
            arguments.Lists = pass.GetSpan(data.Lists);
            arguments.Counts = pass.GetSpan(data.Counts);
            arguments.TileCount = tileCount;
            arguments.TileCapacity = tileCount;
            arguments.Classify = classify ? 1u : 0u;
            (void)shaders->DispatchThreads(pass.List, SceneTileListShader, arguments, tileCount);
        });
    struct ArgumentsData
    {
        RenderGraphView Counts;
        RenderGraphView Arguments;
    };
    graph.AddPass<ArgumentsData>(
        "Scene Light Arguments", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, ArgumentsData &data) {
            data.Counts = pass.ReadBuffer(classCounts, Structured(sizeof(uint32)));
            data.Arguments = pass.WriteBuffer(lightArguments, Structured(sizeof(uint32)));
        },
        [shaders](const ArgumentsData &data, RenderGraphContext &pass) {
            SceneLightArgumentsArguments arguments;
            arguments.Counts = pass.GetSpan(data.Counts);
            arguments.Arguments = pass.GetSpan(data.Arguments);
            arguments.GroupsX = GroupsPerRow;
            (void)shaders->Dispatch(pass.List, SceneLightArgumentsShader, arguments, 1);
        });

    // The tile classes' passes, then a pass for each custom BSDF in use over the Complex tiles.
    List<const ComputeShader<SceneLightArguments> *> passes;
    List<uint32> classes;
    for (uint32 kind = 0; kind < SceneTileClasses; ++kind)
    {
        passes.Add(&SceneLightShaders[kind]);
        classes.Add(kind);
    }
    List<StringView> bsdfs;
    for (const SceneGpuResources::MaterialPermutation &table : scene.Materials)
    {
        for (const String &bsdf : table.CustomBsdfs)
        {
            if (!bsdfs.Contains(bsdf.AsView()))
            {
                bsdfs.Add(bsdf.AsView());
                passes.Add(&context.Shaders.GetMaterialShader(SceneLightCustomShader, bsdf.AsView()));
                classes.Add(2);
            }
        }
    }
    struct LightData
    {
        RenderGraphView View;
        RenderGraphView Lights;
        RenderGraphView Headers;
        RenderGraphView Plane;
        RenderGraphView Records;
        RenderGraphView TileLists;
        RenderGraphView TileCounts;
        RenderGraphView Color;
    };
    for (usize index = 0; index < passes.Count(); ++index)
    {
        const ComputeShader<SceneLightArguments> *shader = passes[index];
        const uint32 kind = classes[index];
        graph.AddPass<LightData>(
            index < SceneTileClasses ? StringView("Scene Light Tiles") : StringView("Scene Light Custom BSDF"),
            RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, LightData &data) {
                data.View = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
                data.Lights = pass.ReadBuffer(lights, Structured(sizeof(SceneGpuLights)));
                data.Headers = pass.ReadBuffer(textures.ClosureHeaders, Structured(sizeof(uint32)));
                data.Plane = pass.ReadBuffer(textures.ClosurePlane, Structured(sizeof(uint32)));
                data.Records = pass.ReadBuffer(textures.ClosureRecords, Structured(sizeof(uint32)));
                data.TileLists = pass.ReadBuffer(classLists, Structured(sizeof(uint32)));
                data.TileCounts = pass.ReadBuffer(classCounts, Structured(sizeof(uint32)));
                data.Color = index == 0 ? pass.OverwriteTexture(textures.Radiance) : pass.WriteTexture(textures.Radiance);
                (void)pass.Buffer(lightArguments, RhiAccess::IndirectArguments);
            },
            [shaders, shader, kind, tileCount, tilesX, lightArguments](const LightData &data,
                                                                       RenderGraphContext &pass) {
                SceneLightArguments arguments;
                arguments.View = pass.GetSpan(data.View);
                arguments.Lights = pass.GetSpan(data.Lights);
                arguments.Headers = pass.GetSpan(data.Headers);
                arguments.Plane = pass.GetSpan(data.Plane);
                arguments.Records = pass.GetSpan(data.Records);
                arguments.TileLists = pass.GetSpan(data.TileLists);
                arguments.TileCounts = pass.GetSpan(data.TileCounts);
                arguments.Color = pass.GetDescriptorIndex(data.Color);
                arguments.TileCapacity = tileCount;
                arguments.TilesX = tilesX;
                arguments.GroupsX = GroupsPerRow;
                (void)shaders->DispatchIndirect(pass.List, *shader, arguments, pass.GetBuffer(lightArguments),
                                                uint64{kind} * 3 * sizeof(uint32));
            });
    }
}

void SceneTonemapFeature::AddPasses(SceneViewContext &context)
{
    SceneViewTextures &textures = context.Textures;
    if (!textures.Radiance.IsValid() || ShowsBuffer(context))
    {
        return;
    }
    RenderGraphBuilder &graph = context.Graph;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 width = context.View.Width;
    const uint32 height = context.View.Height;
    RhiTextureDesc desc;
    desc.Format = RhiFormat::RGBA16Float;
    desc.Width = width;
    desc.Height = height;
    desc.DebugName = "Scene Color";
    textures.SceneColor = graph.CreateTexture("Scene Color", desc);
    // EV100 as a camera meters it: 1 / (1.2 · 2^EV100).
    const float32 exposure = 1.0f / (1.2f * Math::Exp2(SceneRendererSubsystem::Exposure));
    struct TonemapData
    {
        RenderGraphView Radiance;
        RenderGraphView Output;
    };
    graph.AddPass<TonemapData>(
        "Scene Tonemap", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, TonemapData &data) {
            data.Radiance = pass.WriteTexture(textures.Radiance);
            data.Output = pass.OverwriteTexture(textures.SceneColor);
        },
        [shaders, width, height, exposure](const TonemapData &data, RenderGraphContext &pass) {
            SceneTonemapArguments arguments;
            arguments.Radiance = pass.GetDescriptorIndex(data.Radiance);
            arguments.Output = pass.GetDescriptorIndex(data.Output);
            arguments.Width = width;
            arguments.Height = height;
            arguments.Exposure = exposure;
            (void)shaders->DispatchThreads(pass.List, SceneTonemapShader, arguments, width, height);
        });
}
} // namespace Marlin
