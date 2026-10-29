#pragma once

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Containers/FrozenArray.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "World/EntityQuery.h"
#include "World/EntityTemplate.h"
#include "World/Hierarchy.h"
#include "World/World.h"
#include "World/WorldPartition.gen.h"

namespace Marlin
{
/// One cell of a grid: the entities placed in it, converted to their own block so they load on their own.
MSTRUCT()
struct WorldPartitionCell
{
    MGENERATED_BODY()

    /// The cell spans [Coordinates, Coordinates + 1) × CellSize on each axis.
    MPROPERTY()
    IntVector3 Coordinates;

    MPROPERTY()
    SoftAssetRef<EntityTemplate> Content;
};

/// One level of the grid: large cells for terrain and big objects, small ones for detail.
MSTRUCT()
struct WorldPartitionGrid
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Name;

    /// Metres.
    MPROPERTY()
    float64 CellSize = 0;

    /// A cell loads while a streaming source is within this many metres of it.
    MPROPERTY()
    float64 LoadingRange = 0;

    MPROPERTY()
    FrozenArray<WorldPartitionCell> Cells;
};

/// A converted level (Documentation/Architecture/05 §6): what is always loaded, and the grid cells streamed by
/// distance when the level enables partitioning. Without partitioning every entity is in AlwaysLoaded.
MCLASS()
class WORLD_API WorldPartition final : public Asset
{
    MGENERATED_BODY()
public:
    /// Entities without a position, and every entity of a level that does not partition.
    MPROPERTY()
    SoftAssetRef<EntityTemplate> AlwaysLoaded;

    MPROPERTY()
    FrozenArray<WorldPartitionGrid> Grids;
};

/// What streaming centres on, such as the player or a camera; its WorldTransform gives the position.
MSTRUCT(Component)
struct StreamingSource
{
    MGENERATED_BODY()

    /// Multiplies every grid's LoadingRange for this source.
    MPROPERTY()
    float32 RangeScale = 1;
};

/// Loads the cells of the world's partition near its streaming sources and unloads those that fall out of range:
/// each cell's template is loaded asynchronously, instantiated when ready and its entities destroyed when unloaded.
MCLASS(Config = "Engine.WorldPartition")
class WORLD_API WorldPartitionSubsystem final : public WorldSubsystem
{
    MGENERATED_BODY()
public:
    /// A loaded cell is kept until every source is this many times its loading range away, so a source on a cell's
    /// edge does not load and unload it every frame.
    MPROPERTY()
    static float64 UnloadRangeFactor;

    /// Streams *partition* from the next frame on, unloading what the previous one loaded.
    void SetPartition(AssetRef<WorldPartition> partition);

    /// Grid cells loaded, AlwaysLoaded not counted.
    [[nodiscard]] uint32 GetLoadedCellCount() const;
    /// True once AlwaysLoaded is instantiated, or if the partition has none.
    [[nodiscard]] bool IsAlwaysLoadedReady() const;
    [[nodiscard]] bool IsCellLoaded(uint32 grid, IntVector3 coordinates) const;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

private:
    enum class CellStatus : uint8
    {
        Unloaded,
        Loading,
        Loaded,
    };

    struct CellKey
    {
        uint32 Grid = 0;
        IntVector3 Coordinates;

        bool operator==(const CellKey &) const = default;

        [[nodiscard]] uint64 GetHash() const
        {
            return HashCombine(HashInteger(Grid), Coordinates.GetHash());
        }
    };

    struct CellRuntime
    {
        const WorldPartitionCell *Cell = nullptr;
        uint32 Grid = 0;
        CellStatus Status = CellStatus::Unloaded;
        AssetRef<EntityTemplate> Content;
        List<Entity> Entities;
    };

    struct Source
    {
        WorldPosition Position;
        float64 RangeScale = 1;
    };

    void Unload(CellRuntime &cell);
    /// Moves *cell* towards loaded: starts loading *content*, or instantiates it once it has loaded.
    void Advance(CellRuntime &cell, const Guid &content);
    void UnloadAll();
    void Index();
    /// Marks the cells of grid *grid* within *rangeFactor* times the loading range of a source.
    void Gather(uint32 grid, Span<const Source> sources, float64 rangeFactor, List<bool> &marks) const;

    AssetRef<WorldPartition> _partition;
    AssetRef<WorldPartition> _next;
    CellRuntime _alwaysLoaded;
    bool _changed = false;
    bool _indexed = false;
    List<CellRuntime> _cells;
    HashMap<CellKey, uint32> _cellIndex;
    EntityQuery<Read<StreamingSource>, Read<WorldTransform>> _sources;
};
} // namespace Marlin
