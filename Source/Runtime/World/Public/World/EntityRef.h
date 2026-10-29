#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "World/Component.h"
#include "World/Entity.h"

namespace Marlin
{
class World;

/// An entity and its world, for code outside systems: tools, scripts and gameplay logic
/// (Documentation/Architecture/05 §4). Reads look the entity up; changes apply at once, or go to the world's
/// deferred CommandBuffer while systems run.
class WORLD_API EntityRef
{
public:
    EntityRef() = default;

    EntityRef(World &world, Entity entity) : _world(&world), _entity(entity)
    {
    }

    [[nodiscard]] Entity GetEntity() const
    {
        return _entity;
    }

    [[nodiscard]] World *GetWorld() const
    {
        return _world;
    }

    /// True if the entity exists.
    [[nodiscard]] bool IsValid() const;

    [[nodiscard]] const void *GetComponent(ComponentId component) const;
    [[nodiscard]] void *GetComponentForWrite(ComponentId component) const;
    void AddComponent(ComponentId component, const void *value = nullptr) const;
    void SetComponent(ComponentId component, const void *value) const;
    void RemoveComponent(ComponentId component) const;
    void Destroy() const;

    /// The entities attached to this one, as HierarchySystem last saw them.
    [[nodiscard]] Span<const Entity> GetChildren() const;
    [[nodiscard]] EntityRef GetParent() const;
    /// Attaches the entity to *parent*, or detaches it if *parent* is null.
    void SetParent(Entity parent) const;

    template <typename T> [[nodiscard]] const T *Get() const
    {
        return static_cast<const T *>(GetComponent(Components::IdOf<T>()));
    }

    template <typename T> [[nodiscard]] T *GetForWrite() const
    {
        return static_cast<T *>(GetComponentForWrite(Components::IdOf<T>()));
    }

    template <typename T> [[nodiscard]] bool Has() const
    {
        return HasTag(Components::IdOf<T>());
    }

    template <typename T> void Add(const T &value = T()) const
    {
        AddComponent(Components::IdOf<T>(), &value);
    }

    template <typename T> void Set(const T &value) const
    {
        SetComponent(Components::IdOf<T>(), &value);
    }

    template <typename T> void Remove() const
    {
        RemoveComponent(Components::IdOf<T>());
    }

    bool operator==(const EntityRef &) const = default;

private:
    [[nodiscard]] bool HasTag(ComponentId component) const;

    World *_world = nullptr;
    Entity _entity;
};
} // namespace Marlin
