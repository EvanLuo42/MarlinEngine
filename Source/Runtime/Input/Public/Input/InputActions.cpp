#include "Input/InputActions.h"

#include "Engine/Engine.h"

namespace Marlin
{
namespace
{
const InputActionValue RestValue;

/// Samples into the world's InputActions, from nothing when no InputSubsystem runs.
void SampleWorld(SystemContext &context, HashMap<Guid, InputSeen> &seen)
{
    const InputSubsystem *input = Engine::GetSubsystem<InputSubsystem>();
    InputActions *actions = context.GetResource<InputActions>();
    if (actions != nullptr)
    {
        actions->Sample(input != nullptr ? input->GetRecords() : Span<const InputActionRecord>(), seen);
    }
}
} // namespace

const InputActionValue &InputActions::Get(const Guid &action) const
{
    const InputActionValue *value = _values.Find(action);
    return value != nullptr ? *value : RestValue;
}

void InputActions::Sample(Span<const InputActionRecord> records, HashMap<Guid, InputSeen> &seen)
{
    for (const InputActionRecord &record : records)
    {
        InputSeen *last = seen.Find(record.Action);
        if (last == nullptr)
        {
            // First seen now: start from the frame before, so only this frame's events show, not its history.
            last = &seen.FindOrAdd(record.Action);
            last->Presses = record.Presses - record.FramePresses;
            last->Releases = record.Releases - record.FrameReleases;
            if (record.Accumulate)
            {
                last->Total[0] = record.Total[0] - record.Value.X;
                last->Total[1] = record.Total[1] - record.Value.Y;
                last->Total[2] = record.Total[2] - record.Value.Z;
            }
        }
        InputActionValue &value = _values.FindOrAdd(record.Action);
        value.Down = record.Down;
        value.Pressed = record.Presses != last->Presses;
        value.Released = record.Releases != last->Releases;
        if (record.Accumulate)
        {
            value.Value = Vector3(static_cast<float32>(record.Total[0] - last->Total[0]),
                                  static_cast<float32>(record.Total[1] - last->Total[1]),
                                  static_cast<float32>(record.Total[2] - last->Total[2]));
            last->Total[0] = record.Total[0];
            last->Total[1] = record.Total[1];
            last->Total[2] = record.Total[2];
        }
        else
        {
            value.Value = record.Value;
        }
        last->Presses = record.Presses;
        last->Releases = record.Releases;
    }
}

void InputFixedStepSystem::Configure(SystemBuilder &builder)
{
    builder.WriteResource<InputActions>();
}

void InputFixedStepSystem::Execute(SystemContext &context)
{
    SampleWorld(context, _seen);
}

void InputFrameSystem::Configure(SystemBuilder &builder)
{
    builder.WriteResource<InputActions>();
}

void InputFrameSystem::Execute(SystemContext &context)
{
    SampleWorld(context, _seen);
}
} // namespace Marlin
