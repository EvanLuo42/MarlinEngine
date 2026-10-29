#pragma once

#include "Concurrency/Jobs.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "Reflection/ExtensionSet.h"
#include "Renderer/RenderFrame.h"
#include "Renderer/RenderSurface.h"
#include "Renderer/RendererSubsystem.gen.h"

namespace Marlin
{
class RenderGraphSubsystem;
class RhiDevice;
class RhiSubsystem;

/// Runs the frame (Documentation/Architecture/01 §4): at the end of each engine frame it waits for the previous
/// frame's render job, lets every RenderFrameFeature extract on the main thread, then starts the render job, which
/// acquires each surface's next image, builds one RenderGraph with the features' passes in frame-stage order, executes
/// it and presents. So frame N renders while frame N+1 simulates, and at most one render job is in flight. The render
/// job is the frame's only submitter (07 §1.2): it claims RhiSubsystem's per-frame queue work and paces presents.
MCLASS(Config = "Engine.Renderer", Dependencies = "RenderGraphSubsystem")
class RENDERER_API RendererSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Lets surfaces that allow it present in HDR10 when their display is in HDR.
    MPROPERTY()
    static bool Hdr;
    /// The brightness of UI white on an HDR display, in nits; zero follows the display's SDR white level, as the OS
    /// shows SDR windows next to the game.
    MPROPERTY(ConsoleVariable = "r.UiWhiteNits")
    static float32 UiWhiteNits;

    RendererSubsystem();
    ~RendererSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void EndFrame(const FrameTime &time) override;

    /// A swapchain for *desc*'s window; null if the window or device cannot have one. Main thread.
    RenderSurface *CreateSurface(const RenderSurfaceDesc &desc);
    /// Waits for the render job that may present it. Main thread.
    void DestroySurface(RenderSurface *surface);

    /// A frame image slot (07 §5, 09 §2): a SceneView keeps one and binds its output to it each frame; UIPainter draws
    /// it by number. Any thread.
    [[nodiscard]] uint32 AllocateImageSlot();
    void FreeImageSlot(uint32 slot);

    /// Blocks until the render job in flight, if any, has presented. Main thread.
    void WaitForRender();

    [[nodiscard]] RhiDevice *GetDevice() const
    {
        return _device;
    }

private:
    struct Stage
    {
        const TypeInfo *Type = nullptr;
        List<RenderFrameFeature *> Features;
    };

    static void RenderJob(void *argument);
    void Render();
    void UpdateSurface(RenderSurface &surface);
    bool CreateSwapchain(RenderSurface &surface);
    void DiscoverStages();

    RhiSubsystem *_rhi = nullptr;
    RhiDevice *_device = nullptr;
    RenderGraphSubsystem *_graphs = nullptr;
    ExtensionSet<RenderFrameFeature> _features;
    List<Stage> _stages;
    List<UniquePtr<RenderSurface>> _surfaces;
    SpinLock _slotLock;
    List<uint32> _freeSlots;
    uint32 _nextSlot = 1;
    JobCounter _job;
    uint64 _renderFrame = 0;
};
} // namespace Marlin
