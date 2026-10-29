#pragma once

#include "Application/PlatformApplication.h"
#include "Application/PlatformWindow.h"

#include "HAL/MicrosoftHeaders.h"
#include "Templates/TypeTraits.h"

namespace Marlin::WindowsApplication
{
inline constexpr wchar_t WindowClassName[] = L"MarlinWindow";

/// Stored in GWLP_USERDATA.
struct WindowData
{
    DWORD Style = 0;
    DWORD ExStyle = 0;
    WINDOWPLACEMENT Placement = {};
    bool Fullscreen = false;
    bool TrackingMouse = false;
    /// WindowFlags::CustomFrame: no non-client area, and WM_NCHITTEST asks HitRegions.
    bool CustomFrame = false;
    /// WindowDesc::MinWidth and MinHeight, at 96 DPI; 0 leaves them to the OS.
    int32 MinWidth = 0;
    int32 MinHeight = 0;
    /// Guards HitRegions, written by the application's thread and read by the window thread.
    SRWLOCK HitLock;
    uint32 HitRegionCount = 0;
    WindowHitRegion HitRegions[PlatformWindow::MaxHitRegions];
    /// Between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE: the user is moving or resizing the window.
    bool InSizeMove = false;
    /// WindowFlags::Maximized on a window created hidden: the next Show maximizes it, restoring to NormalRect.
    bool ShowMaximized = false;
    RECT NormalRect = {};
    /// The size of the last frame presented to the window (PlatformWindow::NotifyPresented), written by the render
    /// thread, and the auto-reset event set with each; zero until the first.
    volatile LONG PresentedWidth = 0;
    volatile LONG PresentedHeight = 0;
    HANDLE Presented = nullptr;
};

/// The resize border of a window at *dpi* that has none drawn, in pixels; also how far a maximized window hangs
/// past the monitor.
inline int32 GetFrameThickness(UINT dpi)
{
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

inline WindowData *GetWindowData(HWND window)
{
    return reinterpret_cast<WindowData *>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

inline HWND ToHwnd(WindowHandle window)
{
    return reinterpret_cast<HWND>(window.Value);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
/// Forgets per-window state of the application before the window is destroyed.
void OnWindowDestroyed(HWND window);

/// The message the dispatcher window runs a function for: wParam is the function, lParam its argument.
inline constexpr UINT InvokeMessage = WM_APP + 1;

/// The message-only window of the window thread, which owns every window and pumps their messages.
HWND GetDispatcher();
DWORD GetWindowThreadId();

/// Runs *function* on the window thread and waits for it; at once when called there. The OS's modal loops (a window
/// being moved or resized) still run it.
template <typename TFunction> void RunOnWindowThread(TFunction &&function)
{
    if (GetCurrentThreadId() == GetWindowThreadId())
    {
        function();
        return;
    }
    using TCallable = RemoveReference<TFunction>;
    void (*trampoline)(void *) = [](void *argument) { (*static_cast<TCallable *>(argument))(); };
    SendMessageW(GetDispatcher(), InvokeMessage, reinterpret_cast<WPARAM>(trampoline),
                 reinterpret_cast<LPARAM>(&function));
}

/// Runs *function* on the window thread and returns its result.
template <typename TFunction> auto CallOnWindowThread(TFunction &&function) -> decltype(function())
{
    decltype(function()) result{};
    RunOnWindowThread([&] { result = function(); });
    return result;
}
} // namespace Marlin::WindowsApplication
