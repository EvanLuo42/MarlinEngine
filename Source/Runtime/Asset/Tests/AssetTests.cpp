#include "Testing/Testing.h"

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "Asset/BulkStreamer.h"
#include "AssetTestTypes.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformThread.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
int32 TestTextureAsset::Live = 0;

namespace
{
/// Serves blocks written to files under /Temp, as a package catalog would serve them from a package.
class TestBlockStore final : public IAssetStore
{
public:
    explicit TestBlockStore(HashMap<Guid, String> files) : _files(Move(files))
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const String *path = _files.Find(guid);
        AssetLocation location;
        done(path != nullptr && Vfs::OpenRegion(path->AsView(), location.Block) ? &location : nullptr);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "Test blocks";
    }

private:
    HashMap<Guid, String> _files;
};

/// Converts objects into blocks under /Temp/AssetTests and serves them; shuts the table down when it goes.
struct AssetFixture
{
    HashMap<Guid, String> Files;
    AssetStoreId Store = 0;
    String Directory;

    AssetFixture()
    {
        Assets::Initialize();
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/AssetTests", physical));
        Directory = physical.AsView();
        (void)PlatformFile::DeleteDirectory(Directory.CStr(), true);
        MARLIN_REQUIRE(PlatformFile::CreateDirectory(Directory.CStr()));
    }

    ~AssetFixture()
    {
        Assets::RemoveStore(Store);
        Assets::Shutdown();
        (void)PlatformFile::DeleteDirectory(Directory.CStr(), true);
    }

    MARLIN_NONCOPYABLE(AssetFixture);

    void Convert(const Asset &asset, const Guid &guid)
    {
        List<uint8> block;
        MARLIN_REQUIRE(AssetBlock::Write(asset, guid, block));
        WriteBlock(guid, block);
    }

    void WriteBlock(const Guid &guid, const List<uint8> &block)
    {
        StringBuilder<64> name;
        name.Append("/Temp/AssetTests/");
        guid.FormatTo(name, "N");
        name.Append(".mablk");
        MARLIN_REQUIRE(
            Vfs::WriteAll(name.AsView(), StringView(reinterpret_cast<const char *>(block.Data()), block.Count())));
        Files.FindOrAdd(guid) = name.AsView();
    }

    void Serve()
    {
        Store = Assets::AddStore(MakeUnique<TestBlockStore>(Files), 0);
    }
};

Guid MakeGuid(uint32 value)
{
    return {0x5E57A55Eu, 0, 0, value};
}

/// A block in memory whose reads wait until the test lets them through.
class GatedSource final : public IRegionSource
{
public:
    explicit GatedSource(List<uint8> bytes) : _bytes(Move(bytes))
    {
    }

    void Read(uint64 offset, uint64 size, uint8 *buffer, Function<void(bool succeeded)> done) const override
    {
        const ScopeLock lock(_lock);
        _pending.Add({offset, size, buffer, Move(done)});
    }

    [[nodiscard]] bool IsWaiting() const
    {
        const ScopeLock lock(_lock);
        return !_pending.IsEmpty();
    }

    void Open()
    {
        List<Pending> pending;
        {
            const ScopeLock lock(_lock);
            pending = Move(_pending);
            _pending.Clear();
        }
        for (Pending &read : pending)
        {
            MemCopy(read.Buffer, _bytes.Data() + read.Offset, static_cast<usize>(read.Size));
            read.Done(true);
        }
    }

private:
    struct Pending
    {
        uint64 Offset;
        uint64 Size;
        uint8 *Buffer;
        Function<void(bool succeeded)> Done;
    };

    List<uint8> _bytes;
    mutable Mutex _lock;
    mutable List<Pending> _pending;
};

/// Serves fixed locations, with the dependencies a package's table would list.
class ListedStore final : public IAssetStore
{
public:
    void Add(const Guid &guid, const FileRegion &block, Span<const Guid> dependencies)
    {
        Entry &entry = _entries.FindOrAdd(guid);
        entry.Block = block;
        entry.Dependencies.AddRange(dependencies);
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const Entry *entry = _entries.Find(guid);
        if (entry == nullptr)
        {
            done(nullptr);
            return;
        }
        AssetLocation location;
        location.Block = entry->Block;
        location.Dependencies = entry->Dependencies.AsSpan();
        done(&location);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "Listed";
    }

private:
    struct Entry
    {
        FileRegion Block;
        List<Guid> Dependencies;
    };
    HashMap<Guid, Entry> _entries;
};
} // namespace

MARLIN_TEST(Asset, ImageSafety)
{
    StringBuilder<512> problems;
    MARLIN_EXPECT(AssetBlock::CheckImageSafe(TypeOf<TestMaterialAsset>(), &problems));
    MARLIN_EXPECT(problems.IsEmpty());
    MARLIN_EXPECT(!AssetBlock::CheckImageSafe(TypeOf<TestHeapFields>(), &problems));
    MARLIN_EXPECT(problems.AsView().Contains("TestHeapFields.Values"));
    MARLIN_EXPECT(problems.AsView().Contains("TestHeapFields.Text"));
    MARLIN_EXPECT(problems.AsView().Contains("TestHeapFields.Cached"));
    MARLIN_EXPECT(AssetBlock::GetLayoutHash(TypeOf<TestMaterialAsset>()) !=
                  AssetBlock::GetLayoutHash(TypeOf<TestTextureAsset>()));
    MARLIN_EXPECT(AssetBlock::GetLayoutHash(TypeOf<TestMaterialAsset>()) ==
                  AssetBlock::GetLayoutHash(TypeOf<TestMaterialAsset>()));
}

MARLIN_TEST(Asset, LoadWithDependencies)
{
    {
        AssetFixture fixture;
        TestTextureAsset texture;
        texture.Name = FrozenString("Rock_Albedo");
        texture.Width = 2;
        texture.Height = 1;
        const uint8 pixels[] = {10, 20, 30, 40, 50, 60, 70, 80};
        texture.Pixels = FrozenArray<uint8>(Span<const uint8>(pixels));
        fixture.Convert(texture, MakeGuid(1));
        TestTextureAsset detail;
        detail.Width = 7;
        fixture.Convert(detail, MakeGuid(2));

        TestMaterialAsset material;
        material.Name = FrozenString("Rock");
        material.Filter = TestFilter::Anisotropic;
        material.Tint[0] = 0.5f;
        material.Tint[2] = 2.0f;
        material.Flags = -42;
        material.Layers.Resize(2);
        material.Layers[0].Label = FrozenString("Moss");
        material.Layers[0].Weight = 0.25f;
        const uint16 indices[] = {3, 1, 4, 1, 5};
        material.Layers[0].Indices = FrozenArray<uint16>(Span<const uint16>(indices));
        material.Layers[1].Label = FrozenString("Dirt");
        material.BaseColor = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(1));
        material.Details.Resize(3);
        material.Details[0] = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(2));
        material.Details[1] = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(1));
        material.Fallback = SoftAssetRef<TestTextureAsset>(MakeGuid(99));
        fixture.Convert(material, MakeGuid(3));
        material.BaseColor.Reset();
        material.Details.Reset();
        fixture.Serve();

        const AssetRef<TestMaterialAsset> loaded = Assets::LoadSync<TestMaterialAsset>(MakeGuid(3));
        MARLIN_REQUIRE(loaded.IsLoaded());
        const TestMaterialAsset &rock = *loaded;
        MARLIN_EXPECT(rock.GetGuid() == MakeGuid(3));
        MARLIN_EXPECT(rock.Name.AsView() == "Rock");
        MARLIN_EXPECT(rock.Filter == TestFilter::Anisotropic);
        MARLIN_EXPECT(rock.Tint[0] == 0.5f && rock.Tint[1] == 0 && rock.Tint[2] == 2.0f);
        MARLIN_EXPECT(rock.Flags == -42);
        MARLIN_REQUIRE(rock.Layers.Count() == 2);
        MARLIN_EXPECT(!rock.Layers.IsOwned());
        MARLIN_EXPECT(rock.Layers[0].Label.AsView() == "Moss" && rock.Layers[0].Weight == 0.25f);
        MARLIN_EXPECT(rock.Layers[0].Indices.AsSpan().Count() == 5 && rock.Layers[0].Indices[2] == 4);
        MARLIN_EXPECT(rock.Layers[1].Label.AsView() == "Dirt" && rock.Layers[1].Indices.IsEmpty());
        MARLIN_EXPECT(rock.Fallback.GetGuid() == MakeGuid(99));
        MARLIN_EXPECT(!rock.Parent.IsValid());
        MARLIN_EXPECT(rock.PostLoads == 1 && rock.BaseColorReadyAtPostLoad);
        MARLIN_EXPECT(rock.RuntimeNotes.IsEmpty());

        MARLIN_REQUIRE(rock.BaseColor.IsLoaded());
        MARLIN_EXPECT(rock.BaseColor->Name.AsView() == "Rock_Albedo");
        MARLIN_EXPECT(rock.BaseColor->Width == 2 && rock.BaseColor->Pixels.Count() == 8);
        MARLIN_EXPECT(rock.BaseColor->Pixels[7] == 80);
        MARLIN_REQUIRE(rock.Details.Count() == 3);
        MARLIN_EXPECT(rock.Details[0]->Width == 7);
        MARLIN_EXPECT(rock.Details[1] == rock.BaseColor);
        MARLIN_EXPECT(!rock.Details[2].IsValid());
        MARLIN_EXPECT(!rock.Fallback.GetIfLoaded().IsValid());
        MARLIN_EXPECT(TestTextureAsset::Live == 4);
    }
    MARLIN_EXPECT(TestTextureAsset::Live == 0);
    MARLIN_EXPECT(Assets::GetSlotCount() == 0);
}

MARLIN_TEST(Asset, HeldLocatesWaitForStoresAddedLater)
{
    AssetFixture fixture;
    TestTextureAsset texture;
    texture.Width = 3;
    fixture.Convert(texture, MakeGuid(1));
    // As at engine startup: a subsystem loads before the subsystem that owns the store has added it.
    Assets::HoldLocates();
    AssetRef<TestTextureAsset> early = Assets::Load<TestTextureAsset>(MakeGuid(1));
    MARLIN_EXPECT(early.GetSlot()->GetState() == AssetState::Loading);
    MARLIN_EXPECT(!Assets::Wait(*early.GetSlot(), 1000));
    fixture.Serve();
    Assets::ReleaseLocates();
    MARLIN_REQUIRE(Assets::Wait(*early.GetSlot(), 5000));
    Assets::ProcessFrame();
    MARLIN_REQUIRE(early.Get() != nullptr);
    MARLIN_EXPECT(early.Get()->Width == 3);
    early.Reset();
    Assets::CollectGarbage();
}

MARLIN_TEST(Asset, ReleaseAtSafePoint)
{
    AssetFixture fixture;
    TestTextureAsset texture;
    texture.Width = 3;
    fixture.Convert(texture, MakeGuid(10));
    TestMaterialAsset material;
    material.BaseColor = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(10));
    fixture.Convert(material, MakeGuid(11));
    material.BaseColor.Reset();
    fixture.Serve();

    AssetRef<TestMaterialAsset> loaded = Assets::LoadSync<TestMaterialAsset>(MakeGuid(11));
    MARLIN_REQUIRE(loaded.IsLoaded());
    const int32 live = TestTextureAsset::Live;
    AssetRef<TestTextureAsset> texture10 = loaded->BaseColor;
    loaded.Reset();
    MARLIN_EXPECT(Assets::GetSlotCount() == 2);
    Assets::CollectGarbage();
    MARLIN_EXPECT(Assets::GetSlotCount() == 1);
    MARLIN_EXPECT(texture10.IsLoaded() && texture10->Width == 3);
    texture10.Reset();
    MARLIN_EXPECT(TestTextureAsset::Live == live);
    Assets::CollectGarbage();
    MARLIN_EXPECT(TestTextureAsset::Live == live - 1);
    MARLIN_EXPECT(Assets::GetSlotCount() == 0);
}

MARLIN_TEST(Asset, Failures)
{
    AssetFixture fixture;
    TestMaterialAsset material;
    material.BaseColor = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(20));
    fixture.Convert(material, MakeGuid(21));
    material.BaseColor.Reset();

    TestTextureAsset texture;
    List<uint8> block;
    MARLIN_REQUIRE(AssetBlock::Write(texture, MakeGuid(22), block));
    AssetBlockHeader header;
    MemCopy(&header, block.Data(), sizeof(header));
    header.LayoutHash ^= 1;
    MemCopy(block.Data(), &header, sizeof(header));
    fixture.WriteBlock(MakeGuid(22), block);
    fixture.Serve();

    const AssetRef<TestTextureAsset> missing = Assets::LoadSync<TestTextureAsset>(MakeGuid(23));
    MARLIN_EXPECT(missing.GetState() == AssetState::Failed && !missing.IsLoaded());
    const AssetRef<TestMaterialAsset> orphan = Assets::LoadSync<TestMaterialAsset>(MakeGuid(21));
    MARLIN_EXPECT(orphan.GetState() == AssetState::Failed);
    const AssetRef<TestTextureAsset> stale = Assets::LoadSync<TestTextureAsset>(MakeGuid(22));
    MARLIN_EXPECT(stale.GetState() == AssetState::Failed);
    const AssetRef<TestMaterialAsset> wrongType = Assets::LoadSync<TestMaterialAsset>(MakeGuid(22));
    MARLIN_EXPECT(!wrongType.IsLoaded());
}

MARLIN_TEST(Asset, ReferenceCycle)
{
    AssetFixture fixture;
    TestMaterialAsset a;
    a.Parent = AssetRef<TestMaterialAsset>::FromGuid(MakeGuid(31));
    fixture.Convert(a, MakeGuid(30));
    TestMaterialAsset b;
    b.Parent = AssetRef<TestMaterialAsset>::FromGuid(MakeGuid(30));
    fixture.Convert(b, MakeGuid(31));
    a.Parent.Reset();
    b.Parent.Reset();
    fixture.Serve();

    AssetRef<TestMaterialAsset> loaded = Assets::LoadSync<TestMaterialAsset>(MakeGuid(30));
    MARLIN_REQUIRE(loaded.IsLoaded());
    MARLIN_REQUIRE(loaded->Parent.IsLoaded());
    MARLIN_EXPECT(loaded->Parent->Parent == loaded);
    loaded->Parent->Parent.Reset();
    loaded->Parent.Reset();
    loaded.Reset();
    Assets::CollectGarbage();
    MARLIN_EXPECT(Assets::GetSlotCount() == 0);
}

MARLIN_TEST(Asset, LongerReferenceCycles)
{
    // 80 -> 81 -> 82 -> 80, and 83 -> 80 from outside the cycle: every one loads, whatever order they arrive in.
    AssetFixture fixture;
    for (uint32 i = 0; i < 3; ++i)
    {
        TestMaterialAsset material;
        material.Parent = AssetRef<TestMaterialAsset>::FromGuid(MakeGuid(80 + (i + 1) % 3));
        fixture.Convert(material, MakeGuid(80 + i));
    }
    TestMaterialAsset outside;
    outside.Parent = AssetRef<TestMaterialAsset>::FromGuid(MakeGuid(80));
    fixture.Convert(outside, MakeGuid(83));
    outside.Parent.Reset();
    fixture.Serve();

    for (int32 round = 0; round < 20; ++round)
    {
        AssetRef<TestMaterialAsset> entry = Assets::Load<TestMaterialAsset>(MakeGuid(83));
        AssetRef<TestMaterialAsset> member = Assets::Load<TestMaterialAsset>(MakeGuid(81 + round % 2));
        MARLIN_REQUIRE(Assets::Wait(*entry.GetSlot(), 5000) && Assets::Wait(*member.GetSlot(), 5000));
        const TestMaterialAsset *a = entry->Parent.Get();
        MARLIN_REQUIRE(a != nullptr && a->Parent.IsLoaded() && a->Parent->Parent.IsLoaded());
        MARLIN_EXPECT(a->Parent->Parent->Parent.Get() == a);
        MARLIN_EXPECT(a->PostLoads == 1 && a->Parent->PostLoads == 1);
        // Breaking the cycle lets all of it go.
        AssetRef<TestMaterialAsset> first = entry->Parent;
        first->Parent->Parent->Parent.Reset();
        first.Reset();
        entry.Reset();
        member.Reset();
        Assets::CollectGarbage();
        MARLIN_EXPECT(Assets::GetSlotCount() == 0);
    }
}

MARLIN_TEST(Asset, ListedDependenciesStartBeforeTheBlock)
{
    AssetFixture fixture;
    TestTextureAsset texture;
    texture.Width = 9;
    fixture.Convert(texture, MakeGuid(70));
    TestMaterialAsset material;
    material.BaseColor = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(70));
    List<uint8> block;
    MARLIN_REQUIRE(AssetBlock::Write(material, MakeGuid(71), block));
    material.BaseColor.Reset();

    // The material's block waits at the gate; its store lists the texture, as a package's table does.
    const uint64 size = block.Count();
    const SharedPtr<GatedSource> gate(new GatedSource(Move(block)));
    FileRegion gated;
    gated.Size = size;
    gated.Source = SharedPtr<const IRegionSource>(gate.Get());
    FileRegion textureBlock;
    MARLIN_REQUIRE(Vfs::OpenRegion(fixture.Files.Find(MakeGuid(70))->AsView(), textureBlock));
    auto store = MakeUnique<ListedStore>();
    const Guid listed[] = {MakeGuid(70)};
    store->Add(MakeGuid(71), gated, Span<const Guid>(listed));
    store->Add(MakeGuid(70), textureBlock, {});
    fixture.Store = Assets::AddStore(Move(store), 0);

    AssetRef<TestMaterialAsset> loaded = Assets::Load<TestMaterialAsset>(MakeGuid(71));
    MARLIN_EXPECT(gate->IsWaiting());
    AssetSlot *dependency = Assets::FindSlot(MakeGuid(70));
    MARLIN_REQUIRE(dependency != nullptr && dependency->GetState() != AssetState::Unloaded);
    MARLIN_REQUIRE(Assets::Wait(*dependency, 5000));
    MARLIN_EXPECT(loaded.GetState() == AssetState::Loading);
    gate->Open();
    MARLIN_REQUIRE(Assets::Wait(*loaded.GetSlot(), 5000));
    MARLIN_EXPECT(loaded->BaseColor.IsLoaded() && loaded->BaseColor->Width == 9);
    MARLIN_EXPECT(loaded->BaseColor.GetSlot() == dependency);
    loaded.Reset();
    Assets::CollectGarbage();
    MARLIN_EXPECT(Assets::GetSlotCount() == 0);
}

MARLIN_TEST(Asset, CompletionCallbacks)
{
    AssetFixture fixture;
    TestTextureAsset texture;
    texture.Width = 5;
    fixture.Convert(texture, MakeGuid(40));
    fixture.Serve();

    const AssetRef<TestTextureAsset> ref = Assets::Load<TestTextureAsset>(MakeGuid(40));
    int32 calls = 0;
    uint32 width = 0;
    Assets::OnCompleted(*ref.GetSlot(), [&](AssetSlot &slot) {
        ++calls;
        width = static_cast<TestTextureAsset *>(slot.GetObject())->Width;
    });
    MARLIN_REQUIRE(Assets::Wait(*ref.GetSlot()));
    MARLIN_EXPECT(calls == 0);
    Assets::ProcessFrame();
    MARLIN_EXPECT(calls == 1 && width == 5);
    Assets::ProcessFrame();
    MARLIN_EXPECT(calls == 1);
}

MARLIN_TEST(Asset, MarlinTextLinks)
{
    AssetFixture fixture;
    TestMaterialAsset material;
    material.BaseColor = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(50));
    material.Fallback = SoftAssetRef<TestTextureAsset>(MakeGuid(51));
    StringBuilder<1024> text;
    MarlinTextWriteOptions options;
    options.WriteLinkHint = [](const Guid &guid, StringWriter &output) {
        output.Append(guid == MakeGuid(50) ? "/Project/Content/Rock:Rock_Albedo" : "");
    };
    MarlinTextSerializer::WriteObject(text, TypeOf<TestMaterialAsset>(), &material, MakeGuid(52), "Rock", options);
    MARLIN_EXPECT(
        text.AsView().Contains("BaseColor = </Project/Content/Rock:Rock_Albedo#5e57a55e-0000-0000-0000-000000000032>"));
    MARLIN_EXPECT(text.AsView().Contains("Fallback = <#5e57a55e-0000-0000-0000-000000000033>"));

    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    MARLIN_REQUIRE(document.Parse(text.AsView(), diagnostics));
    const MarlinTextObject *object = document.GetObject(0, diagnostics);
    MARLIN_REQUIRE(object != nullptr);
    TestMaterialAsset read;
    MARLIN_REQUIRE(MarlinTextSerializer::ReadObject(*object, TypeOf<TestMaterialAsset>(), &read, diagnostics));
    MARLIN_EXPECT(read.BaseColor.GetGuid() == MakeGuid(50));
    MARLIN_EXPECT(read.BaseColor.GetSlot() == material.BaseColor.GetSlot());
    MARLIN_EXPECT(read.Fallback.GetGuid() == MakeGuid(51));
}

MARLIN_TEST(Asset, HotReload)
{
    AssetFixture fixture;
    TestTextureAsset texture;
    texture.Width = 4;
    fixture.Convert(texture, MakeGuid(60));
    TestMaterialAsset material;
    material.BaseColor = AssetRef<TestTextureAsset>::FromGuid(MakeGuid(60));
    fixture.Convert(material, MakeGuid(61));
    material.BaseColor.Reset();
    fixture.Serve();

    AssetRef<TestMaterialAsset> loaded = Assets::LoadSync<TestMaterialAsset>(MakeGuid(61));
    MARLIN_REQUIRE(loaded.IsLoaded() && loaded->BaseColor.IsLoaded());
    AssetRef<TestTextureAsset> base = loaded->BaseColor;
    AssetSlot &slot = *base.GetSlot();
    int32 reloads = 0;
    const AssetReloadId watch = Assets::WatchReload(slot, [&](AssetSlot &) { ++reloads; });

    // Converted again, as when the indexer announces a changed source: the old object serves until the switch.
    texture.Width = 5;
    fixture.Convert(texture, MakeGuid(60));
    const TestTextureAsset *old = base.Get();
    const int32 live = TestTextureAsset::Live;
    Assets::Reload(slot);
    Assets::Reload(slot);
    for (int32 i = 0; i < 5000 && reloads == 0; ++i)
    {
        PlatformThread::Sleep(1);
        Assets::ProcessFrame();
    }
    MARLIN_EXPECT(reloads == 1 && base.IsLoaded() && base->Width == 5);
    MARLIN_EXPECT(base.Get() != old && loaded->BaseColor->Width == 5);
    MARLIN_EXPECT(TestTextureAsset::Live == live + 1);
    Assets::CollectGarbage();
    MARLIN_EXPECT(TestTextureAsset::Live == live);

    // Unwatched, a reload still switches the slot but calls nothing.
    Assets::UnwatchReload(watch);
    texture.Width = 6;
    fixture.Convert(texture, MakeGuid(60));
    Assets::Reload(slot);
    for (int32 i = 0; i < 5000 && base->Width != 6; ++i)
    {
        PlatformThread::Sleep(1);
        Assets::ProcessFrame();
    }
    MARLIN_EXPECT(base->Width == 6 && reloads == 1);
    base.Reset();
    loaded.Reset();
    Assets::CollectGarbage();
    MARLIN_EXPECT(Assets::GetSlotCount() == 0);
}

MARLIN_TEST(Asset, BulkStreaming)
{
    List<uint8> bytes;
    bytes.Resize(4 * 4096);
    for (usize i = 0; i < bytes.Count(); ++i)
    {
        bytes[i] = static_cast<uint8>(i / 4096 + 1);
    }
    MARLIN_REQUIRE(Vfs::WriteAll("/Temp/BulkStreaming.bin",
                                 StringView(reinterpret_cast<const char *>(bytes.Data()), bytes.Count())));
    FileRegion file;
    MARLIN_REQUIRE(Vfs::OpenRegion("/Temp/BulkStreaming.bin", file));

    BulkStreamerDesc desc;
    desc.MaxInFlightBytes = 4096;
    desc.MaxInFlightRequests = 1;
    BulkStreamer streamer(desc);
    List<BulkRequestId> order;
    HashMap<BulkRequestId, uint8> received;
    const auto record = [&](BulkRequestId id, SharedPtr<const BulkBytes> data) {
        order.Add(id);
        received.Add(id, data.IsValid() && data->GetBytes().Count() == 4096 ? data->GetBytes()[4095] : 0);
    };
    const BulkRequestId low = streamer.Request(file, 0, 4096, 1.0f, record);
    const BulkRequestId high = streamer.Request(file, 4096, 4096, 3.0f, record);
    const BulkRequestId raised = streamer.Request(file, 8192, 4096, 0.5f, record);
    const BulkRequestId cancelled = streamer.Request(file, 12288, 4096, 2.0f, record);
    streamer.SetPriority(raised, 5.0f);
    streamer.ProcessFrame();
    MARLIN_EXPECT(streamer.GetQueuedCount() == 3);
    MARLIN_EXPECT(streamer.GetInFlightCount() == 1 && streamer.GetInFlightBytes() == 4096);
    streamer.Cancel(cancelled);
    for (int32 i = 0; i < 5000 && order.Count() < 3; ++i)
    {
        PlatformThread::Sleep(1);
        streamer.ProcessFrame();
        MARLIN_EXPECT(streamer.GetInFlightCount() <= 1);
    }
    MARLIN_REQUIRE(order.Count() == 3);
    MARLIN_EXPECT(order[0] == raised && order[1] == high && order[2] == low);
    MARLIN_EXPECT(*received.Find(raised) == 3 && *received.Find(high) == 2 && *received.Find(low) == 1);
    MARLIN_EXPECT(streamer.GetQueuedCount() == 0 && streamer.GetInFlightBytes() == 0);

    // A request cancelled in flight never calls back; Flush waits for its read.
    const BulkRequestId dropped = streamer.Request(file, 0, 4096, 1.0f, record);
    streamer.ProcessFrame();
    streamer.Cancel(dropped);
    streamer.Flush();
    MARLIN_EXPECT(order.Count() == 3 && streamer.GetInFlightCount() == 0);
}

MARLIN_TEST(Asset, BulkStreamingQueueOrder)
{
    List<uint8> bytes;
    bytes.Resize(64);
    MARLIN_REQUIRE(
        Vfs::WriteAll("/Temp/BulkQueue.bin", StringView(reinterpret_cast<const char *>(bytes.Data()), bytes.Count())));
    FileRegion file;
    MARLIN_REQUIRE(Vfs::OpenRegion("/Temp/BulkQueue.bin", file));

    // One read at a time, so requests start strictly by priority, then by age among equals.
    BulkStreamerDesc desc;
    desc.MaxInFlightRequests = 1;
    BulkStreamer streamer(desc);
    List<BulkRequestId> order;
    const auto record = [&](BulkRequestId id, SharedPtr<const BulkBytes>) { order.Add(id); };
    constexpr uint32 Count = 200;
    List<BulkRequestId> ids;
    List<float32> priorities;
    for (uint32 i = 0; i < Count; ++i)
    {
        priorities.Add(static_cast<float32>((i * 37) % 23));
        ids.Add(streamer.Request(file, 0, 16, priorities[i], record));
    }
    // Raise some, lower some, and cancel every seventh before anything starts.
    for (uint32 i = 0; i < Count; i += 5)
    {
        priorities[i] = static_cast<float32>((i * 11) % 31) - 4.0f;
        streamer.SetPriority(ids[i], priorities[i]);
    }
    List<bool> cancelled;
    cancelled.Resize(Count);
    for (uint32 i = 3; i < Count; i += 7)
    {
        cancelled[i] = true;
        streamer.Cancel(ids[i]);
    }
    streamer.Cancel(ids[3]);
    streamer.SetPriority(ids[3], 100.0f);
    streamer.Flush();

    List<uint32> expected;
    for (uint32 i = 0; i < Count; ++i)
    {
        if (!cancelled[i])
        {
            expected.Add(i);
        }
    }
    expected.Sort(
        [&](uint32 a, uint32 b) { return priorities[a] > priorities[b] || (priorities[a] == priorities[b] && a < b); });
    MARLIN_REQUIRE(order.Count() == expected.Count());
    bool same = true;
    for (usize i = 0; i < order.Count(); ++i)
    {
        same = same && order[i] == ids[expected[i]];
    }
    MARLIN_EXPECT(same && streamer.GetQueuedCount() == 0);
}
} // namespace Marlin::Tests
