#include "Testing/Testing.h"

#include "Replication/NetBitStream.h"
#include "Replication/Replicator.h"
#include "ReplicationTestTypes.h"
#include "World/Hierarchy.h"

namespace Marlin::Tests
{
namespace
{
/// A server and a client world joined directly, without a transport: Step sends one snapshot and, unless it is lost,
/// returns the acknowledgement.
struct ReplicationPair
{
    EntityManager ServerEntities;
    EntityManager ClientEntities;
    ServerReplicator Server{ServerEntities};
    ClientReplicator Client{ClientEntities};
    uint32 Connection = Server.AddConnection();
    uint32 Tick = 0;
    usize LastSize = 0;

    void Step(bool lose = false, uint32 budget = 4096)
    {
        Server.Update();
        List<uint8> message;
        Server.WriteSnapshot(Connection, ++Tick, budget, message);
        LastSize = message.Count();
        List<uint8> acknowledgement;
        if (!lose && Client.Receive(message.AsSpan(), acknowledgement))
        {
            Server.Receive(Connection, acknowledgement.AsSpan());
        }
    }

    Entity Mirror(Entity serverEntity) const
    {
        const NetworkIdentity *identity = ServerEntities.Get<NetworkIdentity>(serverEntity);
        return identity != nullptr ? Client.FindEntity(identity->Id) : Entity();
    }
};

Entity CreateReplicated(EntityManager &entities, const TestReplicatedState &state)
{
    const Entity entity = entities.CreateEntity();
    entities.Add(entity, NetworkIdentity{});
    entities.Add(entity, state);
    return entity;
}
} // namespace

MARLIN_TEST(Replication, BitStreamsRoundTrip)
{
    List<uint8> bytes;
    NetBitWriter writer(bytes);
    writer.WriteBool(true);
    writer.WriteBits(0x5, 3);
    writer.WriteVarUInt(300);
    writer.WriteVarInt(-77);
    writer.WriteBits(0xDEADBEEFCAFEF00Dull, 64);
    MARLIN_EXPECT(writer.GetBitCount() == 1 + 3 + 16 + 16 + 64 && writer.GetByteCount() == bytes.Count());

    NetBitReader reader(bytes.AsSpan());
    MARLIN_EXPECT(reader.ReadBool() && reader.ReadBits(3) == 0x5);
    MARLIN_EXPECT(reader.ReadVarUInt() == 300 && reader.ReadVarInt() == -77);
    MARLIN_EXPECT(reader.ReadBits(64) == 0xDEADBEEFCAFEF00Dull && !reader.HasOverflowed());
    MARLIN_EXPECT(reader.IsAtEnd());
    MARLIN_EXPECT(reader.ReadBits(8) == 0 && reader.HasOverflowed());
}

MARLIN_TEST(Replication, SchemaFlattensReplicatedFields)
{
    const ReplicatedComponent *schema = ReplicationSchema::Find(Components::IdOf<TestReplicatedState>());
    MARLIN_REQUIRE(schema != nullptr && schema->Fields.Count() == 5);
    MARLIN_EXPECT(schema->Fields[1].Encoding == NetFieldEncoding::Quantized && schema->Fields[1].Quantize == 0.01);
    MARLIN_EXPECT(schema->Fields[2].Encoding == NetFieldEncoding::Entity);
    MARLIN_EXPECT(schema->Fields[3].OwnerOnly && schema->Fields[4].InitialOnly);
    MARLIN_EXPECT(schema->Defaults[0] == 100);
    MARLIN_EXPECT(ReplicationSchema::FromNetIndex(schema->NetIndex) == schema);
    MARLIN_EXPECT(ReplicationSchema::Find(Components::IdOf<LocalTransform>()) == nullptr);
}

MARLIN_TEST(Replication, SnapshotsDeltaAgainstAcknowledgedBaselines)
{
    ReplicationPair pair;
    const Entity other = CreateReplicated(pair.ServerEntities, {});
    TestReplicatedState state;
    state.Health = 80;
    state.Speed = 1.23f;
    state.Target = other;
    state.Ammo = 5;
    state.Seed = 7;
    state.ServerOnly = 3;
    const Entity entity = CreateReplicated(pair.ServerEntities, state);
    pair.ServerEntities.Add(entity, NetOwner{pair.Connection});

    pair.Step();
    const usize first = pair.LastSize;
    MARLIN_EXPECT(pair.ClientEntities.GetEntityCount() == 2);
    const TestReplicatedState *received = pair.ClientEntities.Get<TestReplicatedState>(pair.Mirror(entity));
    MARLIN_REQUIRE(received != nullptr);
    MARLIN_EXPECT(received->Health == 80 && received->Speed > 1.229f && received->Speed < 1.231f);
    MARLIN_EXPECT(received->Target == pair.Mirror(other) && received->Ammo == 5 && received->Seed == 7);
    MARLIN_EXPECT(received->ServerOnly == 0);
    MARLIN_EXPECT(pair.Server.GetAcknowledgedTick(pair.Connection) == pair.Tick);

    TestReplicatedState *changed = pair.ServerEntities.GetForWrite<TestReplicatedState>(entity);
    changed->Health = 40;
    changed->Speed = -2.0f;
    pair.Step();
    MARLIN_EXPECT(pair.LastSize < first);
    received = pair.ClientEntities.Get<TestReplicatedState>(pair.Mirror(entity));
    MARLIN_EXPECT(received->Health == 40 && received->Speed == -2.0f);

    // A lost snapshot: the next one is a delta against the last acknowledged one and still carries the change.
    // InitialOnly fields never change after the first.
    changed = pair.ServerEntities.GetForWrite<TestReplicatedState>(entity);
    changed->Health = 50;
    changed->Seed = 9;
    pair.Step(true);
    MARLIN_EXPECT(pair.ClientEntities.Get<TestReplicatedState>(pair.Mirror(entity))->Health == 40);
    pair.Step();
    received = pair.ClientEntities.Get<TestReplicatedState>(pair.Mirror(entity));
    MARLIN_EXPECT(received->Health == 50 && received->Seed == 7);

    // Destroying an entity removes it on the client and clears references to it.
    const Entity mirror = pair.Mirror(entity);
    pair.ServerEntities.DestroyEntity(other);
    pair.Step();
    MARLIN_EXPECT(pair.ClientEntities.GetEntityCount() == 1);
    MARLIN_EXPECT(pair.ClientEntities.Get<TestReplicatedState>(mirror)->Target.IsNull());
}

MARLIN_TEST(Replication, OwnerOnlyFieldsAndRelevancy)
{
    ReplicationPair pair;
    TestReplicatedState state;
    state.Ammo = 5;
    const Entity owned = CreateReplicated(pair.ServerEntities, state);
    pair.ServerEntities.Add(owned, NetOwner{pair.Connection + 1});
    const Entity mine = CreateReplicated(pair.ServerEntities, state);
    pair.ServerEntities.Add(mine, NetOwner{pair.Connection});

    const Entity viewer = pair.ServerEntities.CreateEntity();
    pair.ServerEntities.Add(viewer, WorldTransform{});
    pair.Server.SetViewer(pair.Connection, viewer);
    const Entity far = CreateReplicated(pair.ServerEntities, state);
    pair.ServerEntities.Add(far, NetRelevancy{10, 1});
    pair.ServerEntities.Add(far, WorldTransform{WorldPosition(100, 0, 0)});

    pair.Step();
    MARLIN_EXPECT(pair.ClientEntities.Get<TestReplicatedState>(pair.Mirror(owned))->Ammo == 0);
    MARLIN_EXPECT(pair.ClientEntities.Get<TestReplicatedState>(pair.Mirror(mine))->Ammo == 5);
    MARLIN_EXPECT(pair.Mirror(far).IsNull() && pair.ClientEntities.GetEntityCount() == 2);

    pair.ServerEntities.GetForWrite<WorldTransform>(far)->Translation = WorldPosition(5, 0, 0);
    pair.Step();
    MARLIN_EXPECT(!pair.Mirror(far).IsNull());
    pair.ServerEntities.GetForWrite<WorldTransform>(far)->Translation = WorldPosition(50, 0, 0);
    pair.Step();
    MARLIN_EXPECT(pair.Mirror(far).IsNull() && pair.ClientEntities.GetEntityCount() == 2);
}

MARLIN_TEST(Replication, BudgetSpreadsEntitiesOverSnapshots)
{
    ReplicationPair pair;
    for (int32 i = 0; i < 200; ++i)
    {
        TestReplicatedState state;
        state.Health = i;
        (void)CreateReplicated(pair.ServerEntities, state);
    }
    pair.Step(false, 256);
    MARLIN_EXPECT(pair.LastSize <= 256);
    MARLIN_EXPECT(pair.ClientEntities.GetEntityCount() > 0 && pair.ClientEntities.GetEntityCount() < 200);
    for (uint32 step = 0; step < 100 && pair.ClientEntities.GetEntityCount() < 200; ++step)
    {
        pair.Step(false, 256);
    }
    MARLIN_EXPECT(pair.ClientEntities.GetEntityCount() == 200);
    pair.Step(false, 256);
    MARLIN_EXPECT(pair.LastSize < 16);
}

MARLIN_TEST(Replication, ConnectionsShareOneCaptureAndFollowChanges)
{
    EntityManager serverEntities;
    EntityManager firstEntities;
    EntityManager secondEntities;
    ServerReplicator server(serverEntities);
    ClientReplicator first(firstEntities);
    ClientReplicator second(secondEntities);
    const uint32 firstConnection = server.AddConnection();
    const uint32 secondConnection = server.AddConnection();
    uint32 tick = 0;
    usize sizes[2] = {};
    const auto step = [&] {
        server.Update();
        ++tick;
        const uint32 connections[] = {firstConnection, secondConnection};
        ClientReplicator *clients[] = {&first, &second};
        for (uint32 i = 0; i < 2; ++i)
        {
            List<uint8> message;
            server.WriteSnapshot(connections[i], tick, 4096, message);
            sizes[i] = message.Count();
            List<uint8> acknowledgement;
            if (clients[i]->Receive(message.AsSpan(), acknowledgement))
            {
                server.Receive(connections[i], acknowledgement.AsSpan());
            }
        }
    };
    const auto netId = [&](Entity entity) { return serverEntities.Get<NetworkIdentity>(entity)->Id; };

    // The target lives in another archetype than the entity that refers to it.
    const Entity target = serverEntities.CreateEntity();
    serverEntities.Add(target, NetworkIdentity{});
    serverEntities.Add(target, TestReplicatedScore{3});
    TestReplicatedState state;
    state.Ammo = 5;
    state.Target = target;
    const Entity entity = CreateReplicated(serverEntities, state);
    serverEntities.Add(entity, NetOwner{firstConnection});
    step();
    const auto mirror = [&](ClientReplicator &client, Entity serverEntity) {
        return client.FindEntity(netId(serverEntity));
    };
    const auto stateOf = [&](EntityManager &entities, ClientReplicator &client) {
        return entities.Get<TestReplicatedState>(mirror(client, entity));
    };
    MARLIN_REQUIRE(stateOf(firstEntities, first) != nullptr && stateOf(secondEntities, second) != nullptr);
    MARLIN_EXPECT(stateOf(firstEntities, first)->Ammo == 5 && stateOf(secondEntities, second)->Ammo == 0);
    MARLIN_EXPECT(stateOf(firstEntities, first)->Target == mirror(first, target));
    MARLIN_EXPECT(stateOf(secondEntities, second)->Target == mirror(second, target));
    MARLIN_EXPECT(secondEntities.Get<TestReplicatedScore>(mirror(second, target))->Points == 3);

    // Nothing changed: both snapshots are just the header and the empty lists.
    step();
    MARLIN_EXPECT(sizes[0] < 8 && sizes[1] < 8);

    // A new owner sees the OwnerOnly field, the old one no longer does.
    serverEntities.Set(entity, NetOwner{secondConnection});
    step();
    MARLIN_EXPECT(stateOf(firstEntities, first)->Ammo == 0 && stateOf(secondEntities, second)->Ammo == 5);

    // Components the server adds and removes come and go on every client.
    serverEntities.Add(entity, TestReplicatedScore{7});
    step();
    MARLIN_EXPECT(firstEntities.Get<TestReplicatedScore>(mirror(first, entity))->Points == 7);
    MARLIN_EXPECT(secondEntities.Get<TestReplicatedScore>(mirror(second, entity))->Points == 7);
    serverEntities.Remove<TestReplicatedScore>(entity);
    step();
    MARLIN_EXPECT(!firstEntities.Has<TestReplicatedScore>(mirror(first, entity)));
    MARLIN_EXPECT(!secondEntities.Has<TestReplicatedScore>(mirror(second, entity)));
    MARLIN_EXPECT(stateOf(secondEntities, second)->Ammo == 5);

    // Destroying the target clears the reference, though the referring chunk was not written.
    const uint32 targetId = netId(target);
    serverEntities.DestroyEntity(target);
    step();
    MARLIN_EXPECT(first.FindEntity(targetId).IsNull() && second.FindEntity(targetId).IsNull());
    MARLIN_EXPECT(stateOf(firstEntities, first)->Target.IsNull() && stateOf(secondEntities, second)->Target.IsNull());
    MARLIN_EXPECT(firstEntities.GetEntityCount() == 1 && secondEntities.GetEntityCount() == 1);
}
} // namespace Marlin::Tests
