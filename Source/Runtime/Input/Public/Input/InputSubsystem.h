#pragma once

#include "Asset/AssetRef.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Engine/Subsystem.h"
#include "Input/InputAssets.h"
#include "Input/InputDevice.h"
#include "Misc/Guid.h"
#include "Reflection/ExtensionSet.h"
#include "String/String.h"
#include "Input/InputSubsystem.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(INPUT_API, LogInput, Info, Warning);

namespace Marlin
{
/// A player's rebinding (Documentation/Architecture/15 §5): the binding called *Binding* in *Context* reads
/// *Control* instead.
MSTRUCT()
struct InputRebind
{
    MGENERATED_BODY()

    MPROPERTY()
    SoftAssetRef<InputContext> Context;
    MPROPERTY()
    String Binding;
    MPROPERTY()
    String Control;
};

/// An action's value this frame and its running totals since the subsystem first saw it, which each stage's reader
/// differences against what it saw last (InputActions).
struct InputActionRecord
{
    Guid Action;
    InputValueType ValueType = InputValueType::Button;
    bool Accumulate = false;
    /// This frame's value; for an Accumulate action, this frame's amount.
    Vector3 Value = Vector3::Zero;
    bool Down = false;
    uint64 Presses = 0;
    uint64 Releases = 0;
    /// The sum of every frame's amount, for Accumulate actions; in double so it stays exact over a long session.
    float64 Total[3] = {};
    /// What this frame added to Presses and Releases, so a reader that first sees the record now starts from the
    /// frame before rather than seeing its whole history.
    uint32 FramePresses = 0;
    uint32 FrameReleases = 0;
};

/// The local player's input (Documentation/Architecture/15 §4). Each frame, after the OS events are pumped, the
/// InputDevices update the controls and the active InputContexts, highest Priority first, turn them into action
/// values; a binding that gives a value consumes its control for the contexts below. Worlds read the actions through
/// their InputActions resource. Settings and the player's rebinds are section Engine.Input.
MCLASS(Config = "Engine.Input", UserConfigurable, Dependencies = "AssetSubsystem",
       OptionalDependencies = "ApplicationSubsystem")
class INPUT_API InputSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Scales Mouse.Delta.
    MPROPERTY()
    static float32 MouseSensitivity;
    /// Makes moving the mouse up give a positive Mouse.Delta Y.
    MPROPERTY()
    static bool InvertMouseY;
    /// Gamepad stick tilt below this is rest, radially; above it the tilt rescales to start from zero.
    MPROPERTY()
    static float32 StickDeadZone;
    MPROPERTY()
    static float32 TriggerDeadZone;
    MPROPERTY()
    static List<InputRebind> Rebinds;

    InputSubsystem();
    ~InputSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// Makes *context* active from the next frame; pushing it again needs as many pops. Any thread.
    void PushContext(const AssetRef<InputContext> &context);
    void PopContext(const AssetRef<InputContext> &context);

    /// Asks for the pointer to be hidden and held, so the mouse only moves Mouse.Delta, for as long as any *owner*
    /// asks. Any thread; applied at the start of the next frame.
    void SetPointerLock(const void *owner, bool locked);

    /// Rebinds the binding called *binding* in *context* to *control* for this session, or undoes that; main
    /// thread. Engine.Input.Rebinds holds the ones a config layer gives.
    void SetRebind(const SoftAssetRef<InputContext> &context, StringView binding, StringView control);
    void ClearRebind(const SoftAssetRef<InputContext> &context, StringView binding);

    /// The records of every action an active context has bound so far; stable until the next BeginFrame.
    [[nodiscard]] Span<const InputActionRecord> GetRecords() const
    {
        return _records;
    }
    [[nodiscard]] const InputActionRecord *FindRecord(const Guid &action) const;

    [[nodiscard]] const InputControls &GetControls() const
    {
        return _controls;
    }

    /// BeginFrame in parts, for tests that set the controls themselves instead of the devices: the queued context
    /// requests, then the contexts turning the controls into actions.
    void ApplyRequests();
    void Evaluate(float64 deltaTime);
    /// The controls, for tests that drive them in place of devices.
    [[nodiscard]] InputControls &GetControlsForTesting()
    {
        return _controls;
    }

private:
    struct ResolvedBinding
    {
        const InputBinding *Binding = nullptr;
        InputControlId Control = InvalidInputControl;
        List<InputControlId> Chord;
        uint32 Record = 0;
    };

    struct ActiveContext
    {
        AssetRef<InputContext> Context;
        uint32 Count = 0;
        /// When it was first pushed: of two contexts of one Priority, the later one wins.
        uint64 Order = 0;
        /// The asset and rebinds the bindings were resolved against; a reload or a rebind resolves them again.
        const InputContext *Resolved = nullptr;
        uint64 RebindHash = 0;
        List<ResolvedBinding> Bindings;
    };

    struct PendingContext
    {
        AssetRef<InputContext> Context;
        bool Push = false;
    };

    void Resolve(ActiveContext &active, uint64 rebindHash);
    [[nodiscard]] uint32 FindOrAddRecord(const InputAction &action);
    [[nodiscard]] InputControlId FindControl(StringView name, const InputContext &context) const;

    ExtensionSet<InputDevice> _devices;
    InputControls _controls;
    List<ActiveContext> _contexts;
    List<InputActionRecord> _records;
    HashMap<Guid, uint32> _recordIndex;
    List<bool> _consumed;
    List<Vector3> _frameValues;
    uint64 _nextOrder = 0;

    SpinLock _pendingLock;
    /// Set by Deinitialize: a world destroyed after it pops its contexts, and queueing that would hold the asset past
    /// the asset shutdown.
    bool _closed = false;
    List<PendingContext> _pending;
    List<const void *> _pointerOwners;
};
} // namespace Marlin
