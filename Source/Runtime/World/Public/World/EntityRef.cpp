#include "World/EntityRef.h"

#include "World/Hierarchy.h"
#include "World/World.h"

namespace Marlin
{
bool EntityRef::IsValid() const
{
    return _world != nullptr && _world->GetEntities().Exists(_entity);
}

const void *EntityRef::GetComponent(ComponentId component) const
{
    return _world != nullptr ? _world->GetEntities().GetComponent(_entity, component) : nullptr;
}

void *EntityRef::GetComponentForWrite(ComponentId component) const
{
    return _world != nullptr ? _world->GetEntities().GetComponentForWrite(_entity, component) : nullptr;
}

bool EntityRef::HasTag(ComponentId component) const
{
    return _world != nullptr && _world->GetEntities().HasComponent(_entity, component);
}

void EntityRef::AddComponent(ComponentId component, const void *value) const
{
    EntityManager &entities = _world->GetEntities();
    if (entities.IsStructureLocked())
    {
        _world->Defer([&](CommandBuffer &commands) { commands.AddComponent(_entity, component, value); });
        return;
    }
    entities.AddComponent(_entity, component, value);
}

void EntityRef::SetComponent(ComponentId component, const void *value) const
{
    EntityManager &entities = _world->GetEntities();
    if (entities.IsStructureLocked())
    {
        _world->Defer([&](CommandBuffer &commands) { commands.SetComponent(_entity, component, value); });
        return;
    }
    entities.SetComponent(_entity, component, value);
}

void EntityRef::RemoveComponent(ComponentId component) const
{
    EntityManager &entities = _world->GetEntities();
    if (entities.IsStructureLocked())
    {
        _world->Defer([&](CommandBuffer &commands) { commands.RemoveComponent(_entity, component); });
        return;
    }
    entities.RemoveComponent(_entity, component);
}

void EntityRef::Destroy() const
{
    EntityManager &entities = _world->GetEntities();
    if (entities.IsStructureLocked())
    {
        _world->Defer([&](CommandBuffer &commands) { commands.DestroyEntity(_entity); });
        return;
    }
    entities.DestroyEntity(_entity);
}

Span<const Entity> EntityRef::GetChildren() const
{
    const Children *children = Get<Children>();
    return children != nullptr ? children->Entities.AsSpan() : Span<const Entity>();
}

EntityRef EntityRef::GetParent() const
{
    const Parent *parent = Get<Parent>();
    return parent != nullptr ? EntityRef(*_world, parent->Value) : EntityRef();
}

void EntityRef::SetParent(Entity parent) const
{
    if (parent.IsNull())
    {
        if (Has<Parent>())
        {
            Remove<Parent>();
        }
        return;
    }
    if (Has<Parent>())
    {
        Set(Parent{parent});
    }
    else
    {
        Add(Parent{parent});
    }
}
} // namespace Marlin
