#pragma once

#include "Application/PlatformWindow.h"
#include "CoreTypes.h"
#include "RHI/RhiDevice.h"
#include "RenderGraph/RenderGraphResources.h"

namespace Marlin
{
struct RenderSurfaceDesc
{
    WindowHandle Window;
    RhiPresentMode PresentMode = RhiPresentMode::Fifo;
    /// Presents in HDR10 when the window's display is in HDR and Engine.Renderer.Hdr allows it; SDR otherwise.
    bool AllowHdr = true;
};

/// The swapchain of one OS window (Documentation/Architecture/09 §4.5): every frame the render job acquires its
/// next image, which the frame's graph imports to end in the Present layout, and presents it. Its image is written by
/// compute passes (UIPainter composes the whole window), so it carries UnorderedAccess usage. Created and destroyed
/// through RendererSubsystem; size changes follow the window at the start of each frame.
class RENDERER_API RenderSurface
{
public:
    [[nodiscard]] WindowHandle GetWindow() const
    {
        return _desc.Window;
    }

    /// Physical pixels.
    [[nodiscard]] uint32 GetWidth() const
    {
        return _width;
    }

    [[nodiscard]] uint32 GetHeight() const
    {
        return _height;
    }

    /// 1 at 96 DPI.
    [[nodiscard]] float32 GetDpiScale() const
    {
        return _dpiScale;
    }

    [[nodiscard]] RhiFormat GetFormat() const
    {
        return _format;
    }

    /// Srgb for SDR; Hdr10 when presenting HDR.
    [[nodiscard]] RhiColorSpace GetColorSpace() const
    {
        return _colorSpace;
    }

    [[nodiscard]] bool IsHdr() const
    {
        return _colorSpace != RhiColorSpace::Srgb;
    }

    /// The brightness of UI white when presenting HDR, in nits: r.UiWhiteNits, or the display's SDR white level.
    [[nodiscard]] float32 GetUiWhiteNits() const
    {
        return _uiWhiteNits;
    }

    /// False while the window is minimized or zero-sized: the frame skips it.
    [[nodiscard]] bool IsPresentable() const
    {
        return _swapchain.IsValid() && _width != 0 && _height != 0 && !_minimized;
    }

private:
    friend class RendererSubsystem;

    RenderSurfaceDesc _desc;
    RhiSwapchain _swapchain;
    RhiFormat _format = RhiFormat::RGB10A2Unorm;
    RhiColorSpace _colorSpace = RhiColorSpace::Srgb;
    uint32 _width = 0;
    uint32 _height = 0;
    float32 _dpiScale = 1;
    float32 _uiWhiteNits = 80;
    /// The display's HDR state when the swapchain was made.
    bool _displayHdr = false;
    uint32 _colorCheck = 0;
    bool _minimized = false;
    /// Present failed because the window's surface changed: the swapchain is recreated next frame.
    bool _lost = false;
    /// The last acquire or present failure was logged, so a persistent one does not log every frame.
    bool _reportedFailure = false;
    /// The image acquired for the frame being rendered.
    RenderGraphExternalTexture _image;
};
} // namespace Marlin
