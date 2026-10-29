#include "Testing/Testing.h"

#include "HAL/PlatformThread.h"
#include "World/WorldPartition.h"
#include "WorldTestBlocks.h"
#include "WorldTestTypes.h"

namespace Marlin::Tests
{
namespace
{
/// A cell template of one entity in the middle of the 10-metre cell at *coordinates*.
void WriteCell(WorldTestBlocks &blocks, IntVector3 coordinates, const Guid &guid)
{
    EntityManager source;
    const ComponentId components[] = {Components::IdOf<LocalTransform>(), Components::IdOf<TestPosition>()};
    const Entity entity = source.CreateEntity(components);
    source.GetForWrite<LocalTransform>(entity)->Translation =
        WorldPosition(coordinates.X * 10.0 + 5, coordinates.Y * 10.0 + 5, coordinates.Z * 10.0 + 5);
    EntityTemplate cell;
    MARLIN_REQUIRE(cell.Capture(source, Span<const Entity>(&entity, 1)));
    blocks.Write(cell, guid);
}

/// Steps frames until *expected* cells and AlwaysLoaded are in, or a few seconds pass.
void AdvanceUntilLoaded(World &world, const WorldPartitionSubsystem &partition, uint32 expected)
{
    for (uint32 frame = 0; frame < 2000; ++frame)
    {
        Assets::ProcessFrame();
        const FrameTime time{frame, 1.0 / 60.0, 0};
        world.BeginFrame(time);
        world.FixedUpdate(time.Delta);
        world.Update(time);
        world.EndFrame(time);
        if (partition.IsAlwaysLoadedReady() && partition.GetLoadedCellCount() == expected)
        {
            return;
        }
        PlatformThread::Sleep(1);
    }
}
} // namespace

MARLIN_TEST(World, PartitionStreamsCellsAroundSources)
{
    WorldTestBlocks blocks("WorldPartitionTests");
    const IntVector3 coordinates[] = {IntVector3(0, 0, 0), IntVector3(1, 0, 0), IntVector3(9, 0, 0)};
    for (uint32 i = 0; i < 3; ++i)
    {
        WriteCell(blocks, coordinates[i], WorldTestBlocks::MakeGuid(10 + i));
    }
    WriteCell(blocks, IntVector3(6, 0, 0), WorldTestBlocks::MakeGuid(20));
    {
        EntityManager source;
        const Entity entity = source.CreateEntity();
        source.Add(entity, TestPosition{});
        EntityTemplate always;
        MARLIN_REQUIRE(always.Capture(source, Span<const Entity>(&entity, 1)));
        blocks.Write(always, WorldTestBlocks::MakeGuid(1));
    }
    {
        WorldPartition level;
        level.AlwaysLoaded = SoftAssetRef<EntityTemplate>(WorldTestBlocks::MakeGuid(1));
        WorldPartitionGrid grids[2];
        grids[0].Name = FrozenString("Main");
        grids[0].CellSize = 10;
        grids[0].LoadingRange = 12;
        WorldPartitionCell main[3];
        for (uint32 i = 0; i < 3; ++i)
        {
            main[i].Coordinates = coordinates[i];
            main[i].Content = SoftAssetRef<EntityTemplate>(WorldTestBlocks::MakeGuid(10 + i));
        }
        grids[0].Cells = FrozenArray<WorldPartitionCell>(Span<const WorldPartitionCell>(main));
        grids[1].Name = FrozenString("Detail");
        grids[1].CellSize = 10;
        grids[1].LoadingRange = 1;
        WorldPartitionCell detail;
        detail.Coordinates = IntVector3(6, 0, 0);
        detail.Content = SoftAssetRef<EntityTemplate>(WorldTestBlocks::MakeGuid(20));
        grids[1].Cells = FrozenArray<WorldPartitionCell>(Span<const WorldPartitionCell>(&detail, 1));
        level.Grids = FrozenArray<WorldPartitionGrid>(Span<const WorldPartitionGrid>(grids));
        blocks.Write(level, WorldTestBlocks::MakeGuid(2));
    }
    blocks.Serve();

    AssetRef<WorldPartition> partition = Assets::LoadSync<WorldPartition>(WorldTestBlocks::MakeGuid(2));
    MARLIN_REQUIRE(partition.IsLoaded());
    MARLIN_EXPECT(partition->AlwaysLoaded.IsValid() && partition->Grids.Count() == 2);
    {
        WorldProfile profile;
        profile.Sets.Add(String("CoreSimulationSet"));
        World world(profile);
        WorldPartitionSubsystem *streaming = world.GetSubsystem<WorldPartitionSubsystem>();
        MARLIN_REQUIRE(streaming != nullptr);
        EntityManager &entities = world.GetEntities();
        const ComponentId components[] = {Components::IdOf<StreamingSource>(), Components::IdOf<LocalTransform>(),
                                          Components::IdOf<WorldTransform>()};
        const Entity player = entities.CreateEntity(components);
        entities.GetForWrite<LocalTransform>(player)->Translation = WorldPosition(9, 5, 5);
        streaming->SetPartition(partition);

        // Near the origin: the two cells around the player and AlwaysLoaded.
        AdvanceUntilLoaded(world, *streaming, 2);
        MARLIN_EXPECT(streaming->IsAlwaysLoadedReady() && streaming->IsCellLoaded(0, IntVector3(0, 0, 0)));
        MARLIN_EXPECT(streaming->IsCellLoaded(0, coordinates[0]) && streaming->IsCellLoaded(0, coordinates[1]));
        MARLIN_EXPECT(!streaming->IsCellLoaded(1, IntVector3(6, 0, 0)));
        MARLIN_EXPECT(entities.GetEntityCount() == 4);

        // Far away: the near cells unload and the far one loads.
        entities.GetForWrite<LocalTransform>(player)->Translation = WorldPosition(95, 5, 5);
        AdvanceUntilLoaded(world, *streaming, 1);
        MARLIN_EXPECT(streaming->GetLoadedCellCount() == 1 && streaming->IsCellLoaded(0, coordinates[2]));
        MARLIN_EXPECT(entities.GetEntityCount() == 3);
    }
}
} // namespace Marlin::Tests
