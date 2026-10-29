#include "Testing/Testing.h"

#include "World/World.h"
#include "WorldTestTypes.h"

namespace Marlin::Tests
{
void TestMoveSystem::Configure(SystemBuilder &builder)
{
    builder.AddQuery(_query);
    builder.WriteResource<TestLog>();
}

void TestMoveSystem::Execute(SystemContext &context)
{
    context.GetResource<TestLog>()->Entries.Add(String("Move"));
    const auto delta = static_cast<float32>(context.GetDeltaTime());
    context.ForEach(_query, [delta](const TestVelocity &velocity, TestPosition &position) {
        position.Value += velocity.Value * delta;
    });
}

void TestWatchSystem::Configure(SystemBuilder &builder)
{
    builder.AddQuery(_query);
}

void TestWatchSystem::Execute(SystemContext &context)
{
    ChangedChunks = 0;
    _query.ForEachChunk(context.GetVersions(), [this](ChunkView &) { ++ChangedChunks; });
}

void TestSpawnSystem::Configure(SystemBuilder &builder)
{
    builder.WriteResource<TestLog>();
}

void TestSpawnSystem::Execute(SystemContext &context)
{
    context.GetResource<TestLog>()->Entries.Add(String("Spawn"));
    CommandBuffer &commands = context.GetCommands();
    const Entity target = commands.CreateEntity();
    commands.Add(target, TestLabel{String("spawned target")});
    const Entity spawner = commands.CreateEntity();
    commands.Add(spawner, TestTarget{target, {}});
}

void TestLateFirstSystem::Configure(SystemBuilder &builder)
{
    builder.WriteResource<TestLog>();
}

void TestLateFirstSystem::Execute(SystemContext &context)
{
    context.GetResource<TestLog>()->Entries.Add(String("LateFirst"));
}

void TestLateSecondSystem::Configure(SystemBuilder &builder)
{
    builder.WriteResource<TestLog>();
}

void TestLateSecondSystem::Execute(SystemContext &context)
{
    context.GetResource<TestLog>()->Entries.Add(String("LateSecond"));
}

void TestOptionalSystem::Configure(SystemBuilder &builder)
{
    builder.WriteResource<TestLog>();
}

void TestOptionalSystem::Execute(SystemContext &context)
{
    context.GetResource<TestLog>()->Entries.Add(String("Optional"));
}

bool TestSchedulingSubsystem::ShouldCreate(const WorldContext &context)
{
    return context.HasSet<TestSchedulingSet>();
}

namespace
{
void FillProfile(WorldProfile &profile, Span<const StringView> sets, WorldClock clock)
{
    profile.Name = String("Test");
    profile.Sets.Clear();
    for (const StringView set : sets)
    {
        profile.Sets.Add(String(set));
    }
    profile.Clock = clock;
}

void RunFrame(World &world, uint64 frame)
{
    const FrameTime time{frame, 0.5, 0.5 * static_cast<float64>(frame)};
    world.BeginFrame(time);
    world.FixedUpdate(0.5);
    world.Update(time);
    world.EndFrame(time);
}
} // namespace

MARLIN_TEST(World, PhasesFollowTheAnchors)
{
    WorldProfile profile;
    const StringView sets[] = {"CoreSimulationSet"};
    FillProfile(profile, sets, WorldClock::Fixed);
    World world(profile);
    const Span<const PhaseInfo> phases = world.GetScheduler().GetPhases();
    const auto indexOf = [&](const TypeInfo &type) {
        for (usize i = 0; i < phases.Count(); ++i)
        {
            if (phases[i].Type == &type)
            {
                return i;
            }
        }
        return phases.Count();
    };
    MARLIN_REQUIRE(indexOf(TypeOf<LatePhase>()) < phases.Count());
    MARLIN_EXPECT(indexOf(TypeOf<InputPhase>()) < indexOf(TypeOf<PrePhysicsPhase>()));
    MARLIN_EXPECT(indexOf(TypeOf<PhysicsPhase>()) < indexOf(TypeOf<PostPhysicsPhase>()));
    MARLIN_EXPECT(indexOf(TypeOf<GameplayPhase>()) < indexOf(TypeOf<NetPhase>()));
    MARLIN_EXPECT(indexOf(TypeOf<NetPhase>()) < indexOf(TypeOf<CameraPhase>()));
    MARLIN_EXPECT(indexOf(TypeOf<UIPhase>()) < indexOf(TypeOf<LatePhase>()));
    MARLIN_EXPECT(phases[indexOf(TypeOf<NetPhase>())].Stage == WorldStage::Fixed);
    MARLIN_EXPECT(phases[indexOf(TypeOf<LatePhase>())].Stage == WorldStage::Variable);
}

MARLIN_TEST(World, SystemsRunByPhaseAccessAndOrder)
{
    WorldProfile profile;
    const StringView sets[] = {"CoreSimulationSet", "TestSchedulingSet"};
    FillProfile(profile, sets, WorldClock::Fixed);
    World world(profile);
    EntityManager &entities = world.GetEntities();
    const Entity entity = entities.CreateEntity();
    entities.Add(entity, TestPosition{Vector3(1, 2, 3)});
    entities.Add(entity, TestVelocity{Vector3(2, 0, 0)});

    SystemScheduler &scheduler = world.GetScheduler();
    MARLIN_EXPECT(scheduler.FindSystem<TestMoveSystem>() != nullptr);
    MARLIN_EXPECT(scheduler.FindSystem<TransformPropagationSystem>() != nullptr);
    MARLIN_EXPECT(scheduler.FindSystem<TestOptionalSystem>() == nullptr);
    MARLIN_EXPECT(world.GetSubsystem<TestSchedulingSubsystem>() != nullptr);
    MARLIN_EXPECT(world.GetSubsystem<TestOptionalSubsystem>() == nullptr);

    RunFrame(world, 1);
    const TestLog *log = world.GetResources().Find<TestLog>();
    MARLIN_REQUIRE(log != nullptr);
    MARLIN_REQUIRE(log->Entries.Count() == 4);
    MARLIN_EXPECT(log->Entries[0] == "Spawn" && log->Entries[1] == "Move");
    MARLIN_EXPECT(log->Entries[2] == "LateFirst" && log->Entries[3] == "LateSecond");
    MARLIN_EXPECT(entities.Get<TestPosition>(entity)->Value == Vector3(2, 2, 3));

    // The Late phase holds both late systems, in their UpdateAfter order.
    List<const TypeInfo *> late;
    const Span<const PhaseInfo> phases = scheduler.GetPhases();
    for (uint32 i = 0; i < phases.Count(); ++i)
    {
        if (phases[i].Type == &TypeOf<LatePhase>())
        {
            scheduler.GetSystems(i, late);
        }
    }
    MARLIN_REQUIRE(late.Count() == 2);
    MARLIN_EXPECT(late[0] == &TypeOf<TestLateFirstSystem>() && late[1] == &TypeOf<TestLateSecondSystem>());

    // The spawned pair was created at the sync point after PrePhysics, with the reference between them resolved.
    EntityQuery<Read<TestTarget>> spawned;
    spawned.Bind(entities);
    uint32 count = 0;
    spawned.ForEach([&](const TestTarget &target) {
        const TestLabel *label = entities.Get<TestLabel>(target.Value);
        MARLIN_EXPECT(label != nullptr && label->Text == "spawned target");
        ++count;
    });
    MARLIN_EXPECT(count == 1);
}

MARLIN_TEST(World, ChangedFilterSkipsUntouchedChunks)
{
    WorldProfile profile;
    const StringView sets[] = {"TestSchedulingSet"};
    FillProfile(profile, sets, WorldClock::Fixed);
    World world(profile);
    EntityManager &entities = world.GetEntities();
    const Entity moving = entities.CreateEntity();
    entities.Add(moving, TestPosition{});
    entities.Add(moving, TestVelocity{Vector3(1, 0, 0)});
    const Entity still = entities.CreateEntity();
    entities.Add(still, TestPosition{});

    TestWatchSystem &watch = *world.GetScheduler().FindSystem<TestWatchSystem>();
    RunFrame(world, 1);
    MARLIN_EXPECT(world.GetScheduler().FindSystem<TestWatchSystem>()->ChangedChunks > 0);
    RunFrame(world, 2);
    MARLIN_EXPECT(watch.ChangedChunks == 1);
    entities.DestroyEntity(moving);
    RunFrame(world, 3);
    MARLIN_EXPECT(watch.ChangedChunks == 0);
}

MARLIN_TEST(World, ProfilesAndPushedSetsChangeAtFrameBoundaries)
{
    WorldProfile paused;
    const StringView pausedSets[] = {"TestSchedulingSet"};
    FillProfile(paused, pausedSets, WorldClock::Paused);
    World world(paused);
    ISystem *move = world.GetScheduler().FindSystem<TestMoveSystem>();
    MARLIN_REQUIRE(move != nullptr);
    RunFrame(world, 1);
    const TestLog &log = *world.GetResources().Find<TestLog>();
    MARLIN_EXPECT(!log.Entries.Contains(String("Move")) && log.Entries.Contains(String("LateFirst")));

    const TypeInfo *optional[] = {&TypeOf<TestOptionalSet>()};
    world.PushSets(optional);
    MARLIN_EXPECT(world.GetScheduler().FindSystem<TestOptionalSystem>() == nullptr);
    WorldProfile running;
    FillProfile(running, pausedSets, WorldClock::Fixed);
    world.SetProfile(running);
    RunFrame(world, 2);
    MARLIN_EXPECT(world.GetScheduler().FindSystem<TestOptionalSystem>() != nullptr);
    MARLIN_EXPECT(world.GetSubsystem<TestOptionalSubsystem>() != nullptr);
    MARLIN_EXPECT(log.Entries.Contains(String("Move")) && log.Entries.Contains(String("Optional")));
    MARLIN_EXPECT(world.GetScheduler().FindSystem<TestMoveSystem>() == move);

    world.PopSets();
    RunFrame(world, 3);
    MARLIN_EXPECT(world.GetScheduler().FindSystem<TestOptionalSystem>() == nullptr);
    MARLIN_EXPECT(world.GetSubsystem<TestOptionalSubsystem>() == nullptr);
    MARLIN_EXPECT(world.GetSubsystem<TestSchedulingSubsystem>() != nullptr);

    WorldProfile core;
    const StringView coreSets[] = {"CoreSimulationSet"};
    FillProfile(core, coreSets, WorldClock::Fixed);
    world.SetProfile(core);
    RunFrame(world, 4);
    MARLIN_EXPECT(world.GetScheduler().FindSystem<TestMoveSystem>() == nullptr);
    MARLIN_EXPECT(world.GetSubsystem<TestSchedulingSubsystem>() == nullptr);
    MARLIN_EXPECT(!world.HasSet(TypeOf<GameplaySet>()));
}

MARLIN_TEST(World, EventsLastTwoUpdates)
{
    EventStore store;
    Events<TestPosition> &events = store.Get<TestPosition>();
    events.Send(TestPosition{Vector3(1, 0, 0)});
    MARLIN_EXPECT(events.GetCurrent().Count() == 1 && events.GetPrevious().IsEmpty());
    store.Update();
    MARLIN_EXPECT(events.GetPrevious().Count() == 1 && events.GetCurrent().IsEmpty());
    uint32 seen = 0;
    events.ForEach([&](const TestPosition &) { ++seen; });
    MARLIN_EXPECT(seen == 1);
    store.Update();
    MARLIN_EXPECT(events.GetPrevious().IsEmpty() && events.GetCurrent().IsEmpty());
    MARLIN_EXPECT(&store.Get<TestPosition>() == &events);
}
} // namespace Marlin::Tests
