#include "World/EntityManager.h"

#include "Diagnostics/AssertFormat.h"
#include "Memory/Allocator.h"
#include "Memory/Memory.h"
#include "Reflection/Reflection.h"

namespace Marlin
{
namespace
{
constexpr uint32 MaxSharedComponents = 8;

using SharedIndices = List<uint32, InlineAllocator<MaxSharedComponents>>;

uint32 AlignUp(uint32 value, uint32 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool GetBit(const uint64 *bits, uint32 index)
{
    return (bits[index / 64] >> (index % 64) & 1) != 0;
}

void SetBit(uint64 *bits, uint32 index, bool value)
{
    const uint64 mask = uint64{1} << (index % 64);
    bits[index / 64] = value ? bits[index / 64] | mask : bits[index / 64] & ~mask;
}

uint64 TypeIdOf(ComponentId component)
{
    return Components::Get(component).GetTypeInfo().GetId();
}

[[maybe_unused]] bool SameComponents(Span<const ComponentId> a, Span<const ComponentId> b)
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

uint64 HashValue(const TypeInfo &type, const void *value)
{
    const TypeOperations &operations = type.GetOperations();
    return operations.Hash != nullptr ? operations.Hash(value) : RapidHash(value, type.GetSize());
}
} // namespace

EntityManager::EntityManager()
{
    // Archetype 0 is the empty one, so GetEmptyArchetype never fails.
    (void)GetOrCreateArchetype({});
}

EntityManager::~EntityManager()
{
    for (Archetype *archetype : _archetypes)
    {
        for (Chunk *chunk : archetype->_chunks)
        {
            for (uint32 i = 0; i < archetype->_columns.Count(); ++i)
            {
                archetype->_columns[i].Type->Destruct(chunk->GetColumnData(i), chunk->_count);
            }
            for (uint32 i = 0; i < archetype->_chunkComponents.Count(); ++i)
            {
                archetype->_chunkComponents[i].Type->Destruct(chunk->GetChunkComponent(i), 1);
            }
            DestroyAt(chunk);
            Memory::Free(chunk);
        }
        if (archetype->_spareChunk != nullptr)
        {
            DestroyAt(archetype->_spareChunk);
            Memory::Free(archetype->_spareChunk);
        }
        delete archetype;
    }
    for (auto &[component, store] : _sharedStores)
    {
        const ComponentType &type = Components::Get(component);
        for (SharedValue &value : store.Values)
        {
            if (value.Data != nullptr)
            {
                type.Destruct(value.Data, 1);
                Memory::Free(value.Data);
            }
        }
    }
}

Archetype &EntityManager::CreateArchetype(Span<const ComponentId> sorted, uint64 hash)
{
    auto *archetype = new Archetype();
    archetype->_components.AddRange(sorted);
    archetype->_hash = hash;
    archetype->_index = static_cast<uint32>(_archetypes.Count());
    for (const ComponentId component : sorted)
    {
        const ComponentType &type = Components::Get(component);
        Archetype::Slot slot;
        slot.Component = component;
        if (type.IsShared())
        {
            slot.Shared = static_cast<int16>(archetype->_shared.Count());
            archetype->_shared.Add({&type, 0});
        }
        else if (type.IsChunkComponent())
        {
            slot.ChunkValue = static_cast<int16>(archetype->_chunkComponents.Count());
            archetype->_chunkComponents.Add({&type, 0});
        }
        else if (type.HasColumn())
        {
            slot.Column = static_cast<int16>(archetype->_columns.Count());
            archetype->_columns.Add({&type, 0, 0});
            archetype->_hasEnableable = archetype->_hasEnableable || type.IsEnableable();
        }
        archetype->_slots.Add(slot);
    }
    MARLIN_ASSERTF(archetype->_shared.Count() <= MaxSharedComponents,
                   "An archetype has more than {0} shared components", MaxSharedComponents);

    // The per-chunk part: the header, shared value indices, chunk components and the column versions.
    uint32 offset = AlignUp(sizeof(Chunk), 8);
    for (ArchetypeChunkValue &value : archetype->_shared)
    {
        value.Offset = offset;
        offset += sizeof(uint32);
    }
    for (ArchetypeChunkValue &value : archetype->_chunkComponents)
    {
        MARLIN_ASSERT(value.Type->GetAlignment() <= ChunkAlignment);
        offset = AlignUp(offset, value.Type->GetAlignment());
        value.Offset = offset;
        offset += value.Type->GetSize();
    }
    offset = AlignUp(offset, alignof(uint32));
    archetype->_versionsOffset = offset;
    offset += static_cast<uint32>(archetype->_columns.Count() * sizeof(uint32));
    const uint32 fixed = offset;

    // The per-row part: the entities, then each column and its enabled bits.
    const auto layout = [&](uint32 capacity) {
        uint32 end = AlignUp(fixed, alignof(Entity));
        archetype->_entitiesOffset = end;
        end += capacity * static_cast<uint32>(sizeof(Entity));
        for (ArchetypeColumn &column : archetype->_columns)
        {
            MARLIN_ASSERT(column.Type->GetAlignment() <= ChunkAlignment);
            end = AlignUp(end, column.Type->GetAlignment());
            column.Offset = end;
            end += capacity * column.Type->GetSize();
            column.EnabledOffset = 0;
            if (column.Type->IsEnableable())
            {
                end = AlignUp(end, alignof(uint64));
                column.EnabledOffset = end;
                end += (capacity + 63) / 64 * static_cast<uint32>(sizeof(uint64));
            }
        }
        return end;
    };
    uint32 rowSize = sizeof(Entity);
    for (const ArchetypeColumn &column : archetype->_columns)
    {
        rowSize += column.Type->GetSize();
    }
    uint32 capacity = fixed < ChunkSize ? (ChunkSize - fixed) / rowSize : 0;
    while (capacity > 0 && layout(capacity) > ChunkSize)
    {
        --capacity;
    }
    MARLIN_ASSERTF(capacity > 0, "The components of an archetype do not fit in a {0}-byte chunk", ChunkSize);
    (void)layout(capacity);
    archetype->_capacity = capacity;

    _archetypes.Add(archetype);
    _archetypesByHash.Add(hash, archetype);
    return *archetype;
}

Archetype &EntityManager::GetOrCreateArchetype(Span<const ComponentId> components)
{
    List<ComponentId, InlineAllocator<16>> sorted;
    for (const ComponentId component : components)
    {
        if (!sorted.Contains(component))
        {
            sorted.Add(component);
        }
    }
    sorted.Sort([](ComponentId a, ComponentId b) { return TypeIdOf(a) < TypeIdOf(b); });
    uint64 hash = HashInteger(sorted.Count());
    for (const ComponentId component : sorted)
    {
        hash = HashCombine(hash, TypeIdOf(component));
    }
    if (Archetype *const *found = _archetypesByHash.Find(hash))
    {
        MARLIN_ASSERTF(SameComponents((*found)->_components.AsSpan(), sorted.AsSpan()), "Archetype hash collision");
        return **found;
    }
    return CreateArchetype(sorted.AsSpan(), hash);
}

Archetype &EntityManager::GetArchetypeWith(Archetype &archetype, ComponentId component)
{
    if (archetype.Has(component))
    {
        return archetype;
    }
    if (Archetype *const *edge = archetype._addEdges.Find(component))
    {
        return **edge;
    }
    List<ComponentId, InlineAllocator<16>> components;
    components.AddRange(archetype._components.AsSpan());
    components.Add(component);
    Archetype &target = GetOrCreateArchetype(components.AsSpan());
    archetype._addEdges.Add(component, &target);
    (void)target._removeEdges.TryAdd(component, &archetype);
    return target;
}

Archetype &EntityManager::GetArchetypeWithout(Archetype &archetype, ComponentId component)
{
    if (!archetype.Has(component))
    {
        return archetype;
    }
    if (Archetype *const *edge = archetype._removeEdges.Find(component))
    {
        return **edge;
    }
    List<ComponentId, InlineAllocator<16>> components;
    for (const ComponentId existing : archetype._components)
    {
        if (existing != component)
        {
            components.Add(existing);
        }
    }
    Archetype &target = GetOrCreateArchetype(components.AsSpan());
    archetype._removeEdges.Add(component, &target);
    (void)target._addEdges.TryAdd(component, &archetype);
    return target;
}

const EntityManager::EntitySlot *EntityManager::FindSlot(Entity entity) const
{
    if (entity.Index >= _slots.Count())
    {
        return nullptr;
    }
    const EntitySlot &slot = _slots[entity.Index];
    return slot.Chunk != nullptr && slot.Generation == entity.Generation ? &slot : nullptr;
}

bool EntityManager::Exists(Entity entity) const
{
    return FindSlot(entity) != nullptr;
}

EntityLocation EntityManager::GetLocation(Entity entity) const
{
    const EntitySlot *slot = FindSlot(entity);
    return slot != nullptr ? EntityLocation{slot->Chunk, slot->Row} : EntityLocation{};
}

Archetype *EntityManager::GetArchetype(Entity entity) const
{
    const EntitySlot *slot = FindSlot(entity);
    return slot != nullptr ? slot->Chunk->_archetype : nullptr;
}

void EntityManager::CheckStructuralChange() const
{
    MARLIN_ASSERTF(_structureLocks == 0, "Structural change while systems run: record it in a CommandBuffer");
}

Entity EntityManager::AllocateEntity()
{
    uint32 index = 0;
    if (!_freeSlots.IsEmpty())
    {
        index = _freeSlots.Pop();
    }
    else
    {
        index = static_cast<uint32>(_slots.Count());
        _slots.AddDefaulted();
    }
    ++_aliveCount;
    return {index, _slots[index].Generation};
}

EntityManager::SharedStore &EntityManager::GetSharedStore(ComponentId component)
{
    return _sharedStores.FindOrAdd(component);
}

const void *EntityManager::GetSharedValue(ComponentId component, uint32 index) const
{
    const SharedStore *store = _sharedStores.Find(component);
    if (store == nullptr || index >= store->Values.Count())
    {
        return nullptr;
    }
    return store->Values[index].Data;
}

uint32 EntityManager::FindOrAddSharedValue(ComponentId component, const void *value)
{
    const ComponentType &type = Components::Get(component);
    MARLIN_ASSERT(type.IsShared());
    SharedStore &store = GetSharedStore(component);
    const uint64 hash = HashValue(type.GetTypeInfo(), value);
    for (uint32 i = 0; i < store.Values.Count(); ++i)
    {
        const SharedValue &existing = store.Values[i];
        if (existing.Data != nullptr && existing.Hash == hash &&
            Reflection::Equals(type.GetTypeInfo(), existing.Data, value))
        {
            return i;
        }
    }
    SharedValue added;
    added.Data = Memory::Allocate(type.GetSize() > 0 ? type.GetSize() : 1, type.GetAlignment());
    added.Hash = hash;
    type.CopyConstruct(added.Data, value, 1);
    if (!store.FreeValues.IsEmpty())
    {
        const uint32 index = store.FreeValues.Pop();
        store.Values[index] = added;
        return index;
    }
    store.Values.Add(added);
    return static_cast<uint32>(store.Values.Count() - 1);
}

void EntityManager::ReleaseSharedValue(ComponentId component, uint32 index)
{
    SharedStore &store = GetSharedStore(component);
    SharedValue &value = store.Values[index];
    MARLIN_ASSERT(value.References > 0);
    if (--value.References > 0)
    {
        return;
    }
    Components::Get(component).Destruct(value.Data, 1);
    Memory::Free(value.Data);
    value = {};
    store.FreeValues.Add(index);
}

Chunk &EntityManager::AllocateChunk(Archetype &archetype, Span<const uint32> shared)
{
    MARLIN_ASSERT(shared.Count() == archetype._shared.Count());
    Chunk *chunk = archetype._spareChunk;
    archetype._spareChunk = nullptr;
    if (chunk == nullptr)
    {
        chunk = ConstructAt(static_cast<Chunk *>(Memory::Allocate(ChunkSize, ChunkAlignment)));
    }
    chunk->_archetype = &archetype;
    chunk->_index = static_cast<uint32>(archetype._chunks.Count());
    archetype._chunks.Add(chunk);
    for (uint32 i = 0; i < archetype._shared.Count(); ++i)
    {
        *reinterpret_cast<uint32 *>(chunk->GetBytes() + archetype._shared[i].Offset) = shared[i];
        ++GetSharedStore(archetype._shared[i].Type->GetId()).Values[shared[i]].References;
    }
    for (uint32 i = 0; i < archetype._chunkComponents.Count(); ++i)
    {
        archetype._chunkComponents[i].Type->Construct(chunk->GetChunkComponent(i), 1);
    }
    const uint32 version = GetVersion();
    for (uint32 i = 0; i < archetype._columns.Count(); ++i)
    {
        chunk->SetVersion(i, version);
    }
    AddToFreeList(*chunk);
    return *chunk;
}

void EntityManager::AddToFreeList(Chunk &chunk)
{
    Archetype &archetype = *chunk._archetype;
    chunk._freeIndex = static_cast<uint32>(archetype._freeChunks.Count());
    chunk._inFreeList = true;
    archetype._freeChunks.Add(&chunk);
}

void EntityManager::RemoveFromFreeList(Chunk &chunk)
{
    Archetype &archetype = *chunk._archetype;
    MARLIN_ASSERT(chunk._inFreeList && archetype._freeChunks[chunk._freeIndex] == &chunk);
    archetype._freeChunks.RemoveAtSwap(chunk._freeIndex);
    if (chunk._freeIndex < archetype._freeChunks.Count())
    {
        archetype._freeChunks[chunk._freeIndex]->_freeIndex = chunk._freeIndex;
    }
    chunk._inFreeList = false;
}

void EntityManager::FreeChunk(Chunk &chunk)
{
    Archetype &archetype = *chunk._archetype;
    for (uint32 i = 0; i < archetype._chunkComponents.Count(); ++i)
    {
        archetype._chunkComponents[i].Type->Destruct(chunk.GetChunkComponent(i), 1);
    }
    for (uint32 i = 0; i < archetype._shared.Count(); ++i)
    {
        ReleaseSharedValue(archetype._shared[i].Type->GetId(), chunk.GetSharedValueIndex(i));
    }
    if (chunk._inFreeList)
    {
        RemoveFromFreeList(chunk);
    }
    const uint32 index = chunk._index;
    archetype._chunks.RemoveAtSwap(index);
    if (index < archetype._chunks.Count())
    {
        archetype._chunks[index]->_index = index;
    }
    if (archetype._shared.IsEmpty() && archetype._spareChunk == nullptr)
    {
        archetype._spareChunk = &chunk;
        return;
    }
    DestroyAt(&chunk);
    Memory::Free(&chunk);
}

Chunk &EntityManager::FindChunkWithRoom(Archetype &archetype, Span<const uint32> shared)
{
    // Without shared components any chunk with room will do: the most recent, which is the most likely to be warm.
    if (shared.IsEmpty() && !archetype._freeChunks.IsEmpty())
    {
        return *archetype._freeChunks.Last();
    }
    for (Chunk *chunk : archetype._freeChunks)
    {
        bool same = true;
        for (uint32 i = 0; i < shared.Count() && same; ++i)
        {
            same = chunk->GetSharedValueIndex(i) == shared[i];
        }
        if (same)
        {
            return *chunk;
        }
    }
    return AllocateChunk(archetype, shared);
}

EntityLocation EntityManager::AllocateRow(Archetype &archetype, Span<const uint32> shared)
{
    Chunk &chunk = FindChunkWithRoom(archetype, shared);
    TouchStructure(archetype);
    const uint32 row = chunk._count++;
    if (chunk.IsFull())
    {
        RemoveFromFreeList(chunk);
    }
    const uint32 version = GetVersion();
    for (uint32 i = 0; i < archetype._columns.Count(); ++i)
    {
        chunk.SetVersion(i, version);
        if (uint64 *bits = chunk.GetEnabledBits(i))
        {
            SetBit(bits, row, true);
        }
    }
    return {&chunk, row};
}

void EntityManager::RemoveRow(Chunk &chunk, uint32 row)
{
    MARLIN_ASSERT(chunk._count > 0);
    Archetype &archetype = *chunk._archetype;
    TouchStructure(archetype);
    const uint32 last = chunk._count - 1;
    if (row != last)
    {
        auto *entities = reinterpret_cast<Entity *>(chunk.GetBytes() + archetype._entitiesOffset);
        for (uint32 i = 0; i < archetype._columns.Count(); ++i)
        {
            const ComponentType &type = *archetype._columns[i].Type;
            auto *data = static_cast<uint8 *>(chunk.GetColumnData(i));
            type.Relocate(data + static_cast<usize>(row) * type.GetSize(),
                          data + static_cast<usize>(last) * type.GetSize(), 1);
            if (uint64 *bits = chunk.GetEnabledBits(i))
            {
                SetBit(bits, row, GetBit(bits, last));
            }
        }
        entities[row] = entities[last];
        _slots[entities[row].Index].Row = row;
    }
    chunk._count = last;
    if (chunk._count == 0)
    {
        FreeChunk(chunk);
    }
    else if (!chunk._inFreeList)
    {
        AddToFreeList(chunk);
    }
}

Entity EntityManager::CreateEntity(Span<const ComponentId> components)
{
    return CreateEntity(GetOrCreateArchetype(components));
}

Entity EntityManager::CreateEntity(Archetype &archetype)
{
    Entity entity;
    CreateEntities(archetype, Span<Entity>(&entity, 1));
    return entity;
}

void EntityManager::CreateEntities(Archetype &archetype, Span<Entity> entities)
{
    SharedIndices shared;
    for (const ArchetypeChunkValue &value : archetype._shared)
    {
        const ReflectedValue initial(value.Type->GetTypeInfo());
        shared.Add(FindOrAddSharedValue(value.Type->GetId(), initial.Get()));
    }
    CreateEntities(archetype, entities, shared.AsSpan());
}

void EntityManager::CreateEntities(Archetype &archetype, Span<Entity> entities, Span<const uint32> shared)
{
    CheckStructuralChange();
    MARLIN_ASSERT(shared.Count() == archetype._shared.Count());
    usize created = 0;
    while (created < entities.Count())
    {
        // Fill a chunk at a time, constructing each column's new rows in one call.
        Chunk &chunk = FindChunkWithRoom(archetype, shared);
        TouchStructure(archetype);
        const uint32 first = chunk._count;
        const uint32 room = chunk.GetCapacity() - first;
        const uint32 count = static_cast<uint32>(entities.Count() - created) < room
                                 ? static_cast<uint32>(entities.Count() - created)
                                 : room;
        auto *chunkEntities = reinterpret_cast<Entity *>(chunk.GetBytes() + archetype._entitiesOffset);
        for (uint32 i = 0; i < count; ++i)
        {
            const Entity entity = AllocateEntity();
            EntitySlot &slot = _slots[entity.Index];
            slot.Chunk = &chunk;
            slot.Row = first + i;
            chunkEntities[first + i] = entity;
            entities[created + i] = entity;
        }
        const uint32 version = GetVersion();
        for (uint32 i = 0; i < archetype._columns.Count(); ++i)
        {
            const ComponentType &type = *archetype._columns[i].Type;
            type.Construct(static_cast<uint8 *>(chunk.GetColumnData(i)) + static_cast<usize>(first) * type.GetSize(),
                           count);
            chunk.SetVersion(i, version);
            if (uint64 *bits = chunk.GetEnabledBits(i))
            {
                for (uint32 row = first; row < first + count; ++row)
                {
                    SetBit(bits, row, true);
                }
            }
        }
        chunk._count += count;
        if (chunk.IsFull())
        {
            RemoveFromFreeList(chunk);
        }
        created += count;
    }
}

void EntityManager::DestroyEntity(Entity entity)
{
    CheckStructuralChange();
    if (FindSlot(entity) == nullptr)
    {
        return;
    }
    EntitySlot &slot = _slots[entity.Index];
    Chunk &chunk = *slot.Chunk;
    const Archetype &archetype = *chunk._archetype;
    for (uint32 i = 0; i < archetype._columns.Count(); ++i)
    {
        const ComponentType &type = *archetype._columns[i].Type;
        type.Destruct(static_cast<uint8 *>(chunk.GetColumnData(i)) + static_cast<usize>(slot.Row) * type.GetSize(), 1);
    }
    RemoveRow(chunk, slot.Row);
    slot.Chunk = nullptr;
    slot.Row = 0;
    // The next occupant gets a new generation; 0 is the null entity and the last one marks deferred entities.
    slot.Generation = slot.Generation + 1 >= Entity::DeferredGeneration ? 1 : slot.Generation + 1;
    _freeSlots.Add(entity.Index);
    --_aliveCount;
}

void EntityManager::MoveEntity(EntitySlot &slot, Archetype &target, Span<const uint32> shared, ComponentId initialized,
                               const void *value)
{
    CheckStructuralChange();
    Chunk &source = *slot.Chunk;
    const uint32 sourceRow = slot.Row;
    const Archetype &from = *source._archetype;
    const Entity entity = source.GetEntities()[sourceRow];
    const EntityLocation location = AllocateRow(target, shared);
    Chunk &destination = *location.Chunk;
    reinterpret_cast<Entity *>(destination.GetBytes() + target._entitiesOffset)[location.Row] = entity;

    // Both column lists are sorted by TypeId, so one walk pairs them: moved, new or dropped.
    const auto rowOf = [](const Chunk &chunk, uint32 column, const ComponentType &type, uint32 row) {
        return static_cast<uint8 *>(chunk.GetColumnData(column)) + static_cast<usize>(row) * type.GetSize();
    };
    const auto targetCount = static_cast<uint32>(target._columns.Count());
    const auto sourceCount = static_cast<uint32>(from._columns.Count());
    uint32 s = 0;
    for (uint32 t = 0; t < targetCount; ++t)
    {
        const ComponentType &type = *target._columns[t].Type;
        const uint64 typeId = type.GetTypeInfo().GetId();
        for (; s < sourceCount && from._columns[s].Type->GetTypeInfo().GetId() < typeId; ++s)
        {
            const ComponentType &dropped = *from._columns[s].Type;
            dropped.Destruct(rowOf(source, s, dropped, sourceRow), 1);
        }
        uint8 *destinationValue = rowOf(destination, t, type, location.Row);
        if (s < sourceCount && from._columns[s].Type == &type)
        {
            type.Relocate(destinationValue, rowOf(source, s, type, sourceRow), 1);
            const uint64 *sourceBits = source.GetEnabledBits(s);
            if (uint64 *bits = destination.GetEnabledBits(t); bits != nullptr && sourceBits != nullptr)
            {
                SetBit(bits, location.Row, GetBit(sourceBits, sourceRow));
            }
            ++s;
        }
        else if (value != nullptr && type.GetId() == initialized)
        {
            type.CopyConstruct(destinationValue, value, 1);
        }
        else
        {
            type.Construct(destinationValue, 1);
        }
    }
    for (; s < sourceCount; ++s)
    {
        const ComponentType &dropped = *from._columns[s].Type;
        dropped.Destruct(rowOf(source, s, dropped, sourceRow), 1);
    }

    // RemoveRow may free the source chunk, so the slot moves to the new row first.
    slot.Chunk = &destination;
    slot.Row = location.Row;
    RemoveRow(source, sourceRow);
}

uint32 EntityManager::GatherShared(const Chunk &source, const Archetype &target, ComponentId overridden,
                                   uint32 overrideIndex, uint32 *shared) const
{
    const Archetype &from = *source._archetype;
    uint32 count = 0;
    for (const ArchetypeChunkValue &value : target._shared)
    {
        const ComponentId component = value.Type->GetId();
        if (component == overridden)
        {
            shared[count++] = overrideIndex;
            continue;
        }
        const int32 index = from.FindShared(component);
        shared[count++] = index >= 0 ? source.GetSharedValueIndex(static_cast<uint32>(index)) : ~uint32{0};
    }
    return count;
}

bool EntityManager::HasComponent(Entity entity, ComponentId component) const
{
    const EntitySlot *slot = FindSlot(entity);
    return slot != nullptr && slot->Chunk->_archetype->Has(component);
}

void EntityManager::AddComponent(Entity entity, ComponentId component, const void *value)
{
    CheckStructuralChange();
    MARLIN_ASSERTF(FindSlot(entity) != nullptr, "Entity {0} does not exist", entity.Index);
    EntitySlot &slot = _slots[entity.Index];
    Archetype &archetype = *slot.Chunk->_archetype;
    if (archetype.Has(component))
    {
        if (value != nullptr)
        {
            SetComponent(entity, component, value);
        }
        return;
    }
    const ComponentType &type = Components::Get(component);
    uint32 sharedIndex = 0;
    if (type.IsShared())
    {
        const ReflectedValue initial(type.GetTypeInfo());
        sharedIndex = FindOrAddSharedValue(component, value != nullptr ? value : initial.Get());
    }
    Archetype &target = GetArchetypeWith(archetype, component);
    uint32 shared[MaxSharedComponents];
    const uint32 count = GatherShared(*slot.Chunk, target, component, sharedIndex, shared);
    MoveEntity(slot, target, Span<const uint32>(shared, count), component, type.IsShared() ? nullptr : value);
}

void EntityManager::RemoveComponent(Entity entity, ComponentId component)
{
    CheckStructuralChange();
    MARLIN_ASSERTF(FindSlot(entity) != nullptr, "Entity {0} does not exist", entity.Index);
    EntitySlot &slot = _slots[entity.Index];
    Archetype &archetype = *slot.Chunk->_archetype;
    if (!archetype.Has(component))
    {
        return;
    }
    Archetype &target = GetArchetypeWithout(archetype, component);
    uint32 shared[MaxSharedComponents];
    const uint32 count = GatherShared(*slot.Chunk, target, InvalidComponentId, 0, shared);
    MoveEntity(slot, target, Span<const uint32>(shared, count));
}

void EntityManager::SetArchetype(Entity entity, Archetype &target)
{
    CheckStructuralChange();
    MARLIN_ASSERTF(FindSlot(entity) != nullptr, "Entity {0} does not exist", entity.Index);
    EntitySlot &slot = _slots[entity.Index];
    if (slot.Chunk->_archetype == &target)
    {
        return;
    }
    uint32 shared[MaxSharedComponents];
    const uint32 count = GatherShared(*slot.Chunk, target, InvalidComponentId, 0, shared);
    for (uint32 i = 0; i < count; ++i)
    {
        if (shared[i] == ~uint32{0})
        {
            const ReflectedValue initial(target._shared[i].Type->GetTypeInfo());
            shared[i] = FindOrAddSharedValue(target._shared[i].Type->GetId(), initial.Get());
        }
    }
    MoveEntity(slot, target, Span<const uint32>(shared, count));
}

const void *EntityManager::GetComponent(Entity entity, ComponentId component) const
{
    const EntitySlot *slot = FindSlot(entity);
    if (slot == nullptr)
    {
        return nullptr;
    }
    const Chunk &chunk = *slot->Chunk;
    const Archetype::Slot *info = chunk._archetype->FindSlot(component);
    if (info == nullptr)
    {
        return nullptr;
    }
    if (info->Column >= 0)
    {
        const ArchetypeColumn &column = chunk._archetype->_columns[static_cast<uint32>(info->Column)];
        return static_cast<const uint8 *>(chunk.GetColumnData(static_cast<uint32>(info->Column))) +
               static_cast<usize>(slot->Row) * column.Type->GetSize();
    }
    if (info->Shared >= 0)
    {
        return GetSharedValue(component, chunk.GetSharedValueIndex(static_cast<uint32>(info->Shared)));
    }
    if (info->ChunkValue >= 0)
    {
        return chunk.GetChunkComponent(static_cast<uint32>(info->ChunkValue));
    }
    // A tag has no value.
    return nullptr;
}

void *EntityManager::GetComponentForWrite(Entity entity, ComponentId component)
{
    const EntitySlot *slot = FindSlot(entity);
    if (slot == nullptr)
    {
        return nullptr;
    }
    const Chunk &chunk = *slot->Chunk;
    const Archetype::Slot *info = chunk._archetype->FindSlot(component);
    if (info == nullptr)
    {
        return nullptr;
    }
    MARLIN_ASSERTF(info->Shared < 0, "Shared component {0} changes through SetComponent",
                   Components::Get(component).GetTypeInfo().GetName());
    if (info->Column >= 0)
    {
        const auto column = static_cast<uint32>(info->Column);
        chunk.SetVersion(column, GetVersion());
        return static_cast<uint8 *>(chunk.GetColumnData(column)) +
               static_cast<usize>(slot->Row) * chunk._archetype->_columns[column].Type->GetSize();
    }
    if (info->ChunkValue >= 0)
    {
        return chunk.GetChunkComponent(static_cast<uint32>(info->ChunkValue));
    }
    return nullptr;
}

void EntityManager::SetComponent(Entity entity, ComponentId component, const void *value)
{
    MARLIN_ASSERTF(FindSlot(entity) != nullptr, "Entity {0} does not exist", entity.Index);
    EntitySlot &slot = _slots[entity.Index];
    const Archetype::Slot *info = slot.Chunk->_archetype->FindSlot(component);
    if (info == nullptr)
    {
        return;
    }
    const ComponentType &type = Components::Get(component);
    if (info->Shared < 0)
    {
        if (void *destination = GetComponentForWrite(entity, component))
        {
            type.CopyAssign(destination, value);
        }
        return;
    }
    // A new shared value moves the entity to the chunks that hold it.
    const uint32 index = FindOrAddSharedValue(component, value);
    if (slot.Chunk->GetSharedValueIndex(static_cast<uint32>(info->Shared)) == index)
    {
        return;
    }
    CheckStructuralChange();
    Archetype &archetype = *slot.Chunk->_archetype;
    uint32 shared[MaxSharedComponents];
    const uint32 count = GatherShared(*slot.Chunk, archetype, component, index, shared);
    MoveEntity(slot, archetype, Span<const uint32>(shared, count));
}

bool EntityManager::IsEnabled(Entity entity, ComponentId component) const
{
    const EntitySlot *slot = FindSlot(entity);
    if (slot == nullptr)
    {
        return false;
    }
    const int32 column = slot->Chunk->_archetype->FindColumn(component);
    if (column < 0)
    {
        return false;
    }
    const uint64 *bits = slot->Chunk->GetEnabledBits(static_cast<uint32>(column));
    MARLIN_ASSERTF(bits != nullptr, "{0} is not an enableable component of the entity",
                   Components::Get(component).GetTypeInfo().GetName());
    return bits != nullptr && GetBit(bits, slot->Row);
}

void EntityManager::SetEnabled(Entity entity, ComponentId component, bool enabled)
{
    const EntitySlot *slot = FindSlot(entity);
    if (slot == nullptr)
    {
        return;
    }
    const int32 column = slot->Chunk->_archetype->FindColumn(component);
    if (column < 0)
    {
        return;
    }
    uint64 *bits = slot->Chunk->GetEnabledBits(static_cast<uint32>(column));
    MARLIN_ASSERTF(bits != nullptr, "{0} is not an enableable component of the entity",
                   Components::Get(component).GetTypeInfo().GetName());
    if (bits != nullptr)
    {
        SetBit(bits, slot->Row, enabled);
        slot->Chunk->SetVersion(static_cast<uint32>(column), GetVersion());
    }
}
} // namespace Marlin
