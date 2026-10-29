#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "Reflection/TypeOf.h"
#include "World/CommandBuffer.h"
#include "World/EntityQuery.h"
#include "World/Resources.h"
#include "World/System.gen.h"

namespace Marlin
{
class EntityManager;
class World;

/// The two clocks a world steps its phases on (Documentation/Architecture/01 §4).
MENUM()
enum class WorldStage : uint8
{
    /// Zero or more times a frame at the fixed rate: simulation.
    Fixed,
    /// Once a frame with the frame's delta: presentation.
    Variable,
};

/// The built-in anchor phases (Documentation/Architecture/05 §3). A phase is a reflected marker,
/// `MSTRUCT(Phase, Stage=Fixed, After=A, Before=B)`; other modules and editor modes add their own between the anchors.
MSTRUCT(Phase, Stage = Fixed)
struct InputPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Fixed, After = InputPhase)
struct PrePhysicsPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Fixed, After = PrePhysicsPhase)
struct PhysicsPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Fixed, After = PhysicsPhase)
struct PostPhysicsPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Fixed, After = PostPhysicsPhase)
struct GameplayPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Fixed, After = GameplayPhase)
struct NetPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Variable)
struct CameraPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Variable, After = CameraPhase)
struct AnimationPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Variable, After = AnimationPhase)
struct UIPhase
{
    MGENERATED_BODY()
};

MSTRUCT(Phase, Stage = Variable, After = UIPhase)
struct LatePhase
{
    MGENERATED_BODY()
};

/// Transform propagation and hierarchy: what every world needs. Like every system set (Documentation/Architecture/05
/// §1.1) it is a reflected marker, `MSTRUCT(SystemSet)`, that anyone may define; a WorldProfile enables sets, and only
/// systems in an enabled set run.
MSTRUCT(SystemSet)
struct CoreSimulationSet
{
    MGENERATED_BODY()
};

/// Game logic and script behaviours; the set of a system that names none.
MSTRUCT(SystemSet)
struct GameplaySet
{
    MGENERATED_BODY()
};

MSTRUCT(SystemSet)
struct PhysicsSet
{
    MGENERATED_BODY()
};

/// Poses, particles, anything rendered.
MSTRUCT(SystemSet)
struct PresentationSet
{
    MGENERATED_BODY()
};

MSTRUCT(SystemSet)
struct NetServerSet
{
    MGENERATED_BODY()
};

MSTRUCT(SystemSet)
struct NetClientSet
{
    MGENERATED_BODY()
};

/// What a system reads or writes, from which the scheduler orders the systems of a phase.
struct SystemAccess
{
    enum class Kind : uint8
    {
        Component,
        Resource,
        Events,
    };

    Kind What = Kind::Component;
    /// A ComponentId, or the TypeInfo of a resource or event type.
    uintptr Key = 0;
    bool Write = false;
};

/// What ISystem::Configure declares its queries and access through.
class WORLD_API SystemBuilder
{
public:
    SystemBuilder(World &world, List<SystemAccess> &access) : _world(&world), _access(&access)
    {
    }

    [[nodiscard]] World &GetWorld() const
    {
        return *_world;
    }

    /// Binds *query* to the world's entities and records its components.
    void AddQuery(EntityQueryBase &query);

    void ReadComponent(ComponentId component);
    void WriteComponent(ComponentId component);
    /// Also creates the resource, so the system finds it with SystemContext::GetResource.
    void ReadResource(const TypeInfo &type);
    void WriteResource(const TypeInfo &type);
    void ReadEvents(const TypeInfo &type);
    void WriteEvents(const TypeInfo &type);

    template <typename T> void ReadComponent()
    {
        ReadComponent(Components::IdOf<T>());
    }

    template <typename T> void WriteComponent()
    {
        WriteComponent(Components::IdOf<T>());
    }

    template <typename T> void ReadResource()
    {
        ReadResource(TypeOf<T>());
    }

    template <typename T> void WriteResource()
    {
        WriteResource(TypeOf<T>());
    }

    template <typename T> void ReadEvents()
    {
        ReadEvents(TypeOf<T>());
    }

    template <typename T> void WriteEvents()
    {
        WriteEvents(TypeOf<T>());
    }

private:
    void Record(SystemAccess::Kind kind, uintptr key, bool write);

    World *_world;
    List<SystemAccess> *_access;
};

/// What a system's Execute works with: the world, its delta, the versions its queries compare against and the
/// CommandBuffer structural changes go to.
class WORLD_API SystemContext
{
public:
    SystemContext(World &world, CommandBuffer &commands) : _world(&world), _commands(&commands)
    {
    }

    [[nodiscard]] World &GetWorld() const
    {
        return *_world;
    }

    [[nodiscard]] EntityManager &GetEntities() const;

    /// The stage's step: the fixed delta in the Fixed stage, the frame's in the Variable stage.
    [[nodiscard]] float64 GetDeltaTime() const
    {
        return _deltaTime;
    }

    [[nodiscard]] const QueryVersions &GetVersions() const
    {
        return _versions;
    }

    /// The system's CommandBuffer, played back after its phase.
    [[nodiscard]] CommandBuffer &GetCommands() const
    {
        return *_commands;
    }

    /// The CommandBuffer of one chunk of a parallel visit; buffers play back in chunk order, so the result does not
    /// depend on which thread ran which chunk.
    [[nodiscard]] CommandBuffer &GetCommands(const ChunkView &chunk);

    [[nodiscard]] ResourceStore &GetResources() const;
    [[nodiscard]] EventStore &GetEvents() const;

    /// A resource declared with SystemBuilder::ReadResource or WriteResource.
    template <typename T> [[nodiscard]] T *GetResource() const
    {
        return GetResources().Find<T>();
    }

    template <typename T> [[nodiscard]] Events<T> &GetEvents() const
    {
        return GetEvents().Get<T>();
    }

    template <typename TQuery, typename TFunction> void ForEach(TQuery &query, TFunction &&function)
    {
        query.ForEach(_versions, Forward<TFunction>(function));
    }

    template <typename TQuery, typename TFunction> void ParallelForEach(TQuery &query, TFunction &&function)
    {
        query.ParallelForEach(_versions, Forward<TFunction>(function));
    }

private:
    friend class SystemScheduler;

    World *_world;
    CommandBuffer *_commands;
    float64 _deltaTime = 0;
    QueryVersions _versions;
    SpinLock _parallelLock;
    List<UniquePtr<CommandBuffer>> _parallelCommands;
};

/// The base of systems (Documentation/Architecture/05 §3): `MCLASS(Phase=PostPhysicsPhase,
/// Sets=(CoreSimulationSet), UpdateAfter=OtherSystem)`. Phase defaults to GameplayPhase and Sets to GameplaySet;
/// UpdateAfter and UpdateBefore order systems of the same phase, and the scheduler orders the rest by their access.
/// A world creates the systems of the sets its profile enables, calls Configure once and Execute each time the phase
/// runs.
MCLASS(Abstract)
class WORLD_API ISystem
{
    MGENERATED_BODY()
public:
    ISystem() = default;
    virtual ~ISystem() = default;
    MARLIN_NONCOPYABLE(ISystem);

    virtual void Configure(SystemBuilder &builder)
    {
    }

    virtual void Execute(SystemContext &context) = 0;
};

/// Makes a system of a type described at run time, which has no C++ constructor, such as a script system
/// (Documentation/Architecture/08 §5). The scheduler deletes what it returns; null when it cannot be made.
using SystemFactory = ISystem *(*)(const TypeInfo &type, World &world);

namespace Systems
{
/// Makes systems of *type*, a registered class type derived from ISystem, with *factory*; its metadata (Phase, Sets,
/// UpdateAfter, UpdateBefore) is read like that of a C++ system.
WORLD_API void RegisterFactory(const TypeInfo &type, SystemFactory factory);
[[nodiscard]] WORLD_API SystemFactory FindFactory(const TypeInfo &type);
} // namespace Systems
} // namespace Marlin
