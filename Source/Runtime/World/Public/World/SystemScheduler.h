#pragma once

#include "Concurrency/Jobs.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "World/System.h"

namespace Marlin
{
/// A phase in execution order.
struct PhaseInfo
{
    const TypeInfo *Type = nullptr;
    WorldStage Stage = WorldStage::Fixed;
};

/// Runs a world's systems (Documentation/Architecture/05 §3): the phases of each stage in the order their After and
/// Before give, and within a phase a graph of the systems, where systems with conflicting access or an
/// UpdateAfter/UpdateBefore run in order and the rest in parallel on the job system. Structural changes recorded by
/// the systems play back after each phase.
class WORLD_API SystemScheduler
{
public:
    explicit SystemScheduler(World &world);
    ~SystemScheduler();
    MARLIN_NONCOPYABLE(SystemScheduler);

    /// Creates the systems of *enabledSets* that do not exist yet, destroys those of sets no longer enabled and
    /// rebuilds each phase's graph. Systems that stay keep their state.
    void Build(Span<const TypeInfo *const> enabledSets);
    /// Runs the phases of *stage*; a phase without systems costs nothing.
    void Run(WorldStage stage, float64 deltaTime);

    [[nodiscard]] Span<const PhaseInfo> GetPhases() const
    {
        return _phases.AsSpan();
    }

    /// The system of exactly *type*, or null if it is not created.
    [[nodiscard]] ISystem *FindSystem(const TypeInfo &type) const;

    template <typename T> [[nodiscard]] T *FindSystem() const
    {
        return static_cast<T *>(FindSystem(TypeOf<T>()));
    }

    /// The systems of phase *phase*, an index into GetPhases, in an order the graph allows.
    void GetSystems(uint32 phase, List<const TypeInfo *> &systems) const;

private:
    struct SystemInstance
    {
        const TypeInfo *Type = nullptr;
        ISystem *System = nullptr;
        uint32 Phase = 0;
        List<const TypeInfo *> Sets;
        List<const TypeInfo *> After;
        List<const TypeInfo *> Before;
        List<SystemAccess> Access;
        CommandBuffer Commands;
        UniquePtr<SystemContext> Context;
        uint32 LastVersion = 0;
    };

    struct PhaseGraph;

    struct Node
    {
        SystemInstance *Instance = nullptr;
        PhaseGraph *Graph = nullptr;
        List<uint32> Successors;
        uint32 Predecessors = 0;
        /// Predecessors that have not finished in the current run.
        uint32 Pending = 0;
    };

    struct PhaseGraph
    {
        List<Node> Nodes;
        JobCounter Counter;
    };

    static void RunNode(void *argument);
    void DiscoverPhases();
    void BuildGraph(uint32 phase);

    World &_world;
    List<PhaseInfo> _phases;
    List<UniquePtr<SystemInstance>> _systems;
    List<UniquePtr<PhaseGraph>> _graphs;
};
} // namespace Marlin
