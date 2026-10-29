#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Reflection/TypeOf.h"
#include "Templates/Function.h"
#include "Engine/Subsystem.gen.h"

namespace Marlin
{
class SubsystemCollection;

/// What a subsystem sees while it is created and initialized. A scope may pass a struct derived from it, such as a
/// world's WorldContext; ShouldCreate then takes that type.
MSTRUCT()
struct SubsystemContext
{
    MGENERATED_BODY()

    /// The collection being initialized; it also finds the subsystems of the scopes around it.
    SubsystemCollection *Collection = nullptr;
};

/// The one unit of lifecycle: anything that starts, ticks or stops is a subsystem of some scope. A subsystem class
/// derives from its scope's base; it exists if its module is linked, unless a static
/// `MFUNCTION() static bool ShouldCreate(const SubsystemContext &context)` says no. `MCLASS(Dependencies="A, B")`
/// names the subsystems it needs: those of its own scope initialize before it, those of outer scopes already have.
/// `MCLASS(OptionalDependencies="A")` only orders it after A when A is linked and created, such as a subsystem that
/// reads the OS events ApplicationSubsystem pumps without linking Application itself.
MCLASS()
class ENGINE_API Subsystem
{
    MGENERATED_BODY()
public:
    Subsystem() = default;
    virtual ~Subsystem() = default;
    MARLIN_NONCOPYABLE(Subsystem);

    /// Called before Initialize, so that a scope's base class can keep what its context carries.
    virtual void Bind(SubsystemContext &context)
    {
    }

    virtual void Initialize(SubsystemContext &context)
    {
    }

    virtual void Deinitialize()
    {
    }
};

/// The time of one frame of the main loop.
struct FrameTime
{
    uint64 Frame = 0;
    /// Seconds since the last frame, clamped.
    float64 Delta = 0;
    /// Seconds since the loop started.
    float64 Elapsed = 0;
};

/// Lives as long as the process; the main loop calls each frame hook in initialization order.
MCLASS()
class ENGINE_API EngineSubsystem : public Subsystem
{
    MGENERATED_BODY()
public:
    /// Once every engine subsystem is initialized, in initialization order: where a target starts the scopes that
    /// need the whole engine, such as the editor's (Documentation/Architecture/01 §2, step 10).
    virtual void Start()
    {
    }

    /// Before any engine subsystem deinitializes, in reverse order: where those scopes stop.
    virtual void Stop()
    {
    }

    virtual void BeginFrame(const FrameTime &time)
    {
    }

    /// Zero or more times per frame, at the fixed rate.
    virtual void FixedUpdate(float64 delta)
    {
    }

    virtual void Update(const FrameTime &time)
    {
    }

    virtual void EndFrame(const FrameTime &time)
    {
    }
};

/// Lives from loading a project to closing it.
MCLASS()
class ENGINE_API ProjectSubsystem : public Subsystem
{
    MGENERATED_BODY()
};

/// The subsystems of one scope, such as the engine or a world: every registered class deriving from the scope's base.
class ENGINE_API SubsystemCollection
{
public:
    /// *outer* is the collection of the enclosing scope, which dependencies may reach into. *contextType* is what
    /// Initialize and Update are given, SubsystemContext or a struct derived from it, and what ShouldCreate takes.
    explicit SubsystemCollection(const TypeInfo &scope, SubsystemCollection *outer = nullptr,
                                 const TypeInfo &contextType = TypeOf<SubsystemContext>());
    ~SubsystemCollection();
    MARLIN_NONCOPYABLE(SubsystemCollection);

    /// Creates the subsystems and initializes them in dependency order. A cycle is fatal; a subsystem whose
    /// dependency was not created is skipped with a warning.
    void Initialize();
    /// Initialize with a context of the collection's context type.
    void Initialize(SubsystemContext &context);
    /// Brings the subsystems in line with what should exist now: those that no longer should, or whose dependency is
    /// gone, are deinitialized in reverse order; new ones are created and initialized in dependency order.
    void Update(SubsystemContext &context);

    /// Types the filter rejects are not created, as if their ShouldCreate said no; set before Initialize.
    void SetFilter(Function<bool(const TypeInfo &)> filter)
    {
        _filter = Move(filter);
    }
    /// Deinitializes and destroys them in reverse order.
    void Deinitialize();

    [[nodiscard]] bool IsInitialized() const
    {
        return _initialized;
    }

    [[nodiscard]] const TypeInfo &GetScope() const
    {
        return _scope;
    }

    /// The subsystem of *type* in this scope or an outer one, or null.
    [[nodiscard]] Subsystem *Find(const TypeInfo &type) const;

    template <typename T> [[nodiscard]] T *Get() const
    {
        return static_cast<T *>(Find(TypeOf<T>()));
    }

    /// This scope's subsystems in initialization order.
    [[nodiscard]] Span<Subsystem *const> GetAll() const
    {
        return _subsystems.AsSpan();
    }

private:
    /// The types that should exist, in dependency order.
    void Plan(SubsystemContext &context, List<const TypeInfo *> &order) const;
    void Create(const TypeInfo &type, SubsystemContext &context);

    const TypeInfo &_scope;
    SubsystemCollection *_outer;
    const TypeInfo &_contextType;
    Function<bool(const TypeInfo &)> _filter;
    List<Subsystem *> _subsystems;
    List<const TypeInfo *> _types;
    bool _initialized = false;
};
} // namespace Marlin

