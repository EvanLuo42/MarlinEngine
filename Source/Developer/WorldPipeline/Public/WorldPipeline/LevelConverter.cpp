#include "WorldPipeline/LevelConverter.h"

#include "Containers/HashMap.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Math/MathUtility.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "World/EntityManager.h"
#include "World/EntityTemplate.h"
#include "World/Hierarchy.h"
#include "World/WorldPartition.h"
#include "WorldPipeline/PrefabSource.h"

namespace Marlin
{
namespace
{
/// The entities of one cell of one grid, or of AlwaysLoaded for Grid -1.
struct Bucket
{
    /// An index into the level's Grids.
    int32 Grid = -1;
    IntVector3 Cell;
    List<Entity> Entities;
};

/// The cell along one axis that holds *value*; cell n spans [n, n + 1) × size.
int32 CellOf(float64 value, float64 size)
{
    return static_cast<int32>(Math::Floor(value / size));
}

/// Where one entity goes, sorted so that the buckets and their entities come out in the same order every time.
struct Placement
{
    int32 Grid = -1;
    IntVector3 Cell;
    uint32 Entity = 0;

    bool operator<(const Placement &other) const
    {
        if (Grid != other.Grid)
        {
            return Grid < other.Grid;
        }
        if (Cell.X != other.Cell.X)
        {
            return Cell.X < other.Cell.X;
        }
        if (Cell.Y != other.Cell.Y)
        {
            return Cell.Y < other.Cell.Y;
        }
        if (Cell.Z != other.Cell.Z)
        {
            return Cell.Z < other.Cell.Z;
        }
        return Entity < other.Entity;
    }
};
} // namespace

const TypeInfo &LevelConverter::GetRuntimeType() const
{
    return TypeOf<WorldPartition>();
}

bool LevelConverter::Convert(ConversionContext &context)
{
    const MarlinTextObject &level = context.GetSource();
    LevelPartitionSettings settings;
    if (const MarlinTextField *partition = level.Find("Partition"))
    {
        MarlinTextDiagnostics diagnostics;
        if (!MarlinTextSerializer::ReadValue(partition->Value, TypeOf<LevelPartitionSettings>(), &settings,
                                             diagnostics) ||
            diagnostics.HasErrors())
        {
            context.Error(Format("{0}: Partition: {1}", level.GetName(), diagnostics.ToString()).AsView());
            return false;
        }
    }
    // RECOVERY-DECISION WP-D6: partitioning on without grids, like off, puts the whole level in AlwaysLoaded.
    const bool partitioned = settings.Enabled && !settings.Grids.IsEmpty();
    for (const LevelGridSettings &grid : settings.Grids)
    {
        if (!(grid.CellSize > 0) || !(grid.LoadingRange >= 0))
        {
            context.Error(Format("{0}: Partition: grid {1} needs a CellSize above 0 and a LoadingRange of at least 0",
                                 level.GetName(), grid.Name)
                              .AsView());
            return false;
        }
    }

    PrefabFlattener flattener(context);
    List<PrefabFlatEntity> entities;
    EntityManager manager;
    List<Entity> created;
    if (!flattener.Flatten(level, entities) || !flattener.Instantiate(entities, manager, created))
    {
        return false;
    }

    // Each hierarchy goes where its root is. Instantiate has checked that every parent chain ends at a root.
    HashMap<Guid, uint32> indices;
    for (usize i = 0; i < entities.Count(); ++i)
    {
        indices.Set(entities[i].Id, static_cast<uint32>(i));
    }
    List<int32> rootGrids;
    rootGrids.Resize(entities.Count(), 0);
    for (usize i = 0; i < entities.Count() && partitioned; ++i)
    {
        const PrefabFlatEntity &entity = entities[i];
        if (entity.Parent.IsValid() || entity.Grid.IsEmpty())
        {
            continue;
        }
        const usize grid = settings.Grids.FindIndex(
            [&](const LevelGridSettings &settingsGrid) { return settingsGrid.Name == entity.Grid; });
        if (grid == List<LevelGridSettings>::NotFound)
        {
            context.Error(Format("{0}: {1} names grid {2}, which the level does not have", level.GetName(), entity.Name,
                                 entity.Grid)
                              .AsView());
            return false;
        }
        rootGrids[i] = static_cast<int32>(grid);
    }
    List<Placement> placements;
    placements.Reserve(entities.Count());
    for (usize i = 0; i < entities.Count(); ++i)
    {
        uint32 root = static_cast<uint32>(i);
        while (entities[root].Parent.IsValid())
        {
            root = *indices.Find(entities[root].Parent);
        }
        Placement &placement = placements.AddDefaulted();
        placement.Entity = static_cast<uint32>(i);
        const LocalTransform *transform = partitioned ? manager.Get<LocalTransform>(created[root]) : nullptr;
        if (transform != nullptr)
        {
            const float64 size = settings.Grids[static_cast<usize>(rootGrids[root])].CellSize;
            placement.Grid = rootGrids[root];
            placement.Cell = IntVector3(CellOf(transform->Translation.X, size), CellOf(transform->Translation.Y, size),
                                        CellOf(transform->Translation.Z, size));
        }
    }
    placements.Sort();
    List<Bucket> buckets;
    for (const Placement &placement : placements)
    {
        if (buckets.IsEmpty() || buckets.Last().Grid != placement.Grid || !(buckets.Last().Cell == placement.Cell))
        {
            Bucket &bucket = buckets.AddDefaulted();
            bucket.Grid = placement.Grid;
            bucket.Cell = placement.Cell;
        }
        buckets.Last().Entities.Add(created[placement.Entity]);
    }

    // Every bucket becomes a template of its own; entity references into other buckets become null.
    WorldPartition result;
    result.Name = FrozenString(level.GetName());
    List<List<WorldPartitionCell>> cells;
    cells.Resize(partitioned ? settings.Grids.Count() : 0);
    for (const Bucket &bucket : buckets)
    {
        const String name = bucket.Grid < 0
                                ? String("AlwaysLoaded")
                                : Format("Cell/{0}/{1}/{2}/{3}", settings.Grids[static_cast<usize>(bucket.Grid)].Name,
                                         bucket.Cell.X, bucket.Cell.Y, bucket.Cell.Z);
        EntityTemplate content;
        StringBuilder<512> problems;
        if (!content.Capture(manager, bucket.Entities.AsSpan(), &problems))
        {
            context.Error(Format("{0}: {1}: {2}", level.GetName(), name, problems.AsView()).AsView());
            return false;
        }
        content.Name = FrozenString(Format("{0} {1}", level.GetName(), name).AsView());
        const Guid uuid = PrefabIds::DeriveNamed(context.GetGuid(), name.AsView());
        context.AddDerivedResult(uuid, content);
        if (bucket.Grid < 0)
        {
            result.AlwaysLoaded = SoftAssetRef<EntityTemplate>(uuid);
            continue;
        }
        WorldPartitionCell &cell = cells[static_cast<usize>(bucket.Grid)].AddDefaulted();
        cell.Coordinates = bucket.Cell;
        cell.Content = SoftAssetRef<EntityTemplate>(uuid);
    }
    List<WorldPartitionGrid> grids;
    for (usize g = 0; g < cells.Count(); ++g)
    {
        const LevelGridSettings &source = settings.Grids[g];
        WorldPartitionGrid &grid = grids.AddDefaulted();
        grid.Name = FrozenString(source.Name.AsView());
        grid.CellSize = source.CellSize;
        grid.LoadingRange = source.LoadingRange;
        grid.Cells = FrozenArray<WorldPartitionCell>(cells[g].AsSpan());
    }
    result.Grids = FrozenArray<WorldPartitionGrid>(grids.AsSpan());
    context.SetResult(result);
    return !context.HasFailed();
}
} // namespace Marlin
