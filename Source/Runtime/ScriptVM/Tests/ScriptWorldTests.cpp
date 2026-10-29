#include "Testing/Testing.h"

#include "Math/Vector.h"
#include "Reflection/Reflection.h"
#include "ScriptVM/ScriptWorld.h"
#include "ScriptWorldRun.h"
#include "World/EntityRef.h"

namespace Marlin::Tests
{
MARLIN_TEST(ScriptVM, ComponentsRegisterAsReflectedTypes)
{
    ScriptWorldRun run(R"(
namespace EcsTypes;

[Component]
public struct Health
{
    public float Current;
    public int Lives;
    public float3 Respawn;
    public Entity Killer;
}
)");
    MARLIN_REQUIREF(run.Run->Ok, "{0}", run.Run->Report());
    const ComponentType *health = ScriptEcs::FindComponent("EcsTypes.Health");
    MARLIN_REQUIRE(health != nullptr);
    const TypeInfo &type = health->GetTypeInfo();
    MARLIN_EXPECT(Reflection::FindType("EcsTypes.Health") == &type);
    MARLIN_EXPECT(type.GetMetadata().Has("Component") && type.GetModule() == "Project");
    MARLIN_REQUIRE(type.GetFields().Count() == 4);
    MARLIN_EXPECT(&type.FindField("Lives")->GetType() == &TypeOf<int32>());
    MARLIN_EXPECT(&type.FindField("Respawn")->GetType() == &TypeOf<Vector3>());
    MARLIN_EXPECT(health->IsTriviallyCopyable() && health->HasColumn());
    // Entity fields are patched like those of C++ components.
    MARLIN_EXPECT(health->GetEntityOffsets().Count() == 1 &&
                  health->GetEntityOffsets()[0] == type.FindField("Killer")->GetOffset());

    // A C++ caller makes and reads them like any component.
    EntityManager &entities = run.TheWorld->GetEntities();
    const Entity entity = entities.CreateEntity();
    EntityRef(*run.TheWorld, entity).AddComponent(health->GetId());
    const auto *value = static_cast<const uint8 *>(EntityRef(*run.TheWorld, entity).GetComponent(health->GetId()));
    MARLIN_REQUIRE(value != nullptr);
    float32 current = -1;
    MemCopy(&current, value + type.FindField("Current")->GetOffset(), sizeof(current));
    MARLIN_EXPECT(current == 0.0f);
}

MARLIN_TEST(ScriptVM, ScriptSystemsRunInTheWorld)
{
    ScriptWorldRun run(R"(
namespace EcsSystems;

[Component] public struct Position { public float3 Value; }
[Component] public struct Velocity { public float3 Value; }
[Component] public struct Stopped { public int Frame; }

public static class State
{
    public static int Spawns;
    public static int Frames;
}

// Runs once per fixed step: it has no components.
[System(UpdateBefore = "EcsSystems.Move")]
public void Spawn()
{
    State.Frames++;
    if (State.Spawns++ == 0)
    {
        // The structure is locked while systems run: the entity is made when the phase ends.
        Entity entity = Entities.Create();
        entity.Set(new Position { Value = float3(0, 0, 0) });
        entity.Set(new Velocity { Value = float3(1, 2, 3) });
    }
}

[System]
public void Move(ref Position position, in Velocity velocity)
{
    position.Value += velocity.Value * Time.Delta;
}

[System(Phase = "LatePhase")]
public void Stop(Entity entity, in Position position)
{
    if (position.Value.x > 2.5f && entity.Has<Velocity>())
    {
        entity.Remove<Velocity>();
        entity.Set(new Stopped { Frame = State.Frames });
    }
}
)");
    MARLIN_REQUIREF(run.Run->Ok, "{0}", run.Run->Report());
    const TypeInfo *move = ScriptEcs::FindSystem("EcsSystems.Move");
    MARLIN_REQUIRE(move != nullptr && ScriptEcs::FindSystem("EcsSystems.Spawn") != nullptr);
    MARLIN_EXPECT(run.TheWorld->GetScheduler().FindSystem(*move) != nullptr);

    for (uint32 frame = 0; frame < 10; ++frame)
    {
        run.RunFrame();
    }
    const ComponentType *position = ScriptEcs::FindComponent("EcsSystems.Position");
    const ComponentType *velocity = ScriptEcs::FindComponent("EcsSystems.Velocity");
    const ComponentType *stopped = ScriptEcs::FindComponent("EcsSystems.Stopped");
    MARLIN_REQUIRE(position != nullptr && velocity != nullptr && stopped != nullptr);
    List<Entity> found;
    run.Find(*position, found);
    MARLIN_REQUIRE(found.Count() == 1);
    const EntityRef entity(*run.TheWorld, found[0]);

    // Made in frame 1, it moves 0.5 s of (1, 2, 3) a frame from frame 2 until frame 7 takes it past 2.5.
    Vector3 value;
    MemCopy(&value, entity.GetComponent(position->GetId()), sizeof(value));
    MARLIN_EXPECTF(value.X == 3.0f && value.Y == 6.0f && value.Z == 9.0f, "{0}", value);
    MARLIN_EXPECT(entity.GetComponent(velocity->GetId()) == nullptr);
    int32 stoppedFrame = 0;
    MARLIN_REQUIRE(entity.GetComponent(stopped->GetId()) != nullptr);
    MemCopy(&stoppedFrame, entity.GetComponent(stopped->GetId()), sizeof(stoppedFrame));
    MARLIN_EXPECT(stoppedFrame == 7);
}

MARLIN_TEST(ScriptVM, SystemsRejectParametersThatAreNotComponents)
{
    ScriptVM vm;
    ScriptRun run(R"(
namespace EcsInvalid;

[System]
public void Takes(int count)
{
}

// Names that the scheduler would stop the process on.
[System(UpdateAfter = "EcsInvalid.Nothing")]
public void OrdersAfterNothing()
{
}

[System(UpdateBefore = "EcsInvalid.OrdersAfterNothing")]
public void OrdersBeforeAnInvalidSystem()
{
}

[System(Phase = "GameplaySet")]
public void RunsInASet()
{
}
)",
                  {}, false, &vm);
    // The module still loads; only the systems are not registered.
    MARLIN_REQUIREF(run.Ok, "{0}", run.Report());
    MARLIN_EXPECT(ScriptEcs::FindSystem("EcsInvalid.Takes") == nullptr);
    MARLIN_EXPECT(ScriptEcs::FindSystem("EcsInvalid.OrdersAfterNothing") == nullptr);
    MARLIN_EXPECT(ScriptEcs::FindSystem("EcsInvalid.OrdersBeforeAnInvalidSystem") == nullptr);
    MARLIN_EXPECT(ScriptEcs::FindSystem("EcsInvalid.RunsInASet") == nullptr);
}
} // namespace Marlin::Tests
