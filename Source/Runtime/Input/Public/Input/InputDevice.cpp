#include "Input/InputDevice.h"

#include "Input/InputSubsystem.h"

namespace Marlin
{
InputControlId InputControls::Register(StringView name, InputControlType type, bool amount)
{
    const Name controlName(name);
    if (const InputControlId *existing = _byName.Find(controlName))
    {
        MARLIN_LOG(LogInput, Warning, "Input control {0} is registered twice; the first one stays", name);
        return *existing;
    }
    const auto id = static_cast<InputControlId>(_controls.Count());
    _controls.Add({controlName, type, amount, Vector2::Zero});
    _byName.FindOrAdd(controlName) = id;
    return id;
}

InputControlId InputControls::Find(StringView name) const
{
    const Name controlName = Name::Find(name);
    if (controlName == Name())
    {
        return InvalidInputControl;
    }
    const InputControlId *id = _byName.Find(controlName);
    return id != nullptr ? *id : InvalidInputControl;
}

bool InputControls::IsActuated(InputControlId control) const
{
    const Control &entry = _controls[control];
    const float32 magnitude = entry.Value.Length();
    return entry.Amount ? magnitude > 0 : magnitude >= 0.5f;
}

void InputControls::BeginFrame()
{
    for (Control &control : _controls)
    {
        if (control.Amount)
        {
            control.Value = Vector2::Zero;
        }
    }
}
} // namespace Marlin
