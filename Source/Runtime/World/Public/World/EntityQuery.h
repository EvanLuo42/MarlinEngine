#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Templates/Function.h"
#include "Templates/TypeTraits.h"
#include "World/Archetype.h"
#include "World/Component.h"
#include "World/Entity.h"

namespace Marlin
{
class EntityManager;

/// Which entities a query visits: those with every Read, Write and Changed component, none of the None components
/// and, if Any is not empty, at least one of the Any components. A disabled enableable component counts as missing.
/// Changed also skips chunks whose column was not written since the system last ran.
struct EntityQueryDesc
{
    List<ComponentId> Read;
    List<ComponentId> Write;
    List<ComponentId> Any;
    List<ComponentId> None;
    /// Filters by column version (Documentation/Architecture/05 §3).
    List<ComponentId> Changed;
};

/// The versions a system's queries compare against: the version at which the system last ran, and the one its
/// writes are stamped with now.
struct QueryVersions
{
    uint32 LastVersion = 0;
    uint32 Version = 0;
};

/// One chunk as a query visits it: its rows, of which IsIncluded tells those the query selects, and typed access
/// to its columns. Writing a column through it marks the column changed.
class WORLD_API ChunkView
{
public:
    ChunkView(const EntityManager &entities, Chunk &chunk, const uint64 *enabled, uint32 version, uint32 ordinal)
        : _entities(&entities), _chunk(&chunk), _enabled(enabled), _version(version), _ordinal(ordinal)
    {
    }

    [[nodiscard]] Chunk &GetChunk() const
    {
        return *_chunk;
    }

    [[nodiscard]] uint32 GetCount() const
    {
        return _chunk->GetCount();
    }

    /// The chunk's position among those the query visits, the same in every run over the same chunks: where a
    /// parallel visit keeps its own CommandBuffer.
    [[nodiscard]] uint32 GetOrdinal() const
    {
        return _ordinal;
    }

    [[nodiscard]] Span<const Entity> GetEntities() const
    {
        return _chunk->GetEntities();
    }

    /// False for a row whose enableable components exclude it.
    [[nodiscard]] bool IsIncluded(uint32 row) const
    {
        return _enabled == nullptr || (_enabled[row / 64] >> (row % 64) & 1) != 0;
    }

    [[nodiscard]] bool Has(ComponentId component) const
    {
        return _chunk->GetArchetype().Has(component);
    }

    /// The column of *component*, or null if the chunk has none.
    [[nodiscard]] const void *ReadColumn(ComponentId component) const;
    /// As ReadColumn, and stamps the column with the current version.
    [[nodiscard]] void *WriteColumn(ComponentId component) const;
    [[nodiscard]] bool HasChanged(ComponentId component, uint32 lastVersion) const;
    /// The chunk's value of a chunk or shared component, or null.
    [[nodiscard]] const void *GetChunkValue(ComponentId component) const;

    template <typename T> [[nodiscard]] Span<const T> Read() const
    {
        const auto *data = static_cast<const T *>(ReadColumn(Components::IdOf<T>()));
        return data != nullptr ? Span<const T>(data, GetCount()) : Span<const T>();
    }

    template <typename T> [[nodiscard]] Span<T> Write() const
    {
        auto *data = static_cast<T *>(WriteColumn(Components::IdOf<T>()));
        return data != nullptr ? Span<T>(data, GetCount()) : Span<T>();
    }

    template <typename T> [[nodiscard]] bool Has() const
    {
        return Has(Components::IdOf<T>());
    }

    /// A chunk or shared component's value for the whole chunk.
    template <typename T> [[nodiscard]] const T *Get() const
    {
        return static_cast<const T *>(GetChunkValue(Components::IdOf<T>()));
    }

private:
    const EntityManager *_entities;
    Chunk *_chunk;
    const uint64 *_enabled;
    uint32 _version;
    uint32 _ordinal;
};

/// A query bound to one EntityManager. It caches the matching archetypes and picks up new ones as they appear.
class WORLD_API EntityQueryBase
{
public:
    EntityQueryBase() = default;
    explicit EntityQueryBase(EntityQueryDesc desc);
    MARLIN_NONCOPYABLE(EntityQueryBase);

    void SetDesc(EntityQueryDesc desc);
    void Bind(EntityManager &entities);

    [[nodiscard]] const EntityQueryDesc &GetDesc() const
    {
        return _desc;
    }

    [[nodiscard]] bool IsBound() const
    {
        return _entities != nullptr;
    }

    [[nodiscard]] EntityManager &GetEntityManager() const
    {
        return *_entities;
    }

    [[nodiscard]] bool Matches(const Archetype &archetype) const;
    /// The matching archetypes, updated with those created since the last call.
    [[nodiscard]] Span<Archetype *const> GetArchetypes();
    /// The entities the query selects now.
    [[nodiscard]] uint32 CalculateEntityCount();

    /// Visits each chunk with selected rows; Changed compares against *versions*.
    void ForEachChunk(const QueryVersions &versions, FunctionRef<void(ChunkView &)> visitor);
    /// As ForEachChunk, visiting chunks in parallel on the job system, and returns once all are done.
    void ParallelForEachChunk(const QueryVersions &versions, FunctionRef<void(ChunkView &)> visitor);
    /// Outside systems: every chunk counts as changed and writes are stamped with the current version.
    void ForEachChunk(FunctionRef<void(ChunkView &)> visitor);

    [[nodiscard]] QueryVersions GetDefaultVersions() const;

private:
    struct Visit
    {
        Chunk *Target = nullptr;
        uint32 Ordinal = 0;
    };

    /// False if the query skips *chunk*. Otherwise *enabled* is null if every row is selected, or *bits*, which
    /// then holds the selected rows. A null *lastVersion* ignores Changed.
    bool Filter(Chunk &chunk, const uint32 *lastVersion, uint64 *bits, const uint64 *&enabled) const;
    /// Whether a Changed column of *chunk* was written after *lastVersion*.
    [[nodiscard]] bool HasChangedColumn(const Chunk &chunk, uint32 lastVersion) const;

    EntityQueryDesc _desc;
    EntityManager *_entities = nullptr;
    List<Archetype *> _archetypes;
    uint32 _seenArchetypes = 0;
    List<ComponentId> _enableableRequired;
    List<ComponentId> _enableableNone;
    /// ParallelForEachChunk's chunks, kept for its memory: a query visits in parallel once at a time.
    List<Visit> _visits;
};

/// The terms of an EntityQuery: Read<T> and Write<T> give the callback a `const T &` or `T &`; the others only filter.
template <typename T> struct Read
{
};
template <typename T> struct Write
{
};
template <typename T> struct Without
{
};
template <typename T> struct WithAny
{
};
/// Selects chunks whose T column was written since the system last ran; combine with Read<T> to read it.
template <typename T> struct Changed
{
};

namespace Private
{
template <typename T> struct QueryTermTraits;

template <typename T> struct QueryTermTraits<Read<T>>
{
    static constexpr bool IsData = true;

    static void Describe(EntityQueryDesc &desc)
    {
        desc.Read.Add(Components::IdOf<T>());
    }

    static const T *Column(const ChunkView &chunk)
    {
        return static_cast<const T *>(chunk.ReadColumn(Components::IdOf<T>()));
    }
};

template <typename T> struct QueryTermTraits<Write<T>>
{
    static constexpr bool IsData = true;

    static void Describe(EntityQueryDesc &desc)
    {
        desc.Write.Add(Components::IdOf<T>());
    }

    static T *Column(const ChunkView &chunk)
    {
        return static_cast<T *>(chunk.WriteColumn(Components::IdOf<T>()));
    }
};

template <typename T> struct QueryTermTraits<Without<T>>
{
    static constexpr bool IsData = false;

    static void Describe(EntityQueryDesc &desc)
    {
        desc.None.Add(Components::IdOf<T>());
    }
};

template <typename T> struct QueryTermTraits<WithAny<T>>
{
    static constexpr bool IsData = false;

    static void Describe(EntityQueryDesc &desc)
    {
        desc.Any.Add(Components::IdOf<T>());
    }
};

template <typename T> struct QueryTermTraits<Changed<T>>
{
    static constexpr bool IsData = false;

    static void Describe(EntityQueryDesc &desc)
    {
        desc.Changed.Add(Components::IdOf<T>());
    }
};

template <typename... T> struct QueryTermList
{
};

template <typename TList, typename... TTerms> struct DataTerms;

template <typename... TData> struct DataTerms<QueryTermList<TData...>>
{
    using Type = QueryTermList<TData...>;
};

template <typename... TData, typename TTerm, typename... TRest>
struct DataTerms<QueryTermList<TData...>, TTerm, TRest...>
{
    using Type =
        typename Conditional<QueryTermTraits<TTerm>::IsData, DataTerms<QueryTermList<TData..., TTerm>, TRest...>,
                             DataTerms<QueryTermList<TData...>, TRest...>>::Type;
};

template <typename TFunction, typename... TColumns>
void ForEachRow(const ChunkView &chunk, TFunction &function, TColumns *...columns)
{
    const uint32 count = chunk.GetCount();
    for (uint32 row = 0; row < count; ++row)
    {
        if (chunk.IsIncluded(row))
        {
            function(columns[row]...);
        }
    }
}

template <typename TFunction, typename... TData>
void ForEachInChunk(QueryTermList<TData...> /*terms*/, const ChunkView &chunk, TFunction &function)
{
    ForEachRow(chunk, function, QueryTermTraits<TData>::Column(chunk)...);
}
} // namespace Private

/// A typed query: `EntityQuery<Read<LocalTransform>, Write<WorldTransform>, Without<Parent>>`. ForEach calls the
/// function with a reference to each data term's value, in the order of the terms.
template <typename... TTerms> class EntityQuery : public EntityQueryBase
{
public:
    EntityQuery() : EntityQueryBase(MakeDesc())
    {
    }

    static EntityQueryDesc MakeDesc()
    {
        EntityQueryDesc desc;
        (Private::QueryTermTraits<TTerms>::Describe(desc), ...);
        return desc;
    }

    template <typename TFunction> void ForEach(const QueryVersions &versions, TFunction &&function)
    {
        ForEachChunk(versions, [&](ChunkView &chunk) { Private::ForEachInChunk(Data(), chunk, function); });
    }

    template <typename TFunction> void ForEach(TFunction &&function)
    {
        ForEachChunk([&](ChunkView &chunk) { Private::ForEachInChunk(Data(), chunk, function); });
    }

    /// The function runs on several threads at once.
    template <typename TFunction> void ParallelForEach(const QueryVersions &versions, TFunction &&function)
    {
        ParallelForEachChunk(versions, [&](ChunkView &chunk) { Private::ForEachInChunk(Data(), chunk, function); });
    }

private:
    using Data = typename Private::DataTerms<Private::QueryTermList<>, TTerms...>::Type;
};
} // namespace Marlin
