#pragma once

#include "Asset/AssetRef.h"
#include "Containers/HashMap.h"
#include "CoreTypes.h"
#include "Input/InputAssets.h"
#include "Input/InputSubsystem.h"
#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "World/System.h"
#include "Input/InputActions.gen.h"

namespace Marlin
{
/// An action as one stage of a world sees it (Documentation/Architecture/15 §4).
MSTRUCT()
struct InputActionValue
{
    MGENERATED_BODY()

    /// The value now; for an Accumulate action, the amount since the stage last ran.
    MPROPERTY()
    Vector3 Value = Vector3::Zero;
    MPROPERTY()
    bool Down = false;
    /// Whether it went down, or up, since the stage last ran; a press lands in exactly one fixed step.
    MPROPERTY()
    bool Pressed = false;
    MPROPERTY()
    bool Released = false;

    [[nodiscard]] float32 GetAxis1D() const
    {
        return Value.X;
    }
    [[nodiscard]] Vector2 GetAxis2D() const
    {
        return {Value.X, Value.Y};
    }
};

/// What a stage's sampler saw of an action last time.
struct InputSeen
{
    uint64 Presses = 0;
    uint64 Releases = 0;
    float64 Total[3] = {};
};

/// The input actions of a world, Resource<InputActions> (Documentation/Architecture/15 §4): InputFixedStepSystem
/// refreshes it at the start of each fixed step and InputFrameSystem at the start of the Variable stage, so each
/// stage reads what happened since it last ran. Declare ReadResource<InputActions> and read it in a later phase.
MSTRUCT()
struct INPUT_API InputActions
{
    MGENERATED_BODY()

    /// The value of *action*; at rest when no active context binds it.
    [[nodiscard]] const InputActionValue &Get(const Guid &action) const;
    template <typename TRef> [[nodiscard]] const InputActionValue &Get(const TRef &action) const
    {
        return Get(action.GetGuid());
    }

    /// Writes the values from InputSubsystem's *records*, against what this stage saw of them last time, which it
    /// updates; what the two systems run.
    void Sample(Span<const InputActionRecord> records, HashMap<Guid, InputSeen> &seen);

private:
    HashMap<Guid, InputActionValue> _values;
};

/// Where the Variable stage samples input: before the cameras, which turn with it.
MSTRUCT(Phase, Stage = Variable, Before = CameraPhase)
struct InputFramePhase
{
    MGENERATED_BODY()
};

/// Refreshes InputActions at the start of each fixed step.
MCLASS(Phase = InputPhase, Sets = (CoreSimulationSet))
class INPUT_API InputFixedStepSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    HashMap<Guid, InputSeen> _seen;
};

/// Refreshes InputActions at the start of the Variable stage.
MCLASS(Phase = InputFramePhase, Sets = (CoreSimulationSet))
class INPUT_API InputFrameSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    HashMap<Guid, InputSeen> _seen;
};
} // namespace Marlin
