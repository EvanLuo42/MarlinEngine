#include "Testing/Testing.h"

#include "String/StringBuilder.h"
#include "World/EntityTemplate.h"
#include "World/Hierarchy.h"
#include "WorldTestBlocks.h"
#include "WorldTestTypes.h"

namespace Marlin::Tests
{
namespace
{
/// Two entities of different archetypes that point at each other and at *outsider*, which stays out of the template.
void BuildCrate(EntityManager &entities, Entity outsider, Entity (&crate)[2])
{
    const ComponentId lid[] = {Components::IdOf<LocalTransform>(), Components::IdOf<TestTeam>(),
                               Components::IdOf<TestAssetLink>()};
    const ComponentId body[] = {Components::IdOf<LocalTransform>(), Components::IdOf<Parent>(),
                                Components::IdOf<TestTarget>(), Components::IdOf<TestFrozen>()};
    crate[0] = entities.CreateEntity(lid);
    crate[1] = entities.CreateEntity(body);
    entities.Set(crate[0], TestTeam{3});
    entities.GetForWrite<LocalTransform>(crate[0])->Translation = WorldPosition(5, 0, 0);
    entities.Set(crate[1], Parent{crate[0]});
    TestTarget target;
    target.Value = crate[0];
    target.Others[0] = outsider;
    target.Others[1] = crate[1];
    entities.Set(crate[1], target);
}
} // namespace

MARLIN_TEST(World, EntityTemplatesRejectHeapComponents)
{
    EntityManager entities;
    const Entity entity = entities.CreateEntity();
    entities.Add(entity, TestLabel{String("crate")});
    EntityTemplate prefab;
    StringBuilder<256> problems;
    MARLIN_EXPECT(!prefab.Capture(entities, Span<const Entity>(&entity, 1), &problems));
    MARLIN_EXPECT(problems.AsView().Contains("TestLabel.Text"));
    MARLIN_EXPECT(EntityTemplate::CanStore(TypeOf<LocalTransform>()) && EntityTemplate::CanStore(TypeOf<Parent>()));
}

MARLIN_TEST(World, EntityTemplatesRoundTripThroughBlocks)
{
    WorldTestBlocks blocks("WorldTemplateTests");
    const Guid linkGuid = WorldTestBlocks::MakeGuid(1);
    const Guid crateGuid = WorldTestBlocks::MakeGuid(2);
    {
        // A template for the crate to link to.
        EntityManager source;
        EntityTemplate link;
        const Entity single = source.CreateEntity();
        source.Add(single, TestPosition{Vector3(1, 2, 3)});
        MARLIN_REQUIRE(link.Capture(source, Span<const Entity>(&single, 1)));
        blocks.Write(link, linkGuid);
    }
    {
        EntityManager source;
        const Entity outsider = source.CreateEntity();
        Entity crate[2];
        BuildCrate(source, outsider, crate);
        source.GetForWrite<TestAssetLink>(crate[0])->Linked = AssetRef<EntityTemplate>::FromGuid(linkGuid);
        EntityTemplate prefab;
        MARLIN_REQUIRE(prefab.Capture(source, crate));
        MARLIN_EXPECT(prefab.EntityCount == 2 && prefab.Groups.Count() == 2);
        blocks.Write(prefab, crateGuid);
    }
    blocks.Serve();

    AssetRef<EntityTemplate> crate = Assets::LoadSync<EntityTemplate>(crateGuid);
    MARLIN_REQUIRE(crate.IsLoaded() && crate->IsUsable());
    EntityManager world;
    const Entity before = world.CreateEntity();
    Entity first[2];
    Entity second[2];
    MARLIN_REQUIRE(crate->Instantiate(world, first));
    MARLIN_REQUIRE(crate->Instantiate(world, second));
    MARLIN_EXPECT(world.GetEntityCount() == 5 && world.Exists(before));

    MARLIN_EXPECT(world.Get<Parent>(first[1])->Value == first[0]);
    MARLIN_EXPECT(world.Get<Parent>(second[1])->Value == second[0]);
    MARLIN_EXPECT(world.Get<LocalTransform>(first[0])->Translation == WorldPosition(5, 0, 0));
    MARLIN_EXPECT(world.Get<TestTeam>(second[0])->Number == 3);
    MARLIN_EXPECT(world.Has<TestFrozen>(second[1]));
    const TestTarget *target = world.Get<TestTarget>(second[1]);
    MARLIN_REQUIRE(target != nullptr);
    MARLIN_EXPECT(target->Value == second[0] && target->Others[0].IsNull() && target->Others[1] == second[1]);
    const TestAssetLink *link = world.Get<TestAssetLink>(first[0]);
    MARLIN_REQUIRE(link != nullptr);
    MARLIN_EXPECT(world.Get<TestAssetLink>(second[0])->Linked == link->Linked);
    MARLIN_EXPECT(world.Get<TestAssetLink>(second[0])->Linked.IsLoaded());
    MARLIN_EXPECT(link->Linked->EntityCount == 1);
}
} // namespace Marlin::Tests
