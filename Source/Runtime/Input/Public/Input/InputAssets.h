#pragma once

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "Input/InputAssets.gen.h"

namespace Marlin
{
/// The shape of an action's value (Documentation/Architecture/15 §3); every value is held as a Vector3.
MENUM()
enum class InputValueType : uint8
{
    Button,
    Axis1D,
    Axis2D,
    Axis3D,
};

/// The component of an action's value a component of a control feeds.
MENUM()
enum class InputAxis : uint8
{
    X,
    Y,
    Z,
    None,
};

/// Something the player does, such as Jump, Move or Look (Documentation/Architecture/15 §3): what gameplay reads,
/// whatever controls the active InputContexts bind to it.
MCLASS(SourceIsRuntime)
class INPUT_API InputAction final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    InputValueType ValueType = InputValueType::Button;
    /// An amount rather than a state: mouse motion adds up as it comes, a held stick adds its value times the frame's
    /// delta, and each reader sees how much came since it last looked, such as how far to turn.
    MPROPERTY()
    bool Accumulate = false;

    InputAction() = default;
    ~InputAction() override = default;
    MARLIN_NONCOPYABLE(InputAction);
};

/// One control of an InputContext feeding an action.
MSTRUCT()
struct InputBinding
{
    MGENERATED_BODY()

    MPROPERTY()
    AssetRef<InputAction> Action;
    /// A control name, `<Device>.<Control>` such as Keyboard.W or Gamepad.LeftStick.
    MPROPERTY()
    FrozenString Control;
    /// Controls that must be held too, such as Mouse.Right; until they are the binding gives nothing.
    MPROPERTY()
    FrozenArray<FrozenString> Chord;
    /// Where the control's X and Y go in the action's value, which is then multiplied by Scale per component.
    MPROPERTY()
    InputAxis XTo = InputAxis::X;
    MPROPERTY()
    InputAxis YTo = InputAxis::Y;
    MPROPERTY()
    Vector3 Scale = Vector3::One;
    /// Names a binding the player may rebind (Engine.Input.Rebinds); empty for one they may not.
    MPROPERTY()
    FrozenString Name;
};

/// A set of bindings that is active as a whole, such as walking, driving or a menu (Documentation/Architecture/15
/// §3). InputSubsystem keeps a stack of them; a binding that gives a value consumes its control for the contexts of
/// lower Priority.
MCLASS(SourceIsRuntime)
class INPUT_API InputContext final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    int32 Priority = 0;
    MPROPERTY()
    FrozenArray<InputBinding> Bindings;

    InputContext() = default;
    ~InputContext() override = default;
    MARLIN_NONCOPYABLE(InputContext);
};
} // namespace Marlin
