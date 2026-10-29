#include "Replication/Replicator.h"

#include "Replication/NetBitStream.h"
#include "World/Hierarchy.h"

namespace Marlin
{
namespace
{
// RECOVERY-DECISION D3: the wire format. A snapshot is: kind (8 bits), tick, base tick (0 for none), then entities
// each behind a 1 bit and the list ended by a 0 bit, then removed network ids the same way. An entity is its id and
// its changed components (1 bit, net index, removed bit, then per leaf a changed bit and the value), ended by a 0 bit.
// An acknowledgement is kind and tick. Numbers are varints, floats raw bits, bools one bit.
void WriteValue(NetBitWriter &writer, const NetLeafField &field, uint64 value)
{
    switch (field.Encoding)
    {
    case NetFieldEncoding::Bool:
        writer.WriteBool(value != 0);
        break;
    case NetFieldEncoding::Signed:
    case NetFieldEncoding::Quantized:
        writer.WriteVarInt(static_cast<int64>(value));
        break;
    case NetFieldEncoding::Unsigned:
    case NetFieldEncoding::Entity:
        writer.WriteVarUInt(value);
        break;
    case NetFieldEncoding::Float32:
        writer.WriteBits(value, 32);
        break;
    case NetFieldEncoding::Float64:
        writer.WriteBits(value, 64);
        break;
    }
}

uint64 ReadValue(NetBitReader &reader, const NetLeafField &field)
{
    switch (field.Encoding)
    {
    case NetFieldEncoding::Bool:
        return reader.ReadBool() ? 1 : 0;
    case NetFieldEncoding::Signed:
    case NetFieldEncoding::Quantized:
        return static_cast<uint64>(reader.ReadVarInt());
    case NetFieldEncoding::Unsigned:
    case NetFieldEncoding::Entity:
        return reader.ReadVarUInt();
    case NetFieldEncoding::Float32:
        return reader.ReadBits(32);
    case NetFieldEncoding::Float64:
        return reader.ReadBits(64);
    }
    return 0;
}

usize LeafCount(uint32 netIndex)
{
    return ReplicationSchema::FromNetIndex(netIndex)->Fields.Count();
}

template <typename T> bool SameValues(const List<T> &a, const List<T> &b)
{
    if (a.Count() != b.Count())
    {
        return false;
    }
    for (usize i = 0; i < a.Count(); ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

template <typename T> void Assign(List<T> &target, const List<T> &values)
{
    target.Clear();
    target.AddRange(values.AsSpan());
}
} // namespace

uint32 NetStatePool::Allocate()
{
    uint32 block = 0;
    if (!_free.IsEmpty())
    {
        block = _free.Pop();
    }
    else
    {
        block = static_cast<uint32>(_blocks.Count());
        _blocks.Add(MakeUnique<NetStateBlock>());
    }
    NetStateBlock &state = *_blocks[block];
    state.References = 1;
    state.Components.Clear();
    state.Leaves.Clear();
    return block;
}

void NetStatePool::Release(uint32 block)
{
    NetStateBlock &state = *_blocks[block];
    MARLIN_ASSERT(state.References > 0);
    if (--state.References == 0)
    {
        _free.Add(block);
    }
}

const NetSnapshotEntry *NetSnapshot::Find(uint32 netId) const
{
    usize low = 0;
    usize high = Entities.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        if (Entities[middle].NetId < netId)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < Entities.Count() && Entities[low].NetId == netId ? &Entities[low] : nullptr;
}

NetSnapshotRing::~NetSnapshotRing() = default;

const NetSnapshot *NetSnapshotRing::Find(uint32 tick) const
{
    for (uint32 i = 0; i < _count; ++i)
    {
        if (_slots[i].Tick == tick)
        {
            return &_slots[i];
        }
    }
    return nullptr;
}

void NetSnapshotRing::Push(NetSnapshot &snapshot)
{
    NetSnapshot &slot = _slots[_next];
    if (_count == NetSnapshotHistory)
    {
        for (const NetSnapshotEntry &entry : slot.Entities)
        {
            Pool.Release(entry.Block);
        }
    }
    else
    {
        ++_count;
    }
    slot.Entities.Clear();
    Swap(slot.Entities, snapshot.Entities);
    slot.Tick = snapshot.Tick;
    _next = (_next + 1) % NetSnapshotHistory;
}

ServerReplicator::ServerReplicator(EntityManager &entities) : _entities(entities)
{
    _identities.Bind(entities);
}

ServerReplicator::~ServerReplicator() = default;

uint32 ServerReplicator::AddConnection()
{
    UniquePtr<Connection> &connection = _connections.Emplace(MakeUnique<Connection>());
    connection->Id = _nextConnection++;
    return connection->Id;
}

void ServerReplicator::RemoveConnection(uint32 connection)
{
    (void)_connections.RemoveAll(
        [connection](const UniquePtr<Connection> &candidate) { return candidate->Id == connection; });
}

ServerReplicator::Connection *ServerReplicator::FindConnection(uint32 connection)
{
    return const_cast<Connection *>(static_cast<const ServerReplicator *>(this)->FindConnection(connection));
}

const ServerReplicator::Connection *ServerReplicator::FindConnection(uint32 connection) const
{
    for (const UniquePtr<Connection> &candidate : _connections)
    {
        if (candidate->Id == connection)
        {
            return candidate.Get();
        }
    }
    return nullptr;
}

void ServerReplicator::SetViewer(uint32 connection, Entity viewer)
{
    if (Connection *found = FindConnection(connection))
    {
        found->Viewer = viewer;
    }
}

uint32 ServerReplicator::GetAcknowledgedTick(uint32 connection) const
{
    const Connection *found = FindConnection(connection);
    return found != nullptr ? found->Acknowledged : 0;
}

const ServerReplicator::Record *ServerReplicator::FindRecord(uint32 netId) const
{
    usize low = 0;
    usize high = _records.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        if (_records[middle].NetId < netId)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < _records.Count() && _records[low].NetId == netId ? &_records[low] : nullptr;
}

ServerReplicator::Record &ServerReplicator::FindOrAddRecord(uint32 netId)
{
    // New ids only grow, so a new record almost always goes at the end.
    if (_records.IsEmpty() || _records.Last().NetId < netId)
    {
        Record &added = _records.AddDefaulted();
        added.NetId = netId;
        return added;
    }
    usize low = 0;
    usize high = _records.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        if (_records[middle].NetId < netId)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low < _records.Count() && _records[low].NetId == netId)
    {
        return _records[low];
    }
    Record &added = _records.Insert(low);
    added.NetId = netId;
    return added;
}

Entity ServerReplicator::FindEntity(uint32 netId) const
{
    const Record *record = FindRecord(netId);
    return record != nullptr ? record->Target : Entity();
}

ServerReplicator::ArchetypeInfo &ServerReplicator::GetInfo(const Archetype &archetype)
{
    while (_archetypes.Count() <= archetype.GetIndex())
    {
        _archetypes.AddDefaulted();
    }
    ArchetypeInfo &info = _archetypes[archetype.GetIndex()];
    if (info.Known)
    {
        return info;
    }
    info.Known = true;
    info.Identity = archetype.FindColumn(Components::IdOf<NetworkIdentity>());
    info.Owner = archetype.FindColumn(Components::IdOf<NetOwner>());
    info.Relevancy = archetype.FindColumn(Components::IdOf<NetRelevancy>());
    info.Transform = archetype.FindColumn(Components::IdOf<WorldTransform>());
    for (const ReplicatedComponent *replicated : ReplicationSchema::GetAll())
    {
        if (!archetype.Has(replicated->Component))
        {
            continue;
        }
        const int32 column = archetype.FindColumn(replicated->Component);
        info.Replicated.Add({replicated, column});
        if (column >= 0)
        {
            info.StateColumns.Add(static_cast<uint32>(column));
        }
        for (const NetLeafField &field : replicated->Fields)
        {
            info.HasEntityLeaves = info.HasEntityLeaves || field.Encoding == NetFieldEncoding::Entity;
        }
    }
    for (const int32 column : {info.Identity, info.Owner})
    {
        if (column >= 0)
        {
            info.StateColumns.Add(static_cast<uint32>(column));
        }
    }
    for (const int32 column : {info.Relevancy, info.Transform})
    {
        if (column >= 0)
        {
            info.RelevancyColumns.Add(static_cast<uint32>(column));
        }
    }
    return info;
}

bool ServerReplicator::Capture(const ArchetypeInfo &info, const Chunk &chunk, uint32 row, Record &record)
{
    const Entity entity = chunk.GetEntities()[row];
    const Span<const ArchetypeColumn> columns = chunk.GetArchetype().GetColumns();
    _components.Clear();
    _leaves.Clear();
    for (const ArchetypeInfo::Column &column : info.Replicated)
    {
        const ReplicatedComponent &replicated = *column.Replicated;
        const void *value = nullptr;
        if (column.Index >= 0)
        {
            const auto index = static_cast<uint32>(column.Index);
            value = static_cast<const uint8 *>(chunk.GetColumnData(index)) +
                    static_cast<usize>(row) * columns[index].Type->GetSize();
        }
        else
        {
            value = _entities.GetComponent(entity, replicated.Component);
        }
        _components.Add(replicated.NetIndex);
        for (usize i = 0; i < replicated.Fields.Count(); ++i)
        {
            const NetLeafField &field = replicated.Fields[i];
            if (value == nullptr)
            {
                _leaves.Add(replicated.Defaults[i]);
                continue;
            }
            uint64 leaf = ReplicationSchema::ReadLeaf(field, value);
            if (field.Encoding == NetFieldEncoding::Entity)
            {
                const Entity target{static_cast<uint32>(leaf), static_cast<uint32>(leaf >> 32)};
                const NetworkIdentity *identity = _entities.Get<NetworkIdentity>(target);
                leaf = identity != nullptr ? identity->Id : 0;
            }
            _leaves.Add(leaf);
        }
    }
    const uint32 owner =
        info.Owner >= 0
            ? static_cast<const NetOwner *>(chunk.GetColumnData(static_cast<uint32>(info.Owner)))[row].Connection
            : 0;
    record.Target = entity;
    record.Location = &chunk.GetArchetype();
    const bool changed =
        owner != record.Owner || !SameValues(_components, record.Components) || !SameValues(_leaves, record.Leaves);
    if (changed)
    {
        record.Owner = owner;
        Assign(record.Components, _components);
        Assign(record.Leaves, _leaves);
    }
    return changed;
}

void ServerReplicator::CaptureRelevancy(const ArchetypeInfo &info, const Chunk &chunk, uint32 row, Record &record) const
{
    record.Range = 0;
    record.Priority = 1;
    if (info.Relevancy >= 0)
    {
        const NetRelevancy &relevancy =
            static_cast<const NetRelevancy *>(chunk.GetColumnData(static_cast<uint32>(info.Relevancy)))[row];
        record.Range = relevancy.Range;
        record.Priority = relevancy.Priority;
    }
    record.HasPosition = info.Transform >= 0;
    if (record.HasPosition)
    {
        record.Position =
            static_cast<const WorldTransform *>(chunk.GetColumnData(static_cast<uint32>(info.Transform)))[row]
                .Translation;
    }
}

void ServerReplicator::Update()
{
    // Writes since the last update are stamped with the version it started, or a later one.
    const bool first = _update == 0;
    const uint32 since = _captureVersion - 1;
    _captureVersion = _entities.AdvanceVersion();
    ++_update;
    const auto changedSince = [&](const Chunk &chunk, Span<const uint32> columns) {
        for (const uint32 column : columns)
        {
            if (first || IsNewerVersion(chunk.GetVersion(column), since))
            {
                return true;
            }
        }
        return false;
    };

    // Rows moved in an archetype whose structure changed, so all of its chunks are read again.
    const Span<Archetype *const> archetypes = _identities.GetArchetypes();
    for (const Archetype *archetype : archetypes)
    {
        const bool known = archetype->GetIndex() < _archetypes.Count() && _archetypes[archetype->GetIndex()].Known;
        ArchetypeInfo &info = GetInfo(*archetype);
        info.StructureChanged = !known || archetype->GetStructureVersion() != info.StructureVersion;
        info.StructureVersion = archetype->GetStructureVersion();
    }

    // New entities get their ids before anything is read, so references to them resolve.
    bool assigned = false;
    for (const Archetype *archetype : archetypes)
    {
        const ArchetypeInfo &info = _archetypes[archetype->GetIndex()];
        for (Chunk *chunk : archetype->GetChunks())
        {
            if (!info.StructureChanged && !changedSince(*chunk, info.StateColumns.AsSpan()))
            {
                continue;
            }
            auto *identities = static_cast<NetworkIdentity *>(chunk->GetColumnData(static_cast<uint32>(info.Identity)));
            for (uint32 row = 0; row < chunk->GetCount(); ++row)
            {
                if (identities[row].Id == 0)
                {
                    identities[row].Id = _nextNetId++;
                    chunk->SetVersion(static_cast<uint32>(info.Identity), _captureVersion);
                    assigned = true;
                }
            }
        }
    }

    // An entity can only have left an archetype whose structure changed: destroyed, moved or without an identity.
    const usize removed = _records.RemoveAll([&](const Record &record) {
        if (!_archetypes[record.Location->GetIndex()].StructureChanged)
        {
            return false;
        }
        const NetworkIdentity *identity = _entities.Get<NetworkIdentity>(record.Target);
        return identity == nullptr || identity->Id != record.NetId;
    });

    // Read what changed. An entity field holds an id, which changes when its target gets one or goes away.
    for (const Archetype *archetype : archetypes)
    {
        const ArchetypeInfo &info = _archetypes[archetype->GetIndex()];
        const bool all = info.StructureChanged || (info.HasEntityLeaves && (assigned || removed > 0));
        for (const Chunk *chunk : archetype->GetChunks())
        {
            const bool state = all || changedSince(*chunk, info.StateColumns.AsSpan());
            if (!state && !changedSince(*chunk, info.RelevancyColumns.AsSpan()))
            {
                continue;
            }
            const auto *identities =
                static_cast<const NetworkIdentity *>(chunk->GetColumnData(static_cast<uint32>(info.Identity)));
            for (uint32 row = 0; row < chunk->GetCount(); ++row)
            {
                Record &record = FindOrAddRecord(identities[row].Id);
                const bool added = record.Seen == 0;
                record.Seen = _update;
                if (((state || added) && Capture(info, *chunk, row, record)) || added)
                {
                    record.Changed = _update;
                }
                CaptureRelevancy(info, *chunk, row, record);
            }
        }
    }
}

bool ServerReplicator::IsRelevant(const Record &record, uint32 connection, const WorldPosition *viewer) const
{
    if (record.Owner != 0 && record.Owner == connection)
    {
        return true;
    }
    if (record.Range <= 0 || viewer == nullptr || !record.HasPosition)
    {
        return true;
    }
    const float64 x = record.Position.X - viewer->X;
    const float64 y = record.Position.Y - viewer->Y;
    const float64 z = record.Position.Z - viewer->Z;
    const float64 range = record.Range;
    return x * x + y * y + z * z <= range * range;
}

uint32 ServerReplicator::WriteEntityDelta(NetBitWriter &writer, const Record &record, bool owned, NetStatePool &pool,
                                          const NetStateBlock *base)
{
    const uint32 block = pool.Allocate();
    NetStateBlock &next = pool.Get(block);
    next.Components.AddRange(record.Components.AsSpan());
    bool written = base == nullptr;
    writer.WriteVarUInt(record.NetId);
    usize leaf = 0;
    usize baseIndex = 0;
    usize baseLeaf = 0;
    for (const uint32 netIndex : record.Components)
    {
        const ReplicatedComponent &replicated = *ReplicationSchema::FromNetIndex(netIndex);
        const usize count = replicated.Fields.Count();
        const uint64 *old = nullptr;
        if (base != nullptr)
        {
            for (; baseIndex < base->Components.Count() && base->Components[baseIndex] < netIndex; ++baseIndex)
            {
                baseLeaf += LeafCount(base->Components[baseIndex]);
            }
            if (baseIndex < base->Components.Count() && base->Components[baseIndex] == netIndex)
            {
                old = base->Leaves.Data() + baseLeaf;
            }
        }
        // What the client will hold: OwnerOnly leaves hidden from others, InitialOnly ones kept once it has them.
        const usize first = next.Leaves.Count();
        bool changed = old == nullptr;
        for (usize i = 0; i < count; ++i)
        {
            const NetLeafField &field = replicated.Fields[i];
            uint64 value = field.OwnerOnly && !owned ? replicated.Defaults[i] : record.Leaves[leaf + i];
            if (old != nullptr && field.InitialOnly)
            {
                value = old[i];
            }
            next.Leaves.Add(value);
            changed = changed || value != (old != nullptr ? old[i] : replicated.Defaults[i]);
        }
        leaf += count;
        if (!changed)
        {
            continue;
        }
        writer.WriteBool(true);
        writer.WriteVarUInt(netIndex);
        writer.WriteBool(false);
        for (usize i = 0; i < count; ++i)
        {
            const uint64 value = next.Leaves[first + i];
            const bool different = value != (old != nullptr ? old[i] : replicated.Defaults[i]);
            writer.WriteBool(different);
            if (different)
            {
                WriteValue(writer, replicated.Fields[i], value);
            }
        }
        written = true;
    }
    if (base != nullptr)
    {
        usize index = 0;
        for (const uint32 netIndex : base->Components)
        {
            while (index < record.Components.Count() && record.Components[index] < netIndex)
            {
                ++index;
            }
            if (index == record.Components.Count() || record.Components[index] != netIndex)
            {
                writer.WriteBool(true);
                writer.WriteVarUInt(netIndex);
                writer.WriteBool(true);
                written = true;
            }
        }
    }
    writer.WriteBool(false);
    if (!written)
    {
        pool.Release(block);
        return ~0u;
    }
    return block;
}

void ServerReplicator::WriteSnapshot(uint32 connection, uint32 tick, uint32 budget, List<uint8> &message)
{
    message.Clear();
    Connection *found = FindConnection(connection);
    MARLIN_ASSERT(found != nullptr);
    if (found == nullptr)
    {
        return;
    }
    Connection &state = *found;
    NetStatePool &pool = state.History.Pool;
    const NetSnapshot *base = state.Acknowledged != 0 ? state.History.Find(state.Acknowledged) : nullptr;
    const Span<const NetSnapshotEntry> baseEntries =
        base != nullptr ? base->Entities.AsSpan() : Span<const NetSnapshotEntry>();
    const WorldTransform *viewerTransform = _entities.Get<WorldTransform>(state.Viewer);
    const WorldPosition *viewer = viewerTransform != nullptr ? &viewerTransform->Translation : nullptr;

    // RECOVERY-DECISION D4: each tick a relevant entity's priority grows by its NetRelevancy.Priority (1 without
    // one) and drops to zero when it is sent or has nothing new; the budget takes entities in priority order, then by
    // network id. The records, the last priorities and the base are all in NetId order, so one walk joins them.
    List<Relevant> &candidates = state.Candidates;
    candidates.Clear();
    usize priorityIndex = 0;
    usize baseIndex = 0;
    for (uint32 i = 0; i < _records.Count(); ++i)
    {
        const Record &record = _records[i];
        if (!IsRelevant(record, connection, viewer))
        {
            continue;
        }
        while (priorityIndex < state.Priorities.Count() && state.Priorities[priorityIndex].NetId < record.NetId)
        {
            ++priorityIndex;
        }
        while (baseIndex < baseEntries.Count() && baseEntries[baseIndex].NetId < record.NetId)
        {
            ++baseIndex;
        }
        Relevant &relevant = candidates.AddDefaulted();
        relevant.Record = i;
        relevant.Priority = record.Priority;
        if (priorityIndex < state.Priorities.Count() && state.Priorities[priorityIndex].NetId == record.NetId)
        {
            relevant.Priority += state.Priorities[priorityIndex].Priority;
        }
        if (baseIndex < baseEntries.Count() && baseEntries[baseIndex].NetId == record.NetId)
        {
            relevant.BaseEntry = static_cast<uint32>(baseIndex);
            relevant.Current = record.Changed <= baseEntries[baseIndex].Captured;
        }
    }
    state.Order.Clear();
    for (uint32 i = 0; i < candidates.Count(); ++i)
    {
        if (!candidates[i].Current)
        {
            state.Order.Add(i);
        }
    }
    state.Order.Sort([&](uint32 a, uint32 b) {
        return candidates[a].Priority != candidates[b].Priority ? candidates[a].Priority > candidates[b].Priority
                                                                : a < b;
    });

    NetBitWriter writer(message);
    writer.WriteBits(static_cast<uint64>(NetMessageKind::Snapshot), 8);
    writer.WriteVarUInt(tick);
    writer.WriteVarUInt(base != nullptr ? base->Tick : 0);
    bool first = true;
    for (const uint32 index : state.Order)
    {
        // An entity takes at least two bytes and the end of the list one bit more, so past this nothing fits.
        if (!first && writer.GetByteCount() + 3 > budget)
        {
            break;
        }
        Relevant &relevant = candidates[index];
        const Record &record = _records[relevant.Record];
        const NetStateBlock *old =
            relevant.BaseEntry != ~0u ? &pool.Get(baseEntries[relevant.BaseEntry].Block) : nullptr;
        state.EntityBits.Clear();
        NetBitWriter entityWriter(state.EntityBits);
        const uint32 block = WriteEntityDelta(entityWriter, record, record.Owner == connection, pool, old);
        if (block == ~0u)
        {
            relevant.Current = true;
            continue;
        }
        // RECOVERY-DECISION D9: an entity that does not fit is skipped for smaller ones behind it; the first always
        // goes, so one entity bigger than the budget cannot stall the queue.
        if (!first && writer.GetByteCount() + entityWriter.GetByteCount() + 1 > budget)
        {
            pool.Release(block);
            continue;
        }
        first = false;
        writer.WriteBool(true);
        writer.WriteBits(state.EntityBits.AsSpan(), entityWriter.GetBitCount());
        relevant.Sent = block;
        relevant.Current = true;
    }
    writer.WriteBool(false);

    // The client keeps what the base holds of the entities still relevant and removes the rest.
    NetSnapshot &next = state.Next;
    next.Tick = tick;
    next.Entities.Clear();
    state.Removed.Clear();
    state.NextPriorities.Clear();
    baseIndex = 0;
    for (const Relevant &relevant : candidates)
    {
        const uint32 netId = _records[relevant.Record].NetId;
        for (; baseIndex < baseEntries.Count() && baseEntries[baseIndex].NetId < netId; ++baseIndex)
        {
            state.Removed.Add(baseEntries[baseIndex].NetId);
        }
        if (baseIndex < baseEntries.Count() && baseEntries[baseIndex].NetId == netId)
        {
            ++baseIndex;
        }
        if (relevant.Sent != ~0u)
        {
            next.Entities.Add({netId, relevant.Sent, _update});
        }
        else if (relevant.BaseEntry != ~0u)
        {
            const NetSnapshotEntry &kept = baseEntries[relevant.BaseEntry];
            pool.AddReference(kept.Block);
            next.Entities.Add({netId, kept.Block, relevant.Current ? _update : kept.Captured});
        }
        state.NextPriorities.Add({netId, relevant.Current ? 0.0 : relevant.Priority});
    }
    for (; baseIndex < baseEntries.Count(); ++baseIndex)
    {
        state.Removed.Add(baseEntries[baseIndex].NetId);
    }
    for (const uint32 netId : state.Removed)
    {
        writer.WriteBool(true);
        writer.WriteVarUInt(netId);
    }
    writer.WriteBool(false);
    Swap(state.Priorities, state.NextPriorities);
    state.History.Push(next);
}

void ServerReplicator::Receive(uint32 connection, Span<const uint8> message)
{
    Connection *found = FindConnection(connection);
    NetBitReader reader(message);
    if (found == nullptr || reader.ReadBits(8) != static_cast<uint64>(NetMessageKind::Acknowledge))
    {
        return;
    }
    const auto tick = static_cast<uint32>(reader.ReadVarUInt());
    if (reader.HasOverflowed() || (found->Acknowledged != 0 && !IsNewerVersion(tick, found->Acknowledged)))
    {
        return;
    }
    // Only a snapshot still in the history can serve as a base.
    if (tick != 0 && found->History.Find(tick) != nullptr)
    {
        found->Acknowledged = tick;
    }
}

ClientReplicator::ClientReplicator(EntityManager &entities) : _entities(entities)
{
}

Entity ClientReplicator::FindEntity(uint32 netId) const
{
    const Entity *entity = _byNetId.Find(netId);
    return entity != nullptr && _entities.Exists(*entity) ? *entity : Entity();
}

bool ClientReplicator::Decode(NetBitReader &reader, const NetSnapshot *base)
{
    NetStatePool &pool = _history.Pool;
    while (reader.ReadBool())
    {
        const auto netId = static_cast<uint32>(reader.ReadVarUInt());
        const uint32 block = pool.Allocate();
        _updated.Add({netId, block, 0});
        NetStateBlock &state = pool.Get(block);
        if (const NetSnapshotEntry *old = base != nullptr ? base->Find(netId) : nullptr)
        {
            const NetStateBlock &previous = pool.Get(old->Block);
            state.Components.AddRange(previous.Components.AsSpan());
            state.Leaves.AddRange(previous.Leaves.AsSpan());
        }
        while (reader.ReadBool())
        {
            const auto netIndex = static_cast<uint32>(reader.ReadVarUInt());
            const ReplicatedComponent *replicated = ReplicationSchema::FromNetIndex(netIndex);
            if (replicated == nullptr || reader.HasOverflowed())
            {
                return false;
            }
            usize index = 0;
            usize offset = 0;
            for (; index < state.Components.Count() && state.Components[index] < netIndex; ++index)
            {
                offset += LeafCount(state.Components[index]);
            }
            const usize count = replicated->Fields.Count();
            const bool exists = index < state.Components.Count() && state.Components[index] == netIndex;
            if (reader.ReadBool())
            {
                if (exists)
                {
                    state.Components.RemoveAt(index);
                    state.Leaves.RemoveAt(offset, count);
                }
                continue;
            }
            if (!exists)
            {
                state.Components.Insert(index, netIndex);
                for (usize i = 0; i < count; ++i)
                {
                    state.Leaves.Insert(offset + i, replicated->Defaults[i]);
                }
            }
            for (usize i = 0; i < count; ++i)
            {
                if (reader.ReadBool())
                {
                    state.Leaves[offset + i] = ReadValue(reader, replicated->Fields[i]);
                }
            }
        }
        if (reader.HasOverflowed())
        {
            return false;
        }
    }
    while (reader.ReadBool())
    {
        _removed.Add(static_cast<uint32>(reader.ReadVarUInt()));
    }
    return !reader.HasOverflowed();
}

bool ClientReplicator::Receive(Span<const uint8> message, List<uint8> &acknowledgement)
{
    NetBitReader reader(message);
    if (reader.ReadBits(8) != static_cast<uint64>(NetMessageKind::Snapshot))
    {
        return false;
    }
    const auto tick = static_cast<uint32>(reader.ReadVarUInt());
    const auto baseTick = static_cast<uint32>(reader.ReadVarUInt());
    if (reader.HasOverflowed() || tick == 0 || (_lastTick != 0 && !IsNewerVersion(tick, _lastTick)))
    {
        return false;
    }
    const NetSnapshot *base = nullptr;
    if (baseTick != 0)
    {
        base = _history.Find(baseTick);
        if (base == nullptr)
        {
            // A base the client never had or already dropped: wait for one against an acknowledged snapshot.
            return false;
        }
    }

    NetStatePool &pool = _history.Pool;
    _updated.Clear();
    _removed.Clear();
    bool valid = Decode(reader, base);
    _updated.Sort([](const NetSnapshotEntry &a, const NetSnapshotEntry &b) { return a.NetId < b.NetId; });
    _removed.Sort();
    for (usize i = 1; i < _updated.Count() && valid; ++i)
    {
        valid = _updated[i - 1].NetId != _updated[i].NetId;
    }
    if (!valid)
    {
        for (const NetSnapshotEntry &entry : _updated)
        {
            pool.Release(entry.Block);
        }
        return false;
    }
    // A removal wins over an update of the same entity.
    usize removedIndex = 0;
    const auto isRemoved = [&](uint32 netId) {
        while (removedIndex < _removed.Count() && _removed[removedIndex] < netId)
        {
            ++removedIndex;
        }
        return removedIndex < _removed.Count() && _removed[removedIndex] == netId;
    };
    (void)_updated.RemoveAll([&](const NetSnapshotEntry &entry) {
        if (!isRemoved(entry.NetId))
        {
            return false;
        }
        pool.Release(entry.Block);
        return true;
    });

    // The new snapshot: the base's entities, with the updated ones replaced and the removed ones gone.
    _next.Tick = tick;
    _next.Entities.Clear();
    const Span<const NetSnapshotEntry> baseEntries =
        base != nullptr ? base->Entities.AsSpan() : Span<const NetSnapshotEntry>();
    usize updatedIndex = 0;
    removedIndex = 0;
    for (const NetSnapshotEntry &entry : baseEntries)
    {
        for (; updatedIndex < _updated.Count() && _updated[updatedIndex].NetId < entry.NetId; ++updatedIndex)
        {
            _next.Entities.Add(_updated[updatedIndex]);
        }
        if (updatedIndex < _updated.Count() && _updated[updatedIndex].NetId == entry.NetId)
        {
            _next.Entities.Add(_updated[updatedIndex++]);
            continue;
        }
        if (!isRemoved(entry.NetId))
        {
            pool.AddReference(entry.Block);
            _next.Entities.Add(entry);
        }
    }
    for (; updatedIndex < _updated.Count(); ++updatedIndex)
    {
        _next.Entities.Add(_updated[updatedIndex]);
    }
    _history.Push(_next);
    _lastTick = tick;
    Apply();

    acknowledgement.Clear();
    NetBitWriter writer(acknowledgement);
    writer.WriteBits(static_cast<uint64>(NetMessageKind::Acknowledge), 8);
    writer.WriteVarUInt(tick);
    return true;
}

void ClientReplicator::Apply()
{
    const NetStatePool &pool = _history.Pool;
    for (const uint32 netId : _removed)
    {
        if (const Entity *entity = _byNetId.Find(netId))
        {
            _entities.DestroyEntity(*entity);
        }
        (void)_byNetId.Remove(netId);
    }
    // Create the new entities first, with all their components, so that entity fields can refer to any of them.
    for (const NetSnapshotEntry &entry : _updated)
    {
        if (!FindEntity(entry.NetId).IsNull())
        {
            continue;
        }
        _components.Clear();
        _components.Add(Components::IdOf<NetworkIdentity>());
        for (const uint32 netIndex : pool.Get(entry.Block).Components)
        {
            _components.Add(ReplicationSchema::FromNetIndex(netIndex)->Component);
        }
        const Entity entity = _entities.CreateEntity(_components.AsSpan());
        _entities.GetForWrite<NetworkIdentity>(entity)->Id = entry.NetId;
        _byNetId.Set(entry.NetId, entity);
    }
    for (const NetSnapshotEntry &entry : _updated)
    {
        const NetStateBlock &state = pool.Get(entry.Block);
        const Entity entity = FindEntity(entry.NetId);
        Archetype *current = _entities.GetArchetype(entity);
        if (current == nullptr)
        {
            continue;
        }
        // One move to the archetype with exactly the replicated components the snapshot has.
        Archetype *target = current;
        for (const ComponentId component : current->GetComponents())
        {
            const ReplicatedComponent *replicated = ReplicationSchema::Find(component);
            if (replicated != nullptr && !state.Components.Contains(replicated->NetIndex))
            {
                target = &_entities.GetArchetypeWithout(*target, component);
            }
        }
        for (const uint32 netIndex : state.Components)
        {
            target = &_entities.GetArchetypeWith(*target, ReplicationSchema::FromNetIndex(netIndex)->Component);
        }
        if (target != current)
        {
            _entities.SetArchetype(entity, *target);
        }
        usize offset = 0;
        for (const uint32 netIndex : state.Components)
        {
            const ReplicatedComponent &replicated = *ReplicationSchema::FromNetIndex(netIndex);
            void *value = _entities.GetComponentForWrite(entity, replicated.Component);
            for (usize i = 0; value != nullptr && i < replicated.Fields.Count(); ++i)
            {
                uint64 leaf = state.Leaves[offset + i];
                if (replicated.Fields[i].Encoding == NetFieldEncoding::Entity)
                {
                    const Entity referenced = FindEntity(static_cast<uint32>(leaf));
                    leaf = static_cast<uint64>(referenced.Index) | static_cast<uint64>(referenced.Generation) << 32;
                }
                ReplicationSchema::WriteLeaf(replicated.Fields[i], value, leaf);
            }
            offset += replicated.Fields.Count();
        }
    }
}
} // namespace Marlin
