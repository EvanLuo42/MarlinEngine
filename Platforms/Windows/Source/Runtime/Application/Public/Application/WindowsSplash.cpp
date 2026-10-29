#include "Application/PlatformSplash.h"

#include "Application/PlatformApplication.h"
#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "Concurrency/SpinLock.h"
#include "Memory/MemoryOps.h"
#include "WindowsApplicationState.h"

#include <dwmapi.h>
#include <initializer_list>
#include <shellscalingapi.h>

namespace Marlin
{
namespace
{
constexpr wchar_t SplashClassName[] = L"MarlinSplash";
constexpr UINT_PTR TickTimer = 1;
constexpr UINT TickMilliseconds = 50;
constexpr usize TextCapacity = 256;
constexpr uint32 MaxFontFiles = 8;

// The layout and colors of the splash design, in pixels at 96 DPI: a 720×400 card with an accent bar on its left.
constexpr float32 CardWidth = 720;
constexpr float32 CardHeight = 400;
constexpr float32 Left = 40;
constexpr float32 Right = CardWidth - 40;
constexpr COLORREF BackgroundColor = RGB(0x16, 0x16, 0x16);
constexpr COLORREF BorderColor = RGB(0x2A, 0x2A, 0x2A);
constexpr COLORREF AccentColor = RGB(0xF0, 0xA5, 0x31);
constexpr COLORREF LogoMarkColor = RGB(0x1A, 0x12, 0x08);
/// The logo: a 22-unit tile with a marlin in profile, three flat shapes facing left: the bill running into the body
/// and the forked tail, and the tall swept sail of its dorsal fin.
constexpr float32 LogoSize = 22;
constexpr float32 LogoRadius = 4;
constexpr POINT LogoBody[] = {{25, 132},  {90, 122},  {150, 120}, {172, 126}, {200, 92}, {186, 136},
                              {200, 178}, {172, 146}, {140, 156}, {90, 150},  {55, 139}};
constexpr POINT LogoSail[] = {{84, 124}, {108, 68}, {136, 36}, {142, 72}, {158, 122}};
/// Logo coordinates are in tenths of a unit; the tile is drawn this much larger and filtered down, as GDI does not
/// antialias polygons.
constexpr int32 LogoOversample = 4;
constexpr COLORREF TitleColor = RGB(0xEC, 0xEC, 0xEC);
constexpr COLORREF BrightColor = RGB(0xD4, 0xD4, 0xD4);
constexpr COLORREF MutedColor = RGB(0x8A, 0x8A, 0x8A);
constexpr COLORREF DimColor = RGB(0x7A, 0x7A, 0x7A);
constexpr COLORREF FaintColor = RGB(0x6A, 0x6A, 0x6A);

struct SplashFonts
{
    HFONT Brand = nullptr;
    HFONT Meta = nullptr;
    HFONT Title = nullptr;
    HFONT Subtitle = nullptr;
    HFONT Task = nullptr;
    HFONT Detail = nullptr;
    HFONT Percent = nullptr;
    HFONT Footer = nullptr;
};

struct SplashState
{
    /// Window thread state.
    HWND Window = nullptr;
    bool ClassRegistered = false;
    bool RoundedByDwm = false;
    float32 Scale = 1;
    SplashFonts Fonts;
    /// Written by Show before the window exists, then read on the window thread.
    wchar_t Title[TextCapacity] = {};
    wchar_t Brand[TextCapacity] = {};
    wchar_t Version[TextCapacity] = {};
    wchar_t Build[TextCapacity] = {};
    wchar_t Footer[TextCapacity] = {};
    wchar_t SansFamily[LF_FACESIZE] = {};
    wchar_t MonoFamily[LF_FACESIZE] = {};
    wchar_t FontFiles[MaxFontFiles][MAX_PATH] = {};
    uint32 FontFileCount = 0;
    void (*Tick)() = nullptr;
    /// The status, under Lock; Dirty until the window repaints.
    SpinLock Lock;
    wchar_t Project[TextCapacity] = {};
    wchar_t Task[TextCapacity] = {};
    wchar_t Detail[TextCapacity] = {};
    wchar_t Stage[TextCapacity] = {};
    float32 Progress = 0;
    bool Dirty = false;
    Atomic<bool> Shown{false};
};

SplashState _splash;

template <usize N> void CopyText(wchar_t (&target)[N], const char *text)
{
    target[0] = L'\0';
    if (text != nullptr && Microsoft::Utf8ToWide(text, target, N) >= N)
    {
        target[0] = L'\0';
    }
}

int32 Px(float32 value)
{
    return static_cast<int32>(value * _splash.Scale + (value >= 0 ? 0.5f : -0.5f));
}

RECT Box(float32 left, float32 top, float32 right, float32 bottom)
{
    return {Px(left), Px(top), Px(right), Px(bottom)};
}

/// A font of *family* at *size* pixels, or of *fallback* when the system substitutes another family for it.
HFONT MakeFont(HDC dc, const wchar_t *family, const wchar_t *fallback, float32 size, int weight)
{
    const auto create = [size, weight](const wchar_t *name) {
        return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
    };
    if (family[0] == L'\0')
    {
        return create(fallback);
    }
    HFONT font = create(family);
    wchar_t face[LF_FACESIZE] = {};
    const HGDIOBJ previous = SelectObject(dc, font);
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, previous);
    if (lstrcmpiW(face, family) != 0)
    {
        DeleteObject(font);
        font = create(fallback);
    }
    return font;
}

void CreateFonts(HDC dc)
{
    const wchar_t *sans = _splash.SansFamily;
    const wchar_t *mono = _splash.MonoFamily;
    SplashFonts &fonts = _splash.Fonts;
    fonts.Brand = MakeFont(dc, mono, L"Consolas", 11, FW_NORMAL);
    fonts.Meta = MakeFont(dc, mono, L"Consolas", 11, FW_NORMAL);
    fonts.Title = MakeFont(dc, sans, L"Segoe UI", 64, FW_LIGHT);
    fonts.Subtitle = MakeFont(dc, sans, L"Segoe UI", 14, FW_NORMAL);
    fonts.Task = MakeFont(dc, sans, L"Segoe UI", 12, FW_NORMAL);
    fonts.Detail = MakeFont(dc, mono, L"Consolas", 11, FW_NORMAL);
    fonts.Percent = MakeFont(dc, mono, L"Consolas", 12, FW_NORMAL);
    fonts.Footer = MakeFont(dc, mono, L"Consolas", 10.5f, FW_NORMAL);
}

void DeleteFonts()
{
    SplashFonts &fonts = _splash.Fonts;
    for (HFONT font :
         {fonts.Brand, fonts.Meta, fonts.Title, fonts.Subtitle, fonts.Task, fonts.Detail, fonts.Percent, fonts.Footer})
    {
        if (font != nullptr)
        {
            DeleteObject(font);
        }
    }
    fonts = {};
}

/// Draws one line of *text* centered in the line box *box*, as CSS places a line, with *extra* pixels between
/// characters.
void DrawLine(HDC dc, HFONT font, COLORREF color, const wchar_t *text, RECT box, UINT align, float32 extra = 0)
{
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetTextCharacterExtra(dc, Px(extra));
    // Not clipped to the box: a CSS line box lets descenders, such as the title's, hang past it.
    DrawTextW(dc, text, -1, &box, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_NOCLIP);
    SetTextCharacterExtra(dc, 0);
}

int32 TextWidth(HDC dc, HFONT font, const wchar_t *text)
{
    SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
    return size.cx;
}

void FillBox(HDC dc, RECT box, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, &box, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

/// Draws the logo tile with its top left at *left*, *top* (design pixels), antialiased by drawing it oversampled on
/// the card's background and filtering it down.
void DrawLogo(HDC dc, float32 left, float32 top)
{
    const int32 size = Px(LogoSize);
    const int32 large = size * LogoOversample;
    const HDC buffer = CreateCompatibleDC(dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(dc, large, large);
    const HGDIOBJ previous = SelectObject(buffer, bitmap);
    SelectObject(buffer, GetStockObject(NULL_PEN));
    SelectObject(buffer, GetStockObject(DC_BRUSH));
    SetDCBrushColor(buffer, BackgroundColor);
    const RECT all = {0, 0, large, large};
    FillRect(buffer, &all, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(buffer, AccentColor);
    const int32 corner = static_cast<int32>(2 * LogoRadius * static_cast<float32>(large) / LogoSize);
    RoundRect(buffer, 0, 0, large + 1, large + 1, corner, corner);

    SetDCBrushColor(buffer, LogoMarkColor);
    SetPolyFillMode(buffer, WINDING);
    const auto fill = [buffer, large](const POINT *shape, int count) {
        POINT points[16];
        for (int i = 0; i < count; ++i)
        {
            points[i] = {MulDiv(shape[i].x, large, static_cast<int>(LogoSize * 10)),
                         MulDiv(shape[i].y, large, static_cast<int>(LogoSize * 10))};
        }
        Polygon(buffer, points, count);
    };
    fill(LogoBody, static_cast<int>(sizeof(LogoBody) / sizeof(POINT)));
    fill(LogoSail, static_cast<int>(sizeof(LogoSail) / sizeof(POINT)));

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, Px(left), Px(top), size, size, buffer, 0, 0, large, large, SRCCOPY);
    SelectObject(buffer, previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void Paint(HDC dc)
{
    wchar_t project[TextCapacity];
    wchar_t task[TextCapacity];
    wchar_t detail[TextCapacity];
    wchar_t stage[TextCapacity];
    float32 progress = 0;
    {
        const ScopeLock lock(_splash.Lock);
        MemCopy(project, _splash.Project, sizeof(project));
        MemCopy(task, _splash.Task, sizeof(task));
        MemCopy(detail, _splash.Detail, sizeof(detail));
        MemCopy(stage, _splash.Stage, sizeof(stage));
        progress = _splash.Progress < 0 ? 0 : _splash.Progress > 1 ? 1 : _splash.Progress;
        _splash.Dirty = false;
    }
    const SplashFonts &fonts = _splash.Fonts;
    SetBkMode(dc, TRANSPARENT);
    FillBox(dc, Box(0, 0, CardWidth, CardHeight), BackgroundColor);
    if (!_splash.RoundedByDwm)
    {
        SetDCBrushColor(dc, BorderColor);
        const RECT frame = Box(0, 0, CardWidth, CardHeight);
        FrameRect(dc, &frame, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    FillBox(dc, Box(0, 0, 4, CardHeight), AccentColor);

    // Header: the logo and brand on the left, the version and build on the right.
    SelectObject(dc, GetStockObject(NULL_PEN));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    DrawLogo(dc, Left, 28);
    DrawLine(dc, fonts.Brand, MutedColor, _splash.Brand, Box(Left + 32, 28, Left + 300, 50), DT_LEFT, 0.88f);
    DrawLine(dc, fonts.Meta, DimColor, _splash.Version, Box(Right - 300, 28, CardWidth - 36, 45.6f), DT_RIGHT);
    DrawLine(dc, fonts.Meta, DimColor, _splash.Build, Box(Right - 300, 45.6f, CardWidth - 36, 63.2f), DT_RIGHT);

    // The title and subtitle, centered between the header and the status.
    DrawLine(dc, fonts.Title, TitleColor, _splash.Title, Box(Left, 150.6f, Right, 214.6f), DT_LEFT, -1.28f);
    if (project[0] != L'\0')
    {
        constexpr wchar_t Opening[] = L"Opening project ";
        const int32 openingWidth = TextWidth(dc, fonts.Subtitle, Opening);
        DrawLine(dc, fonts.Subtitle, MutedColor, Opening, Box(Left, 218.6f, Right, 237.6f), DT_LEFT);
        RECT name = Box(Left, 218.6f, Right, 237.6f);
        name.left += openingWidth;
        DrawLine(dc, fonts.Subtitle, BrightColor, project, name, DT_LEFT);
    }

    // The status: the task, its counts and the percentage, the bar, then the stage and the footer.
    wchar_t percent[8];
    wsprintfW(percent, L"%d%%", static_cast<int>(progress * 100));
    DrawLine(dc, fonts.Percent, AccentColor, percent, Box(Right - 40, 325, Right, 341), DT_RIGHT);
    const int32 detailWidth = detail[0] != L'\0' ? TextWidth(dc, fonts.Detail, detail) : 0;
    RECT detailBox = Box(Right - 52, 325, Right - 52, 341);
    detailBox.left -= detailWidth;
    DrawLine(dc, fonts.Detail, MutedColor, detail, detailBox, DT_RIGHT);
    RECT taskBox = Box(Left, 325, Left, 341);
    taskBox.right = detailBox.left - (detailWidth > 0 ? Px(12) : 0);
    DrawLine(dc, fonts.Task, BrightColor, task, taskBox, DT_LEFT);
    FillBox(dc, Box(Left, 349, Right, 352), BorderColor);
    FillBox(dc, Box(Left, 349, Left + (Right - Left) * progress, 352), AccentColor);
    DrawLine(dc, fonts.Footer, FaintColor, stage, Box(Left, 360, Right - 200, 374), DT_LEFT);
    DrawLine(dc, fonts.Footer, FaintColor, _splash.Footer, Box(Right - 300, 360, Right, 374), DT_RIGHT);
}

LRESULT CALLBACK SplashProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
        if (wParam == TickTimer)
        {
            if (_splash.Tick != nullptr)
            {
                _splash.Tick();
            }
            bool dirty = false;
            {
                const ScopeLock lock(_splash.Lock);
                dirty = _splash.Dirty;
            }
            if (dirty)
            {
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        const HDC target = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        // Drawn off screen and copied at once, so a repaint never flickers.
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        const HGDIOBJ previous = SelectObject(dc, bitmap);
        Paint(dc);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        // The whole card drags it.
        return HTCAPTION;
    case WM_CLOSE:
        // Only the engine closes it.
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool CreateOnWindowThread()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!_splash.ClassRegistered)
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = SplashProcedure;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
        windowClass.lpszClassName = SplashClassName;
        if (RegisterClassExW(&windowClass) == 0)
        {
            return false;
        }
        _splash.ClassRegistered = true;
    }
    for (uint32 i = 0; i < _splash.FontFileCount; ++i)
    {
        AddFontResourceExW(_splash.FontFiles[i], FR_PRIVATE, nullptr);
    }

    // Centered on the primary monitor's work area, at its DPI.
    const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    _splash.Scale = static_cast<float32>(dpi) / USER_DEFAULT_SCREEN_DPI;
    const int32 width = Px(CardWidth);
    const int32 height = Px(CardHeight);
    const RECT &area = info.rcWork;
    const int32 x = area.left + (area.right - area.left - width) / 2;
    const int32 y = area.top + (area.bottom - area.top - height) / 2;

    const HWND window = CreateWindowExW(WS_EX_APPWINDOW, SplashClassName, _splash.Title, WS_POPUP, x, y, width, height,
                                        nullptr, nullptr, instance, nullptr);
    if (window == nullptr)
    {
        return false;
    }
    // Rounded corners and a border drawn by the compositor where it can (Windows 11); else the border is painted.
    constexpr DWORD CornerPreference = 33;     // DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD BorderColorAttribute = 34; // DWMWA_BORDER_COLOR
    const int corners = 2;                     // DWMWCP_ROUND
    const COLORREF border = BorderColor;
    _splash.RoundedByDwm = SUCCEEDED(DwmSetWindowAttribute(window, CornerPreference, &corners, sizeof(corners))) &&
                           SUCCEEDED(DwmSetWindowAttribute(window, BorderColorAttribute, &border, sizeof(border)));
    const HDC dc = GetDC(window);
    CreateFonts(dc);
    ReleaseDC(window, dc);
    _splash.Window = window;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetTimer(window, TickTimer, TickMilliseconds, nullptr);
    return true;
}

void DestroyOnWindowThread()
{
    if (_splash.Window == nullptr)
    {
        return;
    }
    KillTimer(_splash.Window, TickTimer);
    DestroyWindow(_splash.Window);
    _splash.Window = nullptr;
    DeleteFonts();
    for (uint32 i = 0; i < _splash.FontFileCount; ++i)
    {
        RemoveFontResourceExW(_splash.FontFiles[i], FR_PRIVATE, nullptr);
    }
}
} // namespace

namespace PlatformSplash
{
bool Show(const SplashContent &content)
{
    if (_splash.Shown.Load())
    {
        return true;
    }
    if (!PlatformApplication::Initialize())
    {
        return false;
    }
    CopyText(_splash.Title, content.Title);
    CopyText(_splash.Brand, content.Brand);
    CopyText(_splash.Version, content.Version);
    CopyText(_splash.Build, content.Build);
    CopyText(_splash.Footer, content.Footer);
    CopyText(_splash.SansFamily, content.SansFamily);
    CopyText(_splash.MonoFamily, content.MonoFamily);
    _splash.FontFileCount = 0;
    for (uint32 i = 0; i < content.FontFileCount && _splash.FontFileCount < MaxFontFiles; ++i)
    {
        CopyText(_splash.FontFiles[_splash.FontFileCount], content.FontFiles[i]);
        _splash.FontFileCount += _splash.FontFiles[_splash.FontFileCount][0] != L'\0' ? 1 : 0;
    }
    _splash.Tick = content.Tick;
    const bool shown = WindowsApplication::CallOnWindowThread([] { return CreateOnWindowThread(); });
    _splash.Shown.Store(shown);
    return shown;
}

void SetStatus(const SplashStatus &status)
{
    const ScopeLock lock(_splash.Lock);
    CopyText(_splash.Project, status.Project);
    CopyText(_splash.Task, status.Task);
    CopyText(_splash.Detail, status.Detail);
    CopyText(_splash.Stage, status.Stage);
    _splash.Progress = status.Progress;
    _splash.Dirty = true;
}

void Hide()
{
    if (!_splash.Shown.Exchange(false))
    {
        return;
    }
    WindowsApplication::RunOnWindowThread([] { DestroyOnWindowThread(); });
}

bool IsShown()
{
    return _splash.Shown.Load();
}
} // namespace PlatformSplash
} // namespace Marlin
