#include "Testing/Testing.h"

#include "Asset/Assets.h"
#include "ScriptPipeline/ScriptCompiler.h"
#include "ScriptPipeline/ScriptModuleConversion.h"
#include "ScriptVM/ScriptDomain.h"
#include "ScriptVM/ScriptModuleAsset.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
/// Serves converted results exported to files, as packages and GlobalStore serve them to a game.
class ExportedResultStore final : public IAssetStore
{
public:
    void Add(const Guid &guid, StringView path, const ConvertedResultLocation &location)
    {
        _guids.Add(guid);
        _paths.Add(String(path));
        _locations.Add(location);
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const usize index = _guids.IndexOf(guid);
        FileRegion file;
        if (index == List<Guid>::NotFound || !Vfs::OpenRegion(_paths[index].AsView(), file))
        {
            done(nullptr);
            return;
        }
        const ConvertedResultLocation &found = _locations[index];
        AssetLocation location;
        location.Block = file;
        location.Block.Offset = file.Offset + found.BlockOffset;
        location.Block.Size = found.BlockSize;
        location.Bulk = file;
        location.Bulk.Offset = file.Offset + found.BulkOffset;
        location.Bulk.Size = found.BulkSize;
        done(&location);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "Exported script modules";
    }

private:
    List<Guid> _guids;
    List<String> _paths;
    List<ConvertedResultLocation> _locations;
};
} // namespace

MARLIN_TEST(ScriptPipeline, EnginePackageCompiles)
{
    List<String> packages;
    GetScriptPackages(packages);
    MARLIN_REQUIRE(!packages.IsEmpty() && packages[0] == "Engine");

    const ScriptCompileResult result = CompileScriptPackage("Engine", {});
    MARLIN_REQUIREF(result.Succeeded, "{0}", result.Errors);
    MARLIN_EXPECT(result.Module.Strings[result.Module.Package] == "Engine");
    // Every script source is a dependency, including those the package does not use, as a new one could add to it.
    bool world = false;
    bool common = false;
    for (const ScriptSourceDependency &dependency : result.Dependencies)
    {
        world = world || dependency.Path == "/Engine/Language/Scripts/Core/World.mscript";
        common = common || dependency.Path == "/Engine/Language/Common/Core/Marlin.mcommon";
    }
    MARLIN_EXPECT(world && common);
}

/// Every package of the mounted source set compiles: the engine's, and with `-Project=` the project's, such as
/// Samples/Sandbox. A package without scripts is skipped, as its module is empty.
MARLIN_TEST(ScriptPipeline, MountedPackagesCompile)
{
    List<String> packages;
    GetScriptPackages(packages);
    List<String> files;
    GetScriptSourceFiles(files);
    for (const String &package : packages)
    {
        if (HasScripts(package.AsView(), files.AsSpan()))
        {
            const ScriptCompileResult result = CompileScriptPackage(package.AsView(), {});
            MARLIN_EXPECTF(result.Succeeded, "{0}: {1}", package, result.Errors);
        }
    }
}

MARLIN_TEST(ScriptPipeline, ModulesConvertThroughGlobalStoreAndLoadAsAssets)
{
    MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/ScriptPipelineTest"));
    ConversionStore globalStore(ConversionStoreDesc::InProcess("/Temp/ScriptPipelineTest/GlobalStore"));
    ScriptModuleConversion conversion(globalStore);
    ConversionTarget target;
    target.Platform = "Windows";
    const ScriptModuleConversionResult first = conversion.Convert("Engine", target, {});
    MARLIN_REQUIREF(first.Succeeded, "{0}", first.Errors);
    const ScriptModuleConversionResult second = conversion.Convert("Engine", target, {});
    MARLIN_REQUIRE(second.Succeeded);
    MARLIN_EXPECT(second.CacheHit && second.Key == first.Key && second.Files.Count() == first.Files.Count());

    // A package without scripts converts to an empty module instead of failing.
    const ScriptModuleConversionResult empty = conversion.Convert("Nowhere", target, {});
    MARLIN_REQUIREF(empty.Succeeded, "{0}", empty.Errors);

    auto store = MakeUnique<ExportedResultStore>();
    const Guid engine = GetScriptModuleGuid("Engine");
    const Guid nowhere = GetScriptModuleGuid("Nowhere");
    MARLIN_REQUIRE(globalStore.ExportResult(first.Key, "/Temp/ScriptPipelineTest/Engine.bin"));
    MARLIN_REQUIRE(globalStore.ExportResult(empty.Key, "/Temp/ScriptPipelineTest/Nowhere.bin"));
    store->Add(engine, "/Temp/ScriptPipelineTest/Engine.bin", first.Location);
    store->Add(nowhere, "/Temp/ScriptPipelineTest/Nowhere.bin", empty.Location);

    Assets::Initialize();
    const AssetStoreId storeId = Assets::AddStore(Move(store), 0);
    {
        const AssetRef<ScriptModuleAsset> module = Assets::LoadSync<ScriptModuleAsset>(engine);
        MARLIN_REQUIRE(module.Get() != nullptr);
        MARLIN_EXPECT(module->Package.AsView() == "Engine" && !module->Bytecode.IsEmpty());
        const AssetRef<ScriptModuleAsset> none = Assets::LoadSync<ScriptModuleAsset>(nowhere);
        MARLIN_REQUIRE(none.Get() != nullptr);
        MARLIN_EXPECT(none->Bytecode.IsEmpty());

        // The bytes are what the VM reads, and a domain links and initializes them.
        ScriptVM vm;
        String error;
        const ScriptModuleData *data = vm.AddModule(module->Bytecode.AsSpan(), error);
        MARLIN_REQUIREF(data != nullptr, "{0}", error);
        ScriptDomain domain(vm);
        MARLIN_REQUIREF(domain.Load(*data, error), "{0}", error);
        bool found = false;
        for (const ScriptFunctionInfo *function : domain.GetModules()[0]->Functions)
        {
            found = found || function->Name.AsView().StartsWith("Marlin.Entity.IsAlive(");
        }
        MARLIN_EXPECT(found);
    }
    Assets::CollectGarbage();
    Assets::RemoveStore(storeId);
    Assets::Shutdown();
}
} // namespace Marlin::Tests
