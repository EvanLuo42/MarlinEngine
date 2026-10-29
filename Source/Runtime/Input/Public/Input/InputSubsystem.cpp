#include "Input/InputSubsystem.h"

#include "Concurrency/ScopeLock.h"
#include "Config/Config.h"
#include "Hash/Hash.h"
#include "Math/MathUtility.h"

MARLIN_DEFINE_LOG_CATEGORY(LogInput);

namespace Marlin
{
float32 InputSubsystem::MouseSensitivity = 1;
bool InputSubsystem::InvertMouseY = false;
float32 InputSubsystem::StickDeadZone = 0.2f;
float32 InputSubsystem::TriggerDeadZone = 0.05f;
List<InputRebind> InputSubsystem::Rebinds;

namespace
{
void AddToAxis(Vector3 &value, InputAxis axis, float32 amount)
{
    switch (axis)
    {
    case InputAxis::X:
        value.X += amount;
        break;
    case InputAxis::Y:
        value.Y += amount;
        break;
    case InputAxis::Z:
        value.Z += amount;
        break;
    case InputAxis::None:
        break;
    }
}

float32 Magnitude(const Vector3 &value, InputValueType type)
{
    switch (type)
    {
    case InputValueType::Button:
    case InputValueType::Axis1D:
        return Math::Abs(value.X);
    case InputValueType::Axis2D:
        return Vector2(value.X, value.Y).Length();
    case InputValueType::Axis3D:
        return value.Length();
    }
    return 0;
}

uint64 HashRebinds()
{
    uint64 hash = HashInteger(InputSubsystem::Rebinds.Count());
    for (const InputRebind &rebind : InputSubsystem::Rebinds)
    {
        hash = HashCombine(hash, rebind.Context.GetGuid().GetHash());
        hash = HashCombine(hash, GetHash(rebind.Binding.AsView()));
        hash = HashCombine(hash, GetHash(rebind.Control.AsView()));
    }
    return hash;
}
} // namespace

InputSubsystem::InputSubsystem() = default;
InputSubsystem::~InputSubsystem() = default;

void InputSubsystem::Initialize(SubsystemContext & /*context*/)
{
    List<String> disabled;
    (void)Config::TryGet("Engine.Extensions.Disabled", disabled);
    ExtensionContext extensions;
    extensions.Disabled = disabled.AsSpan();
    _devices.Build(extensions);
    for (InputDevice *device : _devices)
    {
        device->RegisterControls(_controls);
    }
}

void InputSubsystem::Deinitialize()
{
    for (InputDevice *device : _devices)
    {
        device->Shutdown();
    }
    _contexts.Clear();
    const ScopeLock lock(_pendingLock);
    _closed = true;
    _pending.Clear();
    _pointerOwners.Clear();
}

void InputSubsystem::BeginFrame(const FrameTime &time)
{
    ApplyRequests();
    bool locked = false;
    {
        const ScopeLock lock(_pendingLock);
        locked = !_pointerOwners.IsEmpty();
    }
    _controls.BeginFrame();
    InputDeviceFrame frame{_controls, time.Delta, locked};
    for (InputDevice *device : _devices)
    {
        device->Update(frame);
    }
    Evaluate(time.Delta);
}

void InputSubsystem::PushContext(const AssetRef<InputContext> &context)
{
    if (context.IsValid())
    {
        const ScopeLock lock(_pendingLock);
        if (!_closed)
        {
            _pending.Add({context, true});
        }
    }
}

void InputSubsystem::PopContext(const AssetRef<InputContext> &context)
{
    if (context.IsValid())
    {
        const ScopeLock lock(_pendingLock);
        if (!_closed)
        {
            _pending.Add({context, false});
        }
    }
}

void InputSubsystem::SetPointerLock(const void *owner, bool locked)
{
    const ScopeLock lock(_pendingLock);
    if (locked)
    {
        if (!_pointerOwners.Contains(owner))
        {
            _pointerOwners.Add(owner);
        }
    }
    else
    {
        (void)_pointerOwners.Remove(owner);
    }
}

void InputSubsystem::SetRebind(const SoftAssetRef<InputContext> &context, StringView binding, StringView control)
{
    for (InputRebind &rebind : Rebinds)
    {
        if (rebind.Context.GetGuid() == context.GetGuid() && rebind.Binding.AsView() == binding)
        {
            rebind.Control = String(control);
            return;
        }
    }
    Rebinds.Add({context, String(binding), String(control)});
}

void InputSubsystem::ClearRebind(const SoftAssetRef<InputContext> &context, StringView binding)
{
    const usize index = Rebinds.FindIndex([&](const InputRebind &rebind) {
        return rebind.Context.GetGuid() == context.GetGuid() && rebind.Binding.AsView() == binding;
    });
    if (index != List<InputRebind>::NotFound)
    {
        Rebinds.RemoveAt(index);
    }
}

const InputActionRecord *InputSubsystem::FindRecord(const Guid &action) const
{
    const uint32 *index = _recordIndex.Find(action);
    return index != nullptr ? &_records[*index] : nullptr;
}

void InputSubsystem::ApplyRequests()
{
    List<PendingContext> pending;
    {
        const ScopeLock lock(_pendingLock);
        pending = Move(_pending);
        _pending.Clear();
    }
    for (PendingContext &request : pending)
    {
        const Guid guid = request.Context.GetGuid();
        ActiveContext *active =
            _contexts.Find([&](const ActiveContext &context) { return context.Context.GetGuid() == guid; });
        if (request.Push)
        {
            if (active == nullptr)
            {
                active = &_contexts.AddDefaulted();
                active->Context = Move(request.Context);
                active->Order = _nextOrder++;
            }
            ++active->Count;
        }
        else if (active == nullptr)
        {
            MARLIN_LOG(LogInput, Warning, "Input context {0} is popped but was not pushed", guid);
        }
        else if (--active->Count == 0)
        {
            _contexts.RemoveAt(static_cast<usize>(active - _contexts.begin()));
        }
    }
}

InputControlId InputSubsystem::FindControl(StringView name, const InputContext &context) const
{
    const InputControlId control = _controls.Find(name);
    if (control == InvalidInputControl)
    {
        MARLIN_LOG(LogInput, Warning, "Input context {0} names control {1}, which no device has; the binding is "
                   "ignored", context.Name.AsView(), name);
    }
    return control;
}

void InputSubsystem::Resolve(ActiveContext &active, uint64 rebindHash)
{
    const InputContext *context = active.Context.Get();
    active.Resolved = context;
    active.RebindHash = rebindHash;
    active.Bindings.Clear();
    if (context == nullptr)
    {
        return;
    }
    for (const InputBinding &binding : context->Bindings)
    {
        const InputAction *action = binding.Action.Get();
        if (action == nullptr)
        {
            MARLIN_LOG(LogInput, Warning, "Input context {0} binds {1} to an action that is not loaded",
                       context->Name.AsView(), binding.Control.AsView());
            continue;
        }
        StringView controlName = binding.Control.AsView();
        if (!binding.Name.IsEmpty())
        {
            for (const InputRebind &rebind : Rebinds)
            {
                if (rebind.Context.GetGuid() == context->GetGuid() && rebind.Binding.AsView() == binding.Name.AsView())
                {
                    controlName = rebind.Control.AsView();
                }
            }
        }
        ResolvedBinding resolved;
        resolved.Binding = &binding;
        resolved.Control = FindControl(controlName, *context);
        bool chordKnown = true;
        for (const FrozenString &chord : binding.Chord)
        {
            const InputControlId control = FindControl(chord.AsView(), *context);
            chordKnown = chordKnown && control != InvalidInputControl;
            resolved.Chord.Add(control);
        }
        if (resolved.Control == InvalidInputControl || !chordKnown)
        {
            continue;
        }
        resolved.Record = FindOrAddRecord(*action);
        active.Bindings.Add(Move(resolved));
    }
}

uint32 InputSubsystem::FindOrAddRecord(const InputAction &action)
{
    if (const uint32 *index = _recordIndex.Find(action.GetGuid()))
    {
        // A reload may have changed what the action is.
        _records[*index].ValueType = action.ValueType;
        _records[*index].Accumulate = action.Accumulate;
        return *index;
    }
    const auto index = static_cast<uint32>(_records.Count());
    InputActionRecord &record = _records.AddDefaulted();
    record.Action = action.GetGuid();
    record.ValueType = action.ValueType;
    record.Accumulate = action.Accumulate;
    _recordIndex.FindOrAdd(action.GetGuid()) = index;
    return index;
}

void InputSubsystem::Evaluate(float64 deltaTime)
{
    const uint64 rebindHash = HashRebinds();
    List<ActiveContext *> order;
    for (ActiveContext &active : _contexts)
    {
        if (active.Context.Get() != active.Resolved || active.RebindHash != rebindHash)
        {
            Resolve(active, rebindHash);
        }
        if (active.Resolved != nullptr)
        {
            order.Add(&active);
        }
    }
    order.Sort([](const ActiveContext *a, const ActiveContext *b) {
        return a->Resolved->Priority != b->Resolved->Priority ? a->Resolved->Priority > b->Resolved->Priority
                                                               : a->Order > b->Order;
    });

    _consumed.Clear();
    _consumed.Resize(_controls.Count(), false);
    _frameValues.Clear();
    _frameValues.Resize(_records.Count(), Vector3::Zero);
    const auto delta = static_cast<float32>(deltaTime);
    List<InputControlId> used;
    for (const ActiveContext *active : order)
    {
        for (const ResolvedBinding &resolved : active->Bindings)
        {
            if (_consumed[resolved.Control])
            {
                continue;
            }
            bool chordHeld = true;
            for (const InputControlId chord : resolved.Chord)
            {
                chordHeld = chordHeld && !_consumed[chord] && _controls.IsActuated(chord);
            }
            const Vector2 control = _controls.Get(resolved.Control);
            if (!chordHeld || (control.X == 0 && control.Y == 0))
            {
                continue;
            }
            const InputBinding &binding = *resolved.Binding;
            Vector3 value = Vector3::Zero;
            AddToAxis(value, binding.XTo, control.X);
            AddToAxis(value, binding.YTo, control.Y);
            value *= binding.Scale;
            // An amount action integrates a state control over the frame: a stick gives so much per second.
            if (_records[resolved.Record].Accumulate && !_controls.IsAmount(resolved.Control))
            {
                value *= delta;
            }
            _frameValues[resolved.Record] += value;
            used.Add(resolved.Control);
        }
        // Consumed for the contexts below, not for the other bindings of this one.
        for (const InputControlId control : used)
        {
            _consumed[control] = true;
        }
        used.Clear();
    }

    for (usize i = 0; i < _records.Count(); ++i)
    {
        InputActionRecord &record = _records[i];
        Vector3 value = _frameValues[i];
        if (record.ValueType == InputValueType::Button)
        {
            value = Vector3(Math::Clamp(value.X, -1.0f, 1.0f), 0, 0);
        }
        const float32 magnitude = Magnitude(value, record.ValueType);
        const bool down = record.Accumulate ? magnitude > 0 : magnitude >= 0.5f;
        record.FramePresses = down && !record.Down ? 1 : 0;
        record.FrameReleases = !down && record.Down ? 1 : 0;
        record.Presses += record.FramePresses;
        record.Releases += record.FrameReleases;
        record.Down = down;
        record.Value = value;
        if (record.Accumulate)
        {
            record.Total[0] += value.X;
            record.Total[1] += value.Y;
            record.Total[2] += value.Z;
        }
    }
}
} // namespace Marlin
