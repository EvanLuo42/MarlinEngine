#include "World/WorldPartition.h"

#include "Asset/Assets.h"
#include "Math/MathUtility.h"

namespace Marlin
{
// RECOVERY-DECISION D2: 1.25, a quarter of the loading range of hysteresis; Engine.WorldPartition can override it.
float64 WorldPartitionSubsystem::UnloadRangeFactor = 1.25;

namespace
{
/// The squared distance from *point* to the cell at *coordinates* of a grid of *size*-metre cells.
float64 DistanceSquaredToCell(const WorldPosition &point, IntVector3 coordinates, float64 size)
{
    const auto axis = [size](float64 value, int32 cell) {
        const float64 low = cell * size;
        const float64 high = low + size;
        return value < low ? low - value : value > high ? value - high : 0.0;
    };
    const float64 x = axis(point.X, coordinates.X);
    const float64 y = axis(point.Y, coordinates.Y);
    const float64 z = axis(point.Z, coordinates.Z);
    return x * x + y * y + z * z;
}

int32 CellOf(float64 value, float64 size)
{
    return static_cast<int32>(Math::Floor(value / size));
}
} // namespace

void WorldPartitionSubsystem::Initialize(SubsystemContext & /*context*/)
{
    _sources.Bind(GetWorld().GetEntities());
}

void WorldPartitionSubsystem::Deinitialize()
{
    UnloadAll();
    _partition.Reset();
    _next.Reset();
    _changed = false;
}

void WorldPartitionSubsystem::SetPartition(AssetRef<WorldPartition> partition)
{
    _next = Move(partition);
    _changed = true;
}

uint32 WorldPartitionSubsystem::GetLoadedCellCount() const
{
    uint32 count = 0;
    for (const CellRuntime &cell : _cells)
    {
        count += cell.Status == CellStatus::Loaded ? 1 : 0;
    }
    return count;
}

bool WorldPartitionSubsystem::IsCellLoaded(uint32 grid, IntVector3 coordinates) const
{
    const uint32 *index = _cellIndex.Find(CellKey{grid, coordinates});
    return index != nullptr && _cells[*index].Status == CellStatus::Loaded;
}

void WorldPartitionSubsystem::Unload(CellRuntime &cell)
{
    EntityManager &entities = GetWorld().GetEntities();
    for (const Entity entity : cell.Entities)
    {
        entities.DestroyEntity(entity);
    }
    cell.Entities.Clear();
    cell.Content.Reset();
    cell.Status = CellStatus::Unloaded;
}

bool WorldPartitionSubsystem::IsAlwaysLoadedReady() const
{
    return _indexed && (_alwaysLoaded.Status == CellStatus::Loaded || !_partition->AlwaysLoaded.IsValid());
}

void WorldPartitionSubsystem::Advance(CellRuntime &cell, const Guid &content)
{
    if (cell.Status == CellStatus::Loaded || !content.IsValid())
    {
        return;
    }
    if (cell.Status == CellStatus::Unloaded)
    {
        cell.Content = Assets::Load<EntityTemplate>(content);
        cell.Status = CellStatus::Loading;
    }
    const AssetState state = cell.Content.GetState();
    if (state == AssetState::Loaded && cell.Content.IsLoaded())
    {
        cell.Entities.Resize(cell.Content->EntityCount);
        if (!cell.Content->Instantiate(GetWorld().GetEntities(), cell.Entities.AsSpan()))
        {
            cell.Entities.Clear();
        }
        cell.Status = CellStatus::Loaded;
    }
    else if (state == AssetState::Failed || state == AssetState::Loaded)
    {
        // Counted as loaded, empty, so a broken cell is not requested every frame.
        MARLIN_LOG(LogWorld, Error, "Streaming content {0} of {1} failed to load", content, _partition->Name.AsView());
        cell.Status = CellStatus::Loaded;
    }
}

void WorldPartitionSubsystem::UnloadAll()
{
    Unload(_alwaysLoaded);
    for (CellRuntime &cell : _cells)
    {
        Unload(cell);
    }
    _cells.Clear();
    _cellIndex.Clear();
    _indexed = false;
}

void WorldPartitionSubsystem::Index()
{
    const WorldPartition &partition = *_partition;
    for (uint32 grid = 0; grid < partition.Grids.Count(); ++grid)
    {
        for (const WorldPartitionCell &cell : partition.Grids[grid].Cells)
        {
            _cellIndex.Set(CellKey{grid, cell.Coordinates}, static_cast<uint32>(_cells.Count()));
            CellRuntime &runtime = _cells.AddDefaulted();
            runtime.Cell = &cell;
            runtime.Grid = grid;
        }
    }
    _indexed = true;
}

void WorldPartitionSubsystem::Gather(uint32 grid, Span<const Source> sources, float64 rangeFactor,
                                     List<bool> &marks) const
{
    const WorldPartitionGrid &info = _partition->Grids[grid];
    if (info.CellSize <= 0)
    {
        return;
    }
    for (const Source &source : sources)
    {
        const float64 range = info.LoadingRange * source.RangeScale * rangeFactor;
        const WorldPosition &point = source.Position;
        const IntVector3 low(CellOf(point.X - range, info.CellSize), CellOf(point.Y - range, info.CellSize),
                             CellOf(point.Z - range, info.CellSize));
        const IntVector3 high(CellOf(point.X + range, info.CellSize), CellOf(point.Y + range, info.CellSize),
                              CellOf(point.Z + range, info.CellSize));
        for (int32 z = low.Z; z <= high.Z; ++z)
        {
            for (int32 y = low.Y; y <= high.Y; ++y)
            {
                for (int32 x = low.X; x <= high.X; ++x)
                {
                    const IntVector3 coordinates(x, y, z);
                    const uint32 *index = _cellIndex.Find(CellKey{grid, coordinates});
                    if (index != nullptr && DistanceSquaredToCell(point, coordinates, info.CellSize) <= range * range)
                    {
                        marks[*index] = true;
                    }
                }
            }
        }
    }
}

void WorldPartitionSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    if (_changed)
    {
        UnloadAll();
        _partition = Move(_next);
        _changed = false;
        if (_partition.IsValid())
        {
            Assets::Load(*_partition.GetSlot());
        }
    }
    if (!_partition.IsLoaded())
    {
        return;
    }
    if (!_indexed)
    {
        Index();
    }
    Advance(_alwaysLoaded, _partition->AlwaysLoaded.GetGuid());

    List<Source> sources;
    _sources.ForEachChunk([&](ChunkView &chunk) {
        const Span<const StreamingSource> streaming = chunk.Read<StreamingSource>();
        const Span<const WorldTransform> transforms = chunk.Read<WorldTransform>();
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            if (chunk.IsIncluded(row))
            {
                sources.Add({transforms[row].Translation, streaming[row].RangeScale});
            }
        }
    });

    // Within the loading range a cell loads; beyond UnloadRangeFactor times it unloads; in between it stays.
    List<bool> load;
    List<bool> keep;
    load.Resize(_cells.Count(), false);
    keep.Resize(_cells.Count(), false);
    for (uint32 grid = 0; grid < _partition->Grids.Count(); ++grid)
    {
        Gather(grid, sources.AsSpan(), 1.0, load);
        Gather(grid, sources.AsSpan(), UnloadRangeFactor, keep);
    }
    for (uint32 i = 0; i < _cells.Count(); ++i)
    {
        CellRuntime &cell = _cells[i];
        if (load[i] || (keep[i] && cell.Status != CellStatus::Unloaded))
        {
            Advance(cell, cell.Cell->Content.GetGuid());
        }
        else if (cell.Status != CellStatus::Unloaded)
        {
            Unload(cell);
        }
    }
}
} // namespace Marlin
