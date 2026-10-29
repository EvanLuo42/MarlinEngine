#include "Testing/Testing.h"

#include "Config/Config.h"
#include "HAL/PlatformThread.h"
#include "World/Hierarchy.h"
#include "World/WorldManagerSubsystem.h"
#include "World/WorldPartition.h"
#include "WorldPipeline/PrefabSource.h"
#include "WorldPipelineTestContent.h"
#include "WorldPipelineTestTypes.h"

namespace Marlin::Tests
{
namespace
{
Guid LevelId(uint32 value)
{
    return {0x1E7E1000u, 0, 0, value};
}

/// A tree with a leaf in the main grid, a rock in the detail grid 300 m away and a sun without a position.
constexpr StringView LevelText = R"(!Version "1.0"

Level
{
    !UUID = "1e7e1000-0000-0000-0000-000000000001"
    !Name = "Valley"
    Members = [<#1e7e1000-0000-0000-0000-000000000002>, <#1e7e1000-0000-0000-0000-000000000003>,
               <#1e7e1000-0000-0000-0000-000000000004>, <#1e7e1000-0000-0000-0000-000000000005>]
    Partition =
    {
        Enabled = true
        Grids = [{ Name = "Main" CellSize = 100 LoadingRange = 150 }, { Name = "Detail" CellSize = 50 LoadingRange = 60 }]
    }
}
PrefabEntity { !UUID = "1e7e1000-0000-0000-0000-000000000002" !Name = "Tree" Components = { LocalTransform = { Translation = (10, 0, 0) } } }
PrefabEntity { !UUID = "1e7e1000-0000-0000-0000-000000000003" !Name = "Leaf" Parent = <#1e7e1000-0000-0000-0000-000000000002> Components = { LocalTransform = { Translation = (0, 3, 0) } } }
PrefabEntity { !UUID = "1e7e1000-0000-0000-0000-000000000004" !Name = "Rock" Grid = "Detail" Components = { LocalTransform = { Translation = (300, 0, 0) } } }
PrefabEntity { !UUID = "1e7e1000-0000-0000-0000-000000000005" !Name = "Sun" Components = { PrefabTestSpinning = { } } }
)";

/// Calls *frame* until *ready* holds, or a few seconds pass.
template <typename TReady, typename TFrame> void AdvanceUntil(TReady ready, TFrame frame)
{
    for (uint32 i = 0; i < 2000 && !ready(); ++i)
    {
        Assets::ProcessFrame();
        frame();
        PlatformThread::Sleep(1);
    }
}

/// The entities attached to an entity that is there too: a leaf that came along with its tree.
uint32 CountLeaves(EntityManager &entities)
{
    EntityQuery<Read<Parent>> attached;
    attached.Bind(entities);
    uint32 count = 0;
    attached.ForEach([&](const Parent &parent) {
        if (entities.Exists(parent.Value))
        {
            ++count;
        }
    });
    return count;
}
} // namespace

MARLIN_TEST(WorldPipeline, LevelsConvertIntoStreamingCells)
{
    ContentFixture content("LevelTest");
    content.Write("Valley.masset", LevelText);
    content.Scan();
    const ConversionResult level = content.Convert(LevelId(1));
    MARLIN_REQUIREF(level.Succeeded, "{0}", level.Errors);
    // The main cell with the tree and its leaf, the detail cell with the rock, and AlwaysLoaded with the sun.
    MARLIN_EXPECT(level.Derived.Count() == 3);
    const Guid mainCell = PrefabIds::DeriveNamed(LevelId(1), "Cell/Main/0/0/0");
    MARLIN_EXPECT(level.Derived.Contains(mainCell));
    {
        const AssetRef<WorldPartition> loaded = Assets::LoadSync<WorldPartition>(LevelId(1));
        const WorldPartition *partition = loaded.Get();
        MARLIN_REQUIRE(partition != nullptr);
        MARLIN_EXPECT(partition->AlwaysLoaded.IsValid());
        MARLIN_REQUIRE(partition->Grids.Count() == 2);
        MARLIN_EXPECT(partition->Grids[0].Cells.Count() == 1 && partition->Grids[1].Cells.Count() == 1);
        MARLIN_EXPECT(partition->Grids[0].Cells[0].Content.GetGuid() == mainCell);
        MARLIN_EXPECT(partition->Grids[1].Cells[0].Coordinates == IntVector3(6, 0, 0));

        // The tree's cell holds the leaf too, still attached.
        const AssetRef<EntityTemplate> cell = Assets::LoadSync<EntityTemplate>(mainCell);
        MARLIN_REQUIRE(cell.IsLoaded() && cell->IsUsable());
        MARLIN_EXPECT(cell->EntityCount == 2);
        EntityManager entities;
        List<Entity> created;
        created.Resize(cell->EntityCount);
        MARLIN_REQUIRE(cell->Instantiate(entities, created));
        MARLIN_EXPECT(CountLeaves(entities) == 1);
    }

    // Converting again finds the level and its cells in the DDC.
    const ConversionResult again = content.Convert(LevelId(1));
    MARLIN_EXPECT(again.Succeeded && again.Derived.Count() == 3 && again.Derived.Contains(mainCell));
    Assets::CollectGarbage();
}

MARLIN_TEST(WorldPipeline, StartupLevelsOpenInAWorldOfTheirProfile)
{
    ContentFixture content("LevelOpenTest");
    content.Write("Valley.masset", LevelText);
    content.Scan();
    MARLIN_REQUIRE(content.Convert(LevelId(1)).Succeeded);
    MARLIN_EXPECT(content.Convert(LevelId(1)).Derived.Count() == 3);

    ConfigDesc desc;
    desc.Directories.Add(String("/Engine/Config"));
    desc.Variables.Add({"Target", "Game"});
    Config::Initialize(desc);
    {
        WorldManagerSubsystem manager;
        MARLIN_EXPECT(!manager.OpenLevel(LevelId(1), "Nowhere"));
        MARLIN_REQUIRE(manager.OpenLevel(LevelId(1), "Game"));
        uint64 frame = 0;
        const auto step = [&] {
            const FrameTime time{frame++, 1.0 / 60.0, 0};
            manager.BeginFrame(time);
            manager.FixedUpdate(time.Delta);
            manager.Update(time);
            manager.EndFrame(time);
        };
        AdvanceUntil([&] { return manager.GetWorlds().Count() == 1; }, step);
        MARLIN_REQUIRE(manager.GetWorlds().Count() == 1 && manager.GetPendingLevelCount() == 0);

        // A player at the origin: the tree's cell and AlwaysLoaded stream in, the rock 300 m away does not.
        World &world = *manager.GetWorlds()[0];
        MARLIN_REQUIRE(world.GetSubsystem<WorldPartitionSubsystem>() != nullptr);
        const WorldPartitionSubsystem &streaming = *world.GetSubsystem<WorldPartitionSubsystem>();
        EntityManager &entities = world.GetEntities();
        const ComponentId components[] = {Components::IdOf<StreamingSource>(), Components::IdOf<LocalTransform>(),
                                          Components::IdOf<WorldTransform>()};
        (void)entities.CreateEntity(components);
        AdvanceUntil([&] { return streaming.IsAlwaysLoadedReady() && streaming.GetLoadedCellCount() == 1; }, step);
        MARLIN_EXPECT(streaming.IsAlwaysLoadedReady() && streaming.IsCellLoaded(0, IntVector3(0, 0, 0)));
        MARLIN_EXPECT(!streaming.IsCellLoaded(1, IntVector3(6, 0, 0)));
        MARLIN_EXPECT(CountLeaves(entities) == 1);
    }
    Config::Shutdown();
    Assets::CollectGarbage();
}
} // namespace Marlin::Tests
