#include "Application/PlatformWindow.h"

#include "Containers/List.h"

#include "WindowsApplicationState.h"

#include <shellapi.h>
#include <shellscalingapi.h>

namespace Marlin::PlatformWindow
{
namespace
{
using WindowsApplication::GetWindowData;
using WindowsApplication::ToHwnd;

void Styles(WindowFlags flags, bool owned, DWORD &style, DWORD &exStyle)
{
    style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    exStyle = 0;
    if (HasAnyFlags(flags, WindowFlags::Borderless))
    {
        style |= WS_POPUP;
    }
    else
    {
        style |= WS_OVERLAPPEDWINDOW;
        if (!HasAnyFlags(flags, WindowFlags::Resizable))
        {
            style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }
    }
    if (HasAnyFlags(flags, WindowFlags::Tool))
    {
        exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    }
    else if (!owned)
    {
        exStyle |= WS_EX_APPWINDOW;
    }
    if (HasAnyFlags(flags, WindowFlags::TopMost))
    {
        exStyle |= WS_EX_TOPMOST;
    }
}

RECT FrameForClient(HWND window, DWORD style, DWORD exStyle, int32 width, int32 height, UINT dpi)
{
    RECT rect = {0, 0, width, height};
    const WindowsApplication::WindowData *data = GetWindowData(window);
    if (data == nullptr || !data->CustomFrame)
    {
        AdjustWindowRectExForDpi(&rect, style, GetMenu(window) != nullptr ? TRUE : FALSE, exStyle, dpi);
    }
    return rect;
}
} // namespace

namespace
{
WindowHandle CreateOnWindowThread(const WindowDesc &desc)
{
    const HWND owner = ToHwnd(desc.Owner);
    DWORD style = 0;
    DWORD exStyle = 0;
    Styles(desc.Flags, owner != nullptr, style, exStyle);

    const bool positioned = desc.X != WindowDesc::DefaultPosition && desc.Y != WindowDesc::DefaultPosition;
    const POINT anchor = positioned ? POINT{desc.X, desc.Y} : POINT{0, 0};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    GetDpiForMonitor(MonitorFromPoint(anchor, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    const bool customFrame = HasAnyFlags(desc.Flags, WindowFlags::CustomFrame);
    RECT frame = {0, 0, desc.Width, desc.Height};
    if (!customFrame)
    {
        AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, dpi);
    }

    wchar_t title[256] = {};
    if (Microsoft::Utf8ToWide(desc.Title, title, 256) >= 256)
    {
        title[0] = L'\0';
    }
    const int x = positioned ? desc.X + frame.left : CW_USEDEFAULT;
    const int y = positioned ? desc.Y + frame.top : CW_USEDEFAULT;
    const HWND window =
        CreateWindowExW(exStyle, WindowsApplication::WindowClassName, title, style, x, y, frame.right - frame.left,
                        frame.bottom - frame.top, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
    {
        return {};
    }
    auto *data = static_cast<WindowsApplication::WindowData *>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WindowsApplication::WindowData)));
    InitializeSRWLock(&data->HitLock);
    data->Presented = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    data->CustomFrame = customFrame;
    data->MinWidth = desc.MinWidth;
    data->MinHeight = desc.MinHeight;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    if (customFrame)
    {
        // WM_NCCALCSIZE ran during creation, before the data said to drop the frame.
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const UINT windowDpi = GetDpiForWindow(window);
    if (windowDpi != dpi)
    {
        const RECT corrected = FrameForClient(window, style, exStyle, desc.Width, desc.Height, windowDpi);
        SetWindowPos(window, nullptr, 0, 0, corrected.right - corrected.left, corrected.bottom - corrected.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    DragAcceptFiles(window, TRUE);
    const bool maximized = HasAnyFlags(desc.Flags, WindowFlags::Maximized);
    if (!HasAnyFlags(desc.Flags, WindowFlags::Hidden))
    {
        ShowWindow(window, maximized                                    ? SW_SHOWMAXIMIZED
                           : HasAnyFlags(desc.Flags, WindowFlags::Tool) ? SW_SHOWNOACTIVATE
                                                                        : SW_SHOW);
    }
    else if (maximized)
    {
        // A hidden window cannot be maximized. A custom-frame window maximized has the monitor's work area as its
        // client area, so it takes that size now; Show then maximizes it without a resize.
        WINDOWPLACEMENT placement = {};
        placement.length = sizeof(placement);
        GetWindowPlacement(window, &placement);
        data->ShowMaximized = true;
        data->NormalRect = placement.rcNormalPosition;
        if (customFrame)
        {
            MONITORINFO monitor = {};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
            const RECT &area = monitor.rcWork;
            SetWindowPos(window, nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    return {reinterpret_cast<uintptr>(window)};
}
} // namespace

WindowHandle Create(const WindowDesc &desc)
{
    return WindowsApplication::CallOnWindowThread([&desc] { return CreateOnWindowThread(desc); });
}

void Destroy(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] {
        WindowsApplication::OnWindowDestroyed(hwnd);
        if (WindowsApplication::WindowData *data = GetWindowData(hwnd); data != nullptr && data->Presented != nullptr)
        {
            CloseHandle(data->Presented);
        }
        HeapFree(GetProcessHeap(), 0, GetWindowData(hwnd));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    });
}

void Show(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] {
        if (WindowsApplication::WindowData *data = GetWindowData(hwnd); data != nullptr && data->ShowMaximized)
        {
            data->ShowMaximized = false;
            WINDOWPLACEMENT placement = {};
            placement.length = sizeof(placement);
            GetWindowPlacement(hwnd, &placement);
            placement.rcNormalPosition = data->NormalRect;
            placement.showCmd = SW_SHOWMAXIMIZED;
            SetWindowPlacement(hwnd, &placement);
            return;
        }
        const bool noActivate = (static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)) & WS_EX_NOACTIVATE) != 0;
        ShowWindow(hwnd, noActivate ? SW_SHOWNOACTIVATE : SW_SHOW);
    });
}

void Hide(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] { ShowWindow(hwnd, SW_HIDE); });
}

void Minimize(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] { ShowWindow(hwnd, SW_MINIMIZE); });
}

void Maximize(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] { ShowWindow(hwnd, SW_MAXIMIZE); });
}

void Restore(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] { ShowWindow(hwnd, SW_RESTORE); });
}

void Focus(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd] {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    });
}

void SetTitle(WindowHandle window, const char *title)
{
    wchar_t wideTitle[256];
    if (Microsoft::Utf8ToWide(title, wideTitle, 256) < 256)
    {
        const HWND hwnd = ToHwnd(window);
        WindowsApplication::RunOnWindowThread([hwnd, &wideTitle] { SetWindowTextW(hwnd, wideTitle); });
    }
}

namespace
{
void SetFullscreenOnWindowThread(HWND hwnd, bool fullscreen)
{
    WindowsApplication::WindowData *data = GetWindowData(hwnd);
    if (data == nullptr || data->Fullscreen == fullscreen)
    {
        return;
    }
    if (fullscreen)
    {
        data->Style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        data->ExStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        data->Placement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &data->Placement);
        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          static_cast<LONG_PTR>((data->Style & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW)) | WS_POPUP));
        SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(data->Style));
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(data->ExStyle));
        SetWindowPlacement(hwnd, &data->Placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    data->Fullscreen = fullscreen;
}
} // namespace

void SetFullscreen(WindowHandle window, bool fullscreen)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd, fullscreen] { SetFullscreenOnWindowThread(hwnd, fullscreen); });
}

void SetClientPosition(WindowHandle window, int32 x, int32 y)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd, x, y] {
        RECT frame;
        GetWindowRect(hwnd, &frame);
        POINT origin = {0, 0};
        ClientToScreen(hwnd, &origin);
        SetWindowPos(hwnd, nullptr, x - (origin.x - frame.left), y - (origin.y - frame.top), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    });
}

void SetClientSize(WindowHandle window, int32 width, int32 height)
{
    const HWND hwnd = ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd, width, height] {
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        const RECT frame = FrameForClient(hwnd, style, exStyle, width, height, GetDpiForWindow(hwnd));
        SetWindowPos(hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    });
}

void RequestClose(WindowHandle window)
{
    PostMessageW(ToHwnd(window), WM_CLOSE, 0, 0);
}

void SetHitTestRegions(WindowHandle window, Span<const WindowHitRegion> regions)
{
    WindowsApplication::WindowData *data = GetWindowData(ToHwnd(window));
    if (data == nullptr)
    {
        return;
    }
    const uint32 count = regions.Count() < MaxHitRegions ? static_cast<uint32>(regions.Count()) : MaxHitRegions;
    AcquireSRWLockExclusive(&data->HitLock);
    for (uint32 index = 0; index < count; ++index)
    {
        data->HitRegions[index] = regions[index];
    }
    data->HitRegionCount = count;
    ReleaseSRWLockExclusive(&data->HitLock);
}

void NotifyPresented(WindowHandle window, int32 width, int32 height)
{
    WindowsApplication::WindowData *data = GetWindowData(ToHwnd(window));
    if (data == nullptr || data->Presented == nullptr)
    {
        return;
    }
    InterlockedExchange(&data->PresentedWidth, width);
    InterlockedExchange(&data->PresentedHeight, height);
    SetEvent(data->Presented);
}

PlatformRect GetClientRect(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    RECT rect;
    ::GetClientRect(hwnd, &rect);
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, rect.right - rect.left, rect.bottom - rect.top};
}

bool IsVisible(WindowHandle window)
{
    return IsWindowVisible(ToHwnd(window)) != 0;
}

bool IsFocused(WindowHandle window)
{
    const HWND hwnd = ToHwnd(window);
    return WindowsApplication::CallOnWindowThread([hwnd] { return GetFocus() == hwnd; });
}

bool IsMinimized(WindowHandle window)
{
    return IsIconic(ToHwnd(window)) != 0;
}

bool IsMaximized(WindowHandle window)
{
    return IsZoomed(ToHwnd(window)) != 0;
}

bool IsFullscreen(WindowHandle window)
{
    const WindowsApplication::WindowData *data = GetWindowData(ToHwnd(window));
    return data != nullptr && data->Fullscreen;
}

float32 GetDpiScale(WindowHandle window)
{
    return static_cast<float32>(GetDpiForWindow(ToHwnd(window))) / USER_DEFAULT_SCREEN_DPI;
}

void *GetNativeHandle(WindowHandle window)
{
    return ToHwnd(window);
}

DisplayColorInfo GetDisplayColor(WindowHandle window)
{
    DisplayColorInfo info;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(ToHwnd(window), MONITOR_DEFAULTTONEAREST), &monitor))
    {
        return info;
    }
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0)
    {
        return info;
    }
    List<DISPLAYCONFIG_PATH_INFO> paths;
    List<DISPLAYCONFIG_MODE_INFO> modes;
    paths.Resize(pathCount);
    modes.Resize(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.Data(), &modeCount, modes.Data(), nullptr) !=
        ERROR_SUCCESS)
    {
        return info;
    }
    for (UINT32 index = 0; index < pathCount; ++index)
    {
        const DISPLAYCONFIG_PATH_INFO &path = paths[index];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, monitor.szDevice) != 0)
        {
            continue;
        }
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = path.targetInfo.adapterId;
        color.header.id = path.targetInfo.id;
        info.Hdr = DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS && color.advancedColorEnabled != 0;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (info.Hdr && DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel != 0)
        {
            info.SdrWhiteNits = static_cast<float32>(white.SDRWhiteLevel) * 80.0f / 1000.0f;
        }
        break;
    }
    return info;
}
} // namespace Marlin::PlatformWindow
