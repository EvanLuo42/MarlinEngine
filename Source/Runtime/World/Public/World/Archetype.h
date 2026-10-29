#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "World/Component.h"
#include "World/Entity.h"

namespace Marlin
{
class Archetype;
class EntityManager;

/// Entities live in fixed-size chunks, one archetype each (Documentation/Architecture/05 §2).
inline constexpr uint32 ChunkSize = 16 * 1024;
inline constexpr uint32 ChunkAlignment = 64;

/// Where a component with a column lives in each chunk of an archetype.
struct ArchetypeColumn
{
    const ComponentType *Type = nullptr;
    /// From the start of the chunk; values are laid out in rows.
    uint32 Offset = 0;
    /// The enabled bits of an enableable component, one per row; zero for any other.
    uint32 EnabledOffset = 0;
};

/// Where a per-chunk value lives: a shared component's value index or a chunk component's value.
struct ArchetypeChunkValue
{
    const ComponentType *Type = nullptr;
    uint32 Offset = 0;
};

/// One block of ChunkSize bytes: this header, then the archetype's per-chunk values, the version of each column, the
/// entities and a column per component. Rows are dense; removing one moves the last row into it.
class WORLD_API Chunk
{
public:
    [[nodiscard]] Archetype &GetArchetype() const
    {
        return *_archetype;
    }

    [[nodiscard]] uint32 GetCount() const
    {
        return _count;
    }

    [[nodiscard]] uint32 GetCapacity() const;

    [[nodiscard]] bool IsFull() const
    {
        return _count == GetCapacity();
    }

    [[nodiscard]] Span<const Entity> GetEntities() const;

    /// The values of column *column*, an index into Archetype::GetColumns.
    [[nodiscard]] void *GetColumnData(uint32 column) const;

    /// The EntityManager version at which column *column* was last written.
    [[nodiscard]] uint32 GetVersion(uint32 column) const
    {
        return GetVersions()[column];
    }

    void SetVersion(uint32 column, uint32 version) const
    {
        GetVersions()[column] = version;
    }

    /// The enabled bits of column *column*, or null if its component is not enableable.
    [[nodiscard]] uint64 *GetEnabledBits(uint32 column) const;

    /// The value index of shared component *shared*, an index into Archetype::GetSharedComponents.
    [[nodiscard]] uint32 GetSharedValueIndex(uint32 shared) const;

    /// The value of chunk component *index*, an index into Archetype::GetChunkComponents.
    [[nodiscard]] void *GetChunkComponent(uint32 index) const;

private:
    friend class EntityManager;

    [[nodiscard]] uint32 *GetVersions() const;
    [[nodiscard]] uint8 *GetBytes() const
    {
        return reinterpret_cast<uint8 *>(const_cast<Chunk *>(this)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }

    Archetype *_archetype = nullptr;
    uint32 _count = 0;
    /// Where it is in the archetype's chunk list, and in its list of chunks with room while _inFreeList.
    uint32 _index = 0;
    uint32 _freeIndex = 0;
    bool _inFreeList = false;
};

/// True if *version* is later than *since*, allowing for the counter wrapping.
[[nodiscard]] inline bool IsNewerVersion(uint32 version, uint32 since)
{
    return static_cast<int32>(version - since) > 0;
}

/// A set of component types and the chunks of the entities that have exactly those components. The EntityManager
/// creates archetypes on demand and keeps them for its lifetime.
class WORLD_API Archetype
{
public:
    /// Every component of the archetype, tags and shared ones included, sorted by TypeId.
    [[nodiscard]] Span<const ComponentId> GetComponents() const
    {
        return _components.AsSpan();
    }

    /// A hash of the sorted TypeIds of the components.
    [[nodiscard]] uint64 GetHash() const
    {
        return _hash;
    }

    /// Where it is in EntityManager::GetArchetypes.
    [[nodiscard]] uint32 GetIndex() const
    {
        return _index;
    }

    [[nodiscard]] bool Has(ComponentId component) const;

    [[nodiscard]] Span<const ArchetypeColumn> GetColumns() const
    {
        return _columns.AsSpan();
    }

    [[nodiscard]] Span<const ArchetypeChunkValue> GetSharedComponents() const
    {
        return _shared.AsSpan();
    }

    [[nodiscard]] Span<const ArchetypeChunkValue> GetChunkComponents() const
    {
        return _chunkComponents.AsSpan();
    }

    /// Indices into GetColumns, GetSharedComponents and GetChunkComponents, or -1.
    [[nodiscard]] int32 FindColumn(ComponentId component) const;
    [[nodiscard]] int32 FindShared(ComponentId component) const;
    [[nodiscard]] int32 FindChunkComponent(ComponentId component) const;

    [[nodiscard]] uint32 GetChunkCapacity() const
    {
        return _capacity;
    }

    [[nodiscard]] Span<Chunk *const> GetChunks() const
    {
        return _chunks.AsSpan();
    }

    [[nodiscard]] uint32 GetEntityCount() const;

    [[nodiscard]] bool HasEnableable() const
    {
        return _hasEnableable;
    }

    /// Changes whenever a row is added to or removed from one of its chunks, which moves rows and may free chunks:
    /// pointers into its chunks taken at an earlier structure version may be stale.
    [[nodiscard]] uint32 GetStructureVersion() const
    {
        return _structureVersion;
    }

private:
    friend class Chunk;
    friend class EntityManager;

    struct Slot
    {
        ComponentId Component = InvalidComponentId;
        int16 Column = -1;
        int16 Shared = -1;
        int16 ChunkValue = -1;
    };

    [[nodiscard]] const Slot *FindSlot(ComponentId component) const;

    List<ComponentId> _components;
    /// One per component, in the order of _components.
    List<Slot> _slots;
    List<ArchetypeColumn> _columns;
    List<ArchetypeChunkValue> _shared;
    List<ArchetypeChunkValue> _chunkComponents;
    List<Chunk *> _chunks;
    /// Chunks with room, which new rows go to first.
    List<Chunk *> _freeChunks;
    /// An emptied chunk kept for the next one the archetype needs, so a row coming and going does not allocate and
    /// free a chunk each time; only for archetypes without shared components, whose chunks hold value references.
    Chunk *_spareChunk = nullptr;
    HashMap<ComponentId, Archetype *> _addEdges;
    HashMap<ComponentId, Archetype *> _removeEdges;
    uint64 _hash = 0;
    uint32 _index = 0;
    uint32 _capacity = 0;
    uint32 _entitiesOffset = 0;
    uint32 _versionsOffset = 0;
    uint32 _structureVersion = 0;
    bool _hasEnableable = false;
};
} // namespace Marlin
