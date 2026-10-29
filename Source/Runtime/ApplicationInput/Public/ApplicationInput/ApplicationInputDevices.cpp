#include "ApplicationInput/ApplicationInputDevices.h"

#include "Application/ApplicationSubsystem.h"
#include "Engine/Engine.h"
#include "Input/InputSubsystem.h"
#include "Math/MathUtility.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
/// The events of the main window this frame; empty without one.
Span<const ApplicationEvent> GetMainWindowEvents(WindowHandle &window)
{
    const ApplicationSubsystem *application = Engine::GetSubsystem<ApplicationSubsystem>();
    if (application == nullptr)
    {
        return {};
    }
    window = application->GetMainWindow();
    return application->GetEvents();
}

/// Rest below *deadZone*, then rescaled so the range starts from zero.
float32 ApplyDeadZone(float32 value, float32 deadZone)
{
    const float32 magnitude = Math::Abs(value);
    if (magnitude <= deadZone || deadZone >= 1)
    {
        return 0;
    }
    return Math::Sign(value) * Min((magnitude - deadZone) / (1 - deadZone), 1.0f);
}

Vector2 ApplyRadialDeadZone(Vector2 value, float32 deadZone)
{
    const float32 magnitude = value.Length();
    if (magnitude <= deadZone || deadZone >= 1)
    {
        return Vector2::Zero;
    }
    return value * (Min((magnitude - deadZone) / (1 - deadZone), 1.0f) / magnitude);
}

constexpr StringView MouseButtonNames[] = {"Left", "Right", "Middle", "X1", "X2"};
} // namespace

void KeyboardDevice::RegisterControls(InputControls &controls)
{
    const auto count = static_cast<usize>(KeyCode::Count);
    _controls.Resize(count, InvalidInputControl);
    _held.Resize(count, false);
    _pressed.Resize(count, false);
    for (const EnumeratorInfo &key : TypeOf<KeyCode>().GetEnumerators())
    {
        const auto index = static_cast<usize>(key.Value);
        if (key.Value != static_cast<int64>(KeyCode::Unknown) && index < count)
        {
            _controls[index] = controls.Register(Format("Keyboard.{0}", key.Name).AsView(), InputControlType::Button);
        }
    }
}

void KeyboardDevice::Update(InputDeviceFrame &frame)
{
    WindowHandle window;
    for (const ApplicationEvent &event : GetMainWindowEvents(window))
    {
        if (event.Window != window)
        {
            continue;
        }
        const auto key = static_cast<usize>(event.Key);
        switch (event.Type)
        {
        case ApplicationEventType::KeyDown:
            if (key < _held.Count())
            {
                _held[key] = true;
                _pressed[key] = true;
            }
            break;
        case ApplicationEventType::KeyUp:
            if (key < _held.Count())
            {
                _held[key] = false;
            }
            break;
        case ApplicationEventType::WindowFocusLost:
            // The key ups go to whichever window has the focus now.
            for (usize i = 0; i < _held.Count(); ++i)
            {
                _held[i] = false;
                _pressed[i] = false;
            }
            break;
        default:
            break;
        }
    }
    for (usize i = 0; i < _controls.Count(); ++i)
    {
        if (_controls[i] != InvalidInputControl)
        {
            frame.Controls.Set(_controls[i], _held[i] || _pressed[i] ? 1.0f : 0.0f);
        }
        _pressed[i] = false;
    }
}

void MouseDevice::RegisterControls(InputControls &controls)
{
    for (uint32 i = 0; i < ButtonCount; ++i)
    {
        _buttons[i] = controls.Register(Format("Mouse.{0}", MouseButtonNames[i]).AsView(), InputControlType::Button);
    }
    _delta = controls.Register("Mouse.Delta", InputControlType::Axis2D, true);
    _wheel = controls.Register("Mouse.Wheel", InputControlType::Axis1D, true);
    _wheelX = controls.Register("Mouse.WheelX", InputControlType::Axis1D, true);
}

void MouseDevice::Update(InputDeviceFrame &frame)
{
    WindowHandle window;
    const Span<const ApplicationEvent> events = GetMainWindowEvents(window);
    if (frame.PointerLocked != _locked && window.IsValid())
    {
        PlatformApplication::SetRelativeMouseMode(window, frame.PointerLocked);
        _locked = frame.PointerLocked;
    }
    const float32 scale = InputSubsystem::MouseSensitivity;
    const float32 scaleY = InputSubsystem::InvertMouseY ? -scale : scale;
    for (const ApplicationEvent &event : events)
    {
        if (event.Window != window)
        {
            continue;
        }
        const auto button = static_cast<uint32>(event.Button);
        switch (event.Type)
        {
        case ApplicationEventType::MouseButtonDown:
            if (button < ButtonCount)
            {
                _held[button] = true;
                _pressed[button] = true;
            }
            break;
        case ApplicationEventType::MouseButtonUp:
            if (button < ButtonCount)
            {
                _held[button] = false;
            }
            break;
        case ApplicationEventType::MouseRawMotion:
            frame.Controls.Add(_delta, Vector2(static_cast<float32>(event.X) * scale,
                                               static_cast<float32>(event.Y) * scaleY));
            break;
        case ApplicationEventType::MouseWheel:
            frame.Controls.Add(_wheel, Vector2(event.WheelY, 0));
            frame.Controls.Add(_wheelX, Vector2(event.WheelX, 0));
            break;
        case ApplicationEventType::WindowFocusLost:
            for (uint32 i = 0; i < ButtonCount; ++i)
            {
                _held[i] = false;
                _pressed[i] = false;
            }
            break;
        default:
            break;
        }
    }
    for (uint32 i = 0; i < ButtonCount; ++i)
    {
        frame.Controls.Set(_buttons[i], _held[i] || _pressed[i] ? 1.0f : 0.0f);
        _pressed[i] = false;
    }
}

void MouseDevice::Shutdown()
{
    if (_locked)
    {
        WindowHandle window;
        (void)GetMainWindowEvents(window);
        if (window.IsValid())
        {
            PlatformApplication::SetRelativeMouseMode(window, false);
        }
        _locked = false;
    }
}

void GamepadDevice::RegisterControls(InputControls &controls)
{
    for (const EnumeratorInfo &button : TypeOf<GamepadButtons>().GetEnumerators())
    {
        if (button.Value != 0)
        {
            _buttons.Add({static_cast<GamepadButtons>(button.Value),
                          controls.Register(Format("Gamepad.{0}", button.Name).AsView(), InputControlType::Button)});
        }
    }
    _leftStick = controls.Register("Gamepad.LeftStick", InputControlType::Axis2D);
    _rightStick = controls.Register("Gamepad.RightStick", InputControlType::Axis2D);
    _leftTrigger = controls.Register("Gamepad.LeftTrigger", InputControlType::Axis1D);
    _rightTrigger = controls.Register("Gamepad.RightTrigger", InputControlType::Axis1D);
}

void GamepadDevice::Update(InputDeviceFrame &frame)
{
    WindowHandle window;
    for (const ApplicationEvent &event : GetMainWindowEvents(window))
    {
        if (event.Window != window)
        {
            continue;
        }
        if (event.Type == ApplicationEventType::WindowFocusGained)
        {
            _focused = true;
        }
        else if (event.Type == ApplicationEventType::WindowFocusLost)
        {
            _focused = false;
        }
    }
    GamepadState state;
    bool connected = false;
    if (_focused && window.IsValid())
    {
        for (uint32 index = 0; index < PlatformGamepad::MaxGamepads && !connected; ++index)
        {
            connected = PlatformGamepad::GetState(index, state);
        }
    }
    if (!connected)
    {
        state = GamepadState();
    }
    for (const Button &button : _buttons)
    {
        frame.Controls.Set(button.Control, HasAnyFlags(state.Buttons, button.Flag) ? 1.0f : 0.0f);
    }
    const float32 stickDeadZone = InputSubsystem::StickDeadZone;
    const float32 triggerDeadZone = InputSubsystem::TriggerDeadZone;
    frame.Controls.Set(_leftStick, ApplyRadialDeadZone({state.LeftStickX, state.LeftStickY}, stickDeadZone));
    frame.Controls.Set(_rightStick, ApplyRadialDeadZone({state.RightStickX, state.RightStickY}, stickDeadZone));
    frame.Controls.Set(_leftTrigger, ApplyDeadZone(state.LeftTrigger, triggerDeadZone));
    frame.Controls.Set(_rightTrigger, ApplyDeadZone(state.RightTrigger, triggerDeadZone));
}
} // namespace Marlin
