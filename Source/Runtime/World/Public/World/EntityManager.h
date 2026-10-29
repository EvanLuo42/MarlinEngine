#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "World/Archetype.h"
#include "World/Component.h"
#include "World/Entity.h"

namespace Marlin
{
/// Where an entity's values are: its chunk and row; a null chunk if the entity does not exist.
struct EntityLocation
{
    Chunk *Chunk = nullptr;
    uint32 Row = 0;
};

/// The archetype ECS of one world (Documentation/Architecture/05 §2): entity slots, archetypes, chunks and the values
/// of shared components. Structural changes (creating or destroying entities, adding or removing components, setting
/// a shared value) happen on one thread and never while systems run; systems record them in a CommandBuffer. Values
/// may be read and written from jobs as the scheduler allows.
class WORLD_API EntityManager
{
public:
    EntityManager();
    ~EntityManager();
    MARLIN_NONCOPYABLE(EntityManager);

    Entity CreateEntity(Span<const ComponentId> components = {});
    Entity CreateEntity(Archetype &archetype);
    /// Creates an entity of *archetype* for each element of *entities*, with default values.
    void CreateEntities(Archetype &archetype, Span<Entity> entities);
    /// As above, with value indices for the archetype's shared components, in GetSharedComponents order.
    void CreateEntities(Archetype &archetype, Span<Entity> entities, Span<const uint32> sharedValues);
    void DestroyEntity(Entity entity);

    [[nodiscard]] bool Exists(Entity entity) const;
    [[nodiscard]] uint32 GetEntityCount() const
    {
        return _aliveCount;
    }

    [[nodiscard]] EntityLocation GetLocation(Entity entity) const;
    /// Null if the entity does not exist.
    [[nodiscard]] Archetype *GetArchetype(Entity entity) const;

    [[nodiscard]] bool HasComponent(Entity entity, ComponentId component) const;
    /// Adds *component* with a copy of *value*, or a default value if it is null; sets the value if the entity
    /// already has it.
    void AddComponent(Entity entity, ComponentId component, const void *value = nullptr);
    void RemoveComponent(Entity entity, ComponentId component);
    /// Moves the entity to *target*: values of components both have are kept, new ones are default, the rest are
    /// destroyed.
    void SetArchetype(Entity entity, Archetype &target);

    /// The value, null for a tag or a missing component. Shared and chunk components give the chunk's value.
    [[nodiscard]] const void *GetComponent(Entity entity, ComponentId component) const;
    /// As GetComponent, and marks the column written; a shared value cannot be written in place.
    [[nodiscard]] void *GetComponentForWrite(Entity entity, ComponentId component);
    /// Copies *value* in. For a shared component this moves the entity to the chunks of the new value.
    void SetComponent(Entity entity, ComponentId component, const void *value);

    /// Whether an enableable component is on for the entity.
    [[nodiscard]] bool IsEnabled(Entity entity, ComponentId component) const;
    void SetEnabled(Entity entity, ComponentId component, bool enabled);

    [[nodiscard]] const void *GetSharedValue(ComponentId component, uint32 index) const;
    /// The index of the shared value equal to *value*, adding a copy if there is none.
    uint32 FindOrAddSharedValue(ComponentId component, const void *value);

    template <typename T> void Add(Entity entity, const T &value = T())
    {
        AddComponent(entity, Components::IdOf<T>(), &value);
    }

    template <typename T> void Remove(Entity entity)
    {
        RemoveComponent(entity, Components::IdOf<T>());
    }

    template <typename T> [[nodiscard]] bool Has(Entity entity) const
    {
        return HasComponent(entity, Components::IdOf<T>());
    }

    template <typename T> [[nodiscard]] const T *Get(Entity entity) const
    {
        return static_cast<const T *>(GetComponent(entity, Components::IdOf<T>()));
    }

    template <typename T> [[nodiscard]] T *GetForWrite(Entity entity)
    {
        return static_cast<T *>(GetComponentForWrite(entity, Components::IdOf<T>()));
    }

    template <typename T> void Set(Entity entity, const T &value)
    {
        SetComponent(entity, Components::IdOf<T>(), &value);
    }

    template <typename T> [[nodiscard]] bool IsEnabled(Entity entity) const
    {
        return IsEnabled(entity, Components::IdOf<T>());
    }

    template <typename T> void SetEnabled(Entity entity, bool enabled)
    {
        SetEnabled(entity, Components::IdOf<T>(), enabled);
    }

    /// The archetype of exactly *components*, in any order.
    Archetype &GetOrCreateArchetype(Span<const ComponentId> components);
    /// *archetype* with or without one more component; the transitions are cached on the archetypes.
    Archetype &GetArchetypeWith(Archetype &archetype, ComponentId component);
    Archetype &GetArchetypeWithout(Archetype &archetype, ComponentId component);

    [[nodiscard]] Archetype &GetEmptyArchetype() const
    {
        return *_archetypes[0];
    }

    /// Every archetype in creation order; new ones are only ever appended.
    [[nodiscard]] Span<Archetype *const> GetArchetypes() const
    {
        return _archetypes.AsSpan();
    }

    /// The version writes are stamped with now (Documentation/Architecture/05 §3, Changed<T>).
    [[nodiscard]] uint32 GetVersion() const
    {
        return _version.Load(MemoryOrder::Relaxed);
    }

    /// Starts a new version and returns it; the scheduler calls it before each system runs.
    uint32 AdvanceVersion()
    {
        return _version.FetchAdd(1, MemoryOrder::Relaxed) + 1;
    }

    /// While locked, a structural change is an error; the scheduler locks the structure while systems run.
    void LockStructure()
    {
        ++_structureLocks;
    }

    void UnlockStructure()
    {
        --_structureLocks;
    }

    [[nodiscard]] bool IsStructureLocked() const
    {
        return _structureLocks != 0;
    }

private:
    struct EntitySlot
    {
        Chunk *Chunk = nullptr;
        uint32 Row = 0;
        /// The generation of the current entity, or of the next one while the slot is free.
        uint32 Generation = 1;
    };

    struct SharedValue
    {
        void *Data = nullptr;
        uint64 Hash = 0;
        /// The chunks that hold this value.
        uint32 References = 0;
    };

    struct SharedStore
    {
        List<SharedValue> Values;
        List<uint32> FreeValues;
    };

    Archetype &CreateArchetype(Span<const ComponentId> sorted, uint64 hash);
    [[nodiscard]] const EntitySlot *FindSlot(Entity entity) const;
    Entity AllocateEntity();
    Chunk &AllocateChunk(Archetype &archetype, Span<const uint32> shared);
    void FreeChunk(Chunk &chunk);
    Chunk &FindChunkWithRoom(Archetype &archetype, Span<const uint32> shared);
    /// A new row with default enabled bits; the caller constructs its values.
    EntityLocation AllocateRow(Archetype &archetype, Span<const uint32> shared);
    /// Fills the row, whose values are already destroyed or moved, with the chunk's last row.
    void RemoveRow(Chunk &chunk, uint32 row);
    /// Moves the entity's row to *target*; a component it gains is default-constructed, or copy-constructed from
    /// *value* if it is *initialized*.
    void MoveEntity(EntitySlot &slot, Archetype &target, Span<const uint32> shared,
                    ComponentId initialized = InvalidComponentId, const void *value = nullptr);
    /// Fills *shared*, room for every shared component of an archetype, with the value indices *target* needs for
    /// an entity of *source*; ~0 where *source* has none. Returns how many.
    uint32 GatherShared(const Chunk &source, const Archetype &target, ComponentId overridden, uint32 overrideIndex,
                        uint32 *shared) const;
    void AddToFreeList(Chunk &chunk);
    void RemoveFromFreeList(Chunk &chunk);
    SharedStore &GetSharedStore(ComponentId component);
    void ReleaseSharedValue(ComponentId component, uint32 index);
    void CheckStructuralChange() const;
    void TouchStructure(Archetype &archetype)
    {
        archetype._structureVersion = ++_structureCounter;
    }

    List<EntitySlot> _slots;
    List<uint32> _freeSlots;
    uint32 _aliveCount = 0;
    List<Archetype *> _archetypes;
    HashMap<uint64, Archetype *> _archetypesByHash;
    HashMap<ComponentId, SharedStore> _sharedStores;
    Atomic<uint32> _version{1};
    uint32 _structureLocks = 0;
    /// Gives each Archetype::GetStructureVersion a value no other change had.
    uint32 _structureCounter = 0;
};
} // namespace Marlin
