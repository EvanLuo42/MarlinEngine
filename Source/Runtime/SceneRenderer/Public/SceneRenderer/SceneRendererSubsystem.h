#pragma once

#include "Asset/AssetRef.h"
#include "Asset/BulkStreamer.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Mesh/Mesh.h"
#include "Misc/Guid.h"
#include "RHI/RhiUploadAllocator.h"
#include "Reflection/ExtensionSet.h"
#include "RenderGraph/RenderGraphResources.h"
#include "Renderer/RenderFrame.h"
#include "SceneRenderer/SceneGpuTypes.h"
#include "SceneRenderer/SceneMaterialRegistry.h"
#include "SceneRenderer/SceneViewFeature.h"
#include "String/String.h"
#include "SceneRenderer/SceneRendererSubsystem.gen.h"

namespace Marlin
{
class RenderScene;
class RhiDevice;
class SceneTextureResidency;
class SceneMaterialRegistry;
class ShaderLibrary;

/// A frame's uploads into its graph from the renderer's upload memory: each call adds a copy pass where it stands, so
/// the passes declared after it read the bytes. Render job only.
class SCENERENDERER_API SceneUploads
{
public:
    struct Region
    {
        uint64 Offset = 0;
        Span<const uint8> Bytes;
    };

    void Begin(RhiUploadAllocator &allocator)
    {
        _allocator = &allocator;
    }

    /// A transient buffer of *bytes*, at least 16 bytes.
    [[nodiscard]] RenderGraphBuffer Upload(RenderGraphBuilder &graph, StringView name, Span<const uint8> bytes);
    /// Writes *regions* into *target*, such as pages into the geometry pool.
    void UploadTo(RenderGraphBuilder &graph, StringView name, RenderGraphBuffer target, Span<const Region> regions);

private:
    RhiUploadAllocator *_allocator = nullptr;
};

/// First-fit ranges of a buffer, merged as they are freed.
class SCENERENDERER_API SceneRangeAllocator
{
public:
    void Reset(uint64 size);
    /// False if no free range holds *size* bytes at *alignment*.
    [[nodiscard]] bool Allocate(uint64 size, uint64 alignment, uint64 &offset);
    void Free(uint64 offset, uint64 size);

private:
    struct Range
    {
        uint64 Offset = 0;
        uint64 Size = 0;
    };

    /// Sorted by offset, none adjacent.
    List<Range> _free;
};

/// Drives the SceneViews of every RenderScene (Documentation/Architecture/07 §5): the geometry pool the GPU Scenes
/// reference, the view stages with their SceneViewFeatures, the view modes, and the frame feature that renders each
/// view into its image slot on SceneViewsFrameStage.
///
/// The geometry pool streams pages (07 §5.3). Every mesh some instance draws has a stable index into the mesh table
/// and keeps its hierarchy, groups and page table entries resident; its pages live in fixed slots of the pool, the
/// cluster records of a slot in the cluster pool. A mesh draws once its root pages, the coarsest levels, are resident;
/// they stay. Finer pages come in when the views' traversal asks for them, read back from the GPU a few frames later,
/// parents before children; when slots run out, the pages used longest ago that no resident page depends on go.
MCLASS(Config = "Engine.SceneRenderer", Dependencies = "RendererSubsystem, ShaderLibrary, AssetSubsystem")
class SCENERENDERER_API SceneRendererSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Bytes of the geometry pool for streamed pages, besides the RootPages slots of the root pages (as Nanite's
    /// r.Nanite.Streaming.StreamingPoolSize, 512 MB, and NumInitialRootPages); slots are SlotSize bytes and the
    /// whole pool at most 4 GiB.
    MPROPERTY()
    static uint64 StreamingPoolSize;
    MPROPERTY()
    static uint32 RootPages;
    MPROPERTY()
    static uint32 SlotSize;
    /// Meshes the mesh table holds, and the hierarchy nodes, groups and pages of all of them together.
    MPROPERTY()
    static uint32 MaxMeshes;
    MPROPERTY()
    static uint32 MaxNodes;
    MPROPERTY()
    static uint32 MaxGroups;
    MPROPERTY()
    static uint32 MaxPages;
    /// Mesh sections, slots of every mesh together, whose material slots the section table holds.
    MPROPERTY()
    static uint32 MaxSections;
    /// The camera's exposure as EV100 (07 §3): 14 to 16 suits daylight.
    MPROPERTY(ConsoleVariable = "r.Scene.Exposure")
    static float32 Exposure;
    /// The lights scenes without light entities get: the sun's illuminance and the sky's ambient illuminance, in lux,
    /// and the direction towards the sun (normalized when used).
    MPROPERTY(ConsoleVariable = "r.Scene.SunIlluminance")
    static float32 SunIlluminance;
    MPROPERTY(ConsoleVariable = "r.Scene.AmbientIlluminance")
    static float32 AmbientIlluminance;
    MPROPERTY()
    static float32 SunDirection[3];
    /// Whether textures stream their mips from what Material Resolve's samples need (UE's r.TextureStreaming); off,
    /// every texture loads whole (SceneTextureResidency).
    MPROPERTY(ConsoleVariable = "r.Scene.TextureStreaming")
    static bool TextureStreaming;
    /// MiB of texture mips streaming keeps resident (r.Streaming.PoolSize); the 64x64 tails stay over it.
    MPROPERTY(ConsoleVariable = "r.Scene.TexturePoolSize")
    static uint32 TexturePoolSize;
    /// Added to the mip every material sample picks and every texture streams (r.MipMapLODBias): positive is
    /// blurrier and streams less.
    MPROPERTY(ConsoleVariable = "r.Scene.TextureMipBias")
    static float32 TextureMipBias;
    /// The material sampler's anisotropy, 1 to 16 (r.MaxAnisotropy).
    MPROPERTY(ConsoleVariable = "r.Scene.MaxAnisotropy")
    static uint32 MaxAnisotropy;
    /// Whether the lighting passes take Simple, Single and Complex tiles apart (07 §4.2); off, every tile drawn is
    /// lit as Complex, to compare or debug the classification.
    MPROPERTY(ConsoleVariable = "r.Scene.MaterialTileClasses")
    static bool MaterialTileClasses;
    /// The longest visible cluster list of a view; a longer one drops clusters.
    MPROPERTY(ConsoleVariable = "r.Scene.MaxVisibleClusters")
    static uint32 MaxVisibleClusters;
    /// The most groups a view's traversal lists, and the entries of its queue.
    MPROPERTY()
    static uint32 MaxVisibleGroups;
    MPROPERTY()
    static uint32 TraversalQueueSize;
    /// Workgroups of 64 threads the persistent traversal runs on.
    MPROPERTY()
    static uint32 TraversalWorkgroups;
    /// The simplification error on screen, in pixels, that a cluster may show before a finer one replaces it.
    MPROPERTY(ConsoleVariable = "r.Scene.LodPixelError")
    static float32 LodPixelError;
    /// Clusters whose longest triangle edge is shorter on screen than this many pixels are rastered in compute rather
    /// than by the hardware, whose quads waste most of their work on triangles of a pixel or two (as Nanite's
    /// r.Nanite.MinPixelsPerEdgeHW); zero rasters all by the hardware. It depends on the GPU more than on the scene.
    MPROPERTY(ConsoleVariable = "r.Scene.SoftwareRasterPixels")
    static float32 SoftwareRasterPixels;
    /// Pages the views may ask for in one frame, and page reads in flight at once.
    MPROPERTY()
    static uint32 MaxPageRequests;
    MPROPERTY(ConsoleVariable = "r.Scene.MaxPageLoads")
    static uint32 MaxPageLoads;
    /// Read pages committed to the pool in one frame; the rest wait for the next.
    MPROPERTY()
    static uint32 MaxPageCommits;
    /// When the pages the views need fill more than QualityMaxPoolPercent of the streamed pool for two frames, the
    /// LOD threshold grows by 3% a frame, down to QualityMinScale of the quality; below QualityMinPoolPercent for 30
    /// frames, it recovers by 1% a frame. So an overcommitted pool coarsens the picture instead of stalling it
    /// (Nanite's r.Nanite.Streaming.QualityScale.*).
    MPROPERTY()
    static float32 QualityMinPoolPercent;
    MPROPERTY()
    static float32 QualityMaxPoolPercent;
    MPROPERTY()
    static float32 QualityMinScale;
    /// Above zero: logs the streaming state every this many frames.
    MPROPERTY(ConsoleVariable = "r.Scene.StreamingStats")
    static uint32 StreamingStats;
    /// The SceneViewMode of views that set none, by type name.
    MPROPERTY()
    static String DefaultViewMode;

    SceneRendererSubsystem();
    ~SceneRendererSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    [[nodiscard]] RhiDevice *GetDevice() const
    {
        return _device;
    }

    /// The textures materials sample; null without a device.
    [[nodiscard]] SceneTextureResidency *GetTextures() const
    {
        return _textures.Get();
    }
    /// The materials meshes and entities draw with, or null without a device.
    [[nodiscard]] SceneMaterialRegistry *GetMaterials() const
    {
        return _materials.Get();
    }

    // Meshes: thread-safe, for the extract systems.

    /// The geometry index of *mesh*, adding a reference; it draws once its pages are resident.
    [[nodiscard]] uint32 AcquireMesh(const AssetRef<Mesh> &mesh);
    void AddMeshReference(uint32 mesh);
    void ReleaseMesh(uint32 mesh);
    /// The material binding of geometry index *mesh* drawn with *overrides*, a material per section or null for its
    /// default (RenderMesh::Materials), adding a reference: what SceneGpuInstance::Materials holds; 0, which needs no
    /// release, without overrides. The entities of one mesh and list share it; it is made once the mesh has loaded,
    /// the mesh's defaults drawing until then.
    [[nodiscard]] uint32 AcquireMaterialBinding(uint32 mesh, Span<const SoftAssetRef<Material>> overrides);
    void AddMaterialBindingReference(uint32 binding);
    void ReleaseMaterialBinding(uint32 binding);

    // Scenes and modes.

    void AddScene(RenderScene &scene);
    void RemoveScene(RenderScene &scene);

    /// Every registered view mode, for menus.
    [[nodiscard]] const ExtensionSet<SceneViewMode> &GetViewModes() const
    {
        return _modes;
    }
    /// The mode of *type*; the default mode for null or a type that is no mode.
    [[nodiscard]] const SceneViewMode *FindViewMode(const TypeInfo *type) const;

    /// Shows views that set no mode of their own in the mode named *name*, by display name (`Buffer/LOD`) or type
    /// name (`SceneLodViewMode`); lists the modes when it names none. Does nothing in Shipping.
    MFUNCTION(ConsoleCommand = "r.Scene.ViewMode")
    static void ViewModeCommand(const String &name);

    // The frame, driven by SceneRendererFrameFeature.

    void Extract();
    void AddPasses(RenderFrame &frame);

private:
    struct MeshEntry;
    struct MaterialBinding;
    struct PageRecord;
    struct Stage
    {
        const TypeInfo *Type = nullptr;
        List<SceneViewFeature *> Features;
    };
    struct GeometryUpload
    {
        /// Which persistent buffer the bytes go into, at Offset.
        enum class Target : uint8
        {
            Geometry,
            Clusters,
            Meshes,
            Nodes,
            Groups,
            PageTable,
            Sections,
            Count,
        };
        Target Into = Target::Geometry;
        uint64 Offset = 0;
        SharedPtr<const BulkBytes> Source;
        uint64 SourceOffset = 0;
        uint64 Size = 0;
        List<uint8> Bytes;
    };
    struct Retired
    {
        uint64 FirstNode = 0;
        uint64 NodeCount = 0;
        uint64 FirstGroup = 0;
        uint64 GroupCount = 0;
        uint64 FirstPage = 0;
        uint64 PageCount = 0;
        uint64 FirstSection = 0;
        uint64 SectionCount = 0;
        uint32 Mesh = 0;
        uint32 FramesLeft = 0;
    };
    /// A GPU-to-CPU copy of a frame's page requests and slot use, read once the GPU has finished that frame.
    struct Readback
    {
        RhiBuffer Buffer;
        /// The Graphics queue value the frame's work completes at, once submitted.
        uint64 SubmitValue = 0;
        /// The frame stamp the views stamped the slots they used with.
        uint32 Frame = 0;
        bool Written = false;
        bool InFlight = false;
    };

    void DiscoverStages();
    void StartReads();
    void OnRootPagesRead(uint32 mesh, SharedPtr<const BulkBytes> bytes);
    void OnPageRead(uint32 page, SharedPtr<const BulkBytes> bytes);
    void ReleasePages(MeshEntry &entry);
    void ProcessReadbacks();
    /// Updates the quality scale from how full the streamed pool would be with the pages a frame needed.
    void UpdateQualityScale(uint32 neededPages);
    /// Queues *page* and the parents it needs, at *priority* (a view's error on screen in pixels) or more.
    void RequestPage(uint32 page, float32 priority, uint32 depth = 0);
    void StartPageLoads();
    void CommitLoadedPages();
    /// Places *page*, whose bytes are *bytes*, into slots and uploads it; false if no slots could be freed for it.
    bool CommitPage(uint32 page, Span<const uint8> bytes, bool pinned);
    /// Evicts pages until a run of *slots* slots is free; false if not enough could go.
    bool FreeSlots(uint32 slots);
    void EvictPage(uint32 page);
    [[nodiscard]] const MeshPage *FindPage(uint32 page, MeshEntry *&entry) const;
    void UploadMeshTable(uint32 mesh, uint32 depthCount);
    void AddUpload(GeometryUpload::Target into, uint64 offset, Span<const uint8> bytes);
    void RenderView(RenderFrame &frame, const SceneViewSnapshot &view, const SceneGpuResources &scene);
    [[nodiscard]] bool EnsureGeometryBuffers();
    [[nodiscard]] uint32 GetSlotCount() const;

    RhiDevice *_device = nullptr;
    ShaderLibrary *_shaders = nullptr;
    BulkStreamer *_streamer = nullptr;
    UniquePtr<SceneTextureResidency> _textures;
    UniquePtr<SceneMaterialRegistry> _materials;
    /// Makes the section tables of new material bindings whose meshes have loaded, and retires those nothing uses;
    /// with _meshLock held.
    void UpdateMaterialBindings();
    /// The material table the render job uploads, taken at Extract.
    SceneMaterialFrame _renderMaterials;
    ExtensionSet<SceneViewFeature> _features;
    ExtensionSet<SceneViewMode> _modes;
    const SceneViewMode *_defaultMode = nullptr;
    List<Stage> _stages;

    mutable SpinLock _meshLock;
    List<UniquePtr<MeshEntry>> _meshes;
    HashMap<Guid, uint32> _meshByGuid;
    List<uint32> _freeMeshes;
    /// Meshes asked for since the last frame, whose root pages BeginFrame starts reading.
    List<uint32> _pendingReads;
    SceneRangeAllocator _nodeRanges;
    SceneRangeAllocator _groupRanges;
    SceneRangeAllocator _sectionRanges;
    SceneRangeAllocator _pageRanges;
    SceneRangeAllocator _slotRanges;
    List<Retired> _retired;
    List<GeometryUpload> _uploads;
    /// Material bindings by id; id 0 is none. Per id, its first section, or ~0u until it is made.
    List<UniquePtr<MaterialBinding>> _bindings;
    List<uint32> _freeBindings;
    List<uint32> _bindingSections;
    /// _bindingSections as the render job uploads it, taken at Extract.
    List<uint32> _renderBindingSections;

    // Streaming, main thread (with _meshLock held).
    List<PageRecord> _pages;
    /// The page in each slot that starts one, or SceneNotResident.
    List<uint32> _slotPages;
    List<uint32> _residentPages;
    /// Pages to read; StartPageLoads takes the highest priorities first.
    List<uint32> _loadQueue;
    /// Pages read and waiting for their parents.
    List<uint32> _loadedPages;
    uint32 _loadsInFlight = 0;
    /// Counted since the last stats line.
    uint32 _statRequests = 0;
    uint32 _statCommits = 0;
    uint32 _statEvictions = 0;
    uint32 _statQueuePeak = 0;
    uint32 _statTraversalFlags = 0;
    /// The streaming quality, 1 down to QualityMinScale: the LOD threshold is LodPixelError over it.
    float32 _qualityScale = 1;
    uint32 _framesOverfull = 0;
    uint32 _framesUnderfull = 0;
    float32 _lastPoolPercent = 0;
    /// The LOD threshold this frame's render job uses.
    float32 _renderLodThreshold = 1;
    /// This frame's number plus one: what the GPU stamps page requests and slot use with.
    uint32 _frameStamp = 1;
    Readback _readbacks[3];
    /// The readback the render job copies into this frame, or none.
    uint32 _renderReadback = ~0u;
    uint32 _lastReadback = ~0u;

    List<RenderScene *> _scenes;

    // Render job.
    UniquePtr<RhiUploadAllocator> _uploadAllocator;
    SceneUploads _frameUploads;
    List<GeometryUpload> _renderUploads;
    uint32 _renderFrameStamp = 1;
    bool _persistentCleared = false;
    RenderGraphExternalBuffer _geometry;
    RenderGraphExternalBuffer _clusters;
    RenderGraphExternalBuffer _meshTable;
    RenderGraphExternalBuffer _nodes;
    RenderGraphExternalBuffer _groups;
    RenderGraphExternalBuffer _sections;
    RenderGraphExternalBuffer _pageTable;
    RenderGraphExternalBuffer _pageRequested;
    RenderGraphExternalBuffer _slotUsed;
    RenderGraphExternalBuffer _requests;
};

/// Renders every SceneView of the frame into its image slot, on the SceneViews frame stage.
MCLASS(Stage = SceneViewsFrameStage)
class SCENERENDERER_API SceneRendererFrameFeature final : public RenderFrameFeature
{
    MGENERATED_BODY()
public:
    void Extract(RenderFrameExtract &frame) override;
    void AddPasses(RenderFrame &frame) override;
};
} // namespace Marlin
