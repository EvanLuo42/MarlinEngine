#include "Application/PlatformInput.h"

#include "HAL/MicrosoftHeaders.h"
#include "HAL/PlatformTime.h"

#include <xinput.h>

namespace Marlin::PlatformGamepad
{
static_assert(MaxGamepads == XUSER_MAX_COUNT);

namespace
{
uint64 _nextProbe[MaxGamepads] = {};

float32 StickAxis(SHORT value)
{
    const float32 axis = static_cast<float32>(value) / 32767.0f;
    return axis < -1.0f ? -1.0f : axis;
}

GamepadButtons TranslateButtons(WORD buttons)
{
    struct Mapping
    {
        WORD XInput;
        GamepadButtons Button;
    };
    static constexpr Mapping Mappings[] = {
        {XINPUT_GAMEPAD_DPAD_UP, GamepadButtons::DPadUp},
        {XINPUT_GAMEPAD_DPAD_DOWN, GamepadButtons::DPadDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, GamepadButtons::DPadLeft},
        {XINPUT_GAMEPAD_DPAD_RIGHT, GamepadButtons::DPadRight},
        {XINPUT_GAMEPAD_START, GamepadButtons::Start},
        {XINPUT_GAMEPAD_BACK, GamepadButtons::Back},
        {XINPUT_GAMEPAD_LEFT_THUMB, GamepadButtons::LeftStickPress},
        {XINPUT_GAMEPAD_RIGHT_THUMB, GamepadButtons::RightStickPress},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, GamepadButtons::LeftShoulder},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, GamepadButtons::RightShoulder},
        {XINPUT_GAMEPAD_A, GamepadButtons::South},
        {XINPUT_GAMEPAD_B, GamepadButtons::East},
        {XINPUT_GAMEPAD_X, GamepadButtons::West},
        {XINPUT_GAMEPAD_Y, GamepadButtons::North},
    };
    GamepadButtons result = GamepadButtons::None;
    for (const Mapping &mapping : Mappings)
    {
        if ((buttons & mapping.XInput) != 0)
        {
            result |= mapping.Button;
        }
    }
    return result;
}
} // namespace

bool GetState(uint32 index, GamepadState &state)
{
    state = {};
    if (index >= MaxGamepads)
    {
        return false;
    }
    const uint64 now = PlatformTime::Ticks();
    if (now < _nextProbe[index])
    {
        return false;
    }
    XINPUT_STATE input = {};
    if (XInputGetState(index, &input) != ERROR_SUCCESS)
    {
        _nextProbe[index] = now + PlatformTime::TicksPerSecond();
        return false;
    }
    _nextProbe[index] = 0;
    const XINPUT_GAMEPAD &pad = input.Gamepad;
    state.Buttons = TranslateButtons(pad.wButtons);
    state.LeftStickX = StickAxis(pad.sThumbLX);
    state.LeftStickY = StickAxis(pad.sThumbLY);
    state.RightStickX = StickAxis(pad.sThumbRX);
    state.RightStickY = StickAxis(pad.sThumbRY);
    state.LeftTrigger = static_cast<float32>(pad.bLeftTrigger) / 255.0f;
    state.RightTrigger = static_cast<float32>(pad.bRightTrigger) / 255.0f;
    return true;
}

void SetVibration(uint32 index, float32 lowFrequency, float32 highFrequency)
{
    if (index >= MaxGamepads)
    {
        return;
    }
    const auto toSpeed = [](float32 value) {
        const float32 positive = value < 0.0f ? 0.0f : value;
        const float32 clamped = positive > 1.0f ? 1.0f : positive;
        return static_cast<WORD>(clamped * 65535.0f);
    };
    XINPUT_VIBRATION vibration = {.wLeftMotorSpeed = toSpeed(lowFrequency), .wRightMotorSpeed = toSpeed(highFrequency)};
    XInputSetState(index, &vibration);
}
} // namespace Marlin::PlatformGamepad
