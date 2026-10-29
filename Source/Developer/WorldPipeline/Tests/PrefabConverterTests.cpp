#include "Testing/Testing.h"

#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformFile.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"
#include "World/EntityTemplate.h"
#include "World/Hierarchy.h"
#include "WorldPipeline/PrefabConverter.h"
#include "WorldPipeline/PrefabSource.h"
#include "WorldPipelineTestTypes.h"

namespace Marlin::Tests
{
namespace
{
Guid Id(uint32 value)
{
    return {0x9F2EFAB0u, 0, 0, value};
}

/// A scratch folder /Temp/PrefabTest mounted as /PrefabTest: sources go in /PrefabTest/Content, converted blocks
/// in /PrefabTest/<UUID>.mablk, served to the asset table it starts. Everything is created and deleted through the
/// VFS, inside the /Temp mount.
class PrefabFixture
{
public:
    PrefabFixture()
    {
        (void)Vfs::Delete("/Temp/PrefabTest");
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/PrefabTest"));
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/PrefabTest/Content"));
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/PrefabTest", physical) && !physical.IsEmpty());
        _physical = physical.AsView();
        _mount = Vfs::Mount("/PrefabTest", MakeUnique<DirectoryMount>(_physical.AsView(), true), 0);
        _folders.Add(String("/PrefabTest/Content"));
        _scanner = MakeUnique<AssetScanner>(_index, _folders);
        Assets::Initialize();
    }

    ~PrefabFixture()
    {
        Assets::RemoveStore(_store);
        Assets::Shutdown();
        _scanner.Reset();
        (void)Vfs::Unmount(_mount);
        (void)Vfs::Delete("/Temp/PrefabTest");
    }

    MARLIN_NONCOPYABLE(PrefabFixture);

    void Write(StringView file, StringView text)
    {
        MARLIN_REQUIRE(Vfs::WriteAll(Format("/PrefabTest/Content/{0}", file).AsView(), text));
        List<String> changed;
        _scanner->ScanAll(changed);
    }

    /// Converts *uuid* and keeps its block for Serve; false with the conversion's errors in *errors*.
    bool Convert(const Guid &uuid, String &errors)
    {
        IndexSourceResolver resolver(_index);
        ConversionStore store(ConversionStoreDesc::InProcess("/PrefabTest/GlobalStore"));
        AssetConversion conversion(resolver, store);
        ConversionTarget target;
        target.Platform = "Windows";
        const ConversionResult result = conversion.Convert(uuid, target);
        List<uint8> bytes;
        List<uint8> bulk;
        if (!result.Succeeded || !store.ReadResult(result.Key, bytes, bulk))
        {
            errors = result.Errors;
            return false;
        }
        const String path = Format("/PrefabTest/{0}.mablk", uuid);
        const StringView block(reinterpret_cast<const char *>(bytes.Data()), bytes.Count());
        MARLIN_REQUIRE(Vfs::WriteAll(path.AsView(), block));
        _blocks.Set(uuid, path);
        return true;
    }

    /// Starts serving the blocks converted so far.
    void Serve()
    {
        class BlockStore final : public IAssetStore
        {
        public:
            explicit BlockStore(HashMap<Guid, String> blocks) : _blocks(Move(blocks))
            {
            }

            void Locate(const Guid &guid, AssetLocateCallback done) override
            {
                const String *path = _blocks.Find(guid);
                AssetLocation location;
                done(path != nullptr && Vfs::OpenRegion(path->AsView(), location.Block) ? &location : nullptr);
            }

            [[nodiscard]] StringView GetName() const override
            {
                return "Prefab test blocks";
            }

        private:
            HashMap<Guid, String> _blocks;
        };
        _store = Assets::AddStore(MakeUnique<BlockStore>(_blocks), 0);
    }

private:
    String _physical;
    MountId _mount = 0;
    AssetIndex _index;
    List<String> _folders;
    UniquePtr<AssetScanner> _scanner;
    HashMap<Guid, String> _blocks;
    AssetStoreId _store = 0;
};

/// Wheel: a spinning hub and a rim. Crate: a body, a lid and an instance of Wheel whose rim has 8 spokes. Yard: two
/// crates, the second with a bigger rim and moved 10 m along x.
String MakeContent()
{
    const Guid rimInCrate = PrefabIds::Derive(Id(6), Id(2));
    return Format(R"(!Version "1.0"

Prefab {{ !UUID = "{0}" !Name = "Wheel" Members = [<#{1}>, <#{2}>] }}
PrefabEntity {{ !UUID = "{1}" !Name = "Hub" Components = {{ LocalTransform = {{ Translation = (0, 0, 0) }} PrefabTestSpinning = {{ }} }} }}
PrefabEntity {{ !UUID = "{2}" !Name = "Rim" Parent = <#{1}> Components = {{ LocalTransform = {{ Translation = (0, 0.5, 0) }} PrefabTestWheel = {{ Radius = 0.5 }} }} }}

Prefab {{ !UUID = "{3}" !Name = "Crate" Members = [<#{4}>, <#{5}>, <#{6}>] }}
PrefabEntity {{ !UUID = "{4}" !Name = "Body" Components = {{ LocalTransform = {{ Translation = (0, 1, 0) }} }} }}
PrefabEntity {{ !UUID = "{5}" !Name = "Lid" Parent = <#{4}> Components = {{ LocalTransform = {{ Translation = (0, 1, 0) }} }} }}
Prefab : <#{0}> {{ !UUID = "{6}" !Name = "Wheel_1" Parent = <#{4}> Overrides = [<#{7}>] }}
PrefabOverride {{ !UUID = "{7}" Instance = <#{6}> Target = "{2}" Components = {{ PrefabTestWheel = {{ Spokes = 8 }} }} RemoveComponents = [] }}

Prefab {{ !UUID = "{8}" !Name = "Yard" Members = [<#{9}>, <#{10}>] }}
Prefab : <#{3}> {{ !UUID = "{9}" !Name = "Crate_1" }}
Prefab : <#{3}> {{ !UUID = "{10}" !Name = "Crate_2" Overrides = [<#{11}>, <#{12}>] }}
PrefabOverride {{ !UUID = "{11}" Instance = <#{10}> Target = "{13}" Components = {{ PrefabTestWheel = {{ Radius = 2 }} }} }}
PrefabOverride {{ !UUID = "{12}" Instance = <#{10}> Target = "{14}" Components = {{ LocalTransform = {{ Translation = (10, 1, 0) }} }} }}
)",
                  Id(0), Id(1), Id(2), Id(3), Id(4), Id(5), Id(6), rimInCrate, Id(8), Id(9), Id(10),
                  PrefabIds::Derive(Id(10), rimInCrate), PrefabIds::Derive(Id(10), Id(4)), rimInCrate, Id(4));
}
} // namespace

MARLIN_TEST(WorldPipeline, PrefabsFlattenInstancesAndOverrides)
{
    PrefabFixture fixture;
    fixture.Write("Prefabs.masset", MakeContent().AsView());
    for (const uint32 prefab : {0u, 3u, 8u})
    {
        String errors;
        MARLIN_REQUIREF(fixture.Convert(Id(prefab), errors), "{0}", errors);
    }
    fixture.Serve();
    {
        const AssetRef<EntityTemplate> wheel = Assets::LoadSync<EntityTemplate>(Id(0));
        MARLIN_REQUIRE(wheel.IsLoaded() && wheel->IsUsable());
        // The hub and the rim differ in components, so each is a group of its own.
        const EntityTemplate &prefab = *wheel.Get();
        MARLIN_EXPECT(prefab.EntityCount == 2 && prefab.Groups.Count() == 2);

        // In the crate the wheel hangs under the body, and the instance's override gave its rim 8 spokes.
        const AssetRef<EntityTemplate> crate = Assets::LoadSync<EntityTemplate>(Id(3));
        MARLIN_REQUIRE(crate.IsLoaded() && crate->IsUsable());
        MARLIN_EXPECT(crate->EntityCount == 4);
        EntityManager entities;
        List<Entity> created;
        created.Resize(crate->EntityCount);
        MARLIN_REQUIRE(crate->Instantiate(entities, created));
        uint32 rims = 0;
        for (const Entity entity : created)
        {
            const PrefabTestWheel *wheel = entities.Get<PrefabTestWheel>(entity);
            if (wheel == nullptr)
            {
                continue;
            }
            ++rims;
            MARLIN_EXPECT(wheel->Spokes == 8 && wheel->Radius == 0.5f);
            MARLIN_REQUIRE(entities.Has<Parent>(entity));
            const Parent parent = *entities.Get<Parent>(entity);
            MARLIN_EXPECT(entities.Has<PrefabTestSpinning>(parent.Value));
            MARLIN_REQUIRE(entities.Has<Parent>(parent.Value));
            const Entity body = entities.Get<Parent>(parent.Value)->Value;
            MARLIN_EXPECT(entities.Exists(body) && !entities.Has<Parent>(body));
        }
        MARLIN_EXPECT(rims == 1);

        // The yard: both crates keep the wheel's override, the second has its own on top.
        const AssetRef<EntityTemplate> yard = Assets::LoadSync<EntityTemplate>(Id(8));
        MARLIN_REQUIRE(yard.IsLoaded() && yard->IsUsable());
        MARLIN_EXPECT(yard->EntityCount == 8);
        EntityManager placed;
        List<Entity> yardEntities;
        yardEntities.Resize(yard->EntityCount);
        MARLIN_REQUIRE(yard->Instantiate(placed, yardEntities));
        int32 spokes = 0;
        uint32 bigRims = 0;
        uint32 moved = 0;
        for (const Entity entity : yardEntities)
        {
            if (const PrefabTestWheel *wheel = placed.Get<PrefabTestWheel>(entity))
            {
                spokes += wheel->Spokes;
                bigRims += wheel->Radius == 2.0f ? 1 : 0;
            }
            const LocalTransform *transform = placed.Get<LocalTransform>(entity);
            moved += transform != nullptr && !placed.Has<Parent>(entity) && transform->Translation.X == 10.0 ? 1 : 0;
        }
        MARLIN_EXPECT(spokes == 16);
        MARLIN_EXPECT(bigRims == 1 && moved == 1);
    }
    Assets::CollectGarbage();
}

MARLIN_TEST(WorldPipeline, PrefabErrorsNameTheObject)
{
    PrefabFixture fixture;
    fixture.Write("Broken.masset", Format(R"(!Version "1.0"

Prefab {{ !UUID = "{0}" !Name = "Broken" Members = [<#{1}>] }}
PrefabEntity {{ !UUID = "{1}" !Name = "Bad" Components = {{ Parent = {{ }} }} }}
)",
                                          Id(20), Id(21))
                                       .AsView());
    String errors;
    MARLIN_EXPECT(!fixture.Convert(Id(20), errors));
    MARLIN_EXPECTF(errors.AsView().Contains("Bad: Parent is computed"), "{0}", errors);
}
} // namespace Marlin::Tests
