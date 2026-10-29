#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "WorldPipeline/LevelConverter.gen.h"

namespace Marlin
{
/// One level of a level's streaming grid.
MSTRUCT()
struct LevelGridSettings
{
    MGENERATED_BODY()

    /// What entities name in their Grid field.
    MPROPERTY()
    String Name;

    /// Metres.
    MPROPERTY()
    float64 CellSize = 128; // RECOVERY-DECISION WP-D5: grid defaults of 128 m cells loaded within 256 m.

    /// A cell loads while a streaming source is within this many metres of it.
    MPROPERTY()
    float64 LoadingRange = 256;
};

/// A level's `Partition = { Enabled = true Grids = [{ Name = "Detail" CellSize = 64 LoadingRange = 128 }] }`.
MSTRUCT()
struct LevelPartitionSettings
{
    MGENERATED_BODY()

    /// Streams the level by grid cells; off, the whole level is AlwaysLoaded.
    MPROPERTY()
    bool Enabled = false;

    /// The first is the grid of entities that name none.
    MPROPERTY()
    List<LevelGridSettings> Grids;
};

/// Converts a Level, a top-level prefab with partition settings (Documentation/Architecture/05 §6), into a
/// WorldPartition. Each hierarchy goes where its root is: with partitioning on, into the cell of the grid its Grid
/// names (the first by default) that holds the root's LocalTransform; otherwise, or without a LocalTransform, into
/// AlwaysLoaded. Every cell and AlwaysLoaded is an EntityTemplate of its own, a derived result with the UUID
/// PrefabIds::DeriveNamed(level, "Cell/<grid>/<x>/<y>/<z>") or (level, "AlwaysLoaded"). References between entities
/// of different cells become null.
MCLASS()
class WORLDPIPELINE_API LevelConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "Level";
    }

    [[nodiscard]] const TypeInfo &GetRuntimeType() const override;

    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }

    bool Convert(ConversionContext &context) override;

    [[nodiscard]] bool ProducesDerivedResults() const override
    {
        return true;
    }
};
} // namespace Marlin
