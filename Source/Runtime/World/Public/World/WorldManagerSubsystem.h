#pragma once

#include "Asset/AssetRef.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "World/World.h"
#include "World/WorldPartition.h"
#include "World/WorldProfile.h"
#include "World/WorldManagerSubsystem.gen.h"

namespace Marlin
{
/// Owns the process's worlds and steps them from the main loop (Documentation/Architecture/01 §4): every world's
/// BeginFrame, FixedUpdate at the fixed rate, Update and EndFrame, in creation order. Where Engine.World says so, it
/// opens the startup level (01 §2 step 10) in a world of the startup profile.
MCLASS(Config = "Engine.World", Dependencies = "AssetSubsystem")
class WORLD_API WorldManagerSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Whether to open the startup level; Config/Engine.mtext turns it on for the Game and Server targets.
    MPROPERTY()
    static bool LoadStartupLevel;

    /// The name of the startup world's profile.
    MPROPERTY()
    static String StartupProfile;

    /// `-Level=<UUID>` on the command line, else StartupLevel in Product.mtext; an invalid Guid if neither is set.
    static Guid FindStartupLevel();

    /// A world of *profile*, which must outlive it, such as one from FindProfile.
    World &CreateWorld(const WorldProfile &profile, StringView name);
    void DestroyWorld(World &world);

    /// The profile *name* from the config layers, loaded once and kept; null if no layer defines it.
    const WorldProfile *FindProfile(StringView name);

    /// Loads level *level*, a WorldPartition, and once it has loaded opens it in a new world of profile *profile*.
    /// False if there is no such profile.
    bool OpenLevel(const Guid &level, StringView profile);

    /// Levels still loading.
    [[nodiscard]] uint32 GetPendingLevelCount() const
    {
        return static_cast<uint32>(_pending.Count());
    }

    [[nodiscard]] Span<const UniquePtr<World>> GetWorlds() const
    {
        return _worlds.AsSpan();
    }

    void Initialize(SubsystemContext &context) override;
    /// Opens the startup level, if Engine.World says so.
    void Start() override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;
    void FixedUpdate(float64 delta) override;
    void Update(const FrameTime &time) override;
    void EndFrame(const FrameTime &time) override;

private:
    struct PendingLevel
    {
        AssetRef<WorldPartition> Level;
        const WorldProfile *Profile = nullptr;
    };

    void OpenPendingLevels();

    SubsystemCollection *_engineSubsystems = nullptr;
    List<UniquePtr<World>> _worlds;
    List<PendingLevel> _pending;
    List<UniquePtr<WorldProfile>> _profiles;
};
} // namespace Marlin
