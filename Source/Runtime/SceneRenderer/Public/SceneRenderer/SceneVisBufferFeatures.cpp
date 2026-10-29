#include "SceneRenderer/SceneVisBufferFeatures.h"

#include "RHI/RhiDevice.h"
#include "Reflection/TypeInfo.h"
#include "SceneRenderer/SceneFeatureHelpers.h"
#include "SceneRenderer/SceneGpuTypes.h"
#include "SceneRenderer/SceneRendererSubsystem.h"
#include "SceneRenderer/SceneShaders.h"
#include "Shader/ShaderLibrary.h"

namespace Marlin
{
namespace
{
using namespace SceneFeatureHelpers;

/// The SceneVisualize.mshader visualization of the view's mode; 0 (Unlit) for modes that are not Buffer modes.
uint32 GetVisualization(const SceneViewContext &context)
{
    const SceneViewMode *mode = context.View.Mode;
    return mode != nullptr && mode->GetType().IsA(TypeOf<SceneVisBufferViewMode>())
               ? static_cast<const SceneVisBufferViewMode *>(mode)->GetVisualization()
               : 0;
}

/// SceneOverdrawViewMode's visualization.
constexpr uint32 VisualizeOverdraw = 10;

/// A view's HZB as the last frame left it, for the first culling phase: R32F, mip 0 half the view's size rounded up
/// to a power of two (SceneHzb.mshader). Remade when the view changes size.
struct SceneHzbState
{
    SceneHzbState() = default;
    MARLIN_NONCOPYABLE(SceneHzbState);

    ~SceneHzbState()
    {
        if (Device != nullptr && Texture.Texture.IsValid())
        {
            Device->Destroy(Texture.Texture);
        }
    }

    RhiDevice *Device = nullptr;
    RenderGraphExternalTexture Texture;
    uint32 ViewWidth = 0;
    uint32 ViewHeight = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 Mips = 0;
    /// It holds what the last frame rendered.
    bool Valid = false;
};

uint32 NextPowerOfTwo(uint32 value)
{
    uint32 power = 1;
    while (power < value)
    {
        power *= 2;
    }
    return power;
}

/// Makes *hzb* fit a view of *width* × *height*; false if its texture cannot be made.
bool FitHzb(SceneHzbState &hzb, RhiDevice &device, uint32 width, uint32 height)
{
    if (hzb.Texture.Texture.IsValid() && hzb.ViewWidth == width && hzb.ViewHeight == height)
    {
        return true;
    }
    if (hzb.Texture.Texture.IsValid())
    {
        device.Destroy(hzb.Texture.Texture);
    }
    hzb.Texture = {};
    hzb.Valid = false;
    hzb.Device = &device;
    hzb.ViewWidth = width;
    hzb.ViewHeight = height;
    hzb.Width = NextPowerOfTwo((width + 1) / 2);
    hzb.Height = NextPowerOfTwo((height + 1) / 2);
    hzb.Mips = 1;
    while ((Max(hzb.Width, hzb.Height) >> hzb.Mips) > 0)
    {
        ++hzb.Mips;
    }
    RhiTextureDesc desc;
    desc.Format = RhiFormat::R32Float;
    desc.Width = hzb.Width;
    desc.Height = hzb.Height;
    desc.MipCount = hzb.Mips;
    desc.Usage = RhiTextureUsage::ShaderResource | RhiTextureUsage::UnorderedAccess;
    desc.DebugName = "Scene HZB";
    Result<RhiTexture, RhiError> texture = device.CreateTexture(desc);
    if (texture.IsError())
    {
        MARLIN_LOG(LogRenderer, Error, "The HZB of a {0}x{1} view could not be created: {2}", width, height,
                   GetErrorName(texture.Error()));
        hzb.ViewWidth = 0;
        hzb.ViewHeight = 0;
        return false;
    }
    hzb.Texture.Texture = texture.Value();
    hzb.Texture.Layout = RhiLayout::Undefined;
    hzb.Texture.FinalLayout = RhiLayout::ShaderResource;
    return true;
}

RenderGraphTextureViewDesc Mip(uint32 mip)
{
    RenderGraphTextureViewDesc view;
    view.Range.FirstMip = mip;
    view.Range.MipCount = 1;
    return view;
}

/// Builds *hzb*, of *mips* mips, from *visBuffer* of a *width* × *height* view.
void AddHzbBuild(RenderGraphBuilder &graph, ShaderLibrary &shaders, StringView name, RenderGraphBuffer visBuffer,
                 uint32 width, uint32 height, RenderGraphTexture hzb, uint32 mips)
{
    struct FirstData
    {
        RenderGraphView VisBuffer;
        RenderGraphView Output;
    };
    ShaderLibrary *library = &shaders;
    const RhiTextureDesc &desc = graph.GetDesc(hzb);
    const uint32 hzbWidth = desc.Width;
    const uint32 hzbHeight = desc.Height;
    graph.AddPass<FirstData>(
        name, RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, FirstData &data) {
            data.VisBuffer = pass.ReadBuffer(visBuffer, Structured(sizeof(uint64)));
            data.Output = pass.OverwriteTexture(hzb, Mip(0));
        },
        [library, width, height, hzbWidth, hzbHeight](const FirstData &data, RenderGraphContext &context) {
            SceneHzbFromVisBufferArguments arguments;
            arguments.VisBuffer = context.GetSpan(data.VisBuffer);
            arguments.Output = context.GetDescriptorIndex(data.Output);
            arguments.Width = width;
            arguments.Height = height;
            (void)library->DispatchThreads(context.List, SceneHzbFromVisBufferShader, arguments, hzbWidth, hzbHeight);
        });
    struct MipData
    {
        RenderGraphView Source;
        RenderGraphView Output;
    };
    for (uint32 mip = 1; mip < mips; ++mip)
    {
        const uint32 mipWidth = Max(hzbWidth >> mip, 1u);
        const uint32 mipHeight = Max(hzbHeight >> mip, 1u);
        graph.AddPass<MipData>(
            name, RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, MipData &data) {
                data.Source = pass.ReadTexture(hzb, Mip(mip - 1));
                data.Output = pass.OverwriteTexture(hzb, Mip(mip));
            },
            [library, mipWidth, mipHeight](const MipData &data, RenderGraphContext &context) {
                SceneHzbMipArguments arguments;
                arguments.Source = context.GetDescriptorIndex(data.Source);
                arguments.Output = context.GetDescriptorIndex(data.Output);
                (void)library->DispatchThreads(context.List, SceneHzbMipShader, arguments, mipWidth, mipHeight);
            });
    }
}

uint32 GetQueueCapacity()
{
    return Max(SceneRendererSubsystem::TraversalQueueSize, 1024u);
}

uint32 GetMaxGroups()
{
    return Max(SceneRendererSubsystem::MaxVisibleGroups, 1024u);
}

/// The nodes the first traversal phase can set aside; past that, it traverses them as if visible.
uint32 GetOccludedCapacity()
{
    return GetQueueCapacity() / 4;
}

/// Traverses the view's LOD trees in *phase*: the first from the instances' roots, testing nodes against the last
/// frame's HZB; the second from the nodes the first set aside, against this frame's HZB of the first raster. An
/// invalid *hzb* tests no occlusion.
void AddTraverse(SceneViewContext &context, uint32 phase, RenderGraphTexture hzb)
{
    struct TraverseData
    {
        RenderGraphView Instances;
        RenderGraphView Meshes;
        RenderGraphView Nodes;
        RenderGraphView Groups;
        RenderGraphView PageTable;
        RenderGraphView PageRequested;
        RenderGraphView Requests;
        RenderGraphView Views;
        RenderGraphView Queue;
        RenderGraphView State;
        RenderGraphView VisibleGroups;
        RenderGraphView Occluded;
        RenderGraphView Hzb;
    };
    const SceneGpuResources &scene = context.Scene;
    const SceneViewTextures &textures = context.Textures;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 queueCapacity = GetQueueCapacity();
    const uint32 maxGroups = GetMaxGroups();
    const uint32 occludedCapacity = GetOccludedCapacity();
    const uint32 maxRequests = scene.MaxRequests;
    const float32 threshold = scene.LodThreshold;
    const uint32 frame = scene.Frame;
    const uint32 traversalGroups = Max(SceneRendererSubsystem::TraversalWorkgroups, 1u);
    const bool useHzb = hzb.IsValid();
    const uint32 hzbMips = textures.HzbMips;
    context.Graph.AddPass<TraverseData>(
        phase == 0 ? StringView("Scene Traverse") : StringView("Scene Traverse (Second Phase)"),
        RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, TraverseData &data) {
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Meshes = pass.ReadBuffer(scene.Meshes, Structured(sizeof(SceneGpuMesh)));
            data.Nodes = pass.ReadBuffer(scene.Nodes, Structured(sizeof(SceneGpuNode)));
            data.Groups = pass.ReadBuffer(scene.Groups, Structured(sizeof(SceneGpuGroup)));
            data.PageTable = pass.ReadBuffer(scene.PageTable, Structured(sizeof(uint32)));
            data.PageRequested = pass.WriteBuffer(scene.PageRequested, Structured(sizeof(uint32)));
            data.Requests = pass.WriteBuffer(scene.Requests, Structured(sizeof(uint32)));
            data.Views = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            data.Queue = pass.WriteBuffer(textures.TraversalQueue, Structured(sizeof(uint64)));
            data.State = pass.WriteBuffer(textures.TraversalState, Structured(sizeof(uint32)));
            data.VisibleGroups = phase == 0 ? pass.OverwriteBuffer(textures.VisibleGroups, Structured(sizeof(uint32)))
                                            : pass.WriteBuffer(textures.VisibleGroups, Structured(sizeof(uint32)));
            // The instance culling starts the list, and the first phase adds to it.
            data.Occluded = phase == 0 ? pass.WriteBuffer(textures.OccludedNodes, Structured(sizeof(uint64)))
                                       : pass.ReadBuffer(textures.OccludedNodes, Structured(sizeof(uint64)));
            if (useHzb)
            {
                data.Hzb = pass.ReadTexture(hzb);
            }
        },
        [shaders, queueCapacity, maxGroups, occludedCapacity, maxRequests, threshold, frame, traversalGroups, useHzb,
         hzbMips, phase](const TraverseData &data, RenderGraphContext &pass) {
            SceneTraverseArguments arguments;
            arguments.Instances = pass.GetSpan(data.Instances);
            arguments.Meshes = pass.GetSpan(data.Meshes);
            arguments.Nodes = pass.GetSpan(data.Nodes);
            arguments.Groups = pass.GetSpan(data.Groups);
            arguments.PageTable = pass.GetSpan(data.PageTable);
            arguments.PageRequested = pass.GetSpan(data.PageRequested);
            arguments.Requests = pass.GetSpan(data.Requests);
            arguments.Views = pass.GetSpan(data.Views);
            arguments.Queue = pass.GetSpan(data.Queue);
            arguments.State = pass.GetSpan(data.State);
            arguments.VisibleGroups = pass.GetSpan(data.VisibleGroups);
            arguments.Occluded = pass.GetSpan(data.Occluded);
            arguments.Hzb = useHzb ? pass.GetDescriptorIndex(data.Hzb) : 0;
            arguments.QueueCapacity = queueCapacity;
            arguments.MaxGroups = maxGroups;
            arguments.MaxRequests = maxRequests;
            arguments.LodThreshold = threshold;
            arguments.Frame = frame;
            arguments.OccludedCapacity = occludedCapacity;
            arguments.HzbMips = hzbMips;
            arguments.Flags = useHzb ? SceneCullHzb : 0;
            arguments.Phase = phase;
            arguments.ThreadCount = traversalGroups * 64;
            (void)shaders->Dispatch(pass.List, SceneTraverseShader, arguments, traversalGroups);
        });
}

/// Culls the clusters of the groups *phase*'s traversal listed: the first phase against the last frame's HZB, keeping
/// what it hides as candidates; the second against this frame's HZB of the first raster, dropping what it hides.
void AddCullGroups(SceneViewContext &context, uint32 phase, RenderGraphTexture hzb)
{
    RenderGraphBuilder &graph = context.Graph;
    const SceneGpuResources &scene = context.Scene;
    const SceneViewTextures &textures = context.Textures;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 maxGroups = GetMaxGroups();
    const uint32 occludedCapacity = GetOccludedCapacity();
    const RenderGraphBuffer groupArguments = CreateArguments(
        graph, phase == 0 ? StringView("Scene Group Arguments") : StringView("Scene Group Arguments (Second Phase)"));

    struct GroupArgumentsData
    {
        RenderGraphView State;
        RenderGraphView Requests;
        RenderGraphView Arguments;
    };
    graph.AddPass<GroupArgumentsData>(
        phase == 0 ? StringView("Scene Group Arguments") : StringView("Scene Group Arguments (Second Phase)"),
        RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, GroupArgumentsData &data) {
            data.State = pass.WriteBuffer(textures.TraversalState, Structured(sizeof(uint32)));
            data.Requests = pass.WriteBuffer(scene.Requests, Structured(sizeof(uint32)));
            data.Arguments = pass.OverwriteBuffer(groupArguments, Structured(sizeof(uint32)));
        },
        [shaders, maxGroups, occludedCapacity, phase](const GroupArgumentsData &data, RenderGraphContext &pass) {
            SceneGroupArgumentsArguments arguments;
            arguments.State = pass.GetSpan(data.State);
            arguments.Requests = pass.GetSpan(data.Requests);
            arguments.Arguments = pass.GetSpan(data.Arguments);
            arguments.MaxGroups = maxGroups;
            arguments.GroupsX = GroupsPerRow;
            arguments.OccludedCapacity = occludedCapacity;
            arguments.Phase = phase;
            (void)shaders->Dispatch(pass.List, SceneGroupArgumentsShader, arguments, 1);
        });

    struct CullData
    {
        RenderGraphView Instances;
        RenderGraphView Meshes;
        RenderGraphView Groups;
        RenderGraphView Clusters;
        RenderGraphView PageTable;
        RenderGraphView SlotUsed;
        RenderGraphView Views;
        RenderGraphView Hzb;
        RenderGraphView VisibleGroups;
        RenderGraphView State;
        RenderGraphView Visible;
        RenderGraphView Candidates;
        RenderGraphView Hardware;
        RenderGraphView Software;
        RenderGraphView Counts;
    };
    const uint32 maxVisible = textures.MaxVisible;
    const bool useHzb = hzb.IsValid();
    const float32 softwarePixels = SceneRendererSubsystem::SoftwareRasterPixels;
    const uint32 hzbMips = textures.HzbMips;
    const float32 threshold = scene.LodThreshold;
    const uint32 frame = scene.Frame;
    graph.AddPass<CullData>(
        phase == 0 ? StringView("Scene Cull Groups") : StringView("Scene Cull Groups (Second Phase)"),
        RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, CullData &data) {
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Meshes = pass.ReadBuffer(scene.Meshes, Structured(sizeof(SceneGpuMesh)));
            data.Groups = pass.ReadBuffer(scene.Groups, Structured(sizeof(SceneGpuGroup)));
            data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
            data.PageTable = pass.ReadBuffer(scene.PageTable, Structured(sizeof(uint32)));
            data.SlotUsed = pass.WriteBuffer(scene.SlotUsed, Structured(sizeof(uint32)));
            data.Views = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            if (useHzb)
            {
                data.Hzb = pass.ReadTexture(hzb);
            }
            data.VisibleGroups = pass.ReadBuffer(textures.VisibleGroups, Structured(sizeof(uint32)));
            data.State = pass.ReadBuffer(textures.TraversalState, Structured(sizeof(uint32)));
            const RenderGraphBufferViewDesc entry = Structured(sizeof(SceneGpuVisibleCluster));
            const RenderGraphBufferViewDesc index = Structured(sizeof(uint32));
            // The first phase starts the lists; the second appends to them.
            data.Visible = phase == 0 ? pass.OverwriteBuffer(textures.VisibleClusters, entry)
                                      : pass.WriteBuffer(textures.VisibleClusters, entry);
            data.Candidates = phase == 0 ? pass.OverwriteBuffer(textures.Candidates, entry)
                                         : pass.WriteBuffer(textures.Candidates, entry);
            data.Hardware = phase == 0 ? pass.OverwriteBuffer(textures.HardwareClusters, index)
                                       : pass.WriteBuffer(textures.HardwareClusters, index);
            data.Software = phase == 0 ? pass.OverwriteBuffer(textures.SoftwareClusters, index)
                                       : pass.WriteBuffer(textures.SoftwareClusters, index);
            data.Counts = pass.WriteBuffer(textures.VisibleCount, index);
            (void)pass.Buffer(groupArguments, RhiAccess::IndirectArguments);
        },
        [shaders, maxVisible, maxGroups, useHzb, hzbMips, threshold, frame, softwarePixels, phase,
         groupArguments](const CullData &data, RenderGraphContext &pass) {
            SceneCullGroupsArguments arguments;
            arguments.Instances = pass.GetSpan(data.Instances);
            arguments.Meshes = pass.GetSpan(data.Meshes);
            arguments.Groups = pass.GetSpan(data.Groups);
            arguments.Clusters = pass.GetSpan(data.Clusters);
            arguments.PageTable = pass.GetSpan(data.PageTable);
            arguments.SlotUsed = pass.GetSpan(data.SlotUsed);
            arguments.Views = pass.GetSpan(data.Views);
            arguments.Hzb = useHzb ? pass.GetDescriptorIndex(data.Hzb) : 0;
            arguments.VisibleGroups = pass.GetSpan(data.VisibleGroups);
            arguments.State = pass.GetSpan(data.State);
            arguments.Visible = pass.GetSpan(data.Visible);
            arguments.Candidates = pass.GetSpan(data.Candidates);
            arguments.Hardware = pass.GetSpan(data.Hardware);
            arguments.Software = pass.GetSpan(data.Software);
            arguments.Counts = pass.GetSpan(data.Counts);
            arguments.HzbMips = hzbMips;
            arguments.MaxVisible = maxVisible;
            arguments.MaxGroups = maxGroups;
            arguments.GroupsX = GroupsPerRow;
            arguments.LodThreshold = threshold;
            arguments.Frame = frame;
            arguments.Flags = useHzb ? SceneCullHzb : 0;
            arguments.SoftwarePixels = softwarePixels;
            arguments.Phase = phase;
            (void)shaders->DispatchIndirect(pass.List, SceneCullGroupsShader, arguments, pass.GetBuffer(groupArguments),
                                            0);
        });
}
} // namespace

void SceneCullingFeature::AddPasses(SceneViewContext &context)
{
    RenderGraphBuilder &graph = context.Graph;
    SceneViewTextures &textures = context.Textures;
    const SceneGpuResources &scene = context.Scene;
    ShaderLibrary *shaders = &context.Shaders;
    textures.MaxVisible = Min(SceneRendererSubsystem::MaxVisibleClusters, SceneMaxVisibleClusters);
    const uint64 listSize = uint64{textures.MaxVisible} * sizeof(SceneGpuVisibleCluster);
    textures.VisibleClusters = CreateBuffer(graph, "Scene Visible Clusters", listSize);
    textures.Candidates = CreateBuffer(graph, "Scene Cluster Candidates", listSize);
    const uint64 indexListSize = uint64{textures.MaxVisible} * sizeof(uint32);
    textures.HardwareClusters = CreateBuffer(graph, "Scene Hardware Raster Clusters", indexListSize);
    textures.SoftwareClusters = CreateBuffer(graph, "Scene Software Raster Clusters", indexListSize);
    textures.VisibleCount = CreateBuffer(graph, "Scene Cluster Counts", SceneCountWords * sizeof(uint32));
    AddClear(graph, "Scene Clear Cluster Counts", textures.VisibleCount, SceneCountWords * sizeof(uint32));

    SceneHzbState &hzb = context.View.View->GetState().Get<SceneHzbState>();
    if (FitHzb(hzb, context.Device, context.View.Width, context.View.Height))
    {
        textures.Hzb = graph.ImportTexture("Scene HZB", hzb.Texture);
        textures.HzbMips = hzb.Mips;
        textures.HzbValid = hzb.Valid;
    }

    // The traversal queue starts empty: every entry the empty value, every counter zero.
    const uint32 queueCapacity = GetQueueCapacity();
    const RenderGraphBuffer queue =
        CreateBuffer(graph, "Scene Traversal Queue", uint64{queueCapacity} * sizeof(uint64));
    const RenderGraphBuffer state = CreateBuffer(graph, "Scene Traversal State", SceneTraversalWords * sizeof(uint32));
    textures.TraversalQueue = queue;
    textures.TraversalState = state;
    textures.VisibleGroups =
        CreateBuffer(graph, "Scene Visible Groups", uint64{GetMaxGroups()} * 2 * sizeof(uint32));
    textures.OccludedNodes =
        CreateBuffer(graph, "Scene Occluded Nodes", uint64{GetOccludedCapacity()} * sizeof(uint64));
    AddClear(graph, "Scene Clear Traversal Queue", queue, uint64{queueCapacity} * sizeof(uint64), 0xFFFFFFFFu);
    AddClear(graph, "Scene Clear Traversal State", state, SceneTraversalWords * sizeof(uint32));

    struct InstanceData
    {
        RenderGraphView Instances;
        RenderGraphView Meshes;
        RenderGraphView Views;
        RenderGraphView Queue;
        RenderGraphView State;
        RenderGraphView Occluded;
        RenderGraphView Hzb;
    };
    const uint32 instances = scene.InstanceCount;
    const uint32 occludedCapacity = GetOccludedCapacity();
    const bool useHzb = textures.HzbValid;
    const uint32 hzbMips = textures.HzbMips;
    graph.AddPass<InstanceData>(
        "Scene Cull Instances", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, InstanceData &data) {
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Meshes = pass.ReadBuffer(scene.Meshes, Structured(sizeof(SceneGpuMesh)));
            data.Views = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            data.Queue = pass.WriteBuffer(queue, Structured(sizeof(uint64)));
            data.State = pass.WriteBuffer(state, Structured(sizeof(uint32)));
            data.Occluded = pass.OverwriteBuffer(textures.OccludedNodes, Structured(sizeof(uint64)));
            if (useHzb)
            {
                data.Hzb = pass.ReadTexture(textures.Hzb);
            }
        },
        [shaders, instances, queueCapacity, occludedCapacity, useHzb, hzbMips](const InstanceData &data,
                                                                             RenderGraphContext &pass) {
            SceneLodInstanceArguments arguments;
            arguments.Instances = pass.GetSpan(data.Instances);
            arguments.Meshes = pass.GetSpan(data.Meshes);
            arguments.Views = pass.GetSpan(data.Views);
            arguments.Queue = pass.GetSpan(data.Queue);
            arguments.State = pass.GetSpan(data.State);
            arguments.Occluded = pass.GetSpan(data.Occluded);
            arguments.Hzb = useHzb ? pass.GetDescriptorIndex(data.Hzb) : 0;
            arguments.InstanceCount = instances;
            arguments.QueueCapacity = queueCapacity;
            arguments.OccludedCapacity = occludedCapacity;
            arguments.HzbMips = hzbMips;
            arguments.Flags = useHzb ? SceneCullHzb : 0;
            (void)shaders->DispatchThreads(pass.List, SceneLodInstanceShader, arguments, Max(instances, 1u), 1);
        });

    AddTraverse(context, 0, textures.HzbValid ? textures.Hzb : RenderGraphTexture());
    AddCullGroups(context, 0, textures.HzbValid ? textures.Hzb : RenderGraphTexture());
}

namespace
{
/// Writes the raster arguments of *phase* (and, for the first, the second phase's culling arguments).
void AddRasterArguments(SceneViewContext &context, uint32 phase, RenderGraphBuffer arguments,
                        RenderGraphBuffer softwareArguments, RenderGraphBuffer candidateArguments)
{
    struct ArgumentsData
    {
        RenderGraphView Counts;
        RenderGraphView Arguments;
        RenderGraphView SoftwareArguments;
        RenderGraphView CandidateArguments;
    };
    SceneViewTextures &textures = context.Textures;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 maxVisible = textures.MaxVisible;
    context.Graph.AddPass<ArgumentsData>(
        phase == 0 ? StringView("Scene Raster Arguments") : StringView("Scene Raster Arguments (Second Phase)"),
        RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, ArgumentsData &data) {
            data.Counts = pass.WriteBuffer(textures.VisibleCount, Structured(sizeof(uint32)));
            data.Arguments = pass.OverwriteBuffer(arguments, Structured(sizeof(uint32)));
            data.SoftwareArguments = pass.OverwriteBuffer(softwareArguments, Structured(sizeof(uint32)));
            data.CandidateArguments =
                phase == 0 ? pass.OverwriteBuffer(candidateArguments, Structured(sizeof(uint32))) : data.Arguments;
        },
        [shaders, maxVisible, phase](const ArgumentsData &data, RenderGraphContext &pass) {
            SceneRasterArguments values;
            values.Counts = pass.GetSpan(data.Counts);
            values.Arguments = pass.GetSpan(data.Arguments);
            values.SoftwareArguments = pass.GetSpan(data.SoftwareArguments);
            values.CandidateArguments = pass.GetSpan(data.CandidateArguments);
            values.MaxVisible = maxVisible;
            values.GroupsX = GroupsPerRow;
            values.Phase = phase;
            (void)shaders->Dispatch(pass.List, SceneRasterArgumentsShader, values, 1);
        });
}

/// Rasterizes the visible clusters of *phase* into the view's visibility buffer.
void AddRaster(SceneViewContext &context, uint32 phase, RenderGraphBuffer arguments)
{
    struct RasterData
    {
        RenderGraphView View;
        RenderGraphView Instances;
        RenderGraphView Clusters;
        RenderGraphView Geometry;
        RenderGraphView Visible;
        RenderGraphView Listed;
        RenderGraphView Count;
        RenderGraphView VisBuffer;
        RenderGraphView Overdraw;
    };
    SceneViewTextures &textures = context.Textures;
    const SceneGpuResources &scene = context.Scene;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 maxVisible = textures.MaxVisible;
    const uint32 flags = GetVisualization(context) == VisualizeOverdraw ? SceneRasterOverdraw : 0;
    const uint32 width = context.View.Width;
    const uint32 height = context.View.Height;
    context.Graph.AddPass<RasterData>(
        phase == 0 ? StringView("Scene VisBuffer") : StringView("Scene VisBuffer (Second Phase)"),
        RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, RasterData &data) {
            data.View = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
            data.Geometry = pass.ReadBuffer(scene.Geometry);
            data.Visible = pass.ReadBuffer(textures.VisibleClusters, Structured(sizeof(SceneGpuVisibleCluster)));
            data.Listed = pass.ReadBuffer(textures.HardwareClusters, Structured(sizeof(uint32)));
            data.Count = pass.ReadBuffer(textures.VisibleCount, Structured(sizeof(uint32)));
            data.VisBuffer = pass.WriteBuffer(textures.VisBuffer, Structured(sizeof(uint64)));
            data.Overdraw = pass.WriteBuffer(textures.Overdraw, Structured(sizeof(uint32)));
            (void)pass.Buffer(arguments, RhiAccess::IndirectArguments);
            pass.SetRenderArea(width, height);
        },
        [shaders, maxVisible, arguments, phase, flags](const RasterData &data, RenderGraphContext &pass) {
            SceneVisBufferArguments values;
            values.View = pass.GetSpan(data.View);
            values.Instances = pass.GetSpan(data.Instances);
            values.Clusters = pass.GetSpan(data.Clusters);
            values.Geometry = pass.GetAddress(data.Geometry);
            values.Visible = pass.GetSpan(data.Visible);
            values.Listed = pass.GetSpan(data.Listed);
            values.Count = pass.GetSpan(data.Count);
            values.VisBuffer = pass.GetSpan(data.VisBuffer);
            values.MaxVisible = maxVisible;
            values.GroupsX = GroupsPerRow;
            values.Phase = phase;
            values.Overdraw = pass.GetSpan(data.Overdraw);
            values.Flags = flags;
            (void)shaders->DrawMeshIndirect(pass.List, GetSceneVisBufferPipeline(), values, pass.GetBuffer(arguments),
                                            0);
        });
}

/// Rasterizes the small visible clusters of *phase* in compute into the view's visibility buffer.
void AddSoftwareRaster(SceneViewContext &context, uint32 phase, RenderGraphBuffer arguments)
{
    struct RasterData
    {
        RenderGraphView View;
        RenderGraphView Instances;
        RenderGraphView Clusters;
        RenderGraphView Geometry;
        RenderGraphView Visible;
        RenderGraphView Listed;
        RenderGraphView Count;
        RenderGraphView VisBuffer;
        RenderGraphView Overdraw;
    };
    SceneViewTextures &textures = context.Textures;
    const SceneGpuResources &scene = context.Scene;
    ShaderLibrary *shaders = &context.Shaders;
    const uint32 maxVisible = textures.MaxVisible;
    const uint32 flags = GetVisualization(context) == VisualizeOverdraw ? SceneRasterOverdraw : 0;
    context.Graph.AddPass<RasterData>(
        phase == 0 ? StringView("Scene Software Raster") : StringView("Scene Software Raster (Second Phase)"),
        RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, RasterData &data) {
            data.View = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
            data.Geometry = pass.ReadBuffer(scene.Geometry);
            data.Visible = pass.ReadBuffer(textures.VisibleClusters, Structured(sizeof(SceneGpuVisibleCluster)));
            data.Listed = pass.ReadBuffer(textures.SoftwareClusters, Structured(sizeof(uint32)));
            data.Count = pass.ReadBuffer(textures.VisibleCount, Structured(sizeof(uint32)));
            data.VisBuffer = pass.WriteBuffer(textures.VisBuffer, Structured(sizeof(uint64)));
            data.Overdraw = pass.WriteBuffer(textures.Overdraw, Structured(sizeof(uint32)));
            (void)pass.Buffer(arguments, RhiAccess::IndirectArguments);
        },
        [shaders, maxVisible, arguments, phase, flags](const RasterData &data, RenderGraphContext &pass) {
            SceneVisBufferArguments values;
            values.View = pass.GetSpan(data.View);
            values.Instances = pass.GetSpan(data.Instances);
            values.Clusters = pass.GetSpan(data.Clusters);
            values.Geometry = pass.GetAddress(data.Geometry);
            values.Visible = pass.GetSpan(data.Visible);
            values.Listed = pass.GetSpan(data.Listed);
            values.Count = pass.GetSpan(data.Count);
            values.VisBuffer = pass.GetSpan(data.VisBuffer);
            values.MaxVisible = maxVisible;
            values.GroupsX = GroupsPerRow;
            values.Phase = phase;
            values.Overdraw = pass.GetSpan(data.Overdraw);
            values.Flags = flags;
            (void)shaders->DispatchIndirect(pass.List, SceneSoftwareRasterShader, values, pass.GetBuffer(arguments), 0);
        });
}
} // namespace

void SceneVisBufferFeature::AddPasses(SceneViewContext &context)
{
    SceneViewTextures &textures = context.Textures;
    if (!textures.VisibleClusters.IsValid())
    {
        return;
    }
    RenderGraphBuilder &graph = context.Graph;
    const uint32 width = context.View.Width;
    const uint32 height = context.View.Height;
    const uint64 visSize = uint64{width} * height * sizeof(uint64);
    textures.VisBuffer = CreateBuffer(graph, "Scene VisBuffer", visSize);
    AddClear(graph, "Scene Clear VisBuffer", textures.VisBuffer, visSize);
    // A count per pixel for the Overdraw mode; the rasters only bind a word otherwise.
    const uint64 overdrawSize =
        GetVisualization(context) == VisualizeOverdraw ? uint64{width} * height * sizeof(uint32) : sizeof(uint32);
    textures.Overdraw = CreateBuffer(graph, "Scene Overdraw", overdrawSize);
    AddClear(graph, "Scene Clear Overdraw", textures.Overdraw, overdrawSize);

    const RenderGraphBuffer firstArguments = CreateArguments(graph, "Scene Raster Arguments");
    const RenderGraphBuffer firstSoftware = CreateArguments(graph, "Scene Software Raster Arguments");
    const RenderGraphBuffer candidateArguments = CreateArguments(graph, "Scene Candidate Arguments");
    AddRasterArguments(context, 0, firstArguments, firstSoftware, candidateArguments);
    AddRaster(context, 0, firstArguments);
    AddSoftwareRaster(context, 0, firstSoftware);

    // The second phase: what the last frame's HZB hid, against an HZB of what the first phase drew.
    if (textures.HzbValid)
    {
        RhiTextureDesc desc = graph.GetDesc(textures.Hzb);
        desc.DebugName = "Scene HZB (First Phase)";
        const RenderGraphTexture firstHzb = graph.CreateTexture("Scene HZB (First Phase)", desc);
        AddHzbBuild(graph, context.Shaders, "Scene Build HZB (First Phase)", textures.VisBuffer, width, height,
                    firstHzb, textures.HzbMips);
        // The nodes the last frame's HZB hid, traversed again against this one: what they no longer hide is listed,
        // its pages asked for, and its clusters culled and appended for the second raster.
        AddTraverse(context, 1, firstHzb);
        AddCullGroups(context, 1, firstHzb);
        struct CandidateData
        {
            RenderGraphView Instances;
            RenderGraphView Clusters;
            RenderGraphView Views;
            RenderGraphView Hzb;
            RenderGraphView Candidates;
            RenderGraphView Visible;
            RenderGraphView Hardware;
            RenderGraphView Software;
            RenderGraphView Counts;
        };
        const SceneGpuResources &scene = context.Scene;
        const float32 softwarePixels = SceneRendererSubsystem::SoftwareRasterPixels;
        ShaderLibrary *shaders = &context.Shaders;
        const uint32 maxVisible = textures.MaxVisible;
        const uint32 hzbMips = textures.HzbMips;
        graph.AddPass<CandidateData>(
            "Scene Cull Candidates", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass, CandidateData &data) {
                data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
                data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
                data.Views = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
                data.Hzb = pass.ReadTexture(firstHzb);
                data.Candidates = pass.ReadBuffer(textures.Candidates, Structured(sizeof(SceneGpuVisibleCluster)));
                data.Visible = pass.WriteBuffer(textures.VisibleClusters, Structured(sizeof(SceneGpuVisibleCluster)));
                data.Hardware = pass.WriteBuffer(textures.HardwareClusters, Structured(sizeof(uint32)));
                data.Software = pass.WriteBuffer(textures.SoftwareClusters, Structured(sizeof(uint32)));
                data.Counts = pass.WriteBuffer(textures.VisibleCount, Structured(sizeof(uint32)));
                (void)pass.Buffer(candidateArguments, RhiAccess::IndirectArguments);
            },
            [shaders, maxVisible, hzbMips, softwarePixels, candidateArguments](const CandidateData &data,
                                                                                RenderGraphContext &pass) {
                SceneCandidateArguments arguments;
                arguments.Instances = pass.GetSpan(data.Instances);
                arguments.Clusters = pass.GetSpan(data.Clusters);
                arguments.Views = pass.GetSpan(data.Views);
                arguments.Hzb = pass.GetDescriptorIndex(data.Hzb);
                arguments.Candidates = pass.GetSpan(data.Candidates);
                arguments.Visible = pass.GetSpan(data.Visible);
                arguments.Hardware = pass.GetSpan(data.Hardware);
                arguments.Software = pass.GetSpan(data.Software);
                arguments.Counts = pass.GetSpan(data.Counts);
                arguments.HzbMips = hzbMips;
                arguments.MaxVisible = maxVisible;
                arguments.SoftwarePixels = softwarePixels;
                (void)shaders->DispatchIndirect(pass.List, SceneCandidateShader, arguments,
                                                pass.GetBuffer(candidateArguments), 0);
            });
        const RenderGraphBuffer secondArguments = CreateArguments(graph, "Scene Raster Arguments (Second Phase)");
        const RenderGraphBuffer secondSoftware =
            CreateArguments(graph, "Scene Software Raster Arguments (Second Phase)");
        AddRasterArguments(context, 1, secondArguments, secondSoftware, {});
        AddRaster(context, 1, secondArguments);
        AddSoftwareRaster(context, 1, secondSoftware);
    }

    // The HZB of everything drawn, for the next frame's first phase.
    if (textures.Hzb.IsValid())
    {
        AddHzbBuild(graph, context.Shaders, "Scene Build HZB", textures.VisBuffer, width, height, textures.Hzb,
                    textures.HzbMips);
        context.View.View->GetState().Get<SceneHzbState>().Valid = true;
    }
}

void SceneVisualizeFeature::AddPasses(SceneViewContext &context)
{
    SceneViewTextures &textures = context.Textures;
    // Lit, and a view with no mode, show their materials through the lighting and tonemap stages instead.
    const SceneViewMode *mode = context.View.Mode;
    if (!textures.VisBuffer.IsValid() || mode == nullptr || !mode->GetType().IsA(TypeOf<SceneVisBufferViewMode>()))
    {
        return;
    }
    const uint32 visualization = GetVisualization(context);
    const float32 softwarePixels = SceneRendererSubsystem::SoftwareRasterPixels;
    RenderGraphBuilder &graph = context.Graph;
    const uint32 width = context.View.Width;
    const uint32 height = context.View.Height;
    RhiTextureDesc desc;
    desc.Format = RhiFormat::RGBA16Float;
    desc.Width = width;
    desc.Height = height;
    desc.DebugName = "Scene Color";
    textures.SceneColor = graph.CreateTexture("Scene Color", desc);

    struct VisualizeData
    {
        RenderGraphView View;
        RenderGraphView Instances;
        RenderGraphView Clusters;
        RenderGraphView Geometry;
        RenderGraphView Visible;
        RenderGraphView VisBuffer;
        RenderGraphView Meshes;
        RenderGraphView Groups;
        RenderGraphView Overdraw;
        RenderGraphView Headers;
        RenderGraphView Plane;
        RenderGraphView Records;
        RenderGraphView TileFlags;
        RenderGraphView Output;
    };
    const SceneGpuResources &scene = context.Scene;
    ShaderLibrary *shaders = &context.Shaders;
    // The closure modes read the material stage's buffers; the others bind a word in their place.
    const bool closures = textures.ClosureHeaders.IsValid();
    RenderGraphBuffer placeholder;
    if (!closures)
    {
        placeholder = CreateBuffer(graph, "Scene Visualize Placeholder", 16);
        AddClear(graph, "Scene Visualize Placeholder", placeholder, 16);
    }
    const uint32 tilesX = textures.TilesX;
    graph.AddPass<VisualizeData>(
        "Scene Visualize", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, VisualizeData &data) {
            data.View = pass.ReadBuffer(textures.ViewData, Structured(sizeof(SceneGpuView)));
            data.Instances = pass.ReadBuffer(scene.Instances, Structured(sizeof(SceneGpuInstance)));
            data.Clusters = pass.ReadBuffer(scene.Clusters, Structured(sizeof(SceneGpuCluster)));
            data.Geometry = pass.ReadBuffer(scene.Geometry);
            data.Visible = pass.ReadBuffer(textures.VisibleClusters, Structured(sizeof(SceneGpuVisibleCluster)));
            data.VisBuffer = pass.ReadBuffer(textures.VisBuffer, Structured(sizeof(uint64)));
            data.Meshes = pass.ReadBuffer(scene.Meshes, Structured(sizeof(SceneGpuMesh)));
            data.Groups = pass.ReadBuffer(scene.Groups, Structured(sizeof(SceneGpuGroup)));
            data.Overdraw = pass.ReadBuffer(textures.Overdraw, Structured(sizeof(uint32)));
            data.Headers = pass.ReadBuffer(closures ? textures.ClosureHeaders : placeholder, Structured(sizeof(uint32)));
            data.Plane = closures ? pass.ReadBuffer(textures.ClosurePlane, Structured(sizeof(uint32))) : data.Headers;
            data.Records = closures ? pass.ReadBuffer(textures.ClosureRecords, Structured(sizeof(uint32))) : data.Headers;
            data.TileFlags = closures ? pass.ReadBuffer(textures.TileFlags, Structured(sizeof(uint32))) : data.Headers;
            data.Output = pass.OverwriteTexture(textures.SceneColor);
        },
        [shaders, visualization, softwarePixels, width, height, tilesX](const VisualizeData &data,
                                                                       RenderGraphContext &pass) {
            SceneVisualizeArguments arguments;
            arguments.View = pass.GetSpan(data.View);
            arguments.Instances = pass.GetSpan(data.Instances);
            arguments.Clusters = pass.GetSpan(data.Clusters);
            arguments.Geometry = pass.GetAddress(data.Geometry);
            arguments.Visible = pass.GetSpan(data.Visible);
            arguments.VisBuffer = pass.GetSpan(data.VisBuffer);
            arguments.Meshes = pass.GetSpan(data.Meshes);
            arguments.Groups = pass.GetSpan(data.Groups);
            arguments.Overdraw = pass.GetSpan(data.Overdraw);
            arguments.Headers = pass.GetSpan(data.Headers);
            arguments.Plane = pass.GetSpan(data.Plane);
            arguments.Records = pass.GetSpan(data.Records);
            arguments.TileFlags = pass.GetSpan(data.TileFlags);
            arguments.TilesX = tilesX;
            arguments.Output = pass.GetDescriptorIndex(data.Output);
            arguments.Mode = visualization;
            arguments.SoftwarePixels = softwarePixels;
            (void)shaders->DispatchThreads(pass.List, SceneVisualizeShader, arguments, width, height);
        });
}
} // namespace Marlin
