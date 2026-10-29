#pragma once

#include "CoreTypes.h"
#include "Input/InputDevice.h"
#include "InputTestTypes.gen.h"

namespace Marlin::Tests
{
/// Controls the tests set themselves: Test.A and Test.B (buttons), Test.Stick (Axis2D) and Test.Delta (an Axis2D
/// amount). Update leaves them alone.
MCLASS()
class TestInputDevice final : public InputDevice
{
    MGENERATED_BODY()
public:
    void RegisterControls(InputControls &controls) override
    {
        (void)controls.Register("Test.A", InputControlType::Button);
        (void)controls.Register("Test.B", InputControlType::Button);
        (void)controls.Register("Test.Stick", InputControlType::Axis2D);
        (void)controls.Register("Test.Delta", InputControlType::Axis2D, true);
    }

    void Update(InputDeviceFrame & /*frame*/) override
    {
    }
};
} // namespace Marlin::Tests
