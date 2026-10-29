#include "World/SystemScheduler.h"

#include "Concurrency/Atomic.h"
#include "Diagnostics/AssertFormat.h"
#include "Reflection/Reflection.h"
#include "String/StringBuilder.h"
#include "World/World.h"

namespace Marlin
{
namespace
{
/// Splits a metadata value such as `(A, B)` or `A` into names.
void ParseNames(Optional<StringView> value, List<StringView> &names)
{
    names.Clear();
    if (!value.HasValue())
    {
        return;
    }
    StringView text = value.Value().Trim();
    if (text.StartsWith('(') && text.EndsWith(')'))
    {
        text = text.Substring(1, text.Length() - 2);
    }
    for (const StringView part : text.Split(','))
    {
        const StringView name = part.Trim();
        if (!name.IsEmpty())
        {
            names.Add(name);
        }
    }
}

bool IsPhase(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("Phase");
}

bool IsSystemSet(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("SystemSet");
}

/// The type *name* that the metadata *key* of *owner* names; fatal if there is none of the kind *accept* takes.
const TypeInfo &ResolveType(const TypeInfo &owner, StringView key, StringView name, bool (*accept)(const TypeInfo &))
{
    const TypeInfo *type = Reflection::FindType(name);
    if (type == nullptr || !accept(*type))
    {
        MARLIN_LOG(LogWorld, Fatal, "{0}: {1}={2} does not name a type of the right kind", owner.GetName(), key, name);
    }
    return *type;
}

bool Conflicts(Span<const SystemAccess> a, Span<const SystemAccess> b)
{
    for (const SystemAccess &first : a)
    {
        for (const SystemAccess &second : b)
        {
            if (first.What == second.What && first.Key == second.Key && (first.Write || second.Write))
            {
                return true;
            }
        }
    }
    return false;
}

bool Contains(Span<const TypeInfo *const> types, const TypeInfo *type)
{
    for (const TypeInfo *candidate : types)
    {
        if (candidate == type)
        {
            return true;
        }
    }
    return false;
}

bool IsSystemType(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Class && !type.IsAbstract() && type.IsA(TypeOf<ISystem>());
}

/// Orders [0, *count*) so that every edge of *successors* points forward, taking the lowest ready index first so the
/// order is stable. False on a cycle.
bool TopologicalSort(uint32 count, const List<List<uint32>> &successors, List<uint32> &order)
{
    order.Clear();
    List<uint32> predecessors;
    predecessors.Resize(count, 0);
    for (const List<uint32> &edges : successors)
    {
        for (const uint32 successor : edges)
        {
            ++predecessors[successor];
        }
    }
    List<bool> done;
    done.Resize(count, false);
    while (order.Count() < count)
    {
        uint32 next = count;
        for (uint32 i = 0; i < count && next == count; ++i)
        {
            if (!done[i] && predecessors[i] == 0)
            {
                next = i;
            }
        }
        if (next == count)
        {
            return false;
        }
        done[next] = true;
        order.Add(next);
        for (const uint32 successor : successors[next])
        {
            --predecessors[successor];
        }
    }
    return true;
}
} // namespace

SystemScheduler::SystemScheduler(World &world) : _world(world)
{
}

SystemScheduler::~SystemScheduler()
{
    for (const UniquePtr<SystemInstance> &instance : _systems)
    {
        delete instance->System;
    }
    _systems.Clear();
}

void SystemScheduler::DiscoverPhases()
{
    List<const TypeInfo *> types;
    Reflection::ForEachType([&](const TypeInfo &type) {
        if (IsPhase(type))
        {
            types.Add(&type);
        }
    });
    types.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName() < b->GetName(); });

    const auto count = static_cast<uint32>(types.Count());
    List<WorldStage> stages;
    for (const TypeInfo *type : types)
    {
        const Optional<StringView> stage = type->GetMetadata().Get("Stage");
        const EnumeratorInfo *enumerator =
            stage.HasValue() ? TypeOf<WorldStage>().FindEnumerator(stage.Value()) : nullptr;
        if (enumerator == nullptr)
        {
            MARLIN_LOG(LogWorld, Fatal, "Phase {0} has an unknown Stage {1}", type->GetName(),
                       stage.ValueOr(StringView()));
        }
        stages.Add(static_cast<WorldStage>(enumerator->Value));
    }

    List<List<uint32>> successors;
    successors.Resize(count);
    List<StringView> names;
    for (uint32 i = 0; i < count; ++i)
    {
        for (const bool after : {true, false})
        {
            const StringView key = after ? "After" : "Before";
            ParseNames(types[i]->GetMetadata().Get(key), names);
            for (const StringView name : names)
            {
                const TypeInfo &other = ResolveType(*types[i], key, name, IsPhase);
                const auto j = static_cast<uint32>(types.IndexOf(&other));
                if (stages[j] != stages[i])
                {
                    MARLIN_LOG(LogWorld, Fatal, "Phase {0} is ordered against {1}, which runs in another stage",
                               types[i]->GetName(), other.GetName());
                }
                after ? successors[j].Add(i) : successors[i].Add(j);
            }
        }
    }
    List<uint32> order;
    if (!TopologicalSort(count, successors, order))
    {
        MARLIN_LOG(LogWorld, Fatal, "The phases' After and Before form a cycle");
    }
    // No edge crosses stages, so putting the Fixed stage first keeps the order valid.
    order.StableSort([&](uint32 a, uint32 b) { return stages[a] < stages[b]; });
    _phases.Clear();
    for (const uint32 index : order)
    {
        _phases.Add({types[index], stages[index]});
    }
}

void SystemScheduler::Build(Span<const TypeInfo *const> enabledSets)
{
    if (_phases.IsEmpty())
    {
        DiscoverPhases();
    }
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(TypeOf<ISystem>(), types);
    types.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName() < b->GetName(); });

    List<UniquePtr<SystemInstance>> kept;
    List<StringView> names;
    for (const TypeInfo *type : types)
    {
        if (!IsSystemType(*type))
        {
            continue;
        }
        const Metadata &metadata = type->GetMetadata();
        List<const TypeInfo *> sets;
        ParseNames(metadata.Get("Sets"), names);
        for (const StringView name : names)
        {
            sets.Add(&ResolveType(*type, "Sets", name, IsSystemSet));
        }
        if (sets.IsEmpty())
        {
            sets.Add(&TypeOf<GameplaySet>());
        }
        bool enabled = false;
        for (const TypeInfo *set : sets)
        {
            enabled = enabled || Contains(enabledSets, set);
        }
        if (!enabled)
        {
            continue;
        }
        const usize existing =
            _systems.FindIndex([&](const UniquePtr<SystemInstance> &instance) { return instance->Type == type; });
        if (existing != List<UniquePtr<SystemInstance>>::NotFound)
        {
            kept.Add(Move(_systems[existing]));
            _systems.RemoveAt(existing);
            continue;
        }

        auto instance = MakeUnique<SystemInstance>();
        instance->Type = type;
        instance->Sets = Move(sets);
        const TypeInfo &phase = ResolveType(*type, "Phase", metadata.GetOr("Phase", "GameplayPhase"), IsPhase);
        instance->Phase =
            static_cast<uint32>(_phases.FindIndex([&](const PhaseInfo &info) { return info.Type == &phase; }));
        for (const bool after : {true, false})
        {
            const StringView key = after ? "UpdateAfter" : "UpdateBefore";
            ParseNames(metadata.Get(key), names);
            for (const StringView name : names)
            {
                (after ? instance->After : instance->Before).Add(&ResolveType(*type, key, name, IsSystemType));
            }
        }
        if (const SystemFactory factory = Systems::FindFactory(*type))
        {
            instance->System = factory(*type, _world);
        }
        else if (void *object = type->New())
        {
            instance->System = static_cast<ISystem *>(type->CastTo(object, TypeOf<ISystem>()));
        }
        if (instance->System == nullptr)
        {
            MARLIN_LOG(LogWorld, Error, "System {0} cannot be constructed", type->GetName());
            continue;
        }
        instance->Context = MakeUnique<SystemContext>(_world, instance->Commands);
        SystemBuilder builder(_world, instance->Access);
        instance->System->Configure(builder);
        kept.Add(Move(instance));
    }
    // What is left belongs to sets no longer enabled.
    for (const UniquePtr<SystemInstance> &instance : _systems)
    {
        delete instance->System;
    }
    _systems = Move(kept);
    _systems.Sort([](const UniquePtr<SystemInstance> &a, const UniquePtr<SystemInstance> &b) {
        return a->Type->GetName() < b->Type->GetName();
    });

    _graphs.Clear();
    for (uint32 i = 0; i < _phases.Count(); ++i)
    {
        _graphs.Add(MakeUnique<PhaseGraph>());
        BuildGraph(i);
    }
}

void SystemScheduler::BuildGraph(uint32 phase)
{
    PhaseGraph &graph = *_graphs[phase];
    List<SystemInstance *> members;
    for (const UniquePtr<SystemInstance> &instance : _systems)
    {
        if (instance->Phase == phase)
        {
            members.Add(instance.Get());
        }
    }
    const auto count = static_cast<uint32>(members.Count());
    if (count == 0)
    {
        return;
    }
    const auto indexOf = [&](const TypeInfo *type) {
        return members.FindIndex([&](const SystemInstance *member) { return member->Type == type; });
    };

    // UpdateAfter and UpdateBefore within the phase fix an order; systems of other phases are ordered already.
    List<List<uint32>> explicitEdges;
    explicitEdges.Resize(count);
    for (uint32 i = 0; i < count; ++i)
    {
        for (const TypeInfo *type : members[i]->After)
        {
            if (const usize j = indexOf(type); j != List<SystemInstance *>::NotFound)
            {
                explicitEdges[j].Add(i);
            }
        }
        for (const TypeInfo *type : members[i]->Before)
        {
            if (const usize j = indexOf(type); j != List<SystemInstance *>::NotFound)
            {
                explicitEdges[i].Add(static_cast<uint32>(j));
            }
        }
    }
    List<uint32> order;
    if (!TopologicalSort(count, explicitEdges, order))
    {
        StringBuilder<256> cycle;
        for (uint32 i = 0; i < count; ++i)
        {
            if (!order.Contains(i))
            {
                cycle.Append(members[i]->Type->GetName());
                cycle.Append(", ");
            }
        }
        MARLIN_LOG(LogWorld, Fatal, "UpdateAfter and UpdateBefore form a cycle among {0}in phase {1}", cycle.AsView(),
                   _phases[phase].Type->GetName());
    }

    // Nodes in that order; a later node depends on an earlier one it is ordered after or conflicts with.
    graph.Nodes.Resize(count);
    for (uint32 k = 0; k < count; ++k)
    {
        Node &node = graph.Nodes[k];
        node.Instance = members[order[k]];
        node.Graph = &graph;
        for (uint32 l = 0; l < k; ++l)
        {
            if (explicitEdges[order[l]].Contains(order[k]) ||
                Conflicts(members[order[l]]->Access.AsSpan(), node.Instance->Access.AsSpan()))
            {
                graph.Nodes[l].Successors.Add(k);
                ++node.Predecessors;
            }
        }
    }
}

void SystemScheduler::RunNode(void *argument)
{
    Node &node = *static_cast<Node *>(argument);
    SystemInstance &instance = *node.Instance;
    SystemContext &context = *instance.Context;
    const uint32 version = context.GetEntities().AdvanceVersion();
    context._versions = {instance.LastVersion, version};
    instance.System->Execute(context);
    instance.LastVersion = version;
    for (const uint32 successor : node.Successors)
    {
        Node &next = node.Graph->Nodes[successor];
        if (AtomicRef<uint32>(next.Pending).FetchAdd(~uint32{0}, MemoryOrder::AcquireRelease) == 1)
        {
            Jobs::Run(JobDecl{&RunNode, &next}, &node.Graph->Counter);
        }
    }
}

void SystemScheduler::Run(WorldStage stage, float64 deltaTime)
{
    EntityManager &entities = _world.GetEntities();
    List<JobDecl> roots;
    for (uint32 i = 0; i < _phases.Count(); ++i)
    {
        PhaseGraph &graph = *_graphs[i];
        if (_phases[i].Stage != stage || graph.Nodes.IsEmpty())
        {
            continue;
        }
        roots.Clear();
        for (Node &node : graph.Nodes)
        {
            node.Pending = node.Predecessors;
            node.Instance->Context->_deltaTime = deltaTime;
            if (node.Predecessors == 0)
            {
                roots.Add({&RunNode, &node});
            }
        }
        entities.LockStructure();
        Jobs::Run(roots.AsSpan(), &graph.Counter);
        Jobs::Wait(graph.Counter);
        entities.UnlockStructure();
        // Playback, and writes outside systems until the next phase, get a version newer than any system's last
        // run, so Changed<T> sees them; stamped with the last system's version they would look already seen.
        (void)entities.AdvanceVersion();

        // The sync point: structural changes in graph order, each system's parallel buffers in chunk order.
        for (Node &node : graph.Nodes)
        {
            node.Instance->Commands.Playback(entities);
            for (const UniquePtr<CommandBuffer> &commands : node.Instance->Context->_parallelCommands)
            {
                if (commands.Get() != nullptr)
                {
                    commands->Playback(entities);
                }
            }
        }
    }
}

ISystem *SystemScheduler::FindSystem(const TypeInfo &type) const
{
    for (const UniquePtr<SystemInstance> &instance : _systems)
    {
        if (instance->Type == &type)
        {
            return instance->System;
        }
    }
    return nullptr;
}

void SystemScheduler::GetSystems(uint32 phase, List<const TypeInfo *> &systems) const
{
    for (const Node &node : _graphs[phase]->Nodes)
    {
        systems.Add(node.Instance->Type);
    }
}
} // namespace Marlin
