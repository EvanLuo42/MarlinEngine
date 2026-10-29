#include "WorldPipeline/PrefabSource.h"

#include "AssetPipeline/AssetConverter.h"
#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "String/Format.h"
#include "World/EntityManager.h"
#include "World/Hierarchy.h"

namespace Marlin
{
namespace
{
// RECOVERY-DECISION WP-D1: nested instances, inheritance chains and entity bases stop at 32 levels.
constexpr uint32 MaxNesting = 32;

/// A link, or a UUID written as a string, such as an override's Target.
bool ReadGuid(const MarlinTextValue &value, Guid &guid)
{
    if (value.Kind == MarlinTextValueKind::ObjectLink)
    {
        guid = value.LinkGuid;
        return guid.IsValid();
    }
    return value.Kind == MarlinTextValueKind::String && Guid::TryParse(value.Text, guid) && guid.IsValid();
}

/// Components the hierarchy and transform systems compute; a prefab sets the entity's Parent field instead.
bool IsDerivedComponent(const TypeInfo &type)
{
    return &type == &TypeOf<Parent>() || &type == &TypeOf<Children>() || &type == &TypeOf<PreviousParent>() ||
           &type == &TypeOf<WorldTransform>();
}

PrefabComponent *FindComponent(PrefabFlatEntity &entity, const ComponentType &type)
{
    PrefabComponent *found = entity.Components.Find([&](const PrefabComponent &c) { return c.Type == &type; });
    return found;
}
} // namespace

namespace PrefabIds
{
Guid Derive(const Guid &instance, const Guid &entity)
{
    // RECOVERY-DECISION WP-D2: a derived UUID is the XXH3-128 of the two UUIDs' words, so it is stable across
    // platforms and runs and two different pairs practically never collide.
    const uint32 words[8] = {instance.A, instance.B, instance.C, instance.D, entity.A, entity.B, entity.C, entity.D};
    const Hash128 hash = Xxh3Hash128(words, sizeof(words));
    return {static_cast<uint32>(hash.High >> 32), static_cast<uint32>(hash.High), static_cast<uint32>(hash.Low >> 32),
            static_cast<uint32>(hash.Low)};
}
Guid DeriveNamed(const Guid &owner, StringView name)
{
    const Hash128 hash = Xxh3Hash128(name.Data(), name.Length());
    const Guid named = {static_cast<uint32>(hash.High >> 32), static_cast<uint32>(hash.High),
                        static_cast<uint32>(hash.Low >> 32), static_cast<uint32>(hash.Low)};
    return Derive(owner, named);
}
} // namespace PrefabIds

bool PrefabFlattener::Fail(const MarlinTextObject &object, StringView message)
{
    // RECOVERY-DECISION WP-D3: an error names the object as `<Type> <Name>: message`, its UUID if it has no name.
    const StringView name = object.GetName();
    _context->Error(name.IsEmpty() ? Format("{0} {1}: {2}", object.GetTypeName(), object.GetUuid(), message).AsView()
                                   : Format("{0} {1}: {2}", object.GetTypeName(), name, message).AsView());
    return false;
}

const MarlinTextObject *PrefabFlattener::Read(const Guid &uuid)
{
    const MarlinTextObject *object = _context->ReadDependency(uuid);
    if (object == nullptr)
    {
        _context->Error(Format("prefab object {0} cannot be found", uuid).AsView());
    }
    return object;
}

bool PrefabFlattener::ResolveLinks(const MarlinTextObject &object, StringView key, List<Guid> &links, uint32 depth)
{
    // The links of the object this one overrides come first, as MarlinText inheritance reads them: `=` replaces
    // them, `+=` adds to them and `-=` removes from them.
    if (const MarlinTextValue *base = object.GetParent())
    {
        if (depth >= MaxNesting)
        {
            return Fail(object, "prefabs inherit too deep or in a cycle");
        }
        const MarlinTextObject *parent = Read(base->LinkGuid);
        if (parent == nullptr || !ResolveLinks(*parent, key, links, depth + 1))
        {
            return false;
        }
    }
    for (const MarlinTextField &field : object.GetFields())
    {
        if (field.IsDirective || field.Condition != nullptr || field.Key != key)
        {
            continue;
        }
        if (field.Value.Kind != MarlinTextValueKind::Array)
        {
            return Fail(object, Format("{0} must be an array of links", key).AsView());
        }
        if (field.Operation == MarlinTextOperation::Assign)
        {
            links.Clear();
        }
        for (const MarlinTextValue &element : field.Value.Elements)
        {
            if (element.Kind != MarlinTextValueKind::ObjectLink || !element.LinkGuid.IsValid())
            {
                return Fail(object, Format("{0} holds something that is not a link", key).AsView());
            }
            if (field.Operation == MarlinTextOperation::Remove)
            {
                (void)links.Remove(element.LinkGuid);
            }
            else
            {
                links.Add(element.LinkGuid);
            }
        }
    }
    return true;
}

bool PrefabFlattener::ReadComponents(const MarlinTextValue &block, PrefabFlatEntity &entity)
{
    if (block.Kind != MarlinTextValueKind::Block)
    {
        _context->Error(Format("{0}: Components must be a block of `Type {{ ... }}`", entity.Name).AsView());
        return false;
    }
    for (const MarlinTextField &field : block.Fields)
    {
        if (field.IsDirective || field.Condition != nullptr)
        {
            continue;
        }
        const TypeInfo *type = Reflection::FindType(field.Key);
        const ComponentType *component = type != nullptr ? Components::Find(*type) : nullptr;
        if (component == nullptr)
        {
            _context->Error(Format("{0}: {1} is not a component", entity.Name, field.Key).AsView());
            return false;
        }
        if (IsDerivedComponent(*type))
        {
            _context->Error(
                Format("{0}: {1} is computed; set the entity's Parent field instead", entity.Name, field.Key).AsView());
            return false;
        }
        // A component the entity already has, from its base or the prefab an override changes, keeps the fields
        // this block leaves out.
        PrefabComponent *target = FindComponent(entity, *component);
        if (target == nullptr)
        {
            target = &entity.Components.AddDefaulted();
            target->Type = component;
            target->Value = MakeUnique<ReflectedValue>(*type);
        }
        MarlinTextDiagnostics diagnostics;
        if (!MarlinTextSerializer::ReadValue(field.Value, *type, target->Value->Get(), diagnostics) ||
            diagnostics.HasErrors())
        {
            _context->Error(diagnostics.ToString(entity.Name.AsView()).AsView());
            return false;
        }
    }
    return true;
}

bool PrefabFlattener::ReadEntity(const MarlinTextObject &object, PrefabFlatEntity &entity, uint32 depth)
{
    // An entity that overrides another starts from it.
    if (const MarlinTextValue *base = object.GetParent())
    {
        if (depth >= MaxNesting)
        {
            return Fail(object, "entities inherit too deep or in a cycle");
        }
        const MarlinTextObject *parent = Read(base->LinkGuid);
        if (parent == nullptr || !ReadEntity(*parent, entity, depth + 1))
        {
            return false;
        }
    }
    entity.Id = object.GetUuid();
    entity.Name = object.GetName().IsEmpty() ? Format("{0}", entity.Id) : String(object.GetName());
    if (const MarlinTextField *parent = object.Find("Parent"))
    {
        if (!ReadGuid(parent->Value, entity.Parent))
        {
            return Fail(object, "Parent must be a link or a UUID");
        }
    }
    if (const MarlinTextField *grid = object.Find("Grid"))
    {
        if (grid->Value.Kind != MarlinTextValueKind::String)
        {
            return Fail(object, "Grid must be the name of a grid of the level's Partition");
        }
        entity.Grid = grid->Value.Text;
    }
    const MarlinTextField *components = object.Find("Components");
    return components == nullptr || ReadComponents(components->Value, entity);
}

bool PrefabFlattener::ApplyOverride(const MarlinTextObject &instance, const Guid &instanceId,
                                    const MarlinTextObject &override, List<PrefabFlatEntity> &entities)
{
    // The override belongs to the instance, or to a prefab variant along its inheritance chain whose overrides the
    // instance inherits.
    Guid owner;
    bool owned = false;
    if (const MarlinTextField *field = override.Find("Instance"); field != nullptr && ReadGuid(field->Value, owner))
    {
        const MarlinTextObject *object = &instance;
        for (uint32 depth = 0; object != nullptr && depth < MaxNesting && !owned; ++depth)
        {
            owned = object->GetUuid() == owner;
            const MarlinTextValue *base = object->GetParent();
            object = !owned && base != nullptr ? Read(base->LinkGuid) : nullptr;
        }
    }
    if (!owned)
    {
        return Fail(override, Format("it overrides another instance than {0}", instance.GetName()).AsView());
    }
    Guid target;
    const MarlinTextField *targetField = override.Find("Target");
    if (targetField == nullptr || !ReadGuid(targetField->Value, target))
    {
        return Fail(override, "Target must name the entity it overrides");
    }
    // The override has the UUID of the entity it changes, so tools find it from the entity.
    if (override.GetUuid() != PrefabIds::Derive(owner, target))
    {
        return Fail(override, "its UUID must be PrefabIds::Derive(Instance, Target)");
    }
    const Guid id = PrefabIds::Derive(instanceId, target);
    PrefabFlatEntity *entity = entities.Find([&](const PrefabFlatEntity &e) { return e.Id == id; });
    if (entity == nullptr)
    {
        return Fail(override, Format("{0} has no entity {1}", instance.GetName(), target).AsView());
    }
    if (const MarlinTextField *removed = override.Find("RemoveComponents"))
    {
        if (removed->Value.Kind != MarlinTextValueKind::Array)
        {
            return Fail(override, "RemoveComponents must be an array of component names");
        }
        for (const MarlinTextValue &name : removed->Value.Elements)
        {
            const bool named = name.Kind == MarlinTextValueKind::Identifier || name.Kind == MarlinTextValueKind::String;
            const TypeInfo *type = named ? Reflection::FindType(name.Text) : nullptr;
            const ComponentType *component = type != nullptr ? Components::Find(*type) : nullptr;
            if (component == nullptr)
            {
                _context->Error(Format("{0}: {1} is not a component", entity->Name, name.Source).AsView());
                return false;
            }
            const usize index =
                entity->Components.FindIndex([&](const PrefabComponent &c) { return c.Type == component; });
            if (index != List<PrefabComponent>::NotFound)
            {
                entity->Components.RemoveAt(index);
            }
        }
    }
    const MarlinTextField *components = override.Find("Components");
    return components == nullptr || ReadComponents(components->Value, *entity);
}

bool PrefabFlattener::Flatten(const MarlinTextObject &prefab, List<PrefabFlatEntity> &entities)
{
    entities.Clear();
    return FlattenMembers(prefab, entities, 0);
}

bool PrefabFlattener::FlattenMembers(const MarlinTextObject &prefab, List<PrefabFlatEntity> &entities, uint32 depth)
{
    if (depth >= MaxNesting)
    {
        return Fail(prefab, "instances nest too deep or in a cycle");
    }
    // An instance inherits the Members of the prefab it overrides.
    List<Guid> members;
    if (!ResolveLinks(prefab, "Members", members, 0))
    {
        return false;
    }
    const usize first = entities.Count();
    for (const Guid &member : members)
    {
        const MarlinTextObject *object = Read(member);
        if (object == nullptr)
        {
            return false;
        }
        const StringView type = object->GetTypeName();
        if (type == "PrefabEntity")
        {
            if (!ReadEntity(*object, entities.AddDefaulted(), 0))
            {
                return false;
            }
        }
        else if (type == "Prefab")
        {
            if (!FlattenMembers(*object, entities, depth + 1))
            {
                return false;
            }
        }
        else
        {
            return Fail(prefab, "a prefab's Members must be PrefabEntity or Prefab objects");
        }
    }
    if (prefab.GetParent() == nullptr)
    {
        return true;
    }

    // An instance: its entities get UUIDs of their own, so two instances of a prefab never share one, and its roots
    // hang under the instance's Parent.
    const Guid instanceId = prefab.GetUuid();
    Guid root;
    if (const MarlinTextField *parent = prefab.Find("Parent"); parent != nullptr && !ReadGuid(parent->Value, root))
    {
        return Fail(prefab, "Parent must be a link or a UUID");
    }
    for (usize i = first; i < entities.Count(); ++i)
    {
        PrefabFlatEntity &entity = entities[i];
        entity.Id = PrefabIds::Derive(instanceId, entity.Id);
        entity.Parent = entity.Parent.IsValid() ? PrefabIds::Derive(instanceId, entity.Parent) : root;
    }
    List<Guid> overrides;
    if (!ResolveLinks(prefab, "Overrides", overrides, 0))
    {
        return false;
    }
    for (const Guid &uuid : overrides)
    {
        const MarlinTextObject *override = Read(uuid);
        if (override == nullptr)
        {
            return false;
        }
        if (override->GetTypeName() != "PrefabOverride")
        {
            return Fail(prefab, "an instance's Overrides must be PrefabOverride objects");
        }
        if (!ApplyOverride(prefab, instanceId, *override, entities))
        {
            return false;
        }
    }
    return true;
}

bool PrefabFlattener::Instantiate(Span<const PrefabFlatEntity> entities, EntityManager &manager, List<Entity> &created)
{
    created.Clear();
    created.Reserve(entities.Count());
    HashMap<Guid, uint32> indices;
    List<ComponentId> components;
    for (usize i = 0; i < entities.Count(); ++i)
    {
        const PrefabFlatEntity &entity = entities[i];
        if (!indices.TryAdd(entity.Id, static_cast<uint32>(i)))
        {
            // RECOVERY-DECISION WP-D4: a prefab placed twice other than through instances repeats UUIDs; refused.
            _context->Error(Format("{0}: its UUID {1} is in the prefab twice", entity.Name, entity.Id).AsView());
            return false;
        }
        components.Clear();
        for (const PrefabComponent &component : entity.Components)
        {
            components.Add(component.Type->GetId());
        }
        if (entity.Parent.IsValid())
        {
            components.Add(Components::IdOf<Parent>());
        }
        const Entity handle = manager.CreateEntity(components.AsSpan());
        for (const PrefabComponent &component : entity.Components)
        {
            if (!component.Type->IsTag())
            {
                manager.AddComponent(handle, component.Type->GetId(), component.Value->Get());
            }
        }
        created.Add(handle);
    }
    for (usize i = 0; i < entities.Count(); ++i)
    {
        const PrefabFlatEntity &entity = entities[i];
        if (!entity.Parent.IsValid())
        {
            continue;
        }
        // The chain up from the entity must end at a root of the prefab.
        const uint32 *index = indices.Find(entity.Parent);
        for (usize steps = 0; index != nullptr && entities[*index].Parent.IsValid(); ++steps)
        {
            if (steps > entities.Count())
            {
                _context->Error(Format("{0}: its parents form a cycle", entity.Name).AsView());
                return false;
            }
            index = indices.Find(entities[*index].Parent);
        }
        const uint32 *parentIndex = indices.Find(entity.Parent);
        if (index == nullptr || parentIndex == nullptr)
        {
            _context->Error(Format("{0}: its parent {1} is not in the prefab", entity.Name, entity.Parent).AsView());
            return false;
        }
        Parent link;
        link.Value = created[*parentIndex];
        manager.Add(created[i], link);
    }
    return true;
}
} // namespace Marlin
