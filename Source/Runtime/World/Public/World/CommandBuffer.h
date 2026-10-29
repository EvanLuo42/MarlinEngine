#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/Arena.h"
#include "World/Component.h"
#include "World/Entity.h"

namespace Marlin
{
class EntityManager;

/// Structural changes recorded while systems run and played back at the sync point after their phase
/// (Documentation/Architecture/05 §2). CreateEntity returns a deferred entity that later commands, and Entity
/// fields of recorded values, may refer to; playback replaces it with the created one. Commands on entities that
/// no longer exist are skipped.
class WORLD_API CommandBuffer
{
public:
    CommandBuffer();
    ~CommandBuffer();
    MARLIN_NONCOPYABLE(CommandBuffer);

    /// A deferred entity: valid only in this buffer's commands until playback.
    Entity CreateEntity(Span<const ComponentId> components = {});
    void DestroyEntity(Entity entity);
    /// Copies *value* now; null records a default value.
    void AddComponent(Entity entity, ComponentId component, const void *value = nullptr);
    void RemoveComponent(Entity entity, ComponentId component);
    void SetComponent(Entity entity, ComponentId component, const void *value);
    void SetEnabled(Entity entity, ComponentId component, bool enabled);

    template <typename T> void Add(Entity entity, const T &value = T())
    {
        AddComponent(entity, Components::IdOf<T>(), &value);
    }

    template <typename T> void Remove(Entity entity)
    {
        RemoveComponent(entity, Components::IdOf<T>());
    }

    template <typename T> void Set(Entity entity, const T &value)
    {
        SetComponent(entity, Components::IdOf<T>(), &value);
    }

    template <typename T> void SetEnabled(Entity entity, bool enabled)
    {
        SetEnabled(entity, Components::IdOf<T>(), enabled);
    }

    [[nodiscard]] bool IsEmpty() const
    {
        return _commands.IsEmpty();
    }

    /// Applies the commands in the order they were recorded, then clears the buffer.
    void Playback(EntityManager &entities);
    void Clear();

private:
    enum class CommandKind : uint8
    {
        Create,
        Destroy,
        Add,
        Remove,
        Set,
        Enable,
        Disable,
    };

    struct Command
    {
        CommandKind Kind = CommandKind::Create;
        Entity Target;
        /// The component, or for Create the number of components in Value.
        ComponentId Component = InvalidComponentId;
        /// A copy of the value, or for Create the component ids; null for none.
        void *Value = nullptr;
    };

    void *CopyValue(ComponentId component, const void *value);
    /// The end of the run of Add and Remove commands that can move their entity at once, starting at *first*.
    [[nodiscard]] usize FindRunEnd(usize first) const;

    List<Command> _commands;
    Arena _values;
    uint32 _deferredCount = 0;
};
} // namespace Marlin
