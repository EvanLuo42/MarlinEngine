#include "CrashDialog.h"

#include "Containers/List.h"
#include "String/StringBuilder.h"

#include "HAL/MicrosoftHeaders.h"

#include <shellapi.h>

namespace Marlin
{
namespace
{
constexpr int32 SendId = 1;
constexpr int32 DontSendId = 2;
constexpr int32 OpenFolderId = 3;
constexpr int32 DescriptionId = 4;
constexpr const wchar_t *ClassName = L"MarlinCrashDialog";
constexpr const wchar_t *EditClass = L"EDIT";
constexpr const wchar_t *StaticClass = L"STATIC";
constexpr const wchar_t *ButtonClass = L"BUTTON";

struct DialogState
{
    HWND Description = nullptr;
    HFONT Font = nullptr;
    StringView Folder;
    bool Send = false;
    bool Done = false;
    String Text;
};

String ReadDescription(HWND edit)
{
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0)
    {
        return {};
    }
    List<wchar_t> text;
    text.Resize(static_cast<usize>(length) + 1);
    GetWindowTextW(edit, text.Data(), length + 1);
    List<char> utf8;
    utf8.Resize(static_cast<usize>(length) * 4 + 1);
    const usize size = Microsoft::WideToUtf8(text.Data(), utf8.Data(), utf8.Count());
    return size < utf8.Count() ? String(utf8.Data(), size) : String();
}

LRESULT CALLBACK DialogProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *state = reinterpret_cast<DialogState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_CREATE: {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }
    case WM_COMMAND:
        if (state == nullptr)
        {
            break;
        }
        switch (LOWORD(wParam))
        {
        case SendId:
            state->Text = ReadDescription(state->Description);
            state->Send = true;
            DestroyWindow(window);
            return 0;
        case DontSendId:
            DestroyWindow(window);
            return 0;
        case OpenFolderId: {
            const Microsoft::WidePath folder(String(state->Folder).CStr());
            ShellExecuteW(window, L"open", folder.Get(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr)
        {
            state->Done = true;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND AddControl(HWND parent, const wchar_t *type, const wchar_t *text, DWORD style, int32 x, int32 y, int32 width,
                int32 height, int32 id, HFONT font)
{
    const HWND control =
        CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
                        reinterpret_cast<HMENU>(static_cast<intptr>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

int32 Scale(int32 value, uint32 dpi)
{
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

List<wchar_t> Widen(StringView text)
{
    const String terminated(text);
    List<wchar_t> wide;
    wide.Resize(text.Length() + 1);
    const usize length = Microsoft::Utf8ToWide(terminated.CStr(), wide.Data(), wide.Count());
    wide.Resize(length < wide.Count() ? length + 1 : 1);
    wide[wide.Count() - 1] = L'\0';
    return wide;
}
} // namespace

bool AskToSendReport(const CrashDialogDesc &desc, String &description)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DialogProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<intptr>(COLOR_BTNFACE + 1));
    windowClass.lpszClassName = ClassName;
    RegisterClassExW(&windowClass);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const uint32 dpi = GetDpiForSystem();
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    DialogState state;
    state.Folder = desc.ReportDirectory;
    state.Font = CreateFontIndirectW(&metrics.lfMessageFont);

    const int32 width = Scale(480, dpi);
    const int32 height = Scale(300, dpi);
    const List<wchar_t> title = Widen(desc.ProductName);
    RECT frame = {0, 0, width, height};
    AdjustWindowRectExForDpi(&frame, WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    const HWND window =
        CreateWindowExW(WS_EX_TOPMOST, ClassName, title.Data(), WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                        frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, &state);
    if (window == nullptr)
    {
        DeleteObject(state.Font);
        return false;
    }

    const int32 margin = Scale(12, dpi);
    const int32 line = Scale(20, dpi);
    const int32 button = Scale(28, dpi);
    const int32 inner = width - 2 * margin;
    const List<wchar_t> headline = Widen(desc.Headline);
    AddControl(window, StaticClass, headline.Data(), 0, margin, margin, inner, line, 0, state.Font);
    // RECOVERY-DECISION D9: dialog/headline text chosen (not recovered).
    AddControl(window, StaticClass, L"What were you doing when it happened? (optional)", 0, margin,
               margin + line + margin / 2, inner, line, 0, state.Font);
    const int32 editTop = margin + 2 * line + margin;
    const int32 editHeight = height - editTop - button - 2 * margin;
    state.Description = AddControl(window, EditClass, L"",
                                   WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                   margin, editTop, inner, editHeight, DescriptionId, state.Font);
    const int32 buttonTop = height - margin - button;
    const int32 buttonWidth = Scale(120, dpi);
    AddControl(window, ButtonClass, L"Open folder", WS_TABSTOP, margin, buttonTop, buttonWidth, button, OpenFolderId,
               state.Font);
    AddControl(window, ButtonClass, L"Don't send", WS_TABSTOP, width - margin - 2 * buttonWidth - margin / 2, buttonTop,
               buttonWidth, button, DontSendId, state.Font);
    AddControl(window, ButtonClass, L"Send", WS_TABSTOP | BS_DEFPUSHBUTTON, width - margin - buttonWidth, buttonTop,
               buttonWidth, button, SendId, state.Font);

    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    SetFocus(state.Description);
    MSG message;
    while (!state.Done && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DeleteObject(state.Font);
    UnregisterClassW(ClassName, instance);
    description = Move(state.Text);
    return state.Send;
}
} // namespace Marlin
