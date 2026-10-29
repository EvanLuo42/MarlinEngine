#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "World/WorldProfile.gen.h"

namespace Marlin
{
/// How a world's time advances.
MENUM()
enum class WorldClock : uint8
{
    /// The Fixed stage at the fixed rate, the Variable stage once a frame.
    Fixed,
    /// Both stages once a frame with the frame's delta.
    Variable,
    /// Time stands still: only the Variable stage runs, with a zero delta.
    Paused,
    /// Like Paused, except that World::Step runs a fixed step: single-stepping, recording.
    Manual,
};

/// The world's part in replication (Documentation/Architecture/05 §7).
MENUM()
enum class WorldNetRole : uint8
{
    None,
    Standalone,
    Server,
    Client,
};

/// World subsystems to add or remove beyond those the enabled sets select, by type name or set name.
MSTRUCT()
struct WorldProfileSubsystems
{
    MGENERATED_BODY()

    MPROPERTY()
    List<String> Include;

    MPROPERTY()
    List<String> Exclude;
};

/// What runs in a world (Documentation/Architecture/05 §1.1): the system sets it enables, the world subsystems it
/// adds or removes, its clock and network role. Data, not a mode enum: section `WorldProfiles.<Name>` of the config
/// layers, so platforms, projects and editor modes override profiles or add their own.
MSTRUCT()
struct WorldProfile
{
    MGENERATED_BODY()

    /// The section name, such as Game.
    MPROPERTY()
    String Name;

    /// A profile this one starts from: its fields first, then this section's, so `Sets += [...]` adds to the base's.
    MPROPERTY()
    String Base;

    /// Names of `MSTRUCT(SystemSet)` types.
    MPROPERTY()
    List<String> Sets;

    MPROPERTY()
    WorldProfileSubsystems Subsystems;

    // RECOVERY-DECISION D1: a section that leaves these out gets Fixed, None and true, the values of a plain game world.
    MPROPERTY()
    WorldClock Clock = WorldClock::Fixed;

    MPROPERTY()
    WorldNetRole NetRole = WorldNetRole::None;

    MPROPERTY()
    bool Rendering = true;

    /// True if Sets names *set*.
    [[nodiscard]] WORLD_API bool Has(const TypeInfo &set) const;

    template <typename T> [[nodiscard]] bool Has() const
    {
        return Has(TypeOf<T>());
    }

    /// Reads section `WorldProfiles.<name>` of the config layers into *profile*, after its Base chain. False if no
    /// layer defines it or its Base chain loops.
    WORLD_API static bool Load(StringView name, WorldProfile &profile);
};
} // namespace Marlin
