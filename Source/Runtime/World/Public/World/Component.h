#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "EnumFlags.h"
#include "Reflection/TypeOf.h"

namespace Marlin
{
/// An index into the process's component registry, in the order components were first used.
using ComponentId = uint32;
inline constexpr ComponentId InvalidComponentId = ~ComponentId{0};
// RECOVERY-DECISION D3: the limits MaxComponentTypes, ChunkAlignment, MaxSharedComponents (EntityManager.cpp) and
// MaxHierarchyDepth (Hierarchy.cpp) are sized for a large game and a 64 KiB job fiber.
inline constexpr uint32 MaxComponentTypes = 4096;

/// How a component is stored (Documentation/Architecture/05 §2), from the markup of its type:
/// `MSTRUCT(Component, Shared)`.
enum class ComponentFlags : uint8
{
    None = 0,
    /// Zero-sized: only the archetype records it, without a column.
    Tag = 1 << 0,
    /// Grouped by value: entities with equal values share chunks, and the chunk holds the value's index.
    Shared = 1 << 1,
    /// One value per chunk, such as the chunk's bounds.
    Chunk = 1 << 2,
    /// Toggled on and off per entity by a bit in the chunk, without moving the entity.
    Enableable = 1 << 3,
};
MARLIN_ENUM_FLAGS(ComponentFlags)

/// A reflected type registered as a component: its size, flags and the operations chunks copy it with.
class WORLD_API ComponentType
{
public:
    [[nodiscard]] const TypeInfo &GetTypeInfo() const
    {
        return *_type;
    }

    [[nodiscard]] ComponentId GetId() const
    {
        return _id;
    }

    /// Zero for a tag.
    [[nodiscard]] uint32 GetSize() const
    {
        return _size;
    }

    [[nodiscard]] uint32 GetAlignment() const
    {
        return _alignment;
    }

    [[nodiscard]] ComponentFlags GetFlags() const
    {
        return _flags;
    }

    [[nodiscard]] bool IsTag() const
    {
        return (_flags & ComponentFlags::Tag) != ComponentFlags::None;
    }

    [[nodiscard]] bool IsShared() const
    {
        return (_flags & ComponentFlags::Shared) != ComponentFlags::None;
    }

    [[nodiscard]] bool IsChunkComponent() const
    {
        return (_flags & ComponentFlags::Chunk) != ComponentFlags::None;
    }

    [[nodiscard]] bool IsEnableable() const
    {
        return (_flags & ComponentFlags::Enableable) != ComponentFlags::None;
    }

    /// True if each entity has a value in a column of its chunk: not a tag, shared or chunk component.
    [[nodiscard]] bool HasColumn() const
    {
        return !IsTag() && !IsShared() && !IsChunkComponent();
    }

    [[nodiscard]] bool IsTriviallyCopyable() const
    {
        return _triviallyCopyable;
    }

    /// The byte offsets of the Entity fields in a value, nested structs and fixed arrays included, so that entities
    /// created by a CommandBuffer or an EntityTemplate can be patched in.
    [[nodiscard]] Span<const uint32> GetEntityOffsets() const
    {
        return _entityOffsets.AsSpan();
    }

    void Construct(void *target, uint32 count) const;
    void Destruct(void *target, uint32 count) const;
    void CopyConstruct(void *target, const void *source, uint32 count) const;
    /// Moves *count* values from *source* to *target* and destroys the sources.
    void Relocate(void *target, void *source, uint32 count) const;
    void CopyAssign(void *target, const void *source) const;

private:
    friend struct ComponentRegistryAccess;

    const TypeInfo *_type = nullptr;
    ComponentId _id = InvalidComponentId;
    uint32 _size = 0;
    uint32 _alignment = 1;
    ComponentFlags _flags = ComponentFlags::None;
    bool _triviallyCopyable = false;
    bool _triviallyDestructible = false;
    List<uint32> _entityOffsets;
};

/// The component registry. A type marked `MSTRUCT(Component)` registers on first use; script components register
/// their dynamic TypeInfo. Ids are never reused.
namespace Components
{
/// The component of *type*, registering it if it is marked Component; null for any other type.
[[nodiscard]] WORLD_API const ComponentType *Find(const TypeInfo &type);
[[nodiscard]] WORLD_API const ComponentType &Get(ComponentId id);
/// Registers a type described at run time, such as a script component whose layout the VM computed
/// (Documentation/Architecture/08 §5).
WORLD_API const ComponentType &RegisterDynamic(const TypeInfo &type);
[[nodiscard]] WORLD_API uint32 GetCount();

namespace Private
{
template <typename T> inline Atomic<ComponentId> CachedId{InvalidComponentId};
} // namespace Private

template <typename T> [[nodiscard]] ComponentId IdOf()
{
    ComponentId id = Private::CachedId<T>.Load(MemoryOrder::Relaxed);
    if (id == InvalidComponentId)
    {
        const ComponentType *type = Find(TypeOf<T>());
        MARLIN_ASSERT(type != nullptr);
        id = type->GetId();
        Private::CachedId<T>.Store(id, MemoryOrder::Relaxed);
    }
    return id;
}
} // namespace Components
} // namespace Marlin
