#pragma once

#include "CoreTypes.h"
#include "SceneRenderer/SceneViewFeature.h"
#include "SceneRenderer/SceneVisBufferFeatures.gen.h"

namespace Marlin
{
/// The first culling phase of a view (Documentation/Architecture/07 §5.3): instances against the frustum, then each
/// cluster of their finest level against the frustum, its backface cone and the HZB the last frame left; what the HZB
/// hides becomes a candidate for the second phase.
MCLASS(Stage = SceneCullingStage)
class SCENERENDERER_API SceneCullingFeature final : public SceneViewFeature
{
    MGENERATED_BODY()
public:
    void AddPasses(SceneViewContext &context) override;
};

/// Rasterizes the visible clusters into the view's visibility buffer, one mesh group per cluster and a 64-bit atomic
/// max per pixel with no attachments, in two phases: the first phase's clusters; then, after building an HZB from
/// them, the candidates it no longer hides. Last it builds the HZB the next frame's first phase tests against.
MCLASS(Stage = SceneVisBufferStage)
class SCENERENDERER_API SceneVisBufferFeature final : public SceneViewFeature
{
    MGENERATED_BODY()
public:
    void AddPasses(SceneViewContext &context) override;
};

/// Writes the view's scene color from its visibility buffer: the Unlit display or a Buffer visualization, by the
/// view's SceneVisBufferViewMode.
MCLASS(Stage = ScenePostStage)
class SCENERENDERER_API SceneVisualizeFeature final : public SceneViewFeature
{
    MGENERATED_BODY()
public:
    void AddPasses(SceneViewContext &context) override;
};

/// A mode that shows the visibility buffer through SceneVisualizeFeature.
MCLASS(Abstract)
class SCENERENDERER_API SceneVisBufferViewMode : public SceneViewMode
{
    MGENERATED_BODY()
public:
    /// The visualization of SceneVisualize.mshader (VisualizeUnlit, VisualizeInstances, ...).
    [[nodiscard]] virtual uint32 GetVisualization() const = 0;
    /// Whether it shows the material stage's closures, which then runs (SceneVisualize.mshader's FirstMaterialMode on).
    [[nodiscard]] bool ShowsClosures() const
    {
        return GetVisualization() >= 11;
    }
};

/// Shaded by the triangles' normals with a light at the camera: what views show until materials and lights exist.
MCLASS()
class SCENERENDERER_API SceneUnlitViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Unlit";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 0;
    }
};

MCLASS()
class SCENERENDERER_API SceneInstancesViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Instances";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 1;
    }
};

MCLASS()
class SCENERENDERER_API SceneClustersViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Clusters";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 2;
    }
};

MCLASS()
class SCENERENDERER_API SceneTrianglesViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Triangles";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 3;
    }
};

MCLASS()
class SCENERENDERER_API SceneDepthViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Depth";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 4;
    }
};

MCLASS()
class SCENERENDERER_API SceneNormalsViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Normals";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 5;
    }
};

/// By the DAG level the traversal picked, as vk_lod_clusters colors it: white for the source triangles to red for
/// the coarsest level.
MCLASS()
class SCENERENDERER_API SceneLodViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/LOD";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 6;
    }
};

/// By the raster that drew each cluster: green the software raster, red the hardware's (07 §5.3).
MCLASS()
class SCENERENDERER_API SceneRasterViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Raster";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 7;
    }
};

/// By the cluster group of the DAG, which LOD changes swap whole.
MCLASS()
class SCENERENDERER_API SceneGroupsViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Groups";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 8;
    }
};

/// By the streaming page, the unit pages stream in and out by.
MCLASS()
class SCENERENDERER_API ScenePagesViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Pages";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 9;
    }
};

/// Fragments rastered per pixel, hidden ones included: blue for one to red for 32 and more.
MCLASS()
class SCENERENDERER_API SceneOverdrawViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Overdraw";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 10;
    }
};
/// The first closure's albedo, as Material Resolve wrote it.
MCLASS()
class SCENERENDERER_API SceneBaseColorViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Base Color";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 11;
    }
};

/// The first closure's shading normal in world space.
MCLASS()
class SCENERENDERER_API SceneShadingNormalViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Shading Normal";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 12;
    }
};

/// The first closure's roughness.
MCLASS()
class SCENERENDERER_API SceneRoughnessViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Roughness";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 13;
    }
};

/// The first closure's metalness.
MCLASS()
class SCENERENDERER_API SceneMetallicViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Metallic";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 14;
    }
};

/// The closures per pixel: none, one, and up to four, and red where one is a custom BSDF's.
MCLASS()
class SCENERENDERER_API SceneClosuresViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Closures";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 15;
    }
};

/// The tile classes the lighting passes take: Simple green, Single yellow, Complex red.
MCLASS()
class SCENERENDERER_API SceneTileClassViewMode final : public SceneVisBufferViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Buffer/Tile Class";
    }
    [[nodiscard]] uint32 GetVisualization() const override
    {
        return 16;
    }
};
} // namespace Marlin
