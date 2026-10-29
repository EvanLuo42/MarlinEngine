#pragma once

#include "CoreTypes.h"
#include "EnumFlags.h"
#include "Reflection/TypeOf.h"
#include "Application/PlatformInput.gen.h"

namespace Marlin
{
/// Physical key positions, named after the US layout; text input comes from TextInput events.
MENUM()
enum class KeyCode : uint8
{
    Unknown,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    Escape,
    Enter,
    Tab,
    Backspace,
    Space,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Left,
    Right,
    Up,
    Down,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
    Menu,
    CapsLock,
    NumLock,
    ScrollLock,
    PrintScreen,
    Pause,
    Grave,
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    /// The key left of Z on ISO keyboards.
    IntlBackslash,
    Numpad0,
    Numpad1,
    Numpad2,
    Numpad3,
    Numpad4,
    Numpad5,
    Numpad6,
    Numpad7,
    Numpad8,
    Numpad9,
    NumpadDecimal,
    NumpadDivide,
    NumpadMultiply,
    NumpadSubtract,
    NumpadAdd,
    NumpadEnter,
    Count,
};

MENUM()
enum class MouseButton : uint8
{
    Left,
    Right,
    Middle,
    X1,
    X2,
};

enum class ModifierKeys : uint8
{
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3,
};
MARLIN_ENUM_FLAGS(ModifierKeys)

enum class CursorShape : uint8
{
    Arrow,
    IBeam,
    Hand,
    Crosshair,
    ResizeHorizontal,
    ResizeVertical,
    /// Top-left to bottom-right.
    ResizeDiagonalDown,
    /// Bottom-left to top-right.
    ResizeDiagonalUp,
    Move,
    Wait,
    NotAllowed,
    Hidden,
};

/// Named by position: South is A on Xbox and Cross on PlayStation.
MENUM(Flags)
enum class GamepadButtons : uint16
{
    None = 0,
    DPadUp = 1 << 0,
    DPadDown = 1 << 1,
    DPadLeft = 1 << 2,
    DPadRight = 1 << 3,
    Start = 1 << 4,
    Back = 1 << 5,
    /// Pressing the stick in.
    LeftStickPress = 1 << 6,
    RightStickPress = 1 << 7,
    LeftShoulder = 1 << 8,
    RightShoulder = 1 << 9,
    South = 1 << 10,
    East = 1 << 11,
    West = 1 << 12,
    North = 1 << 13,
};
MARLIN_ENUM_FLAGS(GamepadButtons)

/// Axes are raw, without dead zones.
struct GamepadState
{
    GamepadButtons Buttons = GamepadButtons::None;
    /// -1 to 1; up is positive.
    float32 LeftStickX = 0;
    float32 LeftStickY = 0;
    float32 RightStickX = 0;
    float32 RightStickY = 0;
    /// 0 to 1.
    float32 LeftTrigger = 0;
    float32 RightTrigger = 0;
};

namespace PlatformGamepad
{
inline constexpr uint32 MaxGamepads = 4;

/// Returns false if no gamepad is connected at *index*.
APPLICATION_API bool GetState(uint32 index, GamepadState &state);
/// Motor speeds from 0 to 1.
APPLICATION_API void SetVibration(uint32 index, float32 lowFrequency, float32 highFrequency);
} // namespace PlatformGamepad
} // namespace Marlin
