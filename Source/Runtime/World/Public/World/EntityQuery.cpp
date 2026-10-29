#include "World/EntityQuery.h"

#include "Concurrency/Jobs.h"
#include "Templates/Bit.h"
#include "World/EntityManager.h"

namespace Marlin
{
namespace
{
/// Enough words for the enabled bits of the fullest possible chunk, one Entity per row.
constexpr uint32 MaxEnabledWords = ChunkSize / sizeof(Entity) / 64;
} // namespace

const void *ChunkView::ReadColumn(ComponentId component) const
{
    const int32 column = _chunk->GetArchetype().FindColumn(component);
    return column >= 0 ? _chunk->GetColumnData(static_cast<uint32>(column)) : nullptr;
}

void *ChunkView::WriteColumn(ComponentId component) const
{
    const int32 column = _chunk->GetArchetype().FindColumn(component);
    if (column < 0)
    {
        return nullptr;
    }
    _chunk->SetVersion(static_cast<uint32>(column), _version);
    return _chunk->GetColumnData(static_cast<uint32>(column));
}

bool ChunkView::HasChanged(ComponentId component, uint32 lastVersion) const
{
    const int32 column = _chunk->GetArchetype().FindColumn(component);
    return column >= 0 && IsNewerVersion(_chunk->GetVersion(static_cast<uint32>(column)), lastVersion);
}

const void *ChunkView::GetChunkValue(ComponentId component) const
{
    const Archetype &archetype = _chunk->GetArchetype();
    if (const int32 index = archetype.FindChunkComponent(component); index >= 0)
    {
        return _chunk->GetChunkComponent(static_cast<uint32>(index));
    }
    if (const int32 shared = archetype.FindShared(component); shared >= 0)
    {
        return _entities->GetSharedValue(component, _chunk->GetSharedValueIndex(static_cast<uint32>(shared)));
    }
    return nullptr;
}

EntityQueryBase::EntityQueryBase(EntityQueryDesc desc)
{
    SetDesc(Move(desc));
}

void EntityQueryBase::SetDesc(EntityQueryDesc desc)
{
    _desc = Move(desc);
    _enableableRequired.Clear();
    _enableableNone.Clear();
    for (const List<ComponentId> *list : {&_desc.Read, &_desc.Write})
    {
        for (const ComponentId component : *list)
        {
            if (Components::Get(component).IsEnableable() && !_enableableRequired.Contains(component))
            {
                _enableableRequired.Add(component);
            }
        }
    }
    for (const ComponentId component : _desc.None)
    {
        if (Components::Get(component).IsEnableable())
        {
            _enableableNone.Add(component);
        }
    }
    _archetypes.Clear();
    _seenArchetypes = 0;
}

void EntityQueryBase::Bind(EntityManager &entities)
{
    if (_entities == &entities)
    {
        return;
    }
    _entities = &entities;
    _archetypes.Clear();
    _seenArchetypes = 0;
}

bool EntityQueryBase::Matches(const Archetype &archetype) const
{
    for (const List<ComponentId> *list : {&_desc.Read, &_desc.Write, &_desc.Changed})
    {
        for (const ComponentId component : *list)
        {
            if (!archetype.Has(component))
            {
                return false;
            }
        }
    }
    for (const ComponentId component : _desc.None)
    {
        // An enableable component may be off for some rows; Filter decides per row.
        if (archetype.Has(component) && !Components::Get(component).IsEnableable())
        {
            return false;
        }
    }
    if (_desc.Any.IsEmpty())
    {
        return true;
    }
    for (const ComponentId component : _desc.Any)
    {
        if (archetype.Has(component))
        {
            return true;
        }
    }
    return false;
}

Span<Archetype *const> EntityQueryBase::GetArchetypes()
{
    MARLIN_ASSERT(_entities != nullptr);
    const Span<Archetype *const> archetypes = _entities->GetArchetypes();
    for (; _seenArchetypes < archetypes.Count(); ++_seenArchetypes)
    {
        if (Matches(*archetypes[_seenArchetypes]))
        {
            _archetypes.Add(archetypes[_seenArchetypes]);
        }
    }
    return _archetypes.AsSpan();
}

QueryVersions EntityQueryBase::GetDefaultVersions() const
{
    return {0, _entities->GetVersion()};
}

bool EntityQueryBase::Filter(Chunk &chunk, const uint32 *lastVersion, uint64 *bits, const uint64 *&enabled) const
{
    enabled = nullptr;
    const uint32 count = chunk.GetCount();
    if (count == 0)
    {
        return false;
    }
    const Archetype &archetype = chunk.GetArchetype();
    if (lastVersion != nullptr && !_desc.Changed.IsEmpty() && !HasChangedColumn(chunk, *lastVersion))
    {
        return false;
    }
    if (!archetype.HasEnableable() || (_enableableRequired.IsEmpty() && _enableableNone.IsEmpty()))
    {
        return true;
    }

    const uint32 words = (count + 63) / 64;
    for (uint32 i = 0; i < words; ++i)
    {
        bits[i] = ~uint64{0};
    }
    if (count % 64 != 0)
    {
        bits[words - 1] = (uint64{1} << (count % 64)) - 1;
    }
    bool filtered = false;
    for (const ComponentId component : _enableableRequired)
    {
        const uint64 *columnBits = chunk.GetEnabledBits(static_cast<uint32>(archetype.FindColumn(component)));
        for (uint32 i = 0; i < words; ++i)
        {
            bits[i] &= columnBits[i];
        }
        filtered = true;
    }
    for (const ComponentId component : _enableableNone)
    {
        const int32 column = archetype.FindColumn(component);
        if (column < 0)
        {
            continue;
        }
        const uint64 *columnBits = chunk.GetEnabledBits(static_cast<uint32>(column));
        for (uint32 i = 0; i < words; ++i)
        {
            bits[i] &= ~columnBits[i];
        }
        filtered = true;
    }
    if (!filtered)
    {
        return true;
    }
    uint64 any = 0;
    for (uint32 i = 0; i < words; ++i)
    {
        any |= bits[i];
    }
    if (any == 0)
    {
        return false;
    }
    enabled = bits;
    return true;
}

bool EntityQueryBase::HasChangedColumn(const Chunk &chunk, uint32 lastVersion) const
{
    const Archetype &archetype = chunk.GetArchetype();
    for (const ComponentId component : _desc.Changed)
    {
        const int32 column = archetype.FindColumn(component);
        if (column >= 0 && IsNewerVersion(chunk.GetVersion(static_cast<uint32>(column)), lastVersion))
        {
            return true;
        }
    }
    return false;
}

uint32 EntityQueryBase::CalculateEntityCount()
{
    uint32 count = 0;
    uint64 bits[MaxEnabledWords];
    for (const Archetype *archetype : GetArchetypes())
    {
        for (Chunk *chunk : archetype->GetChunks())
        {
            const uint64 *enabled = nullptr;
            if (!Filter(*chunk, nullptr, bits, enabled))
            {
                continue;
            }
            if (enabled == nullptr)
            {
                count += chunk->GetCount();
                continue;
            }
            for (uint32 i = 0; i < (chunk->GetCount() + 63) / 64; ++i)
            {
                count += static_cast<uint32>(PopCount(enabled[i]));
            }
        }
    }
    return count;
}

void EntityQueryBase::ForEachChunk(const QueryVersions &versions, FunctionRef<void(ChunkView &)> visitor)
{
    uint32 ordinal = 0;
    uint64 bits[MaxEnabledWords];
    for (const Archetype *archetype : GetArchetypes())
    {
        for (Chunk *chunk : archetype->GetChunks())
        {
            const uint64 *enabled = nullptr;
            if (Filter(*chunk, &versions.LastVersion, bits, enabled))
            {
                ChunkView view(*_entities, *chunk, enabled, versions.Version, ordinal);
                visitor(view);
            }
            ++ordinal;
        }
    }
}

void EntityQueryBase::ForEachChunk(FunctionRef<void(ChunkView &)> visitor)
{
    ForEachChunk(GetDefaultVersions(), visitor);
}

void EntityQueryBase::ParallelForEachChunk(const QueryVersions &versions, FunctionRef<void(ChunkView &)> visitor)
{
    List<Visit> &visits = _visits;
    visits.Clear();
    uint32 ordinal = 0;
    for (const Archetype *archetype : GetArchetypes())
    {
        for (Chunk *chunk : archetype->GetChunks())
        {
            // Chunks that Changed rules out are left out here, ahead of the jobs; ordinals still count them.
            if (!_desc.Changed.IsEmpty() && !HasChangedColumn(*chunk, versions.LastVersion))
            {
                ++ordinal;
                continue;
            }
            visits.Add({chunk, ordinal++});
        }
    }
    Jobs::ParallelFor(static_cast<uint32>(visits.Count()), 1, [&](uint32 begin, uint32 end) {
        uint64 bits[MaxEnabledWords];
        for (uint32 i = begin; i < end; ++i)
        {
            const uint64 *enabled = nullptr;
            if (Filter(*visits[i].Target, &versions.LastVersion, bits, enabled))
            {
                ChunkView view(*_entities, *visits[i].Target, enabled, versions.Version, visits[i].Ordinal);
                visitor(view);
            }
        }
    });
}
} // namespace Marlin
