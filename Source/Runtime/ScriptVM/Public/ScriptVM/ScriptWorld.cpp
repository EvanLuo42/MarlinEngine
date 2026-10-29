#include "ScriptVM/ScriptWorld.h"

#include "Concurrency/ScopeLock.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Math/Vector.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Reflection/Reflection.h"
#include "ScriptVM/ScriptDomain.h"
#include "ScriptVM/ScriptSubsystems.h"
#include "String/Format.h"
#include "World/EntityRef.h"
#include "World/System.h"
#include "World/World.h"

namespace Marlin
{
namespace
{
// Temporary decision: at most 1024 script structs in a process. A TypeOperations entry has no context, so each
// struct takes one of this many operation slots.
constexpr uint32 MaxScriptStructs = 1024;

/// Text for the views reflection keeps, which lives as long as the process.
StringView Keep(StringView text)
{
    if (text.IsEmpty())
    {
        return {};
    }
    auto *copy = static_cast<char *>(Memory::Allocate(text.Length(), 1));
    MemCopy(copy, text.Data(), text.Length());
    return StringView(copy, text.Length());
}

/// A script struct as reflection sees it: a component, or a struct a component's field holds.
struct ScriptStruct
{
    StringView Name;
    uint32 Size = 0;
    uint32 Alignment = 1;
    bool Component = false;
    List<FieldInfo> Fields;
    MetadataEntry Meta[1];
    TypeOperations Operations = {};
    TypeInfo *Type = nullptr;
    /// The value a new instance starts as.
    List<uint8> Defaults;
};

struct ScriptSystemParameter
{
    /// InvalidComponentId for the `Entity` parameter.
    ComponentId Component = InvalidComponentId;
    uint32 Size = 0;
    bool Write = false;
    /// An `in` component the function takes as a value, which the system copies into the parameter's registers.
    bool ByValue = false;
};

struct ScriptSystemInfo
{
    StringView Name;
    /// The function's name in the domain, with its parameters.
    String Function;
    List<ScriptSystemParameter> Parameters;
    List<MetadataEntry> Meta;
    TypeOperations Operations = {};
    TypeInfo *Type = nullptr;
};

struct ScriptEcsRegistry
{
    SpinLock Lock;
    HashMap<String, ScriptStruct *> Structs;
    HashMap<String, const ComponentType *> Components;
    HashMap<String, ScriptSystemInfo *> Systems;
    HashMap<const TypeInfo *, ScriptSystemInfo *> SystemsByType;
};

ScriptEcsRegistry &GetRegistry()
{
    static ScriptEcsRegistry *registry = new ScriptEcsRegistry();
    return *registry;
}

ScriptStruct *GStructSlots[MaxScriptStructs] = {};

template <usize TSlot> void ConstructSlot(void *target)
{
    const ScriptStruct &type = *GStructSlots[TSlot];
    MemCopy(target, type.Defaults.Data(), type.Size);
}

template <usize TSlot> void CopySlot(void *target, const void *source)
{
    MemCopy(target, source, GStructSlots[TSlot]->Size);
}

template <usize TSlot> void MoveSlot(void *target, void *source)
{
    MemCopy(target, source, GStructSlots[TSlot]->Size);
}

void DestructNothing(void *)
{
}

struct SlotOperations
{
    void (*Construct)(void *target);
    void (*Copy)(void *target, const void *source);
    void (*Move)(void *target, void *source);
};

template <usize... TSlots> const SlotOperations *GetSlotOperations(IndexSequence<TSlots...>)
{
    static const SlotOperations table[] = {{&ConstructSlot<TSlots>, &CopySlot<TSlots>, &MoveSlot<TSlots>}...};
    return table;
}

uint32 GStructCount = 0;

void *ToParent(void *object)
{
    return object;
}

StringView ShapeName(const ScriptModuleData &module, const ScriptShape &shape)
{
    return shape.Name < module.Strings.Count() ? module.Strings[shape.Name].AsView() : StringView();
}

/// The qualified name of a function, without its parameters.
StringView FunctionName(StringView signature)
{
    const usize open = signature.IndexOf('(');
    return open != StringView::NotFound ? signature.Substring(0, open) : signature;
}

ScriptStruct *RegisterStruct(ScriptEcsRegistry &registry, const ScriptModuleData &module, uint32 shapeIndex,
                             StringView package, bool component, List<String> &errors);

/// The C++ type a field of shape *shapeIndex* is reflected as; null with an error if a component cannot hold it.
const TypeInfo *FieldType(ScriptEcsRegistry &registry, const ScriptModuleData &module, uint32 shapeIndex,
                          StringView package, List<String> &errors)
{
    const ScriptShape &shape = module.Shapes[shapeIndex];
    const TypeInfo *type = nullptr;
    switch (shape.Kind)
    {
    case ScriptShapeKind::Bool:
        type = &TypeOf<bool>();
        break;
    case ScriptShapeKind::Int:
        switch (shape.Width)
        {
        case 8:
            type = shape.Signed ? &TypeOf<int8>() : &TypeOf<uint8>();
            break;
        case 16:
            type = shape.Signed ? &TypeOf<int16>() : &TypeOf<uint16>();
            break;
        case 32:
            type = shape.Signed ? &TypeOf<int32>() : &TypeOf<uint32>();
            break;
        default:
            type = shape.Signed ? &TypeOf<int64>() : &TypeOf<uint64>();
            break;
        }
        break;
    case ScriptShapeKind::Float:
        type = shape.Width == 32 ? &TypeOf<float32>() : shape.Width == 64 ? &TypeOf<float64>() : nullptr;
        break;
    case ScriptShapeKind::Vector: {
        const ScriptShape &element = module.Shapes[shape.Element];
        if (element.Kind == ScriptShapeKind::Float && element.Width == 32)
        {
            type = shape.Count == 2 ? &TypeOf<Vector2>() : shape.Count == 3 ? &TypeOf<Vector3>() : &TypeOf<Vector4>();
        }
        else if (element.Kind == ScriptShapeKind::Int && element.Width == 32 && element.Signed && shape.Count <= 3)
        {
            type = shape.Count == 2 ? &TypeOf<IntVector2>() : &TypeOf<IntVector3>();
        }
        break;
    }
    case ScriptShapeKind::Struct:
        if (ShapeName(module, shape) == "Marlin.Entity")
        {
            type = &TypeOf<Entity>();
        }
        else if (ScriptStruct *nested = RegisterStruct(registry, module, shapeIndex, package, false, errors))
        {
            type = nested->Type;
        }
        else
        {
            return nullptr;
        }
        break;
    default:
        break;
    }
    // The script's layout must be the C++ type's, so that both read the same bytes.
    if (type == nullptr || type->GetSize() != shape.Size)
    {
        return nullptr;
    }
    return type;
}

ScriptStruct *RegisterStruct(ScriptEcsRegistry &registry, const ScriptModuleData &module, uint32 shapeIndex,
                             StringView package, bool component, List<String> &errors)
{
    const ScriptShape &shape = module.Shapes[shapeIndex];
    const StringView name = ShapeName(module, shape);

    // The fields first: an existing type must have the same ones.
    List<const TypeInfo *> fieldTypes;
    bool ok = true;
    for (const ScriptShapeField &field : shape.Fields)
    {
        const TypeInfo *type = FieldType(registry, module, field.Shape, package, errors);
        if (type == nullptr)
        {
            errors.Add(Format("{0}.{1} is not a number, bool, enum, Entity, float2..4, int2..3 or a struct of them, "
                              "which a component can hold",
                              name, module.Strings[field.Name]));
            ok = false;
        }
        fieldTypes.Add(type);
    }
    if (!ok)
    {
        return nullptr;
    }

    if (ScriptStruct **existing = registry.Structs.Find(name))
    {
        ScriptStruct &type = **existing;
        bool same = type.Size == shape.Size && type.Alignment == shape.Alignment &&
                    type.Fields.Count() == shape.Fields.Count() && type.Component == component;
        for (usize i = 0; same && i < shape.Fields.Count(); ++i)
        {
            const FieldInfo &field = type.Fields[i];
            same = field.GetName() == module.Strings[shape.Fields[i].Name].AsView() &&
                   field.GetOffset() == shape.Fields[i].Offset && &field.GetType() == fieldTypes[i];
        }
        if (!same)
        {
            errors.Add(Format("{0} changed its layout or whether it is a component; the process keeps the first one "
                              "until it restarts",
                              name));
            return nullptr;
        }
        return &type;
    }

    if (GStructCount >= MaxScriptStructs)
    {
        errors.Add(Format("{0} is one more script struct than the {1} a process can hold", name, MaxScriptStructs));
        return nullptr;
    }
    auto *type = new ScriptStruct();
    type->Name = Keep(name);
    type->Size = shape.Size;
    type->Alignment = shape.Alignment > 0 ? shape.Alignment : 1;
    type->Component = component;
    for (usize i = 0; i < shape.Fields.Count(); ++i)
    {
        type->Fields.Add(FieldInfo(Keep(module.Strings[shape.Fields[i].Name].AsView()), *fieldTypes[i],
                                   shape.Fields[i].Offset, Metadata()));
    }
    type->Defaults.Resize(type->Size, 0);
    const uint32 slot = GStructCount;
    const SlotOperations &operations = GetSlotOperations(MakeIndexSequence<MaxScriptStructs>{})[slot];
    type->Operations.Construct = operations.Construct;
    type->Operations.Destruct = &DestructNothing;
    type->Operations.CopyConstruct = operations.Copy;
    type->Operations.MoveConstruct = operations.Move;
    type->Operations.CopyAssign = operations.Copy;
    type->Operations.MoveAssign = operations.Move;
    type->Operations.TriviallyCopyable = true;
    type->Operations.TriviallyDestructible = true;
    type->Meta[0] = {"Component", {}};
    type->Type = new TypeInfo(TypeDescription{
        .Name = type->Name,
        .FullName = type->Name,
        .Module = Keep(package),
        .Kind = TypeKind::Struct,
        .Size = type->Size,
        .Alignment = type->Alignment,
        .Operations = &type->Operations,
        .Fields = Span<FieldInfo>(type->Fields.Data(), type->Fields.Count()),
        .Meta = Metadata(Span<const MetadataEntry>(type->Meta, component ? 1 : 0)),
    });
    if (!Reflection::RegisterDynamicType(*type->Type))
    {
        errors.Add(Format("{0} has the name of a reflected type", name));
        return nullptr;
    }
    GStructSlots[slot] = type;
    ++GStructCount;
    registry.Structs.Add(String(name), type);
    if (component)
    {
        registry.Components.Add(String(name), &Components::RegisterDynamic(*type->Type));
    }
    return type;
}

class ScriptSystem;

ISystem *CreateScriptSystem(const TypeInfo &type, World &world);

/// The system a `[System]` function declares, before it is registered; null with an error if it cannot be one, or
/// without one if the process has it already.
UniquePtr<ScriptSystemInfo> PrepareSystem(ScriptEcsRegistry &registry, const ScriptModuleData &module,
                                          const ScriptAttribute &attribute, List<String> &errors)
{
    const ScriptFunctionRecord &function = module.Functions[attribute.Index];
    const StringView signature = module.Strings[function.Name].AsView();
    const StringView name = FunctionName(signature);
    const ScriptShape &type = module.Shapes[function.Signature];
    if (function.Kind != ScriptFunctionKind::Normal || module.Shapes[type.Element].Kind != ScriptShapeKind::Void)
    {
        errors.Add(Format("system {0} must return void and be neither async nor an iterator", name));
        return nullptr;
    }

    auto info = MakeUnique<ScriptSystemInfo>();
    for (usize i = 0; i < type.Parameters.Count(); ++i)
    {
        const ScriptShape &parameter = module.Shapes[type.Parameters[i]];
        ScriptSystemParameter &result = info->Parameters.AddDefaulted();
        if (parameter.Kind == ScriptShapeKind::Struct && ShapeName(module, parameter) == "Marlin.Entity")
        {
            result.Size = parameter.Size;
            continue;
        }
        // `ref` passes a pointer; `in` a pointer or, for a small struct, the value.
        const bool byValue = parameter.Kind == ScriptShapeKind::Struct;
        const ScriptShape *component = byValue                                      ? &parameter
                                       : parameter.Kind == ScriptShapeKind::Pointer ? &module.Shapes[parameter.Element]
                                                                                    : nullptr;
        const bool readOnly = i < 64 && (function.ReadOnlyParameters & (uint64{1} << i)) != 0;
        const ComponentType *const *found =
            component != nullptr && component->Kind == ScriptShapeKind::Struct && (!byValue || readOnly)
                ? registry.Components.Find(ShapeName(module, *component))
                : nullptr;
        if (found == nullptr)
        {
            errors.Add(
                Format("parameter {0} of system {1} is not a `ref` or `in` component or an Entity", i + 1, name));
            return nullptr;
        }
        for (const ScriptSystemParameter &other : info->Parameters)
        {
            if (&other != &result && other.Component == (*found)->GetId())
            {
                errors.Add(Format("system {0} takes {1} twice", name, (*found)->GetTypeInfo().GetName()));
                return nullptr;
            }
        }
        result.Component = (*found)->GetId();
        result.Size = (*found)->GetSize();
        result.Write = !readOnly;
        result.ByValue = byValue;
    }

    if (ScriptSystemInfo **existing = registry.Systems.Find(name))
    {
        if ((*existing)->Function != signature)
        {
            errors.Add(Format("system {0} is declared again with other parameters; the process keeps the first one "
                              "until it restarts",
                              name));
        }
        return nullptr;
    }
    info->Name = Keep(name);
    info->Function = String(signature);
    for (const ScriptAttributeArgument &argument : attribute.Arguments)
    {
        const StringView key = module.Strings[argument.Name].AsView();
        const StringView value = module.Strings[argument.Value].AsView();
        if (!value.IsEmpty() && (key == "Phase" || key == "Sets" || key == "UpdateAfter" || key == "UpdateBefore"))
        {
            info->Meta.Add({Keep(key), Keep(value)});
        }
    }
    return info;
}

/// Whether the names in *info*'s metadata are of the kinds the scheduler takes: a phase, system sets, and systems
/// that exist or that the *valid* ones of *pending* declare. The scheduler stops the process on a wrong one, as it does
/// for C++ systems, so a script's mistake is caught here instead.
bool CheckSystemNames(const ScriptSystemInfo &info, Span<const UniquePtr<ScriptSystemInfo>> pending,
                      Span<const bool> valid, List<String> &errors)
{
    for (const MetadataEntry &entry : info.Meta)
    {
        StringView text = entry.Value.Trim();
        if (text.StartsWith('(') && text.EndsWith(')'))
        {
            text = text.Substring(1, text.Length() - 2);
        }
        for (const StringView part : text.Split(','))
        {
            const StringView name = part.Trim();
            if (name.IsEmpty())
            {
                continue;
            }
            const TypeInfo *type = Reflection::FindType(name);
            bool ok = false;
            if (entry.Key == "Phase")
            {
                ok = type != nullptr && type->GetKind() == TypeKind::Struct && type->GetMetadata().Has("Phase");
            }
            else if (entry.Key == "Sets")
            {
                ok = type != nullptr && type->GetKind() == TypeKind::Struct && type->GetMetadata().Has("SystemSet");
            }
            else
            {
                ok = type != nullptr && type->GetKind() == TypeKind::Class && !type->IsAbstract() &&
                     type->IsA(TypeOf<ISystem>());
                for (usize i = 0; i < pending.Count(); ++i)
                {
                    ok = ok || (valid[i] && pending[i]->Name == name);
                }
            }
            if (!ok)
            {
                errors.Add(Format("system {0}: {1} = \"{2}\" names no {3}", info.Name, entry.Key, name,
                                  entry.Key == "Phase"  ? StringView("phase")
                                  : entry.Key == "Sets" ? StringView("system set")
                                                        : StringView("system")));
                return false;
            }
        }
    }
    return true;
}

void CommitSystem(ScriptEcsRegistry &registry, UniquePtr<ScriptSystemInfo> info, StringView package,
                  List<String> &errors)
{
    info->Type = new TypeInfo(TypeDescription{
        .Name = info->Name,
        .FullName = info->Name,
        .Module = Keep(package),
        .Kind = TypeKind::Class,
        .Operations = &info->Operations,
        .Parent = &TypeOf<ISystem>,
        .ToParent = &ToParent,
        .Meta = Metadata(Span<const MetadataEntry>(info->Meta.Data(), info->Meta.Count())),
    });
    if (!Reflection::RegisterDynamicType(*info->Type))
    {
        errors.Add(Format("system {0} has the name of a reflected type", info->Name));
        return;
    }
    ScriptSystemInfo *kept = info.Release();
    registry.Systems.Add(String(kept->Name), kept);
    registry.SystemsByType.Add(kept->Type, kept);
    Systems::RegisterFactory(*kept->Type, &CreateScriptSystem);
}

/// Calls a `[System]` function once per entity its components select, or once if it takes none. Faults end that
/// call only; the system logs how many there were and the first.
class ScriptSystem final : public ISystem
{
public:
    explicit ScriptSystem(const ScriptSystemInfo &info) : _info(&info)
    {
    }

    void Configure(SystemBuilder &builder) override
    {
        EntityQueryDesc desc;
        for (const ScriptSystemParameter &parameter : _info->Parameters)
        {
            if (parameter.Component != InvalidComponentId)
            {
                (parameter.Write ? desc.Write : desc.Read).Add(parameter.Component);
            }
        }
        if (!desc.Read.IsEmpty() || !desc.Write.IsEmpty())
        {
            _query.SetDesc(Move(desc));
            builder.AddQuery(_query);
        }
        builder.WriteResource<ScriptDomainAccess>();
    }

    void Execute(SystemContext &context) override
    {
        const ScriptWorldSubsystem *scripts = context.GetWorld().GetSubsystem<ScriptWorldSubsystem>();
        ScriptDomain *domain = scripts != nullptr ? scripts->GetDomain() : nullptr;
        if (domain == nullptr)
        {
            return;
        }
        // By generation, not address: a new domain may take the old one's memory.
        if (scripts->GetDomainGeneration() != _domainGeneration)
        {
            _domainGeneration = scripts->GetDomainGeneration();
            _function = domain->FindFunction(_info->Function.AsView());
            if (_function == nullptr)
            {
                MARLIN_LOG(LogScript, Error, "System {0}: the world's scripts have no {1}", _info->Name,
                           _info->Function);
            }
        }
        if (_function == nullptr)
        {
            return;
        }
        domain->SetDeltaTime(context.GetDeltaTime());
        List<ScriptSlot> arguments;
        arguments.Resize(_function->ParameterSlots);
        uint32 faults = 0;
        ScriptFaultInfo first;
        ScriptBatchCall batch(*domain, *_function);
        const auto invoke = [&] {
            if (!batch.Call(arguments.AsSpan()) && faults++ == 0)
            {
                first = batch.GetFault();
            }
        };

        if (!_query.IsBound())
        {
            invoke();
        }
        else
        {
            const usize count = _info->Parameters.Count();
            const uint32 *offsets = _function->ParameterOffsets.Data();
            List<uint8 *> columns;
            columns.Resize(count, nullptr);
            _query.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
                for (usize i = 0; i < count; ++i)
                {
                    const ScriptSystemParameter &parameter = _info->Parameters[i];
                    if (parameter.Component != InvalidComponentId)
                    {
                        columns[i] = parameter.Write ? static_cast<uint8 *>(chunk.WriteColumn(parameter.Component))
                                                     : const_cast<uint8 *>(static_cast<const uint8 *>(
                                                           chunk.ReadColumn(parameter.Component)));
                    }
                }
                const Span<const Entity> entities = chunk.GetEntities();
                for (uint32 row = 0; row < chunk.GetCount(); ++row)
                {
                    if (!chunk.IsIncluded(row))
                    {
                        continue;
                    }
                    for (usize i = 0; i < count; ++i)
                    {
                        uint8 *slot = arguments[offsets[i]].Bytes;
                        const ScriptSystemParameter &parameter = _info->Parameters[i];
                        if (parameter.Component == InvalidComponentId)
                        {
                            MemCopy(slot, &entities[row], sizeof(Entity));
                            continue;
                        }
                        uint8 *value = columns[i] + static_cast<usize>(row) * parameter.Size;
                        if (parameter.ByValue)
                        {
                            MemCopy(slot, value, parameter.Size);
                        }
                        else
                        {
                            MemCopy(slot, &value, sizeof(value));
                        }
                    }
                    invoke();
                }
            });
        }
        if (faults != 0)
        {
            MARLIN_LOG(LogScript, Error, "System {0} faulted {1} time(s) this run; the first:\n{2}", _info->Name,
                       faults, first.ToString());
        }
    }

private:
    const ScriptSystemInfo *_info;
    EntityQueryBase _query;
    uint64 _domainGeneration = 0;
    const ScriptFunctionInfo *_function = nullptr;
};

ISystem *CreateScriptSystem(const TypeInfo &type, World &world)
{
    (void)world;
    ScriptEcsRegistry &registry = GetRegistry();
    const ScopeLock lock(registry.Lock);
    ScriptSystemInfo *const *info = registry.SystemsByType.Find(&type);
    return info != nullptr ? new ScriptSystem(**info) : nullptr;
}

// The natives of World.mscript.

/// The world of the call's domain; null after a fault if there is none.
World *WorldOf(ScriptCallContext &context)
{
    World *world = context.GetDomain().GetWorld();
    if (world == nullptr)
    {
        context.Fault(ScriptFault::Native, "the script runs in no world");
    }
    return world;
}

/// The component of the struct shape *shape* of the called function's module; null after a fault if it is none.
const ComponentType *ComponentOf(ScriptCallContext &context, uint32 shape)
{
    const ScriptModuleData &module = *context.GetFunction().Module->Data;
    const StringView name = module.Shapes[shape].Kind == ScriptShapeKind::Struct
                                ? ShapeName(module, module.Shapes[shape])
                                : StringView("the type");
    const ComponentType *component = ScriptEcs::FindComponent(name);
    if (component == nullptr)
    {
        context.Fault(ScriptFault::Panic, Format("{0} is not a [Component] struct", name));
    }
    return component;
}

/// The receiver of an Entity method, which is passed by reference.
Entity ReceiverOf(ScriptCallContext &context)
{
    Entity entity;
    MemCopy(&entity, context.Get<const Entity *>(0), sizeof(entity));
    return entity;
}

/// The entity of *context*'s receiver as an EntityRef; false after a fault if it does not exist and *changes* is
/// false or it cannot take changes now.
bool ResolveEntity(ScriptCallContext &context, bool changes, EntityRef &result)
{
    World *world = WorldOf(context);
    if (world == nullptr)
    {
        return false;
    }
    const Entity entity = ReceiverOf(context);
    const bool locked = world->GetEntities().IsStructureLocked();
    if (entity.IsDeferred() ? !(changes && locked) : !world->GetEntities().Exists(entity))
    {
        context.Fault(ScriptFault::Panic, entity.IsDeferred() ? StringView("the deferred entity was already created")
                                                              : StringView("the entity does not exist"));
        return false;
    }
    result = EntityRef(*world, entity);
    return true;
}

bool EntityIsAlive(ScriptCallContext &context, const Entity *self)
{
    World *world = WorldOf(context);
    return world != nullptr && !self->IsDeferred() && world->GetEntities().Exists(*self);
}

void EntityDestroy(ScriptCallContext &context, const Entity *self)
{
    (void)self;
    EntityRef entity;
    if (ResolveEntity(context, true, entity))
    {
        entity.Destroy();
    }
}

void EntityGet(ScriptCallContext &context, const void *)
{
    const ScriptFunctionInfo &function = context.GetFunction();
    const ScriptModuleData &module = *function.Module->Data;
    const ComponentType *component = ComponentOf(context, module.Shapes[function.Record->Signature].Element);
    EntityRef entity;
    if (component == nullptr || !ResolveEntity(context, false, entity))
    {
        return;
    }
    const void *value = entity.GetComponent(component->GetId());
    if (value == nullptr)
    {
        context.Fault(ScriptFault::Panic, Format("the entity has no {0}", component->GetTypeInfo().GetName()));
        return;
    }
    MemSet(context.GetResult(), 0, static_cast<usize>(function.ReturnSlots) * ScriptSlotBytes);
    MemCopy(context.GetResult(), value, component->GetSize());
}

void EntitySet(ScriptCallContext &context, const void *)
{
    const ComponentType *component = ComponentOf(context, context.GetFunction().ParameterShapes[1]);
    EntityRef entity;
    if (component != nullptr && ResolveEntity(context, true, entity))
    {
        // AddComponent replaces the value of a component the entity has.
        entity.AddComponent(component->GetId(), context.GetParameter(1));
    }
}

void EntityHas(ScriptCallContext &context, const void *)
{
    const ComponentType *component = ComponentOf(context, context.GetFunction().ParameterShapes[1]);
    EntityRef entity;
    if (component == nullptr || !ResolveEntity(context, false, entity))
    {
        return;
    }
    context.Return(entity.GetComponent(component->GetId()) != nullptr);
}

void EntityRemove(ScriptCallContext &context, const void *)
{
    const ComponentType *component = ComponentOf(context, context.GetFunction().ParameterShapes[1]);
    EntityRef entity;
    if (component != nullptr && ResolveEntity(context, true, entity))
    {
        entity.RemoveComponent(component->GetId());
    }
}

void EntitiesCreate(ScriptCallContext &context, const void *)
{
    World *world = WorldOf(context);
    if (world == nullptr)
    {
        return;
    }
    Entity entity;
    if (world->GetEntities().IsStructureLocked())
    {
        world->Defer([&](CommandBuffer &commands) { entity = commands.CreateEntity(); });
    }
    else
    {
        entity = world->GetEntities().CreateEntity();
    }
    MemSet(context.GetResult(), 0, ScriptSlotBytes);
    MemCopy(context.GetResult(), &entity, sizeof(entity));
}

float32 TimeDelta(ScriptCallContext &context)
{
    return static_cast<float32>(context.GetDomain().GetDeltaTime());
}
} // namespace

namespace ScriptEcs
{
bool Register(const ScriptModuleData &module, List<String> &errors)
{
    const usize before = errors.Count();
    const StringView package = module.Strings[module.Package].AsView();
    ScriptEcsRegistry &registry = GetRegistry();
    const ScopeLock lock(registry.Lock);
    for (const ScriptAttribute &attribute : module.Attributes)
    {
        if (attribute.Target == ScriptAttributeTarget::Shape &&
            module.Strings[attribute.Name].AsView() == ComponentAttributeName)
        {
            (void)RegisterStruct(registry, module, attribute.Index, package, true, errors);
        }
    }
    // Systems may name each other in UpdateAfter and UpdateBefore, so all are prepared before any is checked.
    List<UniquePtr<ScriptSystemInfo>> systems;
    for (const ScriptAttribute &attribute : module.Attributes)
    {
        if (attribute.Target == ScriptAttributeTarget::Function &&
            module.Strings[attribute.Name].AsView() == SystemAttributeName)
        {
            systems.Add(PrepareSystem(registry, module, attribute, errors));
        }
    }
    // Until no more drop out: a system that names one that dropped out drops out too.
    List<bool> valid;
    for (const UniquePtr<ScriptSystemInfo> &system : systems)
    {
        valid.Add(system.Get() != nullptr);
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (usize i = 0; i < systems.Count(); ++i)
        {
            if (valid[i] && !CheckSystemNames(*systems[i], systems.AsSpan(), valid.AsSpan(), errors))
            {
                valid[i] = false;
                changed = true;
            }
        }
    }
    for (usize i = 0; i < systems.Count(); ++i)
    {
        if (valid[i])
        {
            CommitSystem(registry, Move(systems[i]), package, errors);
        }
    }
    return errors.Count() == before;
}

const ComponentType *FindComponent(StringView name)
{
    ScriptEcsRegistry &registry = GetRegistry();
    const ScopeLock lock(registry.Lock);
    const ComponentType *const *component = registry.Components.Find(name);
    return component != nullptr ? *component : nullptr;
}

const TypeInfo *FindSystem(StringView name)
{
    ScriptEcsRegistry &registry = GetRegistry();
    const ScopeLock lock(registry.Lock);
    ScriptSystemInfo *const *info = registry.Systems.Find(name);
    return info != nullptr ? (*info)->Type : nullptr;
}
} // namespace ScriptEcs

void RegisterScriptWorldNatives(ScriptNativeRegistry &registry)
{
    registry.Register("Entity.IsAlive", &EntityIsAlive);
    registry.Register("Entity.Destroy", &EntityDestroy);
    registry.Register("Entity.Get", &EntityGet);
    registry.Register("Entity.Set", &EntitySet);
    registry.Register("Entity.Has", &EntityHas);
    registry.Register("Entity.Remove", &EntityRemove);
    registry.Register("Entities.Create", &EntitiesCreate);
    registry.Register("Time.Delta", &TimeDelta);
}
} // namespace Marlin
