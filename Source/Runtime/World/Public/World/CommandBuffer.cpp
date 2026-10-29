#include "World/CommandBuffer.h"

#include "Containers/HashMap.h"
#include "Memory/MemoryOps.h"
#include "World/EntityManager.h"

namespace Marlin
{
CommandBuffer::CommandBuffer() : _values(usize{4} * 1024)
{
}

CommandBuffer::~CommandBuffer()
{
    Clear();
}

void *CommandBuffer::CopyValue(ComponentId component, const void *value)
{
    const ComponentType &type = Components::Get(component);
    if (type.GetSize() == 0)
    {
        return nullptr;
    }
    void *copy = _values.Allocate(type.GetSize(), type.GetAlignment() > 8 ? type.GetAlignment() : 8);
    value != nullptr ? type.CopyConstruct(copy, value, 1) : type.Construct(copy, 1);
    return copy;
}

Entity CommandBuffer::CreateEntity(Span<const ComponentId> components)
{
    Command &command = _commands.AddDefaulted();
    command.Kind = CommandKind::Create;
    command.Target = {_deferredCount++, Entity::DeferredGeneration};
    command.Component = static_cast<ComponentId>(components.Count());
    auto *ids = _values.AllocateArray<ComponentId>(components.Count() > 0 ? components.Count() : 1);
    MemCopy(ids, components.Data(), components.SizeInBytes());
    command.Value = ids;
    return command.Target;
}

void CommandBuffer::DestroyEntity(Entity entity)
{
    _commands.Add({CommandKind::Destroy, entity, InvalidComponentId, nullptr});
}

void CommandBuffer::AddComponent(Entity entity, ComponentId component, const void *value)
{
    _commands.Add({CommandKind::Add, entity, component, CopyValue(component, value)});
}

void CommandBuffer::RemoveComponent(Entity entity, ComponentId component)
{
    _commands.Add({CommandKind::Remove, entity, component, nullptr});
}

void CommandBuffer::SetComponent(Entity entity, ComponentId component, const void *value)
{
    _commands.Add({CommandKind::Set, entity, component, CopyValue(component, value)});
}

void CommandBuffer::SetEnabled(Entity entity, ComponentId component, bool enabled)
{
    _commands.Add({enabled ? CommandKind::Enable : CommandKind::Disable, entity, component, nullptr});
}

void CommandBuffer::Clear()
{
    for (const Command &command : _commands)
    {
        if (command.Value != nullptr && (command.Kind == CommandKind::Add || command.Kind == CommandKind::Set))
        {
            Components::Get(command.Component).Destruct(command.Value, 1);
        }
    }
    _commands.Clear();
    _values.Reset();
    _deferredCount = 0;
}

usize CommandBuffer::FindRunEnd(usize first) const
{
    // A run: Add and Remove commands on one target, each component once, no shared Add (that picks a chunk by value).
    const Entity target = _commands[first].Target;
    usize end = first;
    while (end < _commands.Count())
    {
        const Command &command = _commands[end];
        if (command.Target != target || (command.Kind != CommandKind::Add && command.Kind != CommandKind::Remove) ||
            (command.Kind == CommandKind::Add && Components::Get(command.Component).IsShared()))
        {
            break;
        }
        bool repeated = false;
        for (usize i = first; i < end && !repeated; ++i)
        {
            repeated = _commands[i].Component == command.Component;
        }
        if (repeated)
        {
            break;
        }
        ++end;
    }
    return end;
}

void CommandBuffer::Playback(EntityManager &entities)
{
    // The entities created so far, by deferred index.
    List<Entity> created;
    created.Reserve(_deferredCount);
    const auto resolve = [&](Entity entity) -> Entity {
        if (!entity.IsDeferred())
        {
            return entity;
        }
        return entity.Index < created.Count() ? created[entity.Index] : Entity();
    };
    // Recorded values may point at entities created in this buffer.
    const auto patch = [&](const ComponentType &type, void *value) {
        if (value == nullptr)
        {
            return;
        }
        for (const uint32 offset : type.GetEntityOffsets())
        {
            auto *field = reinterpret_cast<Entity *>(static_cast<uint8 *>(value) + offset);
            *field = resolve(*field);
        }
    };
    // The archetype after the run of Add and Remove commands [first, end) on an entity of *archetype*.
    const auto runArchetype = [&](Archetype *archetype, usize first, usize end) {
        for (usize i = first; i < end; ++i)
        {
            const Command &command = _commands[i];
            archetype = command.Kind == CommandKind::Add ? &entities.GetArchetypeWith(*archetype, command.Component)
                                                         : &entities.GetArchetypeWithout(*archetype, command.Component);
        }
        return archetype;
    };
    // The values of the run's Add commands, once the entity is in its final archetype.
    const auto setRunValues = [&](Entity target, usize first, usize end) {
        for (usize i = first; i < end; ++i)
        {
            const Command &command = _commands[i];
            if (command.Kind != CommandKind::Add || command.Value == nullptr)
            {
                continue;
            }
            const ComponentType &type = Components::Get(command.Component);
            patch(type, command.Value);
            if (void *value = entities.GetComponentForWrite(target, command.Component))
            {
                type.CopyAssign(value, command.Value);
            }
        }
    };

    // Adds and removes in a row on one entity, as a Create and its Adds usually come, move it once.
    for (usize index = 0; index < _commands.Count();)
    {
        const Command &command = _commands[index];
        if (command.Kind == CommandKind::Create)
        {
            const Span<const ComponentId> components(static_cast<const ComponentId *>(command.Value),
                                                     command.Component);
            if (created.Count() < command.Target.Index)
            {
                created.Resize(command.Target.Index);
            }
            const usize first = index + 1;
            const usize end = first < _commands.Count() ? FindRunEnd(first) : first;
            if (end == first || _commands[first].Target != command.Target)
            {
                created.Add(entities.CreateEntity(components));
                ++index;
                continue;
            }
            Archetype *archetype = runArchetype(&entities.GetOrCreateArchetype(components), first, end);
            created.Add(entities.CreateEntity(*archetype));
            setRunValues(created.Last(), first, end);
            index = end;
            continue;
        }
        const Entity target = resolve(command.Target);
        if (!entities.Exists(target))
        {
            ++index;
            continue;
        }
        if (command.Kind == CommandKind::Add || command.Kind == CommandKind::Remove)
        {
            const usize end = FindRunEnd(index);
            if (end - index > 1)
            {
                entities.SetArchetype(target, *runArchetype(entities.GetArchetype(target), index, end));
                setRunValues(target, index, end);
                index = end;
                continue;
            }
        }
        switch (command.Kind)
        {
        case CommandKind::Destroy:
            entities.DestroyEntity(target);
            break;
        case CommandKind::Add:
            patch(Components::Get(command.Component), command.Value);
            entities.AddComponent(target, command.Component, command.Value);
            break;
        case CommandKind::Remove:
            entities.RemoveComponent(target, command.Component);
            break;
        case CommandKind::Set:
            patch(Components::Get(command.Component), command.Value);
            if (entities.HasComponent(target, command.Component))
            {
                entities.SetComponent(target, command.Component, command.Value);
            }
            break;
        case CommandKind::Enable:
        case CommandKind::Disable:
            if (entities.HasComponent(target, command.Component))
            {
                entities.SetEnabled(target, command.Component, command.Kind == CommandKind::Enable);
            }
            break;
        case CommandKind::Create:
            break;
        }
        ++index;
    }
    Clear();
}
} // namespace Marlin
