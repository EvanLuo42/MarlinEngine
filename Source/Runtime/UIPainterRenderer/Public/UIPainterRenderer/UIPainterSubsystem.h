#pragma once

#include "Application/PlatformWindow.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Math/Color.h"
#include "Memory/UniquePtr.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiUploadAllocator.h"
#include "RenderGraph/RenderGraphResources.h"
#include "Renderer/RenderFrame.h"
#include "UIPainter/UIPainter.h"
#include "UIPainterRenderer/UIGlyphCache.h"
#include "UIPainterRenderer/UIPainterSubsystem.gen.h"

namespace Marlin
{
class RendererSubsystem;
class RenderSurface;
class RhiDevice;
class ShaderLibrary;
class UITextSubsystem;

/// What one frame of UIPainter held, for debug overlays.
struct UIPainterFrameStats
{
    uint32 Canvases = 0;
    uint32 Commands = 0;
    /// Command-tile pairs the binning passes wrote.
    uint32 TilePairs = 0;
    uint32 Tiles = 0;
    /// Glyphs rasterized into the atlas this frame.
    uint32 AtlasGlyphs = 0;
};

/// One window UIPainter composes (09 §2, 07 §5): its surface, its painter, and the command lists the painter
/// records into, one while the render job draws the other.
class UIPAINTERRENDERER_API UIWindowCanvas
{
public:
    /// Records this frame's UI for the window, from the start of the frame until the renderer takes it.
    [[nodiscard]] UIPainter &GetPainter()
    {
        return _painter;
    }

    /// Records over everything GetPainter records this frame, whenever it is called: for debug overlays and tools
    /// that must stay on top of the game's UI.
    [[nodiscard]] UIPainter &GetOverlayPainter()
    {
        return _overlay;
    }

    [[nodiscard]] RenderSurface &GetSurface() const
    {
        return *_surface;
    }

    /// What shows where nothing is painted; straight linear RGB.
    LinearColor ClearColor = LinearColor::Black;

private:
    friend class UIPainterSubsystem;

    RenderSurface *_surface = nullptr;
    UIPainter _painter;
    UIPainter _overlay;
    UICommandList _overlayList;
    UICommandList _lists[2];
    uint32 _recording = 0;
    /// The list the render job draws, or null.
    UICommandList *_rendering = nullptr;
    bool _ownsSurface = false;
};

/// The GPU side of UIPainter (Documentation/Architecture/09 §2): window canvases, the glyph cache, and the passes that
/// UIPainterFrameFeature adds on the UI frame stage. Each frame a canvas's command stream is binned into 16×16 tiles
/// on the GPU (count, scan, scatter) and every tile is rasterized once with analytic coverage straight into the
/// window's swapchain image, encoded for SDR or for HDR10 with the UI white at RendererSubsystem::UiWhiteNits. Layers
/// render into their own textures first and blur there; a backdrop blur splits its parent into segments. Frame image
/// slots that SceneViews bound are read as graph textures.
MCLASS(Config = "Engine.UIPainter", Dependencies = "RendererSubsystem, ShaderLibrary, UITextSubsystem")
class UIPAINTERRENDERER_API UIPainterSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Composes the application's main window, so the game paints its UI there.
    MPROPERTY()
    static bool MainWindow;

    UIPainterSubsystem();
    ~UIPainterSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// A canvas composing *surface*, which must outlive it. Main thread.
    UIWindowCanvas *CreateCanvas(RenderSurface &surface);
    /// A canvas composing *window* through a surface of its own, which DestroyCanvas destroys; null if the surface
    /// cannot be made. Main thread.
    UIWindowCanvas *CreateWindowCanvas(WindowHandle window);
    void DestroyCanvas(UIWindowCanvas *canvas);

    /// What the last frame the GPU drew held; read it on the main thread.
    [[nodiscard]] const UIPainterFrameStats &GetLastFrameStats() const
    {
        return _stats;
    }

    [[nodiscard]] const UIGlyphCache &GetGlyphCache() const
    {
        return _glyphs;
    }

    /// The main window's canvas; null without one.
    [[nodiscard]] UIWindowCanvas *GetMainCanvas() const
    {
        return _mainCanvas;
    }

    /// Takes what the canvases recorded and resolves its glyphs; the render job draws it next.
    void Extract();
    void AddPasses(RenderFrame &frame);

private:
    struct FrameState;

    void AddCanvasPasses(RenderFrame &frame, FrameState &state, UIWindowCanvas &canvas, RenderGraphTexture target);
    void EnsureGlyphBuffers(uint64 curveBytes, uint64 wordBytes, bool &reupload);

    RendererSubsystem *_renderer = nullptr;
    ShaderLibrary *_shaders = nullptr;
    UITextSubsystem *_text = nullptr;
    RhiDevice *_device = nullptr;
    List<UniquePtr<UIWindowCanvas>> _canvases;
    UIWindowCanvas *_mainCanvas = nullptr;
    UIGlyphCache _glyphs;
    UniquePtr<RhiUploadAllocator> _uploads;
    RhiTexture _atlas;
    RenderGraphExternalTexture _atlasState;
    RhiBuffer _glyphCurves;
    RhiBuffer _glyphWords;
    uint64 _glyphCurveCapacity = 0;
    uint64 _glyphWordCapacity = 0;
    RenderGraphExternalBuffer _glyphCurveState;
    RenderGraphExternalBuffer _glyphWordState;
    RhiSampler _linearSampler;
    RhiSampler _pointSampler;
    /// What Extract took for the render job.
    List<UiAtlasGlyph> _atlasGlyphs;
    /// Glyph curves and words already in the GPU buffers.
    uint32 _uploadedGlyphCurves = 0;
    uint32 _uploadedGlyphWords = 0;
    bool _clearAtlas = false;
    UIPainterFrameStats _stats;
    /// What the render job counts; Extract publishes it as _stats once the job is done.
    UIPainterFrameStats _renderStats;
    /// Holds a splash screen until the UI shaders can draw (Documentation/Architecture/09 §4.6).
    uint32 _readyCheck = 0;
};

/// Adds UIPainter's passes on the UI frame stage, after the SceneViews.
MCLASS(Stage = UiFrameStage)
class UIPAINTERRENDERER_API UIPainterFrameFeature final : public RenderFrameFeature
{
    MGENERATED_BODY()
public:
    void Extract(RenderFrameExtract &frame) override;
    void AddPasses(RenderFrame &frame) override;
};
} // namespace Marlin
