#pragma once

#include "Containers/HashMap.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "Reflection/TypeOf.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "RenderGraph/RenderGraphResources.h"
#include "SceneRenderer/SceneGpuTypes.h"
#include "SceneRenderer/SceneView.h"
#include "Shader/GlobalShader.h"
#include "String/StringView.h"
#include "SceneRenderer/SceneViewFeature.gen.h"

namespace Marlin
{
class RenderFrame;
class RhiDevice;
class ShaderLibrary;

// The stages of one view (Documentation/Architecture/07 §5.4), ordered like the frame stages: reflected structs marked
// `MSTRUCT(SceneViewStage, After = A, Before = B)`. Other modules add their own between these anchors.

/// GPU culling and the visible cluster list.
MSTRUCT(SceneViewStage)
struct SceneCullingStage
{
    MGENERATED_BODY()
};

/// The visibility buffer raster.
MSTRUCT(SceneViewStage, After = SceneCullingStage)
struct SceneVisBufferStage
{
    MGENERATED_BODY()
};

/// Material classification and resolve (07 §3, §4).
MSTRUCT(SceneViewStage, After = SceneVisBufferStage)
struct SceneMaterialStage
{
    MGENERATED_BODY()
};

/// Lights, shadows and GI into the scene color.
MSTRUCT(SceneViewStage, After = SceneMaterialStage)
struct SceneLightingStage
{
    MGENERATED_BODY()
};

MSTRUCT(SceneViewStage, After = SceneLightingStage)
struct SceneTranslucencyStage
{
    MGENERATED_BODY()
};

/// Post processing, upscaling and the Buffer visualizations, last before the view's image is bound.
MSTRUCT(SceneViewStage, After = SceneTranslucencyStage)
struct ScenePostStage
{
    MGENERATED_BODY()
};

/// A display mode of SceneViews (07 §5.4): Lit, Unlit, a Buffer visualization, an editor overlay. A stateless
/// provider found through reflection; it decides which SceneViewFeatures run for the views in it, and the features
/// that write a visualized buffer register the modes that show it. The editor's mode menu lists them.
MCLASS(Abstract, ExtensionPoint)
class SCENERENDERER_API SceneViewMode
{
    MGENERATED_BODY()
public:
    SceneViewMode() = default;
    virtual ~SceneViewMode() = default;
    MARLIN_NONCOPYABLE(SceneViewMode);

    /// The menu entry, such as "Unlit" or "Buffer/Depth".
    [[nodiscard]] virtual StringView GetDisplayName() const = 0;

    /// Whether *feature*, a SceneViewFeature type, runs for views in this mode.
    [[nodiscard]] virtual bool UsesFeature(const TypeInfo &feature) const
    {
        (void)feature;
        return true;
    }
};

/// What the render job keeps for a SceneView from frame to frame, such as HZB and history, by the type of the
/// feature's own state. Render job only.
struct SceneViewState
{
    SceneViewState() = default;
    MARLIN_NONCOPYABLE(SceneViewState);

    template <typename T> T &Get()
    {
        UniquePtr<Entry> &entry = _entries.FindOrAdd(&Key<T>);
        if (entry == nullptr)
        {
            entry = MakeUnique<TypedEntry<T>>();
        }
        return static_cast<TypedEntry<T> *>(entry.Get())->Value;
    }

private:
    struct Entry
    {
        virtual ~Entry() = default;
    };

    template <typename T> struct TypedEntry final : Entry
    {
        T Value;
    };

    /// One address per state type, which needs no reflection.
    template <typename T> static constexpr uint8 Key = 0;

    HashMap<const void *, UniquePtr<Entry>> _entries;
};

/// The view as the last frame rendered it, which the last frame's HZB was made from. Kept in SceneViewState.
struct SceneViewHistory
{
    SceneGpuView View;
    bool Valid = false;
};

/// A view as the render job renders it: what Extract copied from the SceneView.
struct SceneViewSnapshot
{
    SceneView *View = nullptr;
    SceneCamera Camera;
    uint32 Width = 0;
    uint32 Height = 0;
    const SceneViewMode *Mode = nullptr;
    uint32 ImageSlot = 0;
};

/// The GPU Scene of the RenderScene a view observes and the geometry pool, imported into this frame's graph
/// (Documentation/Architecture/07 §5.3).
struct SceneGpuResources
{
    RenderGraphBuffer Instances;
    uint32 InstanceCount = 0;
    /// SceneGpuMesh, SceneGpuNode and SceneGpuGroup tables.
    RenderGraphBuffer Meshes;
    RenderGraphBuffer Nodes;
    RenderGraphBuffer Groups;
    /// A uint per page of every mesh: its slot, or SceneNotResident.
    RenderGraphBuffer PageTable;
    /// A uint per page: the last frame stamp it was requested in.
    RenderGraphBuffer PageRequested;
    /// A uint per slot: the last frame stamp a listed group used it in.
    RenderGraphBuffer SlotUsed;
    /// SceneRequestHeaderWords uint, then the pages requested this frame.
    RenderGraphBuffer Requests;
    uint32 MaxRequests = 0;
    /// The resident clusters, SceneSlotClusters per slot, and their geometry, a slot's bytes per slot.
    RenderGraphBuffer Clusters;
    RenderGraphBuffer Geometry;
    /// This frame's number plus one, never zero.
    uint32 Frame = 0;
    /// The LOD threshold in pixels: LodPixelError, raised while the streaming pool is overcommitted.
    float32 LodThreshold = 1;
    /// A SceneGpuLights: the lights of the scene's views.
    RenderGraphBuffer Lights;
    /// A uint per mesh section: its material slot (SceneGpuMesh::FirstSection).
    RenderGraphBuffer Sections;
    /// A uint per material binding (SceneGpuInstance::Materials): its first section in Sections, ~0u while none.
    RenderGraphBuffer MaterialBindings;
    /// Two uint per material slot: its permutation (an index in Materials, ~0u while none) and its element there.
    RenderGraphBuffer MaterialSlots;
    /// A `[Material]` struct with one variant and the table of its materials' parameter blocks (07 §4.2), which its
    /// material passes are compiled for and read.
    struct MaterialPermutation
    {
        StringView Shader;
        Span<const ShaderVariantValue> Variant;
        uint32 Stride = 0;
        uint32 Count = 0;
        RenderGraphBuffer Parameters;
        Span<const String> CustomBsdfs;
    };
    List<MaterialPermutation> Materials;
};

/// The textures and buffers of one view's frame that the stages hand on: each is invalid until the stage that makes
/// it has run.
struct SceneViewTextures
{
    /// Two SceneGpuView: the view now, then as the last frame rendered it (SceneViewHistory; the view now on its first
    /// frame).
    RenderGraphBuffer ViewData;
    /// SceneGpuVisibleCluster entries, from the culling stage: those visible, and those the last frame's HZB hid.
    RenderGraphBuffer VisibleClusters;
    RenderGraphBuffer Candidates;
    /// The indices in VisibleClusters of the clusters each raster draws: the hardware raster's, of clusters large on
    /// screen, and the software raster's, of small ones.
    RenderGraphBuffer HardwareClusters;
    RenderGraphBuffer SoftwareClusters;
    /// SceneCountWords uint: visible, candidates, those listed for each raster, and those after the first phase.
    RenderGraphBuffer VisibleCount;
    /// The LOD traversal's queue, its SceneTraversalWords of state, the groups it lists (instance and group pairs) and
    /// the nodes its first phase sets aside as occluded, which the second phase traverses after the first raster.
    RenderGraphBuffer TraversalQueue;
    RenderGraphBuffer TraversalState;
    RenderGraphBuffer VisibleGroups;
    RenderGraphBuffer OccludedNodes;
    uint32 MaxVisible = 0;
    /// Width × Height 64-bit visibility values, from the visibility buffer stage.
    RenderGraphBuffer VisBuffer;
    /// Width × Height fragment counts in the Overdraw mode, else a single word the rasters bind.
    RenderGraphBuffer Overdraw;
    /// The view's HZB (SceneHzb.mshader): the last frame's until the visibility buffer stage rebuilds it for the next
    /// frame; invalid on the view's first frame or after it was resized.
    RenderGraphTexture Hzb;
    uint32 HzbMips = 0;
    bool HzbValid = false;
    /// What the view's image slot is bound to after the last stage, in the UI range.
    RenderGraphTexture SceneColor;
    /// The material stage's (07 §4.2): per pixel its permutation, its closure header and its Slab plane; the overflow
    /// records and the count of those used; per 8×8 tile its flags. TilesX tiles to a row, TileCount in all.
    RenderGraphBuffer MaterialBins;
    RenderGraphBuffer ClosureHeaders;
    RenderGraphBuffer ClosurePlane;
    RenderGraphBuffer ClosureRecords;
    RenderGraphBuffer ClosureRecordCount;
    RenderGraphBuffer TileFlags;
    uint32 TilesX = 0;
    uint32 TileCount = 0;
    uint32 RecordCapacity = 0;
    /// The lighting stage's HDR radiance, which the post stage tonemaps into SceneColor.
    RenderGraphTexture Radiance;
};

/// What a SceneViewFeature adds a view's passes with.
struct SceneViewContext
{
    RenderFrame &Frame;
    RenderGraphBuilder &Graph;
    RhiDevice &Device;
    ShaderLibrary &Shaders;
    const SceneViewSnapshot &View;
    const SceneGpuResources &Scene;
    SceneViewTextures &Textures;
};

/// Adds the passes of one view stage for each SceneView (07 §5.4): a stateless provider found through reflection
/// and placed on a view stage with `MCLASS(Stage = SceneVisBufferStage)`, like RenderFrameFeature on the frame stages.
/// AddPasses runs on the render job, once per view whose mode uses the feature, in stage order. State that outlives a
/// frame belongs to the view's SceneViewState or to a subsystem.
MCLASS(Abstract, ExtensionPoint)
class SCENERENDERER_API SceneViewFeature
{
    MGENERATED_BODY()
public:
    SceneViewFeature() = default;
    virtual ~SceneViewFeature() = default;
    MARLIN_NONCOPYABLE(SceneViewFeature);

    virtual void AddPasses(SceneViewContext &context) = 0;
};
} // namespace Marlin
