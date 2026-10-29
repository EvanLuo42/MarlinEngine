#include "Testing/Testing.h"

#include "World/CommandBuffer.h"
#include "World/EntityManager.h"
#include "WorldTestTypes.h"

namespace Marlin::Tests
{
MARLIN_TEST(World, ComponentFlagsComeFromMarkup)
{
    MARLIN_EXPECT(Components::Get(Components::IdOf<TestFrozen>()).IsTag());
    MARLIN_EXPECT(Components::Get(Components::IdOf<TestTeam>()).IsShared());
    MARLIN_EXPECT(Components::Get(Components::IdOf<TestChunkBounds>()).IsChunkComponent());
    MARLIN_EXPECT(Components::Get(Components::IdOf<TestStunned>()).IsEnableable());
    MARLIN_EXPECT(Components::Get(Components::IdOf<TestPosition>()).IsTriviallyCopyable());
    MARLIN_EXPECT(!Components::Get(Components::IdOf<TestLabel>()).IsTriviallyCopyable());
    MARLIN_EXPECT(Components::Find(TypeOf<Vector3>()) == nullptr);
    const Span<const uint32> offsets = Components::Get(Components::IdOf<TestTarget>()).GetEntityOffsets();
    MARLIN_REQUIRE(offsets.Count() == 3);
    MARLIN_EXPECT(offsets[0] == 0 && offsets[1] == sizeof(Entity) && offsets[2] == 2 * sizeof(Entity));
}

MARLIN_TEST(World, CreateAndDestroyReusesSlotsWithNewGenerations)
{
    EntityManager entities;
    const Entity a = entities.CreateEntity();
    const Entity b = entities.CreateEntity();
    MARLIN_EXPECT(entities.Exists(a) && entities.Exists(b) && entities.GetEntityCount() == 2);
    entities.DestroyEntity(a);
    MARLIN_EXPECT(!entities.Exists(a) && entities.Exists(b) && entities.GetEntityCount() == 1);
    const Entity c = entities.CreateEntity();
    MARLIN_EXPECT(c.Index == a.Index && c.Generation != a.Generation && !entities.Exists(a));
    MARLIN_EXPECT(Entity().IsNull() && !a.IsNull());
}

MARLIN_TEST(World, AddAndRemoveKeepValues)
{
    EntityManager entities;
    const ComponentId label = Components::IdOf<TestLabel>();
    const Entity entity = entities.CreateEntity(Span<const ComponentId>(&label, 1));
    entities.Set(entity, TestLabel{String("crate")});
    entities.Add(entity, TestPosition{Vector3(1, 2, 3)});
    entities.Add(entity, TestVelocity{Vector3(4, 5, 6)});
    MARLIN_EXPECT(entities.GetArchetype(entity)->GetComponents().Count() == 3);
    MARLIN_EXPECT(entities.Get<TestPosition>(entity)->Value == Vector3(1, 2, 3));
    MARLIN_EXPECT(entities.Get<TestVelocity>(entity)->Value == Vector3(4, 5, 6));
    MARLIN_EXPECT(entities.Get<TestLabel>(entity)->Text == "crate");

    entities.Remove<TestPosition>(entity);
    entities.Add<TestFrozen>(entity);
    MARLIN_EXPECT(entities.Get<TestPosition>(entity) == nullptr);
    MARLIN_EXPECT(entities.Has<TestFrozen>(entity) && entities.Get<TestFrozen>(entity) == nullptr);
    MARLIN_EXPECT(entities.Get<TestVelocity>(entity)->Value == Vector3(4, 5, 6));
    MARLIN_EXPECT(entities.Get<TestLabel>(entity)->Text == "crate");
    entities.GetForWrite<TestLabel>(entity)->Text = String("barrel");
    entities.Remove<TestFrozen>(entity);
    MARLIN_EXPECT(entities.Get<TestLabel>(entity)->Text == "barrel");
}

MARLIN_TEST(World, ManyEntitiesStayConsistentAcrossChunks)
{
    EntityManager entities;
    const ComponentId position = Components::IdOf<TestPosition>();
    Archetype &archetype = entities.GetOrCreateArchetype(Span<const ComponentId>(&position, 1));
    List<Entity> created;
    created.Resize(20000);
    entities.CreateEntities(archetype, created.AsSpan());
    for (uint32 i = 0; i < created.Count(); ++i)
    {
        entities.Set(created[i], TestPosition{Vector3(static_cast<float32>(i), 0, 0)});
    }
    MARLIN_EXPECT(archetype.GetChunks().Count() > 1 && archetype.GetEntityCount() == 20000);

    // Destroying every other entity moves rows around inside the chunks.
    for (uint32 i = 0; i < created.Count(); i += 2)
    {
        entities.DestroyEntity(created[i]);
    }
    MARLIN_EXPECT(entities.GetEntityCount() == 10000);
    MARLIN_EXPECT(archetype.GetEntityCount() == entities.GetEntityCount());
    bool consistent = true;
    for (uint32 i = 1; i < created.Count(); i += 2)
    {
        const TestPosition *value = entities.Get<TestPosition>(created[i]);
        const EntityLocation location = entities.GetLocation(created[i]);
        consistent = consistent && value != nullptr && value->Value.X == static_cast<float32>(i) &&
                     location.Chunk->GetEntities()[location.Row] == created[i];
    }
    MARLIN_EXPECT(consistent);

    EntityQuery<Read<TestPosition>> query;
    query.Bind(entities);
    MARLIN_EXPECT(query.CalculateEntityCount() == 10000);
    float64 sum = 0;
    query.ForEach([&](const TestPosition &value) { sum += value.Value.X; });
    MARLIN_EXPECT(sum == 10000.0 * 10000.0);
}

MARLIN_TEST(World, SharedValuesGroupEntitiesIntoChunks)
{
    EntityManager entities;
    const ComponentId components[] = {Components::IdOf<TestPosition>(), Components::IdOf<TestTeam>()};
    Archetype &archetype = entities.GetOrCreateArchetype(components);
    Entity red[3];
    Entity blue[2];
    for (Entity &entity : red)
    {
        entity = entities.CreateEntity(archetype);
        entities.Set(entity, TestTeam{1});
    }
    for (Entity &entity : blue)
    {
        entity = entities.CreateEntity(archetype);
        entities.Set(entity, TestTeam{2});
    }
    MARLIN_EXPECT(entities.GetLocation(red[0]).Chunk == entities.GetLocation(red[2]).Chunk);
    MARLIN_EXPECT(entities.GetLocation(red[0]).Chunk != entities.GetLocation(blue[0]).Chunk);
    MARLIN_EXPECT(entities.Get<TestTeam>(blue[1])->Number == 2);
    MARLIN_EXPECT(archetype.GetChunks().Count() == 2);

    // Moving the last red entity to blue leaves one chunk per team.
    for (const Entity entity : red)
    {
        entities.Set(entity, TestTeam{2});
    }
    MARLIN_EXPECT(archetype.GetChunks().Count() == 1 && archetype.GetEntityCount() == 5);
    MARLIN_EXPECT(entities.Get<TestTeam>(red[0])->Number == 2);
}

MARLIN_TEST(World, EnableableComponentsToggleInPlace)
{
    EntityManager entities;
    const Entity entity = entities.CreateEntity();
    entities.Add(entity, TestStunned{2});
    MARLIN_EXPECT(entities.IsEnabled<TestStunned>(entity));
    const EntityLocation before = entities.GetLocation(entity);
    entities.SetEnabled<TestStunned>(entity, false);
    const EntityLocation after = entities.GetLocation(entity);
    MARLIN_EXPECT(before.Chunk == after.Chunk && before.Row == after.Row);
    MARLIN_EXPECT(!entities.IsEnabled<TestStunned>(entity) && entities.Get<TestStunned>(entity)->Seconds == 2);

    EntityQuery<Read<TestStunned>> stunned;
    stunned.Bind(entities);
    EntityQuery<Without<TestStunned>> free;
    free.Bind(entities);
    MARLIN_EXPECT(stunned.CalculateEntityCount() == 0 && free.CalculateEntityCount() == 1);
    entities.SetEnabled<TestStunned>(entity, true);
    MARLIN_EXPECT(stunned.CalculateEntityCount() == 1 && free.CalculateEntityCount() == 0);
}

MARLIN_TEST(World, ChunkComponentsBelongToTheChunk)
{
    EntityManager entities;
    const ComponentId components[] = {Components::IdOf<TestPosition>(), Components::IdOf<TestChunkBounds>()};
    Archetype &archetype = entities.GetOrCreateArchetype(components);
    const Entity a = entities.CreateEntity(archetype);
    const Entity b = entities.CreateEntity(archetype);
    MARLIN_EXPECT(archetype.GetColumns().Count() == 1 && archetype.GetChunkComponents().Count() == 1);
    entities.GetForWrite<TestChunkBounds>(a)->Radius = 7;
    MARLIN_EXPECT(entities.Get<TestChunkBounds>(b)->Radius == 7);
}

MARLIN_TEST(World, ArchetypesDoNotDependOnComponentOrder)
{
    EntityManager entities;
    const ComponentId ab[] = {Components::IdOf<TestPosition>(), Components::IdOf<TestVelocity>()};
    const ComponentId ba[] = {Components::IdOf<TestVelocity>(), Components::IdOf<TestPosition>()};
    MARLIN_EXPECT(&entities.GetOrCreateArchetype(ab) == &entities.GetOrCreateArchetype(ba));
    const Entity entity = entities.CreateEntity(ba);
    MARLIN_EXPECT(entities.GetArchetype(entity) == &entities.GetOrCreateArchetype(ab));
    const Entity other = entities.CreateEntity();
    entities.Add<TestVelocity>(other);
    entities.Add<TestPosition>(other);
    MARLIN_EXPECT(entities.GetArchetype(other) == entities.GetArchetype(entity));
}

MARLIN_TEST(World, VersionsTrackWrites)
{
    EntityManager entities;
    const ComponentId position = Components::IdOf<TestPosition>();
    const Entity entity = entities.CreateEntity(Span<const ComponentId>(&position, 1));
    const uint32 created = entities.GetVersion();
    const uint32 version = entities.AdvanceVersion();
    MARLIN_EXPECT(IsNewerVersion(version, created) && !IsNewerVersion(created, version));
    entities.GetForWrite<TestPosition>(entity)->Value = Vector3(1, 0, 0);
    const EntityLocation location = entities.GetLocation(entity);
    const int32 column = location.Chunk->GetArchetype().FindColumn(position);
    MARLIN_REQUIRE(column >= 0);
    MARLIN_EXPECT(location.Chunk->GetVersion(static_cast<uint32>(column)) == version);
    MARLIN_EXPECT(IsNewerVersion(1, ~uint32{0}));
}

MARLIN_TEST(World, CommandBufferMovesEachEntityOnce)
{
    EntityManager entities;
    const ComponentId position = Components::IdOf<TestPosition>();
    const ComponentId velocity = Components::IdOf<TestVelocity>();
    const Entity existing = entities.CreateEntity(Span<const ComponentId>(&position, 1));

    CommandBuffer commands;
    const Entity first = commands.CreateEntity();
    commands.Add(first, TestPosition{Vector3(1, 2, 3)});
    commands.Add(first, TestVelocity{Vector3(4, 5, 6)});
    commands.Add<TestFrozen>(first);
    const Entity second = commands.CreateEntity(Span<const ComponentId>(&velocity, 1));
    commands.Add(second, TestTarget{first, {existing, first}});
    commands.Remove<TestVelocity>(second);
    commands.Add(existing, TestVelocity{Vector3(7, 8, 9)});
    commands.Add(existing, TestLabel{String("crate")});
    commands.Remove<TestPosition>(existing);
    commands.Playback(entities);

    MARLIN_REQUIRE(entities.GetEntityCount() == 3);
    const Archetype *archetype = entities.GetArchetype(existing);
    MARLIN_EXPECT(archetype->GetComponents().Count() == 2 && !entities.Has<TestPosition>(existing));
    MARLIN_EXPECT(entities.Get<TestVelocity>(existing)->Value == Vector3(7, 8, 9));
    MARLIN_EXPECT(entities.Get<TestLabel>(existing)->Text == "crate");
    for (const Archetype *candidate : entities.GetArchetypes())
    {
        if (candidate->GetComponents().Count() != 3 || !candidate->Has(Components::IdOf<TestFrozen>()))
        {
            continue;
        }
        const Span<Chunk *const> chunks = candidate->GetChunks();
        MARLIN_REQUIRE(chunks.Count() == 1 && chunks[0]->GetCount() == 1);
        const Entity created = chunks[0]->GetEntities()[0];
        MARLIN_EXPECT(entities.Get<TestPosition>(created)->Value == Vector3(1, 2, 3));
        MARLIN_EXPECT(entities.Get<TestVelocity>(created)->Value == Vector3(4, 5, 6));
        for (const Archetype *other : entities.GetArchetypes())
        {
            const Span<Chunk *const> others = other->GetChunks();
            if (others.Count() == 1 && others[0]->GetCount() == 1 && other->Has(Components::IdOf<TestTarget>()))
            {
                const TestTarget *target = entities.Get<TestTarget>(others[0]->GetEntities()[0]);
                MARLIN_EXPECT(target->Value == created && target->Others[0] == existing);
                MARLIN_EXPECT(target->Others[1] == created && other->GetComponents().Count() == 1);
            }
        }
    }
    // The archetypes a run passes through on the way are never filled.
    const ComponentId positionAndVelocity[] = {position, velocity};
    MARLIN_EXPECT(entities.GetOrCreateArchetype(positionAndVelocity).GetStructureVersion() == 0);
}
} // namespace Marlin::Tests
