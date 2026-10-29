#pragma once

#include "Application/PlatformInput.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Input/InputDevice.h"
#include "ApplicationInput/ApplicationInputDevices.gen.h"

namespace Marlin
{
/// Keyboard.<KeyCode> for every physical key of the main window (Documentation/Architecture/15 §2), named by
/// KeyCode. A key pressed and let go within one frame still reads down for that frame.
MCLASS()
class APPLICATIONINPUT_API KeyboardDevice final : public InputDevice
{
    MGENERATED_BODY()
public:
    void RegisterControls(InputControls &controls) override;
    void Update(InputDeviceFrame &frame) override;

private:
    List<InputControlId> _controls;
    List<bool> _held;
    List<bool> _pressed;
};

/// Mouse.Left, Right, Middle, X1 and X2; Mouse.Delta, the raw motion scaled by Engine.Input.MouseSensitivity with
/// right and down positive; Mouse.Wheel and Mouse.WheelX in notches. Holds the pointer while InputSubsystem's
/// pointer lock asks.
MCLASS()
class APPLICATIONINPUT_API MouseDevice final : public InputDevice
{
    MGENERATED_BODY()
public:
    void RegisterControls(InputControls &controls) override;
    void Update(InputDeviceFrame &frame) override;
    void Shutdown() override;

private:
    static constexpr uint32 ButtonCount = 5;

    InputControlId _buttons[ButtonCount] = {};
    bool _held[ButtonCount] = {};
    bool _pressed[ButtonCount] = {};
    InputControlId _delta = InvalidInputControl;
    InputControlId _wheel = InvalidInputControl;
    InputControlId _wheelX = InvalidInputControl;
    bool _locked = false;
};

/// The first connected gamepad: Gamepad.<GamepadButtons>, Gamepad.LeftStick and RightStick with up positive and
/// Engine.Input.StickDeadZone, Gamepad.LeftTrigger and RightTrigger with TriggerDeadZone. At rest while the main
/// window does not have the focus.
MCLASS()
class APPLICATIONINPUT_API GamepadDevice final : public InputDevice
{
    MGENERATED_BODY()
public:
    void RegisterControls(InputControls &controls) override;
    void Update(InputDeviceFrame &frame) override;

private:
    struct Button
    {
        GamepadButtons Flag = GamepadButtons::None;
        InputControlId Control = InvalidInputControl;
    };

    List<Button> _buttons;
    InputControlId _leftStick = InvalidInputControl;
    InputControlId _rightStick = InvalidInputControl;
    InputControlId _leftTrigger = InvalidInputControl;
    InputControlId _rightTrigger = InvalidInputControl;
    bool _focused = true;
};
} // namespace Marlin
