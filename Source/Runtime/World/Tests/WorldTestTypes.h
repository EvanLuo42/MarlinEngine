#pragma once

#include "Asset/AssetRef.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "String/String.h"
#include "World/Entity.h"
#include "World/EntityTemplate.h"
#include "World/System.h"
#include "World/World.h"
#include "World/WorldProfile.h"
#include "WorldTestTypes.gen.h"

namespace Marlin::Tests
{
MSTRUCT(Component)
struct TestPosition
{
    MGENERATED_BODY()

    MPROPERTY()
    Vector3 Value;
};

MSTRUCT(Component)
struct TestVelocity
{
    MGENERATED_BODY()

    MPROPERTY()
    Vector3 Value;
};

/// A component with heap data, which chunks copy through its operations.
MSTRUCT(Component)
struct TestLabel
{
    MGENERATED_BODY()

    MPROPERTY()
    String Text;
};

MSTRUCT(Component, Tag)
struct TestFrozen
{
    MGENERATED_BODY()
};

MSTRUCT(Component, Shared)
struct TestTeam
{
    MGENERATED_BODY()

    MPROPERTY()
    int32 Number = 0;

    bool operator==(const TestTeam &) const = default;
};

MSTRUCT(Component, Enableable)
struct TestStunned
{
    MGENERATED_BODY()

    MPROPERTY()
    float32 Seconds = 0;
};

MSTRUCT(Component, Chunk)
struct TestChunkBounds
{
    MGENERATED_BODY()

    MPROPERTY()
    float32 Radius = 0;
};

MSTRUCT(Component)
struct TestTarget
{
    MGENERATED_BODY()

    MPROPERTY()
    Entity Value;

    MPROPERTY()
    Entity Others[2];
};

/// A component that holds an asset reference, which templates carry as links.
MSTRUCT(Component)
struct TestAssetLink
{
    MGENERATED_BODY()

    MPROPERTY()
    AssetRef<EntityTemplate> Linked;

    MPROPERTY()
    int32 Weight = 0;
};

/// Keeps these systems and subsystems out of every world but the tests' own.
MSTRUCT(SystemSet)
struct TestSchedulingSet
{
    MGENERATED_BODY()
};

MSTRUCT(SystemSet)
struct TestOptionalSet
{
    MGENERATED_BODY()
};

/// A resource the test systems write their names to as they run.
MSTRUCT()
struct TestLog
{
    MGENERATED_BODY()

    MPROPERTY()
    List<String> Entries;
};

MCLASS(Sets = (TestSchedulingSet))
class TestMoveSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    EntityQuery<Read<TestVelocity>, Write<TestPosition>> _query;
};

/// Runs after TestMoveSystem through its access to TestPosition, and counts the chunks moved since it last ran.
MCLASS(Sets = (TestSchedulingSet))
class TestWatchSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

    uint32 ChangedChunks = 0;

private:
    EntityQuery<Changed<TestPosition>> _query;
};

/// Spawns an entity pointing at another new one, through a CommandBuffer.
MCLASS(Phase = PrePhysicsPhase, Sets = (TestSchedulingSet))
class TestSpawnSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;
};

MCLASS(Phase = LatePhase, Sets = (TestSchedulingSet), UpdateAfter = TestLateFirstSystem)
class TestLateSecondSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;
};

MCLASS(Phase = LatePhase, Sets = (TestSchedulingSet))
class TestLateFirstSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;
};

MCLASS(Sets = (TestOptionalSet))
class TestOptionalSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;
};

/// Exists only while TestOptionalSet is active.
MCLASS(Sets = (TestOptionalSet))
class TestOptionalSubsystem final : public WorldSubsystem
{
    MGENERATED_BODY()
};

/// Decides for itself, from the world's sets.
MCLASS()
class TestSchedulingSubsystem final : public WorldSubsystem
{
    MGENERATED_BODY()
public:
    MFUNCTION()
    static bool ShouldCreate(const WorldContext &context);
};
} // namespace Marlin::Tests
