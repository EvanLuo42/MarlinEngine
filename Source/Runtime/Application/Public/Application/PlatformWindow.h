#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EnumFlags.h"

namespace Marlin
{
/// Positions and sizes are in physical pixels; positions are relative to the virtual desktop.
struct PlatformRect
{
    int32 X = 0;
    int32 Y = 0;
    int32 Width = 0;
    int32 Height = 0;
};

struct WindowHandle
{
    uintptr Value = 0;

    bool IsValid() const
    {
        return Value != 0;
    }

    bool operator==(const WindowHandle &) const = default;
};

enum class WindowFlags : uint8
{
    None = 0,
    Resizable = 1 << 0,
    /// No title bar or frame.
    Borderless = 1 << 1,
    /// Created hidden; call Show.
    Hidden = 1 << 2,
    TopMost = 1 << 3,
    /// Not shown in the taskbar, and never activated by a click.
    Tool = 1 << 4,
    /// No OS title bar: the client area covers the whole window and the application draws its own, naming the parts
    /// that drag the window with SetHitTestRegions. The OS still moves, snaps, resizes and shadows it.
    CustomFrame = 1 << 5,
    /// Opens maximized; restoring returns to the size asked for. With Hidden it already has the maximized client size
    /// where the platform can give it one while hidden, so frames drawn before Show fit, and Show shows it maximized.
    Maximized = 1 << 6,
};
MARLIN_ENUM_FLAGS(WindowFlags)

/// What a point of a CustomFrame window does when the mouse presses it.
enum class WindowHitArea : uint8
{
    /// Goes to the application as mouse input.
    Client,
    /// Drags the window; a double click maximizes or restores it, a right click opens the system menu.
    Caption,
};

/// A part of a CustomFrame window, in client pixels.
struct WindowHitRegion
{
    PlatformRect Rect;
    WindowHitArea Area = WindowHitArea::Client;
};

struct WindowDesc
{
    static constexpr int32 DefaultPosition = -0x7FFFFFFF;

    /// UTF-8.
    const char *Title = "";
    int32 X = DefaultPosition;
    int32 Y = DefaultPosition;
    /// Client area.
    int32 Width = 1280;
    int32 Height = 720;
    /// The smallest client size the user can resize the window to, at 96 DPI (scaled by the window's DPI); 0 leaves
    /// it to the OS.
    int32 MinWidth = 0;
    int32 MinHeight = 0;
    WindowFlags Flags = WindowFlags::Resizable;
    /// Stays above its owner and closes with it.
    WindowHandle Owner;
};

/// How the display a window is on presents color.
struct DisplayColorInfo
{
    /// The OS composes the display in HDR (Windows' "Use HDR"), so an HDR10 swapchain shows as HDR.
    bool Hdr = false;
    /// The brightness the OS gives SDR white on that display, in nits: Windows' SDR content brightness when HDR is on,
    /// 80 otherwise.
    float32 SdrWhiteNits = 80;
};

/// Callable from any thread; the platform runs them on its window thread where the OS needs that.
namespace PlatformWindow
{
/// Returns an invalid handle on failure.
APPLICATION_API WindowHandle Create(const WindowDesc &desc);
APPLICATION_API void Destroy(WindowHandle window);

APPLICATION_API void Show(WindowHandle window);
APPLICATION_API void Hide(WindowHandle window);
APPLICATION_API void Minimize(WindowHandle window);
APPLICATION_API void Maximize(WindowHandle window);
APPLICATION_API void Restore(WindowHandle window);
APPLICATION_API void Focus(WindowHandle window);
APPLICATION_API void SetTitle(WindowHandle window, const char *title);
/// Borderless, covering the monitor the window is on; false restores the previous placement.
APPLICATION_API void SetFullscreen(WindowHandle window, bool fullscreen);

/// Moves the window so that its client area starts at (*x*, *y*).
APPLICATION_API void SetClientPosition(WindowHandle window, int32 x, int32 y);
APPLICATION_API void SetClientSize(WindowHandle window, int32 width, int32 height);
APPLICATION_API PlatformRect GetClientRect(WindowHandle window);

APPLICATION_API bool IsVisible(WindowHandle window);
APPLICATION_API bool IsFocused(WindowHandle window);
APPLICATION_API bool IsMinimized(WindowHandle window);
APPLICATION_API bool IsMaximized(WindowHandle window);
APPLICATION_API bool IsFullscreen(WindowHandle window);
/// 1 at 96 DPI.
APPLICATION_API float32 GetDpiScale(WindowHandle window);
/// A frame of *width* × *height* pixels was presented to *window*; any thread. While the user drags the window's
/// edge, the platform holds each resize step until a frame of the new size is presented (or a short time passes), so
/// the window never shows a large area the renderer has not drawn yet.
APPLICATION_API void NotifyPresented(WindowHandle window, int32 width, int32 height);
/// Asks *window* to close the way the OS close button does: a WindowCloseRequested event follows.
APPLICATION_API void RequestClose(WindowHandle window);

inline constexpr uint32 MaxHitRegions = 64;
/// CustomFrame windows: replaces the regions the OS tests the mouse against. A later region wins over an earlier
/// one; points in none, and regions past MaxHitRegions, are Client. The edges stay resize borders unless maximized.
APPLICATION_API void SetHitTestRegions(WindowHandle window, Span<const WindowHitRegion> regions);

/// HWND on Windows.
APPLICATION_API void *GetNativeHandle(WindowHandle window);
/// The color mode of the display that holds most of *window*.
APPLICATION_API DisplayColorInfo GetDisplayColor(WindowHandle window);
} // namespace PlatformWindow
} // namespace Marlin
