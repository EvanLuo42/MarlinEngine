#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "Reflection/Reflection.h"
#include "String/String.h"
#include "World/Component.h"
#include "World/Entity.h"

namespace Marlin
{
class ConversionContext;
class EntityManager;
class MarlinTextObject;
struct MarlinTextValue;

/// The UUIDs of what a prefab becomes: the same source always gives the same UUIDs, so references to an instance's
/// entities, overrides and a level's cells stay stable across conversions.
namespace PrefabIds
{
/// The UUID of the entity *entity* of a prefab once instance *instance* places it.
[[nodiscard]] WORLDPIPELINE_API Guid Derive(const Guid &instance, const Guid &entity);
/// A UUID derived from *owner* for a part of it that has no source object, such as a streaming cell of a level.
[[nodiscard]] WORLDPIPELINE_API Guid DeriveNamed(const Guid &owner, StringView name);
} // namespace PrefabIds

/// One component of a flattened entity: its type and its value as the source and the overrides set it.
struct PrefabComponent
{
    const ComponentType *Type = nullptr;
    UniquePtr<ReflectedValue> Value;
};

/// An entity of a prefab with every nested instance and override applied.
struct PrefabFlatEntity
{
    Guid Id;
    /// The entity it is attached to; invalid for a root.
    Guid Parent;
    String Name;
    /// The partition grid a level places the hierarchy under this entity in, if it is a root; empty for the
    /// level's first grid.
    String Grid;
    List<PrefabComponent> Components;
};

/// Reads the source objects of a prefab (Documentation/Architecture/05 §5) into a flat list of entities:
///
///     Prefab { !UUID = "..." !Name = "Crate" Members = [<#Body>, <#Wheel_1>] }
///     PrefabEntity { !UUID = "..." !Name = "Body" Parent = <#...> Grid = "Detail"
///                    Components = { LocalTransform = { Translation = (0, 1, 0) } } }
///     Prefab : <#Wheel> { !UUID = "..." !Name = "Wheel_1" Parent = <#Body> Overrides = [<#...>] }
///     PrefabOverride { !UUID = "..." Instance = <#Wheel_1> Target = "<UUID of the entity in Wheel>"
///                      Components = { Wheel = { Spokes = 8 } } RemoveComponents = [Spinning] }
///
/// Members are PrefabEntity objects and nested Prefab objects. A Prefab that overrides another (`: <#Base>`) is an
/// instance of it: it inherits the base's Members, its entities get the UUIDs PrefabIds::Derive(instance, entity),
/// its roots are attached to the instance's Parent, and its Overrides change the components of single entities.
/// An override's own UUID is PrefabIds::Derive(Instance, Target), the UUID of the entity it changes. Parent, Children,
/// PreviousParent and WorldTransform are computed and cannot be written as components. Every object read is
/// recorded as an input of the conversion.
class WORLDPIPELINE_API PrefabFlattener
{
public:
    explicit PrefabFlattener(ConversionContext &context) : _context(&context)
    {
    }

    /// Flattens *prefab*, a Prefab or a Level, into *entities*; false with errors on the context if a source
    /// object is malformed.
    bool Flatten(const MarlinTextObject &prefab, List<PrefabFlatEntity> &entities);

    /// Creates *entities* in *manager*, attached through Parent, with *created* in the same order.
    bool Instantiate(Span<const PrefabFlatEntity> entities, EntityManager &manager, List<Entity> &created);

private:
    bool FlattenMembers(const MarlinTextObject &prefab, List<PrefabFlatEntity> &entities, uint32 depth);
    bool ApplyOverride(const MarlinTextObject &instance, const Guid &instanceId, const MarlinTextObject &override,
                       List<PrefabFlatEntity> &entities);
    bool ReadEntity(const MarlinTextObject &object, PrefabFlatEntity &entity, uint32 depth);
    bool ReadComponents(const MarlinTextValue &block, PrefabFlatEntity &entity);
    bool ResolveLinks(const MarlinTextObject &object, StringView key, List<Guid> &links, uint32 depth);
    const MarlinTextObject *Read(const Guid &uuid);
    bool Fail(const MarlinTextObject &object, StringView message);

    ConversionContext *_context;
};
} // namespace Marlin
