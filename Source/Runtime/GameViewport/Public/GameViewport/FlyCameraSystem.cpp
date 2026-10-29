#include "GameViewport/FlyCameraSystem.h"

#include "Engine/Engine.h"
#include "Input/InputActions.h"
#include "Input/InputSubsystem.h"
#include "Math/MathUtility.h"
#include "Math/Quaternion.h"

namespace Marlin
{
namespace
{
/// How much one step of the Speed action scales Speed, and the range it stays in.
constexpr float32 SpeedStep = 1.2f;
constexpr float32 MinSpeed = 0.05f;
constexpr float32 MaxSpeed = 5000;
/// Just short of straight up or down, where yaw has no meaning.
constexpr float32 MaxPitch = Math::HalfPi - 0.01f;
} // namespace

FlyCameraSystem::~FlyCameraSystem()
{
    UseContext({});
    if (InputSubsystem *input = Engine::GetSubsystem<InputSubsystem>(); input != nullptr && _locked)
    {
        input->SetPointerLock(this, false);
    }
}

void FlyCameraSystem::Configure(SystemBuilder &builder)
{
    builder.AddQuery(_cameras);
    builder.ReadResource<InputActions>();
}

void FlyCameraSystem::UseContext(const AssetRef<InputContext> &context)
{
    if (context == _pushed)
    {
        return;
    }
    if (InputSubsystem *input = Engine::GetSubsystem<InputSubsystem>())
    {
        input->PopContext(_pushed);
        input->PushContext(context);
    }
    _pushed = context;
}

void FlyCameraSystem::Execute(SystemContext &context)
{
    const InputActions *actions = context.GetResource<InputActions>();
    InputSubsystem *input = Engine::GetSubsystem<InputSubsystem>();
    if (actions == nullptr || input == nullptr)
    {
        return;
    }
    const auto delta = static_cast<float32>(context.GetDeltaTime());
    bool any = false;
    bool engaged = false;
    _cameras.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const FlyCameraInput *bindings = chunk.Get<FlyCameraInput>();
        if (bindings == nullptr)
        {
            return;
        }
        if (!any)
        {
            UseContext(bindings->Context);
            any = true;
        }
        engaged = engaged || actions->Get(bindings->Engage).Down;
        const float32 steps = actions->Get(bindings->Speed).GetAxis1D();
        const Vector2 look = actions->Get(bindings->Look).GetAxis2D();
        const bool boost = actions->Get(bindings->Boost).Down;
        Vector3 move = actions->Get(bindings->Move).Value;
        if (move.Length() > 1)
        {
            move = move.Normalized();
        }

        const Span<FlyCamera> flies = chunk.Write<FlyCamera>();
        const Span<LocalTransform> locals = chunk.Write<LocalTransform>();
        const Span<WorldTransform> worlds = chunk.Write<WorldTransform>();
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            if (!chunk.IsIncluded(row))
            {
                continue;
            }
            FlyCamera &fly = flies[row];
            LocalTransform &local = locals[row];
            if (steps != 0)
            {
                fly.Speed = Math::Clamp(fly.Speed * Math::Pow(SpeedStep, steps), MinSpeed, MaxSpeed);
            }
            // Yaw about world up, then pitch, so the horizon stays level.
            if (look.X != 0 || look.Y != 0)
            {
                const Vector3 euler = local.Rotation.ToEuler();
                const float32 pitch = Math::Clamp(euler.X + look.Y, -MaxPitch, MaxPitch);
                local.Rotation = Quaternion::FromEuler(pitch, euler.Y + look.X, 0);
            }
            if (move.X != 0 || move.Y != 0 || move.Z != 0)
            {
                const float32 speed = fly.Speed * (boost ? fly.BoostScale : 1.0f);
                const Vector3 direction =
                    local.Rotation.GetRight() * move.X + Vector3::Up * move.Y - local.Rotation.GetForward() * move.Z;
                local.Translation += direction * (speed * delta);
            }
            worlds[row] = {local.Translation, local.Rotation, local.Scale};
        }
    });
    if (!any)
    {
        UseContext({});
    }
    if (engaged != _locked)
    {
        input->SetPointerLock(this, engaged);
        _locked = engaged;
    }
}
} // namespace Marlin
