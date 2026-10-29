#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"
#include "Templates/Function.h"
#include "World/CommandBuffer.h"
#include "World/EntityManager.h"
#include "World/Resources.h"
#include "World/SystemScheduler.h"
#include "World/WorldProfile.h"
#include "World/World.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(WORLD_API, LogWorld, Info, Warning);

namespace Marlin
{
class World;

/// What a WorldSubsystem's `static bool ShouldCreate(const WorldContext &)` sees.
MSTRUCT()
struct WorldContext : SubsystemContext
{
    MGENERATED_BODY()

    World *Owner = nullptr;

    /// True if the world has *set* active, from its profile or pushed.
    [[nodiscard]] WORLD_API bool HasSet(const TypeInfo &set) const;

    template <typename T> [[nodiscard]] bool HasSet() const
    {
        return HasSet(TypeOf<T>());
    }

    [[nodiscard]] WORLD_API const WorldProfile &GetProfile() const;
};

/// One per world: the scheduler's companions such as PhysicsScene, RenderScene, ScriptDomain and NetDriver
/// (Documentation/Architecture/05 §1). `MCLASS(Sets=(PhysicsSet))` creates it only while one of those sets is active;
/// the profile's Subsystems Include and Exclude add or remove it by name or set.
MCLASS(Abstract)
class WORLD_API WorldSubsystem : public Subsystem
{
    MGENERATED_BODY()
public:
    [[nodiscard]] World &GetWorld() const
    {
        return *_world;
    }

    void Bind(SubsystemContext &context) override;

    /// Before the world's phases run in a frame.
    virtual void BeginFrame(const FrameTime &time)
    {
    }

    /// After the world's phases ran in a frame.
    virtual void EndFrame(const FrameTime &time)
    {
    }

private:
    World *_world = nullptr;
};

/// An EntityManager and its systems, resources, events and world subsystems (Documentation/Architecture/05 §1).
/// A WorldProfile decides what runs; profile and set changes take effect at the next frame boundary, when the
/// scheduler rebuilds and subsystems are created or destroyed by difference.
class WORLD_API World
{
public:
    /// *profile* must outlive the world or the next SetProfile. *outer* is the collection of the enclosing scope,
    /// usually the engine's.
    explicit World(const WorldProfile &profile, StringView name = {}, SubsystemCollection *outer = nullptr);
    ~World();
    MARLIN_NONCOPYABLE(World);

    [[nodiscard]] StringView GetName() const
    {
        return _name.AsView();
    }

    [[nodiscard]] EntityManager &GetEntities()
    {
        return _entities;
    }

    [[nodiscard]] ResourceStore &GetResources()
    {
        return _resources;
    }

    [[nodiscard]] EventStore &GetEvents()
    {
        return _events;
    }

    [[nodiscard]] SystemScheduler &GetScheduler()
    {
        return _scheduler;
    }

    [[nodiscard]] SubsystemCollection &GetSubsystems()
    {
        return _subsystems;
    }

    template <typename T> [[nodiscard]] T *GetSubsystem() const
    {
        return _subsystems.Get<T>();
    }

    [[nodiscard]] const WorldProfile &GetProfile() const
    {
        return *_profile;
    }

    /// Switches to *profile* at the next frame boundary; it must outlive the world or the next SetProfile.
    void SetProfile(const WorldProfile &profile);
    /// Adds *sets* on top of the profile's at the next frame boundary, such as an editor mode's; PopSets removes them.
    void PushSets(Span<const TypeInfo *const> sets);
    void PopSets();

    [[nodiscard]] bool HasSet(const TypeInfo &set) const;

    [[nodiscard]] Span<const TypeInfo *const> GetActiveSets() const
    {
        return _activeSets.AsSpan();
    }

    /// The frame hooks, called by WorldManagerSubsystem; the clock decides what each runs.
    void BeginFrame(const FrameTime &time);
    /// One step of the Fixed stage, zero or more times a frame.
    void FixedUpdate(float64 delta);
    void Update(const FrameTime &time);
    void EndFrame(const FrameTime &time);
    /// Runs one step of the Fixed stage now, for a Manual clock: single-stepping and recording.
    void Step(float64 delta);

    /// Records structural changes from outside systems while the structure is locked, such as EntityRef in a system;
    /// thread-safe. They play back after the running stage.
    void Defer(FunctionRef<void(CommandBuffer &)> record);
    void PlaybackDeferred();

private:
    void ApplyChanges();
    [[nodiscard]] bool AllowsSubsystem(const TypeInfo &type) const;

    String _name;
    const WorldProfile *_profile;
    EntityManager _entities;
    ResourceStore _resources;
    EventStore _events;
    SystemScheduler _scheduler;
    SubsystemCollection _subsystems;
    SpinLock _deferredLock;
    CommandBuffer _deferred;
    List<List<const TypeInfo *>> _pushedSets;
    List<const TypeInfo *> _activeSets;
    bool _dirty = true;
};
} // namespace Marlin
