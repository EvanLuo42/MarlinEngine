#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "RenderGraph/RenderGraphResources.h"
#include "Renderer/RenderSurface.h"
#include "Renderer/RenderFrame.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(RENDERER_API, LogRenderer, Info, Warning);

namespace Marlin
{
/// Anchor stage: the SceneRenderer renders every SceneView of the frame into its image slot (07 §5). Frame stages
/// are reflected structs marked `MSTRUCT(RenderFrameStage, After = A, Before = B)`, ordered like World phases;
/// other modules add their own between the anchors.
MSTRUCT(RenderFrameStage)
struct SceneViewsFrameStage
{
    MGENERATED_BODY()
};

/// Anchor stage: UIPainter composes every surface, reading the images SceneViews bound (09 §2).
MSTRUCT(RenderFrameStage, After = SceneViewsFrameStage)
struct UiFrameStage
{
    MGENERATED_BODY()
};

/// What the values of a frame image mean, so the composer knows how to bring them to the display.
enum class RenderImageRange : uint8
{
    /// Display-referred, 1 is the UI white: SDR content and tonemapped SDR views.
    Ui,
    /// Linear Rec. 709 in nits: views tonemapped for an HDR display.
    DisplayNits,
};

/// A texture of this frame's graph bound to an image slot.
struct RenderFrameImage
{
    RenderGraphTexture Texture;
    RenderImageRange Range = RenderImageRange::Ui;
};

/// A surface as one frame renders it: *Target* is its acquired image, imported to end in the Present layout.
struct RenderFrameSurface
{
    RenderSurface *Surface = nullptr;
    RenderGraphTexture Target;
};

/// One frame as the render job builds it: the graph, the surfaces being presented, and the image slots. Features of
/// earlier stages bind slots that later stages read, such as a SceneView's output that UIPainter draws.
class RENDERER_API RenderFrame
{
public:
    RenderFrame(RenderGraphBuilder &graph, uint64 index) : Graph(graph), Index(index)
    {
    }
    MARLIN_NONCOPYABLE(RenderFrame);

    RenderGraphBuilder &Graph;
    /// The engine frame this is the render of.
    const uint64 Index;

    [[nodiscard]] Span<const RenderFrameSurface> GetSurfaces() const
    {
        return _surfaces.AsSpan();
    }

    [[nodiscard]] const RenderFrameSurface *FindSurface(const RenderSurface &surface) const;

    /// Binds *slot*, from RendererSubsystem::AllocateImageSlot, to *image* for the rest of the frame.
    void BindImage(uint32 slot, const RenderFrameImage &image);
    /// Null if nothing bound *slot* this frame.
    [[nodiscard]] const RenderFrameImage *FindImage(uint32 slot) const;

private:
    friend class RendererSubsystem;

    List<RenderFrameSurface> _surfaces;
    HashMap<uint32, RenderFrameImage> _images;
};

/// The main-thread hand-off before a frame's render job starts: the previous frame's job has finished, so a feature
/// may take what this frame recorded (such as swapping double-buffered command streams) without locks.
class RENDERER_API RenderFrameExtract
{
public:
    explicit RenderFrameExtract(uint64 index) : Index(index)
    {
    }
    MARLIN_NONCOPYABLE(RenderFrameExtract);

    const uint64 Index;
};

/// Adds a frame's passes (Documentation/Architecture/13 §5): a stateless provider found through reflection and
/// placed on a frame stage with `MCLASS(Stage = UiFrameStage)`. Extract runs on the main thread at the end of the
/// frame; AddPasses runs on the render job, in stage order, while the next frame simulates. State that outlives a
/// frame belongs to a subsystem the feature finds with Engine::GetSubsystem.
MCLASS(Abstract, ExtensionPoint)
class RENDERER_API RenderFrameFeature
{
    MGENERATED_BODY()
public:
    RenderFrameFeature() = default;
    virtual ~RenderFrameFeature() = default;
    MARLIN_NONCOPYABLE(RenderFrameFeature);

    virtual void Extract(RenderFrameExtract &frame)
    {
        (void)frame;
    }

    virtual void AddPasses(RenderFrame &frame) = 0;
};
} // namespace Marlin
