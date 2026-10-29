#include "Testing/Testing.h"

#include "Config/Config.h"
#include "ConfigTestTypes.h"
#include "HAL/PlatformFile.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
int32 ConfigTestRenderer::ShadowQuality = 1;
float32 ConfigTestRenderer::Gamma = 2.2f;

namespace
{
constexpr StringView EngineLayer = R"(
Renderer
{
    ShadowQuality = 2
    Gamma = 2.4
    Features = [Bloom, Fog]
    Backend = "Vulkan"
    @if Platform == Windows
    {
        Backend = "D3D12"
    }
}
Loop { FixedRate = 30 }
)";

constexpr StringView PlatformLayer = R"(
Renderer { Features += [RayTracing] }
)";

constexpr StringView ConsoleLayer = R"(
Renderer
{
    Backend = "Agc"
    Features += [Tessellation]
    @if Target == Server
    {
        Backend = "None"
    }
}
)";

constexpr StringView GenericLayer = R"(
Renderer { Features += [Generic] }
)";

constexpr StringView ProjectLayer = R"(
Renderer
{
    ShadowQuality = 3
    Features -= [Fog]
}
)";

struct ConfigFixture
{
    String Root;
    List<MountId> Mounts;

    ConfigFixture()
    {
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", path));
        path.Append("/ConfigTest");
        Root = path.AsView();
        (void)PlatformFile::DeleteDirectory(Root.CStr(), true);
        Write("Engine/Config/Test.mtext", EngineLayer);
        Write("Engine/Platforms/Windows/Config/Test.mtext", PlatformLayer);
        Write("Engine/Platforms/PS5/Config/Test.mtext", ConsoleLayer);
        Write("Engine/Platforms/Generic/Config/Test.mtext", GenericLayer);
        Write("Project/Config/Test.mtext", ProjectLayer);
        Mounts.Add(Vfs::Mount("/ConfigEngine", MakeUnique<DirectoryMount>(Root + "/Engine", false), 0));
        Mounts.Add(
            Vfs::Mount("/ConfigEngine", MakeUnique<DirectoryMount>(Root + "/Engine/Platforms/Windows", false), 10));
        Mounts.Add(Vfs::Mount("/ConfigProject", MakeUnique<DirectoryMount>(Root + "/Project", false), 0));

        ConfigDesc desc;
        desc.Directories.Add(String("/ConfigEngine/Config"));
        desc.Directories.Add(String("/ConfigProject/Config"));
        desc.Overrides.Add(String("Test.Loop.FixedRate=120"));
        desc.Variables.Add({"Platform", "Windows"});
        Config::Initialize(desc);
    }

    ~ConfigFixture()
    {
        Config::Shutdown();
        for (const MountId id : Mounts)
        {
            Vfs::Unmount(id);
        }
        (void)PlatformFile::DeleteDirectory(Root.CStr(), true);
    }

    MARLIN_NONCOPYABLE(ConfigFixture);

    void Write(StringView relative, StringView text) const
    {
        StringBuilder<512> path(Root.AsView());
        path.Append('/').Append(relative);
        StringView parent;
        StringView name;
        MARLIN_REQUIRE(path.AsView().SplitLast('/', parent, name));
        (void)PlatformFile::CreateDirectory(String(parent).CStr());
        const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
        MARLIN_REQUIRE(file.IsValid() && PlatformFile::Write(file, text.Data(), text.Length()));
        PlatformFile::Close(file);
    }
};
} // namespace

MARLIN_TEST(Config, LayersOverrideInOrder)
{
    const ConfigFixture fixture;
    MARLIN_EXPECT(Config::GetOr<int32>("Test.Renderer.ShadowQuality", 0) == 3);
    MARLIN_EXPECT(Config::GetOr<float32>("Test.Renderer.Gamma", 0) == 2.4f);
    MARLIN_EXPECT(Config::GetOr<int32>("Test.Loop.FixedRate", 0) == 120);
    MARLIN_EXPECT(Config::GetOr<int32>("Test.Loop.Missing", 7) == 7);
    MARLIN_EXPECT(Config::GetOr<int32>("Test.Nothing.Here", 9) == 9);

    int32 sections = 0;
    Config::ForEachSection("Test.Renderer", [&](const MarlinTextObject &, StringView) { ++sections; });
    MARLIN_EXPECT(sections == 3);
}

MARLIN_TEST(Config, BindsStaticAndInstanceFields)
{
    const ConfigFixture fixture;
    MARLIN_EXPECT(ConfigTestRenderer::ShadowQuality == 3 && ConfigTestRenderer::Gamma == 2.4f);

    ConfigTestRenderer renderer;
    MARLIN_EXPECT(Config::Apply(renderer));
    MARLIN_EXPECT(renderer.Backend == "D3D12");
    MARLIN_REQUIRE(renderer.Features.Count() == 2);
    MARLIN_EXPECT(renderer.Features[0] == "Bloom" && renderer.Features[1] == "RayTracing");

    ConfigTestRenderer::ShadowQuality = 0;
    Config::Reload();
    MARLIN_EXPECT(ConfigTestRenderer::ShadowQuality == 3);
}
MARLIN_TEST(Config, RestrictedUserLayersSetOnlyUserConfigurableSections)
{
    const ConfigFixture fixture;
    fixture.Write("User/Config/Test.mtext",
                  "Settings { Volume = 5 }\nCheats { GodMode = 1 }\nLoop { FixedRate = 240 }\n");
    const MountId user = Vfs::Mount("/ConfigUser", MakeUnique<DirectoryMount>(fixture.Root + "/User", false), 0);

    ConfigDesc desc;
    desc.Directories.Add(String("/ConfigEngine/Config"));
    desc.UserDirectories.Add(String("/ConfigUser/Config"));
    desc.Overrides.Add(String("Test.Loop.FixedRate=120"));
    desc.RestrictUser = true;
    Config::Initialize(desc);
    ConfigTestSettings settings;
    ConfigTestCheats cheats;
    MARLIN_EXPECT(Config::Apply(settings) && settings.Volume == 5);
    MARLIN_EXPECT(Config::Apply(cheats) && cheats.GodMode == 0);
    MARLIN_EXPECT(Config::GetOr<int32>("Test.Loop.FixedRate", 0) == 30);

    desc.RestrictUser = false;
    Config::Initialize(desc);
    MARLIN_EXPECT(Config::Apply(cheats) && cheats.GodMode == 1);
    MARLIN_EXPECT(Config::GetOr<int32>("Test.Loop.FixedRate", 0) == 120);
    Vfs::Unmount(user);
}

MARLIN_TEST(Config, ViewsReadAnotherPlatformsLayers)
{
    const ConfigFixture fixture;
    List<String> layers;
    MARLIN_REQUIRE(Config::GetPlatformLayers("PS5", layers));
    MARLIN_EXPECT(layers.Count() == 3 && layers[0] == "PS5" && layers.Last() == "Generic");
    MARLIN_EXPECT(!Config::GetPlatformLayers("Amiga", layers));

    ConfigTarget target;
    target.Platform = "PS5";
    target.Variables.Add({String("Target"), String("Game")});
    const ConfigView *view = Config::GetView(target);
    MARLIN_REQUIRE(view != nullptr);
    MARLIN_EXPECT(Config::GetView(target) == view);

    ConfigTestRenderer::ShadowQuality = 1;
    ConfigTestRenderer renderer;
    MARLIN_EXPECT(view->Apply("Test.Renderer", TypeOf<ConfigTestRenderer>(), &renderer));
    MARLIN_EXPECT(renderer.Backend == "Agc");
    MARLIN_REQUIRE(renderer.Features.Count() == 3);
    MARLIN_EXPECT(renderer.Features[0] == "Bloom" && renderer.Features[1] == "Generic" &&
                  renderer.Features[2] == "Tessellation");
    MARLIN_EXPECT(ConfigTestRenderer::ShadowQuality == 1);

    target.Variables[0].Value = "Server";
    ConfigTestRenderer server;
    MARLIN_EXPECT(Config::GetView(target)->Apply("Test.Renderer", TypeOf<ConfigTestRenderer>(), &server));
    MARLIN_EXPECT(server.Backend == "None");

    List<String> paths;
    view->GetSourcePaths("Test", paths);
    MARLIN_EXPECT(paths.Contains(String("/ConfigEngine/Platforms/PS5/Config/Test.mtext")));
    MARLIN_EXPECT(!paths.Contains(String("/ConfigEngine/Platforms/Windows/Config/Test.mtext")));

    ConfigTestRenderer host;
    MARLIN_EXPECT(Config::Apply(host));
    MARLIN_EXPECT(host.Backend == "D3D12");
}
} // namespace Marlin::Tests
