#include "Testing/Testing.h"

#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "Material/Material.h"
#include "Math/Float16.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
/// Serves conversion results to the asset table, each from the file it was exported to.
class ResultStore final : public IAssetStore
{
public:
    void Add(const Guid &guid, String path, ConvertedResultLocation location)
    {
        _results.Add({guid, Move(path), Move(location)});
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        for (const Result &result : _results)
        {
            FileRegion file;
            if (result.Uuid != guid || !Vfs::OpenRegion(result.Path.AsView(), file))
            {
                continue;
            }
            AssetLocation location;
            location.Block = file;
            location.Block.Offset = file.Offset + result.Location.BlockOffset;
            location.Block.Size = result.Location.BlockSize;
            location.Bulk = file;
            location.Bulk.Offset = file.Offset + result.Location.BulkOffset;
            location.Bulk.Size = result.Location.BulkSize;
            done(&location);
            return;
        }
        done(nullptr);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "MaterialTest";
    }

private:
    struct Result
    {
        Guid Uuid;
        String Path;
        ConvertedResultLocation Location;
    };
    List<Result> _results;
};

/// A scratch folder at /Temp/MaterialPipelineTest mounted as /MaterialTest.
struct MaterialFixture
{
    MountId Mount = 0;

    MaterialFixture()
    {
        (void)Vfs::Delete("/Temp/MaterialPipelineTest");
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/MaterialPipelineTest"));
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/MaterialPipelineTest/Content"));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/MaterialPipelineTest", path) && !path.IsEmpty());
        Mount = Vfs::Mount("/MaterialTest", MakeUnique<DirectoryMount>(path.AsView(), true), 0);
    }

    ~MaterialFixture()
    {
        if (Mount != 0)
        {
            Vfs::Unmount(Mount);
        }
        (void)Vfs::Delete("/Temp/MaterialPipelineTest");
    }

    MARLIN_NONCOPYABLE(MaterialFixture);
};

/// The half at *offset* of *bytes*, as a float.
float32 HalfAt(const FrozenArray<uint8> &bytes, uint32 offset)
{
    uint16 bits = 0;
    MemCopy(&bits, bytes.Data() + offset, sizeof(bits));
    return static_cast<float32>(Float16::FromBits(bits));
}
} // namespace

MARLIN_TEST(MaterialPipeline, MaterialsAndInstancesConvert)
{
    MaterialFixture fixture;
    // The engine's Default material: BaseColor (half3), Roughness, Metallic (halves). Grey and Rough override parts
    // of it; Shiny overrides Rough; the others are errors.
    MARLIN_REQUIRE(Vfs::WriteAll("/MaterialTest/Content/Materials.masset", R"(!Version "1.0"

Material
{
    !UUID = "3a7e0000-0000-0000-0000-000000000001"
    !Name = "Rough"
    Shader = </Engine/Language/Shaders/Materials/Default:Marlin.Materials.Default#ef2b48e6-a831-4053-b260-725ce54ce3de>
    BaseColor = (1, 0.5, 0.25)
    Roughness = 0.75
}

MaterialInstance : <Rough#3a7e0000-0000-0000-0000-000000000001>
{
    !UUID = "3a7e0000-0000-0000-0000-000000000002"
    !Name = "Shiny"
    Roughness = 0.125
    Metallic = 1
}

Material
{
    !UUID = "3a7e0000-0000-0000-0000-000000000003"
    !Name = "Unknown"
    Shader = </Engine/Language/Shaders/Materials/Default:Marlin.Materials.Default#ef2b48e6-a831-4053-b260-725ce54ce3de>
    Glossiness = 1
}

Material
{
    !UUID = "3a7e0000-0000-0000-0000-000000000004"
    !Name = "OutOfRange"
    Shader = </Engine/Language/Shaders/Materials/Default:Marlin.Materials.Default#ef2b48e6-a831-4053-b260-725ce54ce3de>
    Roughness = 2
}

MaterialInstance : <Rough#3a7e0000-0000-0000-0000-000000000001>
{
    !UUID = "3a7e0000-0000-0000-0000-000000000005"
    !Name = "Reshaded"
    Shader = </Engine/Language/Shaders/Materials/Default:Marlin.Materials.Default#ef2b48e6-a831-4053-b260-725ce54ce3de>
}
)"));
    AssetIndex index;
    List<String> folders;
    folders.Add("/MaterialTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    IndexSourceResolver resolver(index);
    ConversionStore globalStore(ConversionStoreDesc::InProcess("/MaterialTest/GlobalStore"));
    AssetConversion conversion(resolver, globalStore);
    ConversionTarget target;
    target.Platform = "Windows";
    Assets::Initialize();
    UniquePtr<ResultStore> store = MakeUnique<ResultStore>();
    ResultStore *results = store.Get();
    const AssetStoreId storeId = Assets::AddStore(Move(store), 0);

    const Guid rough = {0x3a7e0000, 0, 0, 1};
    const Guid shiny = {0x3a7e0000, 0, 0, 2};
    for (const Guid &uuid : {rough, shiny})
    {
        const ConversionResult result = conversion.Convert(uuid, target);
        MARLIN_REQUIREF(result.Succeeded, "{0}", result.Errors);
        bool reflects = false;
        for (const DependencyInput &input : result.Inputs)
        {
            reflects = reflects || (input.Kind == DependencyInputKind::Converter &&
                                    input.Section.AsView().StartsWith("MaterialShader:"));
        }
        MARLIN_EXPECT(reflects);
        const String path = Format("/MaterialTest/{0}.bin", uuid == rough ? "Rough" : "Shiny");
        MARLIN_REQUIRE(globalStore.ExportResult(result.Key, path.AsView()));
        results->Add(uuid, path, result.Location);
        // Unchanged, it is a hit: the reflection, an input, is the same.
        const ConversionResult again = conversion.Convert(uuid, target);
        MARLIN_EXPECT(again.Succeeded && again.CacheHit);
    }
    {
        const AssetRef<Material> loaded = Assets::LoadSync<Material>(rough);
        MARLIN_REQUIRE(loaded.Get() != nullptr);
        MARLIN_EXPECT(loaded->Shader.AsView() == "Marlin.Materials.Default" && loaded->Textures.IsEmpty());
        // half3 BaseColor at 0, then Roughness and Metallic.
        MARLIN_REQUIRE(loaded->Parameters.Count() >= 10);
        MARLIN_EXPECT(HalfAt(loaded->Parameters, 0) == 1 && HalfAt(loaded->Parameters, 2) == 0.5f &&
                      HalfAt(loaded->Parameters, 4) == 0.25f);
        MARLIN_EXPECT(HalfAt(loaded->Parameters, 6) == 0.75f && HalfAt(loaded->Parameters, 8) == 0);
    }
    {
        const AssetRef<Material> loaded = Assets::LoadSync<Material>(shiny);
        MARLIN_REQUIRE(loaded.Get() != nullptr);
        MARLIN_EXPECT(loaded->Name.AsView() == "Shiny" && loaded->Shader.AsView() == "Marlin.Materials.Default");
        // Its parent's color, its own roughness and metal.
        MARLIN_EXPECT(HalfAt(loaded->Parameters, 0) == 1 && HalfAt(loaded->Parameters, 6) == 0.125f &&
                      HalfAt(loaded->Parameters, 8) == 1);
    }
    const ConversionResult unknown = conversion.Convert({0x3a7e0000, 0, 0, 3}, target);
    MARLIN_EXPECTF(!unknown.Succeeded && unknown.Errors.AsView().Contains("`Glossiness` is not a parameter"), "{0}",
                   unknown.Errors);
    const ConversionResult outOfRange = conversion.Convert({0x3a7e0000, 0, 0, 4}, target);
    MARLIN_EXPECTF(!outOfRange.Succeeded && outOfRange.Errors.AsView().Contains("outside its range [0, 1]"), "{0}",
                   outOfRange.Errors);
    const ConversionResult reshaded = conversion.Convert({0x3a7e0000, 0, 0, 5}, target);
    MARLIN_EXPECTF(!reshaded.Succeeded && reshaded.Errors.AsView().Contains("draws with its parent's shader"), "{0}",
                   reshaded.Errors);
    Assets::RemoveStore(storeId);
    Assets::Shutdown();
}
} // namespace Marlin::Tests
