#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "String/Name.h"
#include "Input/InputDevice.gen.h"

namespace Marlin
{
/// The shape of a control's value; every value is held as a Vector2.
MENUM()
enum class InputControlType : uint8
{
    Button,
    Axis1D,
    Axis2D,
};

using InputControlId = uint32;
inline constexpr InputControlId InvalidInputControl = ~0u;

/// The controls of every device (Documentation/Architecture/15 §2): names such as Keyboard.W, registered once at
/// startup, and their values this frame. A state control holds how far it is pressed; an amount control, such as
/// mouse motion, how much it moved this frame, and starts every frame at zero.
class INPUT_API InputControls
{
public:
    /// Adds control *name*; a name that is taken keeps its first control, which is returned with a warning.
    InputControlId Register(StringView name, InputControlType type, bool amount = false);
    /// The control of *name*, or InvalidInputControl.
    [[nodiscard]] InputControlId Find(StringView name) const;

    [[nodiscard]] usize Count() const
    {
        return _controls.Count();
    }
    [[nodiscard]] Name GetName(InputControlId control) const
    {
        return _controls[control].ControlName;
    }
    [[nodiscard]] InputControlType GetType(InputControlId control) const
    {
        return _controls[control].Type;
    }
    [[nodiscard]] bool IsAmount(InputControlId control) const
    {
        return _controls[control].Amount;
    }

    [[nodiscard]] Vector2 Get(InputControlId control) const
    {
        return _controls[control].Value;
    }
    /// Pressed at least halfway, or moved at all for an amount.
    [[nodiscard]] bool IsActuated(InputControlId control) const;

    /// The value of a state control.
    void Set(InputControlId control, Vector2 value)
    {
        _controls[control].Value = value;
    }
    void Set(InputControlId control, float32 value)
    {
        _controls[control].Value = Vector2(value, 0);
    }
    /// Adds to the value of an amount control.
    void Add(InputControlId control, Vector2 amount)
    {
        _controls[control].Value += amount;
    }

    /// Zeroes the amounts, before the devices update.
    void BeginFrame();

private:
    struct Control
    {
        Name ControlName;
        InputControlType Type = InputControlType::Button;
        bool Amount = false;
        Vector2 Value = Vector2::Zero;
    };

    List<Control> _controls;
    HashMap<Name, InputControlId> _byName;
};

/// What a device updates its controls from.
struct InputDeviceFrame
{
    InputControls &Controls;
    float64 DeltaTime = 0;
    /// Whether something holds the pointer (InputSubsystem::SetPointerLock): the mouse device hides and confines
    /// the cursor while it does.
    bool PointerLocked = false;
};

/// A source of controls (Documentation/Architecture/15 §2): keyboard, mouse and gamepad come with the engine in
/// ApplicationInput; wheels, flight sticks or console-specific pads are more implementations, found through
/// reflection like every Provider (13 §3).
MCLASS(Abstract, ExtensionPoint)
class INPUT_API InputDevice
{
    MGENERATED_BODY()
public:
    InputDevice() = default;
    virtual ~InputDevice() = default;
    MARLIN_NONCOPYABLE(InputDevice);

    /// Registers the device's controls and keeps their ids; once, at startup.
    virtual void RegisterControls(InputControls &controls) = 0;
    /// Writes this frame's values, on the main thread after the OS events are pumped.
    virtual void Update(InputDeviceFrame &frame) = 0;
    /// Lets go of what the device holds, such as the pointer, before the engine shuts down.
    virtual void Shutdown()
    {
    }
};
} // namespace Marlin
