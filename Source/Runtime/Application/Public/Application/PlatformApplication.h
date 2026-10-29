#pragma once

#include "Application/PlatformInput.h"
#include "Application/PlatformWindow.h"
#include "CoreTypes.h"

namespace Marlin
{
enum class ApplicationEventType : uint8
{
    /// The user asked to close the window; nothing closes it until Destroy.
    WindowCloseRequested,
    /// Width and Height hold the new client size; zero while minimized.
    WindowResized,
    /// X and Y hold the new client position.
    WindowMoved,
    WindowFocusGained,
    WindowFocusLost,
    /// DpiScale holds the new scale; a WindowResized follows.
    WindowDpiChanged,
    KeyDown,
    KeyUp,
    /// Character holds one Unicode code point.
    TextInput,
    /// GetComposition returns the new composition text, empty when composition ends.
    TextComposition,
    /// X and Y hold the client position.
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    /// WheelX and WheelY in notches; positive is right and away from the user.
    MouseWheel,
    /// X and Y hold unaccelerated motion, reported even when the cursor is clipped or hidden.
    MouseRawMotion,
    MouseLeave,
    /// GetDroppedFiles returns the paths until the next PumpMessages.
    FilesDropped,
    /// Monitors were added, removed or rearranged.
    MonitorsChanged,
};

struct ApplicationEvent
{
    ApplicationEventType Type = ApplicationEventType::MouseMove;
    WindowHandle Window;
    int32 X = 0;
    int32 Y = 0;
    int32 Width = 0;
    int32 Height = 0;
    float32 WheelX = 0;
    float32 WheelY = 0;
    float32 DpiScale = 1;
    uint32 Character = 0;
    KeyCode Key = KeyCode::Unknown;
    MouseButton Button = MouseButton::Left;
    ModifierKeys Modifiers = ModifierKeys::None;
    bool Repeat = false;
};

struct MonitorInfo
{
    PlatformRect Bounds;
    /// Bounds without the taskbar and docked bars.
    PlatformRect WorkArea;
    float32 DpiScale = 1;
    bool IsPrimary = false;
    /// UTF-8.
    char Name[64] = {};
};

/// Called from the main thread. Where the OS ties windows to the thread that pumps them, the platform owns a window
/// thread that does nothing else, so that the OS's modal loops (moving or resizing a window) never stop the frame.
namespace PlatformApplication
{
APPLICATION_API bool Initialize();
APPLICATION_API void Shutdown();

/// Takes the events the OS sent since the last call, for PollEvent, with the composition and dropped files as of
/// the last of them.
APPLICATION_API void PumpMessages();
APPLICATION_API bool PollEvent(ApplicationEvent &event);

APPLICATION_API uint32 GetMonitors(MonitorInfo *monitors, uint32 maxCount);

APPLICATION_API void SetCursorShape(CursorShape shape);
/// Screen coordinates.
APPLICATION_API void GetCursorPosition(int32 &x, int32 &y);
APPLICATION_API void SetCursorPosition(int32 x, int32 y);
/// Whether *button* is held now, wherever the cursor is; for drags that outlive the element they started on.
APPLICATION_API bool IsMouseButtonDown(MouseButton button);
/// Keeps mouse events coming to *window* while a button is held outside it.
APPLICATION_API void SetMouseCapture(WindowHandle window, bool capture);
/// Hides the cursor and confines it to *window*, for camera control through MouseRawMotion.
APPLICATION_API void SetRelativeMouseMode(WindowHandle window, bool enabled);

/// Same buffer convention as PlatformProcess.
APPLICATION_API usize GetClipboardText(char *buffer, usize capacity);
APPLICATION_API bool SetClipboardText(const char *text);

/// Turns the IME on while a text field has focus.
APPLICATION_API void SetTextInputEnabled(WindowHandle window, bool enabled);
/// Places the IME candidate window next to the caret, in client coordinates.
APPLICATION_API void SetTextInputArea(WindowHandle window, const PlatformRect &caret);
/// Same buffer convention as PlatformProcess.
APPLICATION_API usize GetComposition(char *buffer, usize capacity);

APPLICATION_API uint32 GetDroppedFileCount();
/// Same buffer convention as PlatformProcess; paths use '/'.
APPLICATION_API usize GetDroppedFile(uint32 index, char *buffer, usize capacity);

/// Blocks until the user dismisses it.
APPLICATION_API void ShowMessageBox(const char *title, const char *message);
} // namespace PlatformApplication
} // namespace Marlin
