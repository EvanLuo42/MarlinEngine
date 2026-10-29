#include "Application/PlatformApplication.h"

#include "Concurrency/SpinLock.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "Memory/MemoryOps.h"
#include "WindowsApplicationState.h"

#include <imm.h>
#include <shellapi.h>
#include <shellscalingapi.h>

namespace Marlin
{
namespace
{
constexpr uint32 EventCapacity = 1024;
constexpr usize CompositionCapacity = 1024;

struct EventQueue
{
    ApplicationEvent Events[EventCapacity];
    uint32 Head = 0;
    uint32 Count = 0;
};

struct ApplicationState
{
    /// What the window thread queued since the last PumpMessages, under Lock.
    EventQueue Pending;
    HDROP PendingDrop = nullptr;
    char PendingComposition[CompositionCapacity] = {};
    SpinLock Lock;
    /// What PumpMessages handed to the main thread.
    EventQueue Delivered;
    HDROP Drop = nullptr;
    char Composition[CompositionCapacity] = {};
    /// Window thread state.
    HCURSOR Cursor = nullptr;
    bool CursorHidden = false;
    HWND RelativeWindow = nullptr;
    wchar_t HighSurrogate = 0;
    HWND Dispatcher = nullptr;
    DWORD ThreadId = 0;
    ThreadHandle Thread;
    bool Started = false;
    bool Initialized = false;
};

ApplicationState _application;

void PushEvent(const ApplicationEvent &event)
{
    ScopeLock lock(_application.Lock);
    EventQueue &queue = _application.Pending;
    // A high-rate mouse sends raw motion a thousand times a second or more: consecutive motion adds up into one.
    if (event.Type == ApplicationEventType::MouseRawMotion && queue.Count > 0)
    {
        ApplicationEvent &last = queue.Events[(queue.Head + queue.Count - 1) % EventCapacity];
        if (last.Type == ApplicationEventType::MouseRawMotion && last.Window.Value == event.Window.Value)
        {
            last.X += event.X;
            last.Y += event.Y;
            return;
        }
    }
    if (queue.Count == EventCapacity)
    {
        return;
    }
    queue.Events[(queue.Head + queue.Count) % EventCapacity] = event;
    ++queue.Count;
}

ApplicationEvent MakeEvent(ApplicationEventType type, HWND window)
{
    ApplicationEvent event;
    event.Type = type;
    event.Window = {reinterpret_cast<uintptr>(window)};
    return event;
}

ModifierKeys CurrentModifiers()
{
    ModifierKeys modifiers = ModifierKeys::None;
    if (GetKeyState(VK_SHIFT) < 0)
    {
        modifiers |= ModifierKeys::Shift;
    }
    if (GetKeyState(VK_CONTROL) < 0)
    {
        modifiers |= ModifierKeys::Control;
    }
    if (GetKeyState(VK_MENU) < 0)
    {
        modifiers |= ModifierKeys::Alt;
    }
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
    {
        modifiers |= ModifierKeys::Super;
    }
    return modifiers;
}

KeyCode TranslateKey(WPARAM virtualKey, LPARAM lParam)
{
    if (virtualKey == VK_PAUSE)
    {
        return KeyCode::Pause;
    }
    if (virtualKey == VK_NUMLOCK)
    {
        return KeyCode::NumLock;
    }
    const auto scanCode = static_cast<uint32>((static_cast<uint64>(lParam) >> 16) & 0xFF);
    const bool extended = (static_cast<uint64>(lParam) & (1u << 24)) != 0;
    if (extended)
    {
        switch (scanCode)
        {
        case 0x1C:
            return KeyCode::NumpadEnter;
        case 0x1D:
            return KeyCode::RightControl;
        case 0x35:
            return KeyCode::NumpadDivide;
        case 0x37:
            return KeyCode::PrintScreen;
        case 0x38:
            return KeyCode::RightAlt;
        case 0x47:
            return KeyCode::Home;
        case 0x48:
            return KeyCode::Up;
        case 0x49:
            return KeyCode::PageUp;
        case 0x4B:
            return KeyCode::Left;
        case 0x4D:
            return KeyCode::Right;
        case 0x4F:
            return KeyCode::End;
        case 0x50:
            return KeyCode::Down;
        case 0x51:
            return KeyCode::PageDown;
        case 0x52:
            return KeyCode::Insert;
        case 0x53:
            return KeyCode::Delete;
        case 0x5B:
            return KeyCode::LeftSuper;
        case 0x5C:
            return KeyCode::RightSuper;
        case 0x5D:
            return KeyCode::Menu;
        default:
            return KeyCode::Unknown;
        }
    }

    static constexpr KeyCode Table[0x59] = {
        KeyCode::Unknown,
        KeyCode::Escape,
        KeyCode::Digit1,
        KeyCode::Digit2,
        KeyCode::Digit3,
        KeyCode::Digit4,
        KeyCode::Digit5,
        KeyCode::Digit6,
        KeyCode::Digit7,
        KeyCode::Digit8,
        KeyCode::Digit9,
        KeyCode::Digit0,
        KeyCode::Minus,
        KeyCode::Equals,
        KeyCode::Backspace,
        KeyCode::Tab,
        KeyCode::Q,
        KeyCode::W,
        KeyCode::E,
        KeyCode::R,
        KeyCode::T,
        KeyCode::Y,
        KeyCode::U,
        KeyCode::I,
        KeyCode::O,
        KeyCode::P,
        KeyCode::LeftBracket,
        KeyCode::RightBracket,
        KeyCode::Enter,
        KeyCode::LeftControl,
        KeyCode::A,
        KeyCode::S,
        KeyCode::D,
        KeyCode::F,
        KeyCode::G,
        KeyCode::H,
        KeyCode::J,
        KeyCode::K,
        KeyCode::L,
        KeyCode::Semicolon,
        KeyCode::Apostrophe,
        KeyCode::Grave,
        KeyCode::LeftShift,
        KeyCode::Backslash,
        KeyCode::Z,
        KeyCode::X,
        KeyCode::C,
        KeyCode::V,
        KeyCode::B,
        KeyCode::N,
        KeyCode::M,
        KeyCode::Comma,
        KeyCode::Period,
        KeyCode::Slash,
        KeyCode::RightShift,
        KeyCode::NumpadMultiply,
        KeyCode::LeftAlt,
        KeyCode::Space,
        KeyCode::CapsLock,
        KeyCode::F1,
        KeyCode::F2,
        KeyCode::F3,
        KeyCode::F4,
        KeyCode::F5,
        KeyCode::F6,
        KeyCode::F7,
        KeyCode::F8,
        KeyCode::F9,
        KeyCode::F10,
        KeyCode::NumLock,
        KeyCode::ScrollLock,
        KeyCode::Numpad7,
        KeyCode::Numpad8,
        KeyCode::Numpad9,
        KeyCode::NumpadSubtract,
        KeyCode::Numpad4,
        KeyCode::Numpad5,
        KeyCode::Numpad6,
        KeyCode::NumpadAdd,
        KeyCode::Numpad1,
        KeyCode::Numpad2,
        KeyCode::Numpad3,
        KeyCode::Numpad0,
        KeyCode::NumpadDecimal,
        KeyCode::Unknown,
        KeyCode::Unknown,
        KeyCode::IntlBackslash,
        KeyCode::F11,
        KeyCode::F12,
    };
    if (scanCode < sizeof(Table) / sizeof(Table[0]))
    {
        return Table[scanCode];
    }
    if (scanCode >= 0x64 && scanCode <= 0x6E)
    {
        return static_cast<KeyCode>(static_cast<uint32>(KeyCode::F13) + (scanCode - 0x64));
    }
    return scanCode == 0x76 ? KeyCode::F24 : KeyCode::Unknown;
}

void PushMouseButton(HWND window, MouseButton button, bool down, LPARAM lParam)
{
    ApplicationEvent event =
        MakeEvent(down ? ApplicationEventType::MouseButtonDown : ApplicationEventType::MouseButtonUp, window);
    event.Button = button;
    event.X = static_cast<int16>(LOWORD(lParam));
    event.Y = static_cast<int16>(HIWORD(lParam));
    event.Modifiers = CurrentModifiers();
    PushEvent(event);
}

void ClipToClient(HWND window)
{
    RECT rect;
    ::GetClientRect(window, &rect);
    POINT corners[2] = {{rect.left, rect.top}, {rect.right, rect.bottom}};
    MapWindowPoints(window, nullptr, corners, 2);
    const RECT clip = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    ClipCursor(&clip);
}

void SetPendingComposition(const char *text)
{
    ScopeLock lock(_application.Lock);
    usize length = 0;
    while (text[length] != '\0' && length + 1 < CompositionCapacity)
    {
        ++length;
    }
    MemCopy(_application.PendingComposition, text, length);
    _application.PendingComposition[length] = '\0';
}

void UpdateComposition(HWND window)
{
    char composition[CompositionCapacity] = {};
    const HIMC context = ImmGetContext(window);
    if (context == nullptr)
    {
        SetPendingComposition(composition);
        return;
    }
    const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    if (bytes > 0)
    {
        const usize length = static_cast<usize>(bytes) / sizeof(wchar_t);
        auto *text = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(wchar_t)));
        if (text != nullptr)
        {
            ImmGetCompositionStringW(context, GCS_COMPSTR, text, static_cast<DWORD>(bytes));
            text[length] = L'\0';
            if (Microsoft::WideToUtf8(text, composition, CompositionCapacity) >= CompositionCapacity)
            {
                composition[0] = '\0';
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
    }
    ImmReleaseContext(window, context);
    SetPendingComposition(composition);
}

void PushCharacter(HWND window, wchar_t unit)
{
    if (unit >= 0xD800 && unit <= 0xDBFF)
    {
        _application.HighSurrogate = unit;
        return;
    }
    uint32 codePoint = unit;
    if (unit >= 0xDC00 && unit <= 0xDFFF)
    {
        if (_application.HighSurrogate == 0)
        {
            return;
        }
        codePoint = 0x10000 + ((static_cast<uint32>(_application.HighSurrogate) - 0xD800) << 10) +
                    (static_cast<uint32>(unit) - 0xDC00);
    }
    _application.HighSurrogate = 0;
    if (codePoint < 0x20 || codePoint == 0x7F)
    {
        return;
    }
    ApplicationEvent event = MakeEvent(ApplicationEventType::TextInput, window);
    event.Character = codePoint;
    PushEvent(event);
}

struct MonitorList
{
    MonitorInfo *Monitors;
    uint32 MaxCount;
    uint32 Count;
};

BOOL CALLBACK AddMonitor(HMONITOR monitor, [[maybe_unused]] HDC deviceContext, [[maybe_unused]] LPRECT clip,
                         LPARAM context)
{
    auto &list = *reinterpret_cast<MonitorList *>(context);
    if (list.Count == list.MaxCount)
    {
        return FALSE;
    }
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
    {
        return TRUE;
    }
    MonitorInfo &result = list.Monitors[list.Count++];
    result = {};
    result.Bounds = {
        info.rcMonitor.left,
        info.rcMonitor.top,
        info.rcMonitor.right - info.rcMonitor.left,
        info.rcMonitor.bottom - info.rcMonitor.top,
    };
    result.WorkArea = {
        info.rcWork.left,
        info.rcWork.top,
        info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top,
    };
    result.IsPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        result.DpiScale = static_cast<float32>(dpiX) / USER_DEFAULT_SCREEN_DPI;
    }
    if (Microsoft::WideToUtf8(info.szDevice, result.Name, sizeof(result.Name)) >= sizeof(result.Name))
    {
        result.Name[0] = '\0';
    }
    return TRUE;
}

LPCWSTR SystemCursor(CursorShape shape)
{
    switch (shape)
    {
    case CursorShape::IBeam:
        return IDC_IBEAM;
    case CursorShape::Hand:
        return IDC_HAND;
    case CursorShape::Crosshair:
        return IDC_CROSS;
    case CursorShape::ResizeHorizontal:
        return IDC_SIZEWE;
    case CursorShape::ResizeVertical:
        return IDC_SIZENS;
    case CursorShape::ResizeDiagonalDown:
        return IDC_SIZENWSE;
    case CursorShape::ResizeDiagonalUp:
        return IDC_SIZENESW;
    case CursorShape::Move:
        return IDC_SIZEALL;
    case CursorShape::Wait:
        return IDC_WAIT;
    case CursorShape::NotAllowed:
        return IDC_NO;
    case CursorShape::Arrow:
    case CursorShape::Hidden:
        return IDC_ARROW;
    }
    return IDC_ARROW;
}

/// How long a resize step waits at most for a frame of its size.
constexpr ULONGLONG ResizeWaitMilliseconds = 100;
} // namespace

namespace WindowsApplication
{
/// Holds a resize step the user makes until a frame of *width* × *height* was presented to the window (or
/// ResizeWaitMilliseconds pass): the renderer resizes the swapchain a frame later, and until then the new area shows
/// nothing.
void WaitForPresent(WindowData &data, uint32 width, uint32 height)
{
    if (data.Presented == nullptr || data.PresentedWidth == 0)
    {
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + ResizeWaitMilliseconds;
    for (;;)
    {
        if (static_cast<uint32>(InterlockedCompareExchange(&data.PresentedWidth, 0, 0)) == width &&
            static_cast<uint32>(InterlockedCompareExchange(&data.PresentedHeight, 0, 0)) == height)
        {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return;
        }
        // Messages other threads send this one, such as the main thread's calls into it, are handled meanwhile.
        const DWORD result =
            MsgWaitForMultipleObjectsEx(1, &data.Presented, static_cast<DWORD>(deadline - now), QS_SENDMESSAGE, 0);
        if (result == WAIT_OBJECT_0 + 1)
        {
            MSG message;
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        else if (result != WAIT_OBJECT_0)
        {
            return;
        }
    }
}

bool HasCustomFrame(HWND window)
{
    const WindowData *data = GetWindowData(window);
    return data != nullptr && data->CustomFrame;
}

/// WM_NCHITTEST of a CustomFrame window: the resize edges unless maximized, then the application's regions, last
/// first.
LRESULT HitTestCustomFrame(HWND window, LPARAM lParam)
{
    POINT point = {static_cast<int16>(LOWORD(lParam)), static_cast<int16>(HIWORD(lParam))};
    ScreenToClient(window, &point);
    RECT client;
    ::GetClientRect(window, &client);
    if (!IsZoomed(window))
    {
        const int32 border = GetFrameThickness(GetDpiForWindow(window));
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;
        if (top)
        {
            return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        }
        if (bottom)
        {
            return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        }
        if (left || right)
        {
            return left ? HTLEFT : HTRIGHT;
        }
    }
    WindowData *data = GetWindowData(window);
    LRESULT result = HTCLIENT;
    AcquireSRWLockShared(&data->HitLock);
    for (uint32 index = data->HitRegionCount; index-- > 0;)
    {
        const PlatformRect &rect = data->HitRegions[index].Rect;
        if (point.x >= rect.X && point.x < rect.X + rect.Width && point.y >= rect.Y && point.y < rect.Y + rect.Height)
        {
            result = data->HitRegions[index].Area == WindowHitArea::Caption ? HTCAPTION : HTCLIENT;
            break;
        }
    }
    ReleaseSRWLockShared(&data->HitLock);
    return result;
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCCALCSIZE:
        if (wParam == TRUE && HasCustomFrame(window))
        {
            // The client area is the whole window; maximized, the window hangs past the monitor by its frame.
            if (IsZoomed(window))
            {
                RECT &rect = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam)->rgrc[0];
                const int32 frame = GetFrameThickness(GetDpiForWindow(window));
                rect.left += frame;
                rect.top += frame;
                rect.right -= frame;
                rect.bottom -= frame;
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO:
        if (const WindowData *data = GetWindowData(window);
            data != nullptr && (data->MinWidth > 0 || data->MinHeight > 0))
        {
            // The minimum is a client size at 96 DPI; the OS wants the whole window's at the window's DPI.
            const UINT dpi = GetDpiForWindow(window);
            RECT rect = {0, 0, MulDiv(data->MinWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
                         MulDiv(data->MinHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
            if (!data->CustomFrame)
            {
                AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                                         static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), dpi);
            }
            auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
            if (data->MinWidth > 0)
            {
                info->ptMinTrackSize.x = rect.right - rect.left;
            }
            if (data->MinHeight > 0)
            {
                info->ptMinTrackSize.y = rect.bottom - rect.top;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (HasCustomFrame(window))
        {
            return HitTestCustomFrame(window, lParam);
        }
        break;
    case WM_NCACTIVATE:
        if (HasCustomFrame(window))
        {
            // Keeps DefWindowProc from painting the caption the client area covers.
            return DefWindowProcW(window, message, wParam, -1);
        }
        break;
    case WM_CLOSE:
        PushEvent(MakeEvent(ApplicationEventType::WindowCloseRequested, window));
        return 0;
    case WM_SIZE: {
        ApplicationEvent event = MakeEvent(ApplicationEventType::WindowResized, window);
        event.Width = LOWORD(lParam);
        event.Height = HIWORD(lParam);
        PushEvent(event);
        if (_application.RelativeWindow == window && GetFocus() == window)
        {
            ClipToClient(window);
        }
        if (WindowData *data = GetWindowData(window); data != nullptr && data->InSizeMove && wParam != SIZE_MINIMIZED)
        {
            WaitForPresent(*data, LOWORD(lParam), HIWORD(lParam));
        }
        break;
    }
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        if (WindowData *data = GetWindowData(window))
        {
            data->InSizeMove = message == WM_ENTERSIZEMOVE;
        }
        break;
    case WM_MOVE: {
        ApplicationEvent event = MakeEvent(ApplicationEventType::WindowMoved, window);
        event.X = static_cast<int16>(LOWORD(lParam));
        event.Y = static_cast<int16>(HIWORD(lParam));
        PushEvent(event);
        if (_application.RelativeWindow == window && GetFocus() == window)
        {
            ClipToClient(window);
        }
        break;
    }
    case WM_SETFOCUS:
        PushEvent(MakeEvent(ApplicationEventType::WindowFocusGained, window));
        if (_application.RelativeWindow == window)
        {
            ClipToClient(window);
        }
        break;
    case WM_KILLFOCUS:
        PushEvent(MakeEvent(ApplicationEventType::WindowFocusLost, window));
        if (_application.RelativeWindow == window)
        {
            ClipCursor(nullptr);
        }
        break;
    case WM_DPICHANGED: {
        const auto *suggested = reinterpret_cast<const RECT *>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ApplicationEvent event = MakeEvent(ApplicationEventType::WindowDpiChanged, window);
        event.DpiScale = static_cast<float32>(HIWORD(wParam)) / USER_DEFAULT_SCREEN_DPI;
        PushEvent(event);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        ApplicationEvent event = MakeEvent(down ? ApplicationEventType::KeyDown : ApplicationEventType::KeyUp, window);
        event.Key = TranslateKey(wParam, lParam);
        event.Repeat = down && (static_cast<uint64>(lParam) & (1u << 30)) != 0;
        event.Modifiers = CurrentModifiers();
        PushEvent(event);
        if (message == WM_SYSKEYDOWN && wParam == VK_F4)
        {
            break;
        }
        if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
        {
            return 0;
        }
        break;
    }
    case WM_CHAR:
        PushCharacter(window, static_cast<wchar_t>(wParam));
        return 0;
    case WM_SYSCHAR:
        return 0;
    case WM_IME_COMPOSITION:
        if ((static_cast<uint64>(lParam) & GCS_COMPSTR) != 0)
        {
            UpdateComposition(window);
            PushEvent(MakeEvent(ApplicationEventType::TextComposition, window));
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        SetPendingComposition("");
        PushEvent(MakeEvent(ApplicationEventType::TextComposition, window));
        break;
    case WM_MOUSEMOVE: {
        WindowData *data = GetWindowData(window);
        if (data != nullptr && !data->TrackingMouse)
        {
            TRACKMOUSEEVENT track = {
                .cbSize = sizeof(TRACKMOUSEEVENT),
                .dwFlags = TME_LEAVE,
                .hwndTrack = window,
                .dwHoverTime = 0,
            };
            data->TrackingMouse = TrackMouseEvent(&track) != 0;
        }
        ApplicationEvent event = MakeEvent(ApplicationEventType::MouseMove, window);
        event.X = static_cast<int16>(LOWORD(lParam));
        event.Y = static_cast<int16>(HIWORD(lParam));
        event.Modifiers = CurrentModifiers();
        PushEvent(event);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (WindowData *data = GetWindowData(window))
        {
            data->TrackingMouse = false;
        }
        PushEvent(MakeEvent(ApplicationEventType::MouseLeave, window));
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        PushMouseButton(window, MouseButton::Left, message == WM_LBUTTONDOWN, lParam);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        PushMouseButton(window, MouseButton::Right, message == WM_RBUTTONDOWN, lParam);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        PushMouseButton(window, MouseButton::Middle, message == WM_MBUTTONDOWN, lParam);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        PushMouseButton(window, GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2,
                        message == WM_XBUTTONDOWN, lParam);
        return TRUE;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        ApplicationEvent event = MakeEvent(ApplicationEventType::MouseWheel, window);
        const float32 notches = static_cast<float32>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        if (message == WM_MOUSEWHEEL)
        {
            event.WheelY = notches;
        }
        else
        {
            event.WheelX = notches;
        }
        event.Modifiers = CurrentModifiers();
        PushEvent(event);
        return 0;
    }
    case WM_INPUT: {
        RAWINPUT input;
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) !=
                static_cast<UINT>(-1) &&
            input.header.dwType == RIM_TYPEMOUSE && (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
            (input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0))
        {
            ApplicationEvent event = MakeEvent(ApplicationEventType::MouseRawMotion, window);
            event.X = input.data.mouse.lLastX;
            event.Y = input.data.mouse.lLastY;
            PushEvent(event);
        }
        break;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            const bool hidden = _application.CursorHidden || _application.RelativeWindow == window;
            SetCursor(hidden ? nullptr : _application.Cursor);
            return TRUE;
        }
        break;
    case WM_DROPFILES: {
        HDROP replaced = nullptr;
        {
            ScopeLock lock(_application.Lock);
            replaced = _application.PendingDrop;
            _application.PendingDrop = reinterpret_cast<HDROP>(wParam);
        }
        if (replaced != nullptr)
        {
            DragFinish(replaced);
        }
        PushEvent(MakeEvent(ApplicationEventType::FilesDropped, window));
        return 0;
    }
    case WM_DISPLAYCHANGE:
        PushEvent(MakeEvent(ApplicationEventType::MonitorsChanged, window));
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void OnWindowDestroyed(HWND window)
{
    if (_application.RelativeWindow == window)
    {
        ClipCursor(nullptr);
        _application.RelativeWindow = nullptr;
    }
}

HWND GetDispatcher()
{
    return _application.Dispatcher;
}

DWORD GetWindowThreadId()
{
    return _application.ThreadId;
}
} // namespace WindowsApplication

namespace
{
constexpr wchar_t DispatcherClassName[] = L"MarlinWindowDispatcher";

LRESULT CALLBACK DispatcherProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WindowsApplication::InvokeMessage)
    {
        reinterpret_cast<void (*)(void *)>(wParam)(reinterpret_cast<void *>(lParam));
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool StartWindowThread()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowsApplication::WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.lpszClassName = WindowsApplication::WindowClassName;
    WNDCLASSEXW dispatcherClass = {};
    dispatcherClass.cbSize = sizeof(dispatcherClass);
    dispatcherClass.lpfnWndProc = DispatcherProcedure;
    dispatcherClass.hInstance = instance;
    dispatcherClass.lpszClassName = DispatcherClassName;
    if (RegisterClassExW(&windowClass) == 0 || RegisterClassExW(&dispatcherClass) == 0)
    {
        return false;
    }
    _application.Dispatcher =
        CreateWindowExW(0, DispatcherClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (_application.Dispatcher == nullptr)
    {
        return false;
    }
    const RAWINPUTDEVICE mouse = {.usUsagePage = 0x01, .usUsage = 0x02, .dwFlags = 0, .hwndTarget = nullptr};
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    return true;
}

struct WindowThreadStart
{
    Event Ready{EventReset::Manual};
    bool Succeeded = false;
};

void WindowThread(void *argument)
{
    auto &start = *static_cast<WindowThreadStart *>(argument);
    _application.ThreadId = GetCurrentThreadId();
    start.Succeeded = StartWindowThread();
    start.Ready.Set();
    if (!start.Succeeded)
    {
        return;
    }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(_application.Dispatcher);
    _application.Dispatcher = nullptr;
    UnregisterClassW(WindowsApplication::WindowClassName, GetModuleHandleW(nullptr));
    UnregisterClassW(DispatcherClassName, GetModuleHandleW(nullptr));
}

void FinishDrop(HDROP &drop)
{
    if (drop != nullptr)
    {
        DragFinish(drop);
        drop = nullptr;
    }
}
} // namespace

namespace PlatformApplication
{
bool Initialize()
{
    if (_application.Initialized)
    {
        return true;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    _application.Cursor = LoadCursorW(nullptr, IDC_ARROW);
    WindowThreadStart start;
    ThreadDesc desc;
    desc.Name = "Marlin Window";
    desc.Entry = WindowThread;
    desc.Argument = &start;
    desc.Priority = ThreadPriority::AboveNormal;
    _application.Thread = PlatformThread::Create(desc);
    if (!_application.Thread.IsValid())
    {
        return false;
    }
    start.Ready.Wait();
    if (!start.Succeeded)
    {
        PlatformThread::Join(_application.Thread);
        _application.Thread = {};
        return false;
    }
    _application.Initialized = true;
    return true;
}

void Shutdown()
{
    if (!_application.Initialized)
    {
        return;
    }
    WindowsApplication::RunOnWindowThread([] {
        ClipCursor(nullptr);
        PostQuitMessage(0);
    });
    PlatformThread::Join(_application.Thread);
    _application.Thread = {};
    _application.ThreadId = 0;
    FinishDrop(_application.Drop);
    FinishDrop(_application.PendingDrop);
    _application.Initialized = false;
}

void PumpMessages()
{
    FinishDrop(_application.Drop);
    ScopeLock lock(_application.Lock);
    EventQueue &pending = _application.Pending;
    EventQueue &delivered = _application.Delivered;
    while (pending.Count != 0 && delivered.Count != EventCapacity)
    {
        delivered.Events[(delivered.Head + delivered.Count) % EventCapacity] = pending.Events[pending.Head];
        ++delivered.Count;
        pending.Head = (pending.Head + 1) % EventCapacity;
        --pending.Count;
    }
    _application.Drop = _application.PendingDrop;
    _application.PendingDrop = nullptr;
    MemCopy(_application.Composition, _application.PendingComposition, CompositionCapacity);
}

bool PollEvent(ApplicationEvent &event)
{
    EventQueue &queue = _application.Delivered;
    if (queue.Count == 0)
    {
        return false;
    }
    event = queue.Events[queue.Head];
    queue.Head = (queue.Head + 1) % EventCapacity;
    --queue.Count;
    return true;
}

uint32 GetMonitors(MonitorInfo *monitors, uint32 maxCount)
{
    MonitorList list = {.Monitors = monitors, .MaxCount = maxCount, .Count = 0};
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&list));
    return list.Count;
}

void SetCursorShape(CursorShape shape)
{
    WindowsApplication::RunOnWindowThread([shape] {
        _application.CursorHidden = shape == CursorShape::Hidden;
        _application.Cursor = LoadCursorW(nullptr, SystemCursor(shape));
        POINT point;
        if (GetCursorPos(&point))
        {
            const HWND window = WindowFromPoint(point);
            if (window != nullptr && GetWindowLongPtrW(window, GWLP_WNDPROC) ==
                                         reinterpret_cast<LONG_PTR>(WindowsApplication::WindowProcedure))
            {
                SetCursor(_application.CursorHidden ? nullptr : _application.Cursor);
            }
        }
    });
}

void GetCursorPosition(int32 &x, int32 &y)
{
    POINT point = {};
    GetCursorPos(&point);
    x = point.x;
    y = point.y;
}

void SetCursorPosition(int32 x, int32 y)
{
    SetCursorPos(x, y);
}

bool IsMouseButtonDown(MouseButton button)
{
    // The virtual keys name the physical buttons; swapped buttons make the right one primary.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    int key = VK_LBUTTON;
    switch (button)
    {
    case MouseButton::Left:
        key = swapped ? VK_RBUTTON : VK_LBUTTON;
        break;
    case MouseButton::Right:
        key = swapped ? VK_LBUTTON : VK_RBUTTON;
        break;
    case MouseButton::Middle:
        key = VK_MBUTTON;
        break;
    case MouseButton::X1:
        key = VK_XBUTTON1;
        break;
    case MouseButton::X2:
        key = VK_XBUTTON2;
        break;
    }
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void SetMouseCapture(WindowHandle window, bool capture)
{
    const HWND hwnd = WindowsApplication::ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd, capture] {
        if (capture)
        {
            SetCapture(hwnd);
        }
        else if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }
    });
}

void SetRelativeMouseMode(WindowHandle window, bool enabled)
{
    const HWND hwnd = WindowsApplication::ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd, enabled] {
        if (enabled)
        {
            _application.RelativeWindow = hwnd;
            if (GetFocus() == hwnd)
            {
                ClipToClient(hwnd);
            }
            SetCursor(nullptr);
        }
        else if (_application.RelativeWindow == hwnd)
        {
            _application.RelativeWindow = nullptr;
            ClipCursor(nullptr);
            SetCursor(_application.CursorHidden ? nullptr : _application.Cursor);
        }
    });
}

usize GetClipboardText(char *buffer, usize capacity)
{
    if (!OpenClipboard(nullptr))
    {
        return 0;
    }
    usize length = 0;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr)
    {
        if (const auto *text = static_cast<const wchar_t *>(GlobalLock(data)))
        {
            length = Microsoft::WideToUtf8(text, buffer, capacity);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return length;
}

bool SetClipboardText(const char *text)
{
    const usize length = Microsoft::Utf8ToWide(text, nullptr, 0);
    if (length == 0 && text[0] != '\0')
    {
        return false;
    }
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (length + 1) * sizeof(wchar_t));
    if (memory == nullptr)
    {
        return false;
    }
    auto *wide = static_cast<wchar_t *>(GlobalLock(memory));
    Microsoft::Utf8ToWide(text, wide, length + 1);
    GlobalUnlock(memory);
    if (!OpenClipboard(nullptr))
    {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool succeeded = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!succeeded)
    {
        GlobalFree(memory);
    }
    return succeeded;
}

void SetTextInputEnabled(WindowHandle window, bool enabled)
{
    const HWND hwnd = WindowsApplication::ToHwnd(window);
    WindowsApplication::RunOnWindowThread(
        [hwnd, enabled] { ImmAssociateContextEx(hwnd, nullptr, enabled ? IACE_DEFAULT : 0); });
}

void SetTextInputArea(WindowHandle window, const PlatformRect &caret)
{
    const HWND hwnd = WindowsApplication::ToHwnd(window);
    WindowsApplication::RunOnWindowThread([hwnd, caret] {
        const HIMC context = ImmGetContext(hwnd);
        if (context == nullptr)
        {
            return;
        }
        COMPOSITIONFORM composition = {};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = {caret.X, caret.Y};
        ImmSetCompositionWindow(context, &composition);
        CANDIDATEFORM candidate = {};
        candidate.dwIndex = 0;
        candidate.dwStyle = CFS_EXCLUDE;
        candidate.ptCurrentPos = {caret.X, caret.Y + caret.Height};
        candidate.rcArea = {caret.X, caret.Y, caret.X + caret.Width, caret.Y + caret.Height};
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(hwnd, context);
    });
}

usize GetComposition(char *buffer, usize capacity)
{
    usize length = 0;
    while (_application.Composition[length] != '\0')
    {
        ++length;
    }
    if (length < capacity)
    {
        MemCopy(buffer, _application.Composition, length + 1);
    }
    return length;
}

uint32 GetDroppedFileCount()
{
    return _application.Drop != nullptr ? DragQueryFileW(_application.Drop, 0xFFFFFFFFu, nullptr, 0) : 0;
}

usize GetDroppedFile(uint32 index, char *buffer, usize capacity)
{
    if (_application.Drop == nullptr)
    {
        return 0;
    }
    const UINT wideLength = DragQueryFileW(_application.Drop, index, nullptr, 0);
    if (wideLength == 0)
    {
        return 0;
    }
    auto *path = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, (wideLength + 1) * sizeof(wchar_t)));
    if (path == nullptr)
    {
        return 0;
    }
    DragQueryFileW(_application.Drop, index, path, wideLength + 1);
    const usize length = Microsoft::WideToUtf8Path(path, buffer, capacity);
    HeapFree(GetProcessHeap(), 0, path);
    return length;
}

void ShowMessageBox(const char *title, const char *message)
{
    const usize titleLength = Microsoft::Utf8ToWide(title, nullptr, 0);
    const usize messageLength = Microsoft::Utf8ToWide(message, nullptr, 0);
    auto *text =
        static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, (titleLength + messageLength + 2) * sizeof(wchar_t)));
    if (text == nullptr)
    {
        return;
    }
    Microsoft::Utf8ToWide(title, text, titleLength + 1);
    Microsoft::Utf8ToWide(message, text + titleLength + 1, messageLength + 1);
    MessageBoxW(nullptr, text + titleLength + 1, text, MB_OK | MB_ICONINFORMATION);
    HeapFree(GetProcessHeap(), 0, text);
}
} // namespace PlatformApplication
} // namespace Marlin
