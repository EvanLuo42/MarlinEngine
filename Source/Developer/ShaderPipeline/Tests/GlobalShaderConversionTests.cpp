#include "Testing/Testing.h"

#include "Config/Config.h"
#include "ShaderPipeline/GlobalShaderConversion.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
/// A scratch folder at /Temp/GlobalShaderConversionTest mounted as /Project, so its Language folder joins the shader
/// source set.
struct ProjectFixture
{
    MountId Mount = 0;

    ProjectFixture()
    {
        (void)Vfs::Delete("/Temp/GlobalShaderConversionTest");
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/GlobalShaderConversionTest/Language/Shaders/Passes"));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/GlobalShaderConversionTest", path) && !path.IsEmpty());
        Mount = Vfs::Mount("/Project", MakeUnique<DirectoryMount>(path.AsView(), true), 0);
    }

    ~ProjectFixture()
    {
        if (Mount != 0)
        {
            Vfs::Unmount(Mount);
        }
        (void)Vfs::Delete("/Temp/GlobalShaderConversionTest");
    }

    MARLIN_NONCOPYABLE(ProjectFixture);
};
} // namespace

MARLIN_TEST(GlobalShaderConversion, FilesItDidNotUseMissOnlyWhenTheyShadowAName)
{
    FileInfo existing;
    if (Vfs::GetInfo("/Project", existing))
    {
        // A mounted project would mix its own shaders into the source set.
        return;
    }
    ProjectFixture fixture;
    MARLIN_REQUIRE(Vfs::WriteAll("/Project/Language/Shaders/Passes/Incremental.mshader", R"(
namespace IncrementalTest;

public struct Args { public Span<float, Access.ReadWrite> Output; }

[Compute(64, 1, 1)]
void Main([DispatchThreadId] uint3 id, [Uniform] in Args args)
{
    args.Output[id.x] = Sqrt(float(id.x));
}
)"));
    // Engine.Shaders.Profiles from the engine's own config.
    ConfigDesc config;
    config.Directories.Add(String("/Engine/Config"));
    config.Variables.Add({"Platform", "Windows"});
    Config::Initialize(config);
    ConversionStore store(ConversionStoreDesc::InProcess("/Temp/GlobalShaderConversionTest/GlobalStore"));
    GlobalShaderConversion conversion(store);
    ConversionTarget target;
    target.Platform = "Windows";
    const StringView entryPoint = "IncrementalTest.Main";

    const GlobalShaderConversionResult first = conversion.Convert(entryPoint, target, {});
    MARLIN_REQUIREF(first.Succeeded, "{0}", first.Errors);
    MARLIN_EXPECT(!first.CacheHit);
    const GlobalShaderConversionResult again = conversion.Convert(entryPoint, target, {});
    MARLIN_EXPECT(again.Succeeded && again.CacheHit && again.Key == first.Key);

    // A file the shader did not use and whose names it never looked up changes nothing.
    MARLIN_REQUIRE(Vfs::WriteAll("/Project/Language/Shaders/Other.mshader",
                                 "namespace IncrementalOther; public float Sqrt(float x) => x;"));
    const GlobalShaderConversionResult unrelated = conversion.Convert(entryPoint, target, {});
    MARLIN_EXPECT(unrelated.Succeeded && unrelated.CacheHit && unrelated.Key == first.Key);

    // One that declares `Sqrt` nearer than Marlin's would be found instead, so the shader compiles again.
    MARLIN_REQUIRE(Vfs::WriteAll("/Project/Language/Shaders/Shadow.mshader",
                                 "namespace IncrementalTest; public float Sqrt(float x) => x * 2;"));
    const GlobalShaderConversionResult shadowed = conversion.Convert(entryPoint, target, {});
    MARLIN_REQUIREF(shadowed.Succeeded, "{0}", shadowed.Errors);
    MARLIN_EXPECT(!shadowed.CacheHit && shadowed.Key != first.Key);
    bool usesShadow = false;
    for (const String &file : shadowed.Files)
    {
        usesShadow = usesShadow || file.AsView().EndsWith("/Shadow.mshader");
    }
    MARLIN_EXPECT(usesShadow);
    const GlobalShaderConversionResult settled = conversion.Convert(entryPoint, target, {});
    MARLIN_EXPECT(settled.Succeeded && settled.CacheHit && settled.Key == shadowed.Key);
    Config::Shutdown();
}
} // namespace Marlin::Tests
