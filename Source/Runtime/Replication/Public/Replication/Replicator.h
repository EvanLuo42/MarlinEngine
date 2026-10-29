#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Math/WorldPosition.h"
#include "Memory/UniquePtr.h"
#include "Replication/ReplicationSchema.h"
#include "World/EntityManager.h"
#include "World/EntityQuery.h"

namespace Marlin
{
class NetBitReader;
class NetBitWriter;

/// One entity's replicated values as a connection holds them: the net indices of its components, sorted, and the
/// leaves of those components in that order, entities as network ids. Snapshots share blocks: an entity that a
/// snapshot keeps unchanged from its base refers to the block of the base.
struct NetStateBlock
{
    uint32 References = 0;
    List<uint32> Components;
    List<uint64> Leaves;
};

/// The blocks of the snapshots of one connection, or of a client, recycled so that steady-state snapshots allocate
/// nothing.
class REPLICATION_API NetStatePool
{
public:
    NetStatePool() = default;
    MARLIN_NONCOPYABLE(NetStatePool);

    /// A new, empty block with one reference.
    uint32 Allocate();

    void AddReference(uint32 block)
    {
        ++_blocks[block]->References;
    }

    void Release(uint32 block);

    [[nodiscard]] NetStateBlock &Get(uint32 block)
    {
        return *_blocks[block];
    }

    [[nodiscard]] const NetStateBlock &Get(uint32 block) const
    {
        return *_blocks[block];
    }

private:
    List<UniquePtr<NetStateBlock>> _blocks;
    List<uint32> _free;
};

/// An entity in a snapshot.
struct NetSnapshotEntry
{
    uint32 NetId = 0;
    uint32 Block = 0;
    /// On the server, the ServerReplicator update at which the block last matched the entity's values.
    uint32 Captured = 0;
};

/// What a client holds after applying the snapshot of Tick: the baseline later snapshots are deltas against.
struct NetSnapshot
{
    uint32 Tick = 0;
    /// Sorted by NetId.
    List<NetSnapshotEntry> Entities;

    [[nodiscard]] const NetSnapshotEntry *Find(uint32 netId) const;
};

enum class NetMessageKind : uint8
{
    Snapshot,
    Acknowledge,
};

// RECOVERY-DECISION D7: 32 snapshots, half a second at 60 Hz, before an unacknowledged connection gets full state.
/// Snapshots a server keeps per connection, and a client keeps, as possible baselines.
inline constexpr uint32 NetSnapshotHistory = 32;

/// The last NetSnapshotHistory snapshots and the pool of the blocks they refer to.
class REPLICATION_API NetSnapshotRing
{
public:
    NetSnapshotRing() = default;
    ~NetSnapshotRing();
    MARLIN_NONCOPYABLE(NetSnapshotRing);

    [[nodiscard]] const NetSnapshot *Find(uint32 tick) const;
    /// Takes the entries of *snapshot*, and the block references they hold, dropping the oldest snapshot when full;
    /// *snapshot* is left empty, for reuse.
    void Push(NetSnapshot &snapshot);

    NetStatePool Pool;

private:
    NetSnapshot _slots[NetSnapshotHistory];
    uint32 _count = 0;
    uint32 _next = 0;
};

/// The server side of replication over one world's entities (Documentation/Architecture/05 §7): each connection gets
/// snapshots of the relevant entities by priority within a byte budget, delta-encoded against the last snapshot it
/// acknowledged, so a lost snapshot costs nothing but its own content. Update captures the replicated entities once
/// for all connections, reading only the chunks written since the last update.
class REPLICATION_API ServerReplicator
{
public:
    explicit ServerReplicator(EntityManager &entities);
    ~ServerReplicator();
    MARLIN_NONCOPYABLE(ServerReplicator);

    uint32 AddConnection();
    void RemoveConnection(uint32 connection);
    /// The entity whose WorldTransform relevancy ranges are measured from, such as the connection's player.
    void SetViewer(uint32 connection, Entity viewer);

    /// Gives entities with a NetworkIdentity their ids, forgets destroyed ones and captures what changed; call once
    /// before the snapshots of a tick.
    void Update();
    /// Writes the snapshot of *tick* for *connection*, at most about *budget* bytes; the most urgent entity is always
    /// sent so a small budget cannot starve the queue. After Update, the snapshots of different connections may be
    /// written at the same time on different threads.
    void WriteSnapshot(uint32 connection, uint32 tick, uint32 budget, List<uint8> &message);
    /// Handles a message from *connection*: an acknowledgement.
    void Receive(uint32 connection, Span<const uint8> message);

    [[nodiscard]] Entity FindEntity(uint32 netId) const;
    [[nodiscard]] uint32 GetAcknowledgedTick(uint32 connection) const;

private:
    /// A replicated entity as Update last captured it; _records is in NetId order.
    struct Record
    {
        uint32 NetId = 0;
        Entity Target;
        const Archetype *Location = nullptr;
        /// The update that last saw the entity, and the last one at which its values or owner changed.
        uint32 Seen = 0;
        uint32 Changed = 0;
        uint32 Owner = 0;
        float32 Range = 0;
        float32 Priority = 1;
        bool HasPosition = false;
        WorldPosition Position;
        /// As in NetStateBlock, with the OwnerOnly leaves that other connections do not see.
        List<uint32> Components;
        List<uint64> Leaves;
    };

    /// What Update needs of an archetype with a NetworkIdentity; an archetype never changes its components.
    struct ArchetypeInfo
    {
        struct Column
        {
            const ReplicatedComponent *Replicated = nullptr;
            /// -1 for a component without a column: a tag, shared or chunk component.
            int32 Index = -1;
        };

        bool Known = false;
        bool HasEntityLeaves = false;
        bool StructureChanged = false;
        uint32 StructureVersion = 0;
        int32 Identity = -1;
        int32 Owner = -1;
        int32 Relevancy = -1;
        int32 Transform = -1;
        /// Sorted by NetIndex.
        List<Column> Replicated;
        /// The columns whose writes change what is sent, and those that only change relevancy.
        List<uint32> StateColumns;
        List<uint32> RelevancyColumns;
    };

    struct PriorityEntry
    {
        uint32 NetId = 0;
        float64 Priority = 0;
    };

    /// A relevant entity of the snapshot being written.
    struct Relevant
    {
        uint32 Record = 0;
        /// An index into the base's entries, or ~0.
        uint32 BaseEntry = ~0u;
        /// The block sent, or ~0.
        uint32 Sent = ~0u;
        /// Whether the client's block matches the entity now, sent or not.
        bool Current = false;
        float64 Priority = 0;
    };

    struct Connection
    {
        uint32 Id = 0;
        Entity Viewer;
        uint32 Acknowledged = 0;
        NetSnapshotRing History;
        /// Of the entities relevant at the last snapshot, by NetId.
        List<PriorityEntry> Priorities;
        // Scratch, kept for its memory.
        List<PriorityEntry> NextPriorities;
        List<Relevant> Candidates;
        List<uint32> Order;
        List<uint32> Removed;
        List<uint8> EntityBits;
        NetSnapshot Next;
    };

    Connection *FindConnection(uint32 connection);
    const Connection *FindConnection(uint32 connection) const;
    ArchetypeInfo &GetInfo(const Archetype &archetype);
    [[nodiscard]] const Record *FindRecord(uint32 netId) const;
    Record &FindOrAddRecord(uint32 netId);
    /// Reads the row into *record*; true if what a connection would receive changed.
    bool Capture(const ArchetypeInfo &info, const Chunk &chunk, uint32 row, Record &record);
    void CaptureRelevancy(const ArchetypeInfo &info, const Chunk &chunk, uint32 row, Record &record) const;
    [[nodiscard]] bool IsRelevant(const Record &record, uint32 connection, const WorldPosition *viewer) const;
    /// Writes *record* as a delta against *base*, the block the client holds or null, and returns a new block of
    /// *pool* with what the client will hold then, or ~0 if there is nothing to send.
    static uint32 WriteEntityDelta(NetBitWriter &writer, const Record &record, bool owned, NetStatePool &pool,
                                   const NetStateBlock *base);

    EntityManager &_entities;
    uint32 _nextNetId = 1;
    uint32 _nextConnection = 1;
    uint32 _update = 0;
    /// The EntityManager version Update started a new one at; writes since are stamped with it or later.
    uint32 _captureVersion = 0;
    List<Record> _records;
    /// By Archetype::GetIndex.
    List<ArchetypeInfo> _archetypes;
    List<UniquePtr<Connection>> _connections;
    EntityQuery<Read<NetworkIdentity>> _identities;
    // Scratch, kept for its memory.
    List<uint64> _leaves;
    List<uint32> _components;
};

/// The client side: applies snapshots to its own entities, creating and destroying them, and answers each with an
/// acknowledgement.
class REPLICATION_API ClientReplicator
{
public:
    explicit ClientReplicator(EntityManager &entities);
    MARLIN_NONCOPYABLE(ClientReplicator);

    /// Applies a snapshot message; true if it was new and decoded, with *acknowledgement* filled to send back.
    bool Receive(Span<const uint8> message, List<uint8> &acknowledgement);

    /// The client entity of server network id *netId*, or null.
    [[nodiscard]] Entity FindEntity(uint32 netId) const;

    [[nodiscard]] uint32 GetLastTick() const
    {
        return _lastTick;
    }

private:
    bool Decode(NetBitReader &reader, const NetSnapshot *base);
    void Apply();

    EntityManager &_entities;
    NetSnapshotRing _history;
    HashMap<uint32, Entity> _byNetId;
    uint32 _lastTick = 0;
    // Scratch, kept for its memory.
    List<NetSnapshotEntry> _updated;
    List<uint32> _removed;
    List<ComponentId> _components;
    NetSnapshot _next;
};
} // namespace Marlin
