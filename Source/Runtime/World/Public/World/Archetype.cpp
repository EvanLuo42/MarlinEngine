#include "World/Archetype.h"

namespace Marlin
{
uint32 Chunk::GetCapacity() const
{
    return _archetype->_capacity;
}

Span<const Entity> Chunk::GetEntities() const
{
    return {reinterpret_cast<const Entity *>(GetBytes() + _archetype->_entitiesOffset), _count};
}

void *Chunk::GetColumnData(uint32 column) const
{
    return GetBytes() + _archetype->_columns[column].Offset;
}

uint64 *Chunk::GetEnabledBits(uint32 column) const
{
    const uint32 offset = _archetype->_columns[column].EnabledOffset;
    return offset != 0 ? reinterpret_cast<uint64 *>(GetBytes() + offset) : nullptr;
}

uint32 Chunk::GetSharedValueIndex(uint32 shared) const
{
    return *reinterpret_cast<const uint32 *>(GetBytes() + _archetype->_shared[shared].Offset);
}

void *Chunk::GetChunkComponent(uint32 index) const
{
    return GetBytes() + _archetype->_chunkComponents[index].Offset;
}

uint32 *Chunk::GetVersions() const
{
    return reinterpret_cast<uint32 *>(GetBytes() + _archetype->_versionsOffset);
}

const Archetype::Slot *Archetype::FindSlot(ComponentId component) const
{
    // Archetypes have few components; a linear search beats a map.
    for (const Slot &slot : _slots)
    {
        if (slot.Component == component)
        {
            return &slot;
        }
    }
    return nullptr;
}

bool Archetype::Has(ComponentId component) const
{
    return FindSlot(component) != nullptr;
}

int32 Archetype::FindColumn(ComponentId component) const
{
    const Slot *slot = FindSlot(component);
    return slot != nullptr ? slot->Column : -1;
}

int32 Archetype::FindShared(ComponentId component) const
{
    const Slot *slot = FindSlot(component);
    return slot != nullptr ? slot->Shared : -1;
}

int32 Archetype::FindChunkComponent(ComponentId component) const
{
    const Slot *slot = FindSlot(component);
    return slot != nullptr ? slot->ChunkValue : -1;
}

uint32 Archetype::GetEntityCount() const
{
    uint32 count = 0;
    for (const Chunk *chunk : _chunks)
    {
        count += chunk->GetCount();
    }
    return count;
}
} // namespace Marlin
