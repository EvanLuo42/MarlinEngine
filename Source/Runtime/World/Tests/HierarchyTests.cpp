#include "Testing/Testing.h"

#include "World/EntityRef.h"
#include "World/Hierarchy.h"
#include "World/World.h"
#include "WorldTestTypes.h"

namespace Marlin::Tests
{
namespace
{
void FillCoreProfile(WorldProfile &profile)
{
    profile.Name = String("Core");
    profile.Sets.Add(String("CoreSimulationSet"));
    profile.Clock = WorldClock::Fixed;
}

Entity CreateNode(World &world, WorldPosition translation, Entity parent = {})
{
    EntityManager &entities = world.GetEntities();
    const ComponentId components[] = {Components::IdOf<LocalTransform>(), Components::IdOf<WorldTransform>()};
    const Entity entity = entities.CreateEntity(components);
    entities.GetForWrite<LocalTransform>(entity)->Translation = translation;
    if (!parent.IsNull())
    {
        entities.Add(entity, Parent{parent});
    }
    return entity;
}

uint32 WorldVersion(const EntityManager &entities, Entity entity)
{
    const EntityLocation location = entities.GetLocation(entity);
    const int32 column = location.Chunk->GetArchetype().FindColumn(Components::IdOf<WorldTransform>());
    return location.Chunk->GetVersion(static_cast<uint32>(column));
}

void RunFrame(World &world)
{
    const FrameTime time{1, 1.0 / 60.0, 0};
    world.BeginFrame(time);
    world.FixedUpdate(time.Delta);
    world.Update(time);
    world.EndFrame(time);
}
} // namespace

MARLIN_TEST(World, TransformsPropagateDownTheHierarchy)
{
    WorldProfile profile;
    FillCoreProfile(profile);
    World world(profile);
    EntityManager &entities = world.GetEntities();
    const Entity root = CreateNode(world, WorldPosition(1e9, 0, 0));
    entities.GetForWrite<LocalTransform>(root)->Scale = Vector3(2, 2, 2);
    const Entity child = CreateNode(world, WorldPosition(0, 2, 3), root);
    const Entity grandchild = CreateNode(world, WorldPosition(0, -2, 0), child);
    RunFrame(world);

    const WorldTransform *leaf = entities.Get<WorldTransform>(child);
    MARLIN_EXPECT(leaf->Translation == WorldPosition(1e9, 4, 6));
    MARLIN_EXPECT(leaf->Scale == Vector3(2, 2, 2));
    MARLIN_EXPECT(entities.Get<WorldTransform>(grandchild)->Translation == WorldPosition(1e9, 0, 6));

    const EntityRef rootRef(world, root);
    MARLIN_REQUIRE(rootRef.GetChildren().Count() == 1);
    MARLIN_EXPECT(rootRef.GetChildren()[0] == child);
    MARLIN_EXPECT(EntityRef(world, grandchild).GetParent().GetEntity() == child);

    // Moving the grandchild to the root relinks it; detaching it unlinks it.
    EntityRef(world, grandchild).SetParent(root);
    RunFrame(world);
    MARLIN_EXPECT(rootRef.GetChildren().Count() == 2 && EntityRef(world, child).GetChildren().IsEmpty());
    MARLIN_EXPECT(entities.Get<WorldTransform>(grandchild)->Translation == WorldPosition(1e9, -4, 0));
    EntityRef(world, grandchild).SetParent({});
    RunFrame(world);
    MARLIN_EXPECT(rootRef.GetChildren().Count() == 1 && !entities.Has<PreviousParent>(grandchild));
    MARLIN_EXPECT(entities.Get<WorldTransform>(grandchild)->Translation == WorldPosition(0, -2, 0));
}

MARLIN_TEST(World, DeepHierarchiesPropagateLevelByLevel)
{
    WorldProfile profile;
    FillCoreProfile(profile);
    World world(profile);
    List<Entity> chain;
    chain.Add(CreateNode(world, WorldPosition(1, 0, 0)));
    for (uint32 i = 1; i < 40; ++i)
    {
        chain.Add(CreateNode(world, WorldPosition(1, 0, 0), chain.Last()));
    }
    RunFrame(world);
    MARLIN_EXPECT(world.GetEntities().Get<WorldTransform>(chain.Last())->Translation == WorldPosition(40, 0, 0));
}

MARLIN_TEST(World, TransformPropagationWritesOnlyWhatMoved)
{
    WorldProfile profile;
    FillCoreProfile(profile);
    World world(profile);
    EntityManager &entities = world.GetEntities();
    const Entity root = CreateNode(world, WorldPosition(1, 0, 0));
    const Entity child = CreateNode(world, WorldPosition(0, 1, 0), root);
    const Entity grandchild = CreateNode(world, WorldPosition(0, 0, 1), child);
    // A second tree in chunks of its own.
    const Entity otherRoot = CreateNode(world, WorldPosition(5, 0, 0));
    entities.Add(otherRoot, TestFrozen{});
    const Entity otherChild = CreateNode(world, WorldPosition(0, 5, 0), otherRoot);
    entities.Add(otherChild, TestFrozen{});
    RunFrame(world);
    RunFrame(world);
    MARLIN_EXPECT(entities.Get<WorldTransform>(grandchild)->Translation == WorldPosition(1, 1, 1));
    const uint32 rootVersion = WorldVersion(entities, root);
    const uint32 childVersion = WorldVersion(entities, child);
    const uint32 otherRootVersion = WorldVersion(entities, otherRoot);
    const uint32 otherChildVersion = WorldVersion(entities, otherChild);

    // Nothing moved: no WorldTransform chunk is written.
    RunFrame(world);
    MARLIN_EXPECT(WorldVersion(entities, root) == rootVersion && WorldVersion(entities, child) == childVersion);
    MARLIN_EXPECT(WorldVersion(entities, otherRoot) == otherRootVersion);
    MARLIN_EXPECT(WorldVersion(entities, otherChild) == otherChildVersion);

    // Moving the root rewrites its tree and leaves the other one alone.
    entities.GetForWrite<LocalTransform>(root)->Translation = WorldPosition(2, 0, 0);
    RunFrame(world);
    MARLIN_EXPECT(entities.Get<WorldTransform>(grandchild)->Translation == WorldPosition(2, 1, 1));
    MARLIN_EXPECT(IsNewerVersion(WorldVersion(entities, root), rootVersion));
    MARLIN_EXPECT(IsNewerVersion(WorldVersion(entities, child), childVersion));
    MARLIN_EXPECT(WorldVersion(entities, otherRoot) == otherRootVersion);
    MARLIN_EXPECT(WorldVersion(entities, otherChild) == otherChildVersion);

    // Moving a child alone reaches its own subtree; spawning elsewhere keeps the results right.
    entities.GetForWrite<LocalTransform>(otherChild)->Translation = WorldPosition(0, 6, 0);
    (void)CreateNode(world, WorldPosition(9, 9, 9));
    RunFrame(world);
    MARLIN_EXPECT(entities.Get<WorldTransform>(otherChild)->Translation == WorldPosition(5, 6, 0));
    MARLIN_EXPECT(WorldVersion(entities, otherRoot) == otherRootVersion);
    MARLIN_EXPECT(entities.Get<WorldTransform>(grandchild)->Translation == WorldPosition(2, 1, 1));

    // Destroying a parent leaves its child at its local transform.
    entities.DestroyEntity(otherRoot);
    RunFrame(world);
    MARLIN_EXPECT(entities.Get<WorldTransform>(otherChild)->Translation == WorldPosition(0, 6, 0));
}
} // namespace Marlin::Tests
