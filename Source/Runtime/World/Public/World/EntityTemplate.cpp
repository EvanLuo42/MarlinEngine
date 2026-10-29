#include "World/EntityTemplate.h"

#include "Asset/AssetBlock.h"
#include "Containers/HashMap.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "World/EntityManager.h"
#include "World/World.h"

namespace Marlin
{
namespace
{
/// Strong references keep their asset loaded, so they are listed as links rather than stored in the bytes; a soft
/// reference is a Guid and stays in the bytes.
bool IsStrongLink(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::ObjectLink && type.GetExtension() == &Private::StrongAssetLink;
}

void CollectStrongLinks(const TypeInfo &type, uint32 base, List<uint32> &offsets)
{
    for (const FieldInfo *field : type.GetFields())
    {
        const TypeInfo *fieldType = &field->GetType();
        const uint32 offset = base + field->GetOffset();
        const uint32 count = fieldType->GetKind() == TypeKind::FixedArray ? fieldType->GetFixedCount() : 1;
        const uint32 stride = fieldType->GetKind() == TypeKind::FixedArray ? fieldType->GetElementType()->GetSize() : 0;
        fieldType = fieldType->GetKind() == TypeKind::FixedArray ? fieldType->GetElementType() : fieldType;
        for (uint32 i = 0; i < count; ++i)
        {
            if (IsStrongLink(*fieldType))
            {
                offsets.Add(offset + i * stride);
            }
            else if (fieldType->GetKind() == TypeKind::Struct)
            {
                CollectStrongLinks(*fieldType, offset + i * stride, offsets);
            }
        }
    }
}

/// True if values of *type* are plain data a template can hold; otherwise writes a line per field that is not, named
/// from *path*.
bool CheckStorable(const TypeInfo &type, StringView path, StringWriter *problems)
{
    bool storable = true;
    for (const FieldInfo *field : type.GetFields())
    {
        StringBuilder<128> fieldPath;
        FormatTo(fieldPath, "{0}.{1}", path, field->GetName());
        const TypeInfo *fieldType = &field->GetType();
        while (fieldType->GetKind() == TypeKind::FixedArray)
        {
            fieldType = fieldType->GetElementType();
        }
        if (fieldType->GetKind() == TypeKind::Struct)
        {
            storable = CheckStorable(*fieldType, fieldPath.AsView(), problems) && storable;
        }
        else if (!fieldType->IsPrimitive() && fieldType->GetKind() != TypeKind::Enum &&
                 fieldType->GetKind() != TypeKind::ObjectLink)
        {
            if (problems != nullptr)
            {
                FormatTo(*problems, "{0} is a {1}, which an entity template cannot hold\n", fieldPath.AsView(),
                         fieldType->GetName());
            }
            storable = false;
        }
    }
    return storable;
}

/// Appends *count* values of *type* from *source*, the first of them row *row* of the column: entity references to
/// captured entities become template indices, others null; strong asset references move to *links*.
void StoreValues(const ComponentType &type, const uint8 *source, uint32 count, uint32 row,
                 const HashMap<Entity, uint32> &indices, List<uint8> &data, List<EntityTemplateLink> &links)
{
    const uint32 size = type.GetSize();
    const usize start = data.Count();
    data.Resize(start + static_cast<usize>(count) * size);
    MemCopy(data.Data() + start, source, static_cast<usize>(count) * size);
    List<uint32> linkOffsets;
    CollectStrongLinks(type.GetTypeInfo(), 0, linkOffsets);
    for (uint32 i = 0; i < count; ++i)
    {
        uint8 *value = data.Data() + start + static_cast<usize>(i) * size;
        for (const uint32 offset : type.GetEntityOffsets())
        {
            auto *entity = reinterpret_cast<Entity *>(value + offset);
            const uint32 *index = entity->IsNull() ? nullptr : indices.Find(*entity);
            *entity = index != nullptr ? Entity{*index, Entity::DeferredGeneration} : Entity();
        }
        for (const uint32 offset : linkOffsets)
        {
            const auto *reference =
                reinterpret_cast<const AssetRef<Asset> *>(source + static_cast<usize>(i) * size + offset);
            if (reference->IsValid())
            {
                EntityTemplateLink &link = links.AddDefaulted();
                link.Offset = (row + i) * size + offset;
                link.Target = *reference;
            }
            MemZero(value + offset, sizeof(AssetRef<Asset>));
        }
    }
}

/// Copies *count* values, from row *row* of *column*, over default values at *target*: template indices become the
/// created entities and links become asset references.
void LoadValues(const ComponentType &type, const EntityTemplateColumn &column, uint32 row, uint32 count, uint8 *target,
                Span<const Entity> created)
{
    const uint32 size = type.GetSize();
    MemCopy(target, column.Data.AsSpan().Data() + static_cast<usize>(row) * size, static_cast<usize>(count) * size);
    for (uint32 i = 0; i < count; ++i)
    {
        for (const uint32 offset : type.GetEntityOffsets())
        {
            auto *entity = reinterpret_cast<Entity *>(target + static_cast<usize>(i) * size + offset);
            if (entity->IsDeferred())
            {
                *entity = entity->Index < created.Count() ? created[entity->Index] : Entity();
            }
        }
    }
    const uint32 begin = row * size;
    const uint32 end = (row + count) * size;
    for (const EntityTemplateLink &link : column.Links)
    {
        if (link.Offset >= begin && link.Offset < end)
        {
            // The bytes hold a null reference, so assigning takes a reference of its own.
            *reinterpret_cast<AssetRef<Asset> *>(target + (link.Offset - begin)) = link.Target;
        }
    }
}

bool SameIndices(Span<const uint32> a, Span<const uint32> b)
{
    if (a.Count() != b.Count())
    {
        return false;
    }
    for (usize i = 0; i < a.Count(); ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

void FillColumn(EntityTemplateColumn &column, const ComponentType &type, List<uint8> &data,
                List<EntityTemplateLink> &links)
{
    column.Component = type.GetTypeInfo().GetName();
    column.LayoutHash = AssetBlock::GetLayoutHash(type.GetTypeInfo());
    column.Data = FrozenArray<uint8>(data.AsSpan());
    column.Links = FrozenArray<EntityTemplateLink>(links.AsSpan());
    data.Clear();
    links.Clear();
}
} // namespace

bool EntityTemplate::CanStore(const TypeInfo &type, StringWriter *problems)
{
    return CheckStorable(type, type.GetName(), problems);
}

void EntityTemplate::PostLoad()
{
    Resolve();
}

void EntityTemplate::Resolve()
{
    _resolved.Clear();
    _usable = true;
    const auto resolve = [&](StringView name) -> const ComponentType * {
        const TypeInfo *type = Reflection::FindType(name);
        const ComponentType *component = type != nullptr ? Components::Find(*type) : nullptr;
        if (component == nullptr)
        {
            MARLIN_LOG(LogWorld, Error, "Entity template {0} uses {1}, which is not a linked component", Name.AsView(),
                       name);
            _usable = false;
        }
        return component;
    };
    const auto resolveColumns = [&](const FrozenArray<EntityTemplateColumn> &columns, List<ResolvedColumn> &resolved) {
        for (const EntityTemplateColumn &column : columns)
        {
            const ComponentType *component = resolve(column.Component.AsView());
            if (component != nullptr && AssetBlock::GetLayoutHash(component->GetTypeInfo()) != column.LayoutHash)
            {
                MARLIN_LOG(LogWorld, Error, "Entity template {0} was made with another layout of {1}; convert it again",
                           Name.AsView(), column.Component.AsView());
                _usable = false;
            }
            resolved.Add({component});
        }
    };
    for (const EntityTemplateGroup &group : Groups)
    {
        ResolvedGroup &resolved = _resolved.AddDefaulted();
        for (const FrozenString &name : group.Components)
        {
            if (const ComponentType *component = resolve(name.AsView()))
            {
                resolved.Components.Add(component->GetId());
            }
        }
        resolveColumns(group.Columns, resolved.Columns);
        resolveColumns(group.SharedValues, resolved.SharedValues);
    }
}

bool EntityTemplate::Capture(const EntityManager &manager, Span<const Entity> entities, StringWriter *problems)
{
    HashMap<Entity, uint32> indices;
    for (uint32 i = 0; i < entities.Count(); ++i)
    {
        indices.Set(entities[i], i);
    }

    // Entities group by archetype and shared values, in the order they were given.
    List<const Archetype *> archetypes;
    List<List<uint32>> sharedValues;
    List<List<uint32>> members;
    List<uint32> shared;
    bool storable = true;
    for (uint32 i = 0; i < entities.Count(); ++i)
    {
        const EntityLocation location = manager.GetLocation(entities[i]);
        if (location.Chunk == nullptr)
        {
            if (problems != nullptr)
            {
                FormatTo(*problems, "Entity {0} does not exist\n", entities[i].Index);
            }
            storable = false;
            continue;
        }
        const Archetype &archetype = location.Chunk->GetArchetype();
        shared.Clear();
        for (uint32 s = 0; s < archetype.GetSharedComponents().Count(); ++s)
        {
            shared.Add(location.Chunk->GetSharedValueIndex(s));
        }
        usize group = 0;
        while (group < archetypes.Count() &&
               (archetypes[group] != &archetype || !SameIndices(sharedValues[group].AsSpan(), shared.AsSpan())))
        {
            ++group;
        }
        if (group == archetypes.Count())
        {
            archetypes.Add(&archetype);
            sharedValues.Add(shared);
            members.AddDefaulted();
            for (const ArchetypeColumn &column : archetype.GetColumns())
            {
                storable = CanStore(column.Type->GetTypeInfo(), problems) && storable;
            }
            for (const ArchetypeChunkValue &value : archetype.GetSharedComponents())
            {
                storable = CanStore(value.Type->GetTypeInfo(), problems) && storable;
            }
        }
        members[group].Add(i);
    }
    if (!storable)
    {
        return false;
    }

    EntityCount = static_cast<uint32>(entities.Count());
    List<EntityTemplateGroup> groups;
    List<uint8> data;
    List<EntityTemplateLink> links;
    for (usize g = 0; g < archetypes.Count(); ++g)
    {
        const Archetype &archetype = *archetypes[g];
        EntityTemplateGroup &group = groups.AddDefaulted();
        List<FrozenString> names;
        for (const ComponentId component : archetype.GetComponents())
        {
            names.Add(FrozenString(Components::Get(component).GetTypeInfo().GetName()));
        }
        group.Components = FrozenArray<FrozenString>(names.AsSpan());
        group.Entities = FrozenArray<uint32>(members[g].AsSpan());

        List<EntityTemplateColumn> columns;
        for (uint32 c = 0; c < archetype.GetColumns().Count(); ++c)
        {
            const ComponentType &type = *archetype.GetColumns()[c].Type;
            for (uint32 row = 0; row < members[g].Count(); ++row)
            {
                const EntityLocation location = manager.GetLocation(entities[members[g][row]]);
                const auto *source = static_cast<const uint8 *>(location.Chunk->GetColumnData(c)) +
                                     static_cast<usize>(location.Row) * type.GetSize();
                StoreValues(type, source, 1, row, indices, data, links);
            }
            FillColumn(columns.AddDefaulted(), type, data, links);
        }
        group.Columns = FrozenArray<EntityTemplateColumn>(columns.AsSpan());

        columns.Clear();
        for (uint32 s = 0; s < archetype.GetSharedComponents().Count(); ++s)
        {
            const ComponentType &type = *archetype.GetSharedComponents()[s].Type;
            const auto *value = static_cast<const uint8 *>(manager.GetSharedValue(type.GetId(), sharedValues[g][s]));
            StoreValues(type, value, 1, 0, indices, data, links);
            FillColumn(columns.AddDefaulted(), type, data, links);
        }
        group.SharedValues = FrozenArray<EntityTemplateColumn>(columns.AsSpan());
    }
    Groups = FrozenArray<EntityTemplateGroup>(groups.AsSpan());
    Resolve();
    return true;
}

bool EntityTemplate::Instantiate(EntityManager &manager, Span<Entity> entities) const
{
    MARLIN_ASSERT(entities.Count() == EntityCount);
    if (!_usable || entities.Count() != EntityCount)
    {
        return false;
    }
    // Create every entity first, so that values can refer to entities of any group.
    List<Entity> created;
    List<uint32> shared;
    for (usize g = 0; g < Groups.Count(); ++g)
    {
        const EntityTemplateGroup &group = Groups[g];
        const ResolvedGroup &resolved = _resolved[g];
        Archetype &archetype = manager.GetOrCreateArchetype(resolved.Components.AsSpan());
        shared.Clear();
        for (const ArchetypeChunkValue &value : archetype.GetSharedComponents())
        {
            const usize index = resolved.SharedValues.FindIndex(
                [&](const ResolvedColumn &column) { return column.Type == value.Type; });
            const ReflectedValue initial(value.Type->GetTypeInfo());
            if (index != List<ResolvedColumn>::NotFound)
            {
                LoadValues(*value.Type, group.SharedValues[index], 0, 1, static_cast<uint8 *>(initial.Get()), entities);
            }
            shared.Add(manager.FindOrAddSharedValue(value.Type->GetId(), initial.Get()));
        }
        created.Resize(group.Entities.Count());
        manager.CreateEntities(archetype, created.AsSpan(), shared.AsSpan());
        for (usize i = 0; i < created.Count(); ++i)
        {
            entities[group.Entities[i]] = created[i];
        }
    }

    for (usize g = 0; g < Groups.Count(); ++g)
    {
        const EntityTemplateGroup &group = Groups[g];
        const ResolvedGroup &resolved = _resolved[g];
        for (usize c = 0; c < group.Columns.Count(); ++c)
        {
            const ComponentType &type = *resolved.Columns[c].Type;
            // Consecutive entities of a group sit in consecutive rows of a chunk: copy each run at once.
            uint32 row = 0;
            while (row < group.Entities.Count())
            {
                const EntityLocation first = manager.GetLocation(entities[group.Entities[row]]);
                uint32 count = 1;
                while (row + count < group.Entities.Count())
                {
                    const EntityLocation next = manager.GetLocation(entities[group.Entities[row + count]]);
                    if (next.Chunk != first.Chunk || next.Row != first.Row + count)
                    {
                        break;
                    }
                    ++count;
                }
                const int32 column = first.Chunk->GetArchetype().FindColumn(type.GetId());
                auto *target = static_cast<uint8 *>(first.Chunk->GetColumnData(static_cast<uint32>(column))) +
                               static_cast<usize>(first.Row) * type.GetSize();
                type.Destruct(target, count);
                LoadValues(type, group.Columns[c], row, count, target, entities);
                row += count;
            }
        }
    }
    return true;
}
} // namespace Marlin
