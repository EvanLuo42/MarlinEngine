#include "Testing/Testing.h"

#include "Config/Config.h"
#include "HAL/PlatformFile.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"
#include "World/WorldManagerSubsystem.h"
#include "World/WorldProfile.h"

namespace Marlin::Tests
{
namespace
{
/// The config layers of a target: /Engine/Config and a project layer at /WorldProfileProject/Config, in a folder
/// under /Temp that it creates and deletes again.
class ProfileConfig
{
public:
    explicit ProfileConfig(StringView target)
    {
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/WorldProfileTests", physical));
        MARLIN_REQUIRE(physical.AsView().EndsWith("WorldProfileTests"));
        _physical = physical.AsView();
        (void)PlatformFile::DeleteDirectory(_physical.CStr(), true);
        MARLIN_REQUIRE(PlatformFile::CreateDirectory(_physical.CStr()));
        _mount = Vfs::Mount("/WorldProfileProject", MakeUnique<DirectoryMount>(_physical.AsView(), true), 0);
        MARLIN_REQUIRE(Vfs::CreateDirectory("/WorldProfileProject/Config"));
        MARLIN_REQUIRE(Vfs::WriteAll("/WorldProfileProject/Config/WorldProfiles.mtext", R"(!Version "1.0"

Sandbox
{
    Base = "Client"
    Sets -= ["PhysicsSet"]
    Clock = Variable
}

EditorEdit
{
    Rendering = false
}

Loop
{
    Base = "Loop"
}
)"));
        ConfigDesc desc;
        desc.Directories.Add(String("/Engine/Config"));
        desc.Directories.Add(String("/WorldProfileProject/Config"));
        desc.Variables.Add({"Target", target});
        Config::Initialize(desc);
    }

    ~ProfileConfig()
    {
        Config::Shutdown();
        (void)Vfs::Unmount(_mount);
        (void)PlatformFile::DeleteDirectory(_physical.CStr(), true);
    }

    MARLIN_NONCOPYABLE(ProfileConfig);

private:
    String _physical;
    MountId _mount = 0;
};

void ReadStartupSettings(bool &load, String &profile)
{
    WorldManagerSubsystem::LoadStartupLevel = false;
    WorldManagerSubsystem::StartupProfile.Clear();
    MARLIN_EXPECT(Config::Apply(TypeOf<WorldManagerSubsystem>(), nullptr));
    load = WorldManagerSubsystem::LoadStartupLevel;
    profile = WorldManagerSubsystem::StartupProfile;
}

String JoinSets(const WorldProfile &profile)
{
    StringBuilder<256> joined;
    for (usize i = 0; i < profile.Sets.Count(); ++i)
    {
        joined.Append(i > 0 ? ", " : "");
        joined.Append(profile.Sets[i].AsView());
    }
    return String(joined.AsView());
}
} // namespace

MARLIN_TEST(World, ProfilesComeFromConfigLayers)
{
    const ProfileConfig config("Game");
    WorldProfile game;
    MARLIN_REQUIRE(WorldProfile::Load("Game", game));
    MARLIN_EXPECT(game.Name == "Game" && game.Has<PresentationSet>() && game.Clock == WorldClock::Fixed);
    WorldProfile server;
    MARLIN_REQUIRE(WorldProfile::Load("Server", server));
    MARLIN_EXPECT(!server.Rendering && server.NetRole == WorldNetRole::Server && server.Clock == WorldClock::Fixed);

    // The project layer overrides EditorEdit, which EditorSimulate starts from.
    WorldProfile simulate;
    MARLIN_REQUIRE(WorldProfile::Load("EditorSimulate", simulate));
    MARLIN_EXPECT(simulate.Clock == WorldClock::Fixed && simulate.NetRole == WorldNetRole::None && !simulate.Rendering);
    WorldProfile preview;
    MARLIN_REQUIRE(WorldProfile::Load("AssetPreview", preview));
    MARLIN_EXPECT(preview.Clock == WorldClock::Variable && !preview.Has<GameplaySet>());

    // A project profile on top of an engine one, removing a set its base chain added.
    WorldProfile sandbox;
    MARLIN_REQUIRE(WorldProfile::Load("Sandbox", sandbox));
    MARLIN_EXPECT(sandbox.NetRole == WorldNetRole::Client && sandbox.Clock == WorldClock::Variable);
    MARLIN_EXPECTF(JoinSets(sandbox) == "CoreSimulationSet, GameplaySet, PresentationSet, NetClientSet", "{0}",
                   JoinSets(sandbox));

    WorldProfile missing;
    MARLIN_EXPECT(!WorldProfile::Load("Nowhere", missing) && !WorldProfile::Load("Loop", missing));

    WorldManagerSubsystem manager;
    const WorldProfile *found = manager.FindProfile("Server");
    MARLIN_EXPECT(found != nullptr && found == manager.FindProfile("Server") &&
                  manager.FindProfile("Nowhere") == nullptr);
    MARLIN_EXPECT(!manager.OpenLevel(Guid{0x3E57B10Cu, 0, 0, 1}, "Nowhere") && manager.GetPendingLevelCount() == 0);
}

MARLIN_TEST(World, ServersStartInTheServerProfile)
{
    const ProfileConfig config("Server");
    bool load = false;
    String startup;
    ReadStartupSettings(load, startup);
    MARLIN_EXPECT(load && startup == "Server");
}

MARLIN_TEST(World, OtherTargetsDoNotOpenALevel)
{
    const ProfileConfig config("MarlinTests");
    bool load = true;
    String startup;
    ReadStartupSettings(load, startup);
    MARLIN_EXPECT(!load && startup == "Game");
}
} // namespace Marlin::Tests
