#pragma once

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Containers/FrozenArray.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "World/Component.h"
#include "World/Entity.h"
#include "World/EntityTemplate.gen.h"

namespace Marlin
{
class EntityManager;
class StringWriter;

/// An asset reference inside a column's bytes, where a byte copy cannot carry it.
MSTRUCT()
struct EntityTemplateLink
{
    MGENERATED_BODY()

    /// From the start of the column's Data.
    MPROPERTY()
    uint32 Offset = 0;

    MPROPERTY()
    AssetRef<Asset> Target;
};

/// One component's values for every entity of a group, laid out as the component; strong asset references are zero
/// in the bytes and listed in Links, entity references hold template indices (Entity::DeferredGeneration).
MSTRUCT()
struct EntityTemplateColumn
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Component;

    /// AssetBlock::GetLayoutHash of the component when the template was made; a mismatch refuses the template.
    MPROPERTY()
    uint64 LayoutHash = 0;

    MPROPERTY()
    FrozenArray<uint8> Data;

    MPROPERTY()
    FrozenArray<EntityTemplateLink> Links;
};

/// Entities of one archetype with the same shared values.
MSTRUCT()
struct EntityTemplateGroup
{
    MGENERATED_BODY()

    /// Every component of the archetype, tags and shared ones included.
    MPROPERTY()
    FrozenArray<FrozenString> Components;

    /// The template index of each entity, in row order.
    MPROPERTY()
    FrozenArray<uint32> Entities;

    MPROPERTY()
    FrozenArray<EntityTemplateColumn> Columns;

    /// One value per shared component.
    MPROPERTY()
    FrozenArray<EntityTemplateColumn> SharedValues;
};

/// What a Prefab or a level becomes at conversion (Documentation/Architecture/05 §5): there is no prefab at run time,
/// only entities laid out by archetype, ready to be copied into chunks a column at a time. Components in a template
/// must be plain data: numbers, enums, structs and fixed arrays of them, entities and asset references.
MCLASS()
class WORLD_API EntityTemplate final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    uint32 EntityCount = 0;

    MPROPERTY()
    FrozenArray<EntityTemplateGroup> Groups;

    void PostLoad() override;

    /// Fills the template from *entities* of *manager*; an entity reference to one of them becomes its template
    /// index, any other becomes null. False if a component cannot be stored, with the reasons written to *problems*.
    /// Chunk components are not captured.
    bool Capture(const EntityManager &manager, Span<const Entity> entities, StringWriter *problems = nullptr);

    /// False if a component is not linked or its layout changed since the template was made.
    [[nodiscard]] bool IsUsable() const
    {
        return _usable;
    }

    /// Creates EntityCount entities in *manager*; *entities* gets them by template index.
    bool Instantiate(EntityManager &manager, Span<Entity> entities) const;

    /// True if components of *type* can be stored in a template; otherwise writes why to *problems*.
    static bool CanStore(const TypeInfo &type, StringWriter *problems = nullptr);

private:
    struct ResolvedColumn
    {
        const ComponentType *Type = nullptr;
    };

    struct ResolvedGroup
    {
        List<ComponentId> Components;
        List<ResolvedColumn> Columns;
        List<ResolvedColumn> SharedValues;
    };

    void Resolve();

    List<ResolvedGroup> _resolved;
    bool _usable = false;
};
} // namespace Marlin
