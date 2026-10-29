#include "Testing/Testing.h"

#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetIndexClient.h"
#include "AssetPipeline/AssetIndexService.h"
#include "AssetPipeline/AssetIndexShared.h"
#include "AssetPipeline/AssetJournal.h"
#include "AssetPipeline/AssetScanner.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "Net/NetLoop.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
constexpr StringView Rock = R"(!Version "1.0"

TextureSource
{
    !UUID = "01832501-7918-3ee4-b177-2f200aab91b3"
    !Name = "Rock_Albedo"
    SourceFile = `source:Rock.png#7c1e0000-0000-0000-0000-000000000001`
}

Material
{
    !UUID = "9a0b0000-0000-0000-0000-000000000002"
    !Name = "Rock"
    Tags = [Foliage, "Wet"]
    BaseColor = <Rock_Albedo#01832501-7918-3ee4-b177-2f200aab91b3>
    Layers = [{ Mask = <#9a0b0000-0000-0000-0000-000000000003> }]
}

Material : <Rock#9a0b0000-0000-0000-0000-000000000002>
{
    !UUID = "9a0b0000-0000-0000-0000-000000000003"
    !Name = "Rock_Big"
}
)";

const Guid Albedo = {0x01832501, 0x79183ee4, 0xb1772f20, 0x0aab91b3};
const Guid RockGuid = {0x9a0b0000, 0, 0, 2};
const Guid BigGuid = {0x9a0b0000, 0, 0, 3};

/// A writable folder mounted at /AssetIndexTest, removed afterwards. It lives at /Temp/<name> and is created and
/// removed only through the VFS, inside the /Temp mount: a failed lookup can never turn into an empty path or the
/// root of a drive. The native path only serves to mount it.
struct ScratchContent
{
    String Physical;
    MountId Mount = 0;
    String Virtual;

    explicit ScratchContent(StringView name) : Virtual(Format("/Temp/{0}", name))
    {
        MARLIN_REQUIRE(!name.IsEmpty() && !name.Contains('/') && !name.Contains('.'));
        (void)Vfs::Delete(Virtual.AsView());
        MARLIN_REQUIRE(Vfs::CreateDirectory(Format("{0}/Content", Virtual).AsView()));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath(Virtual.AsView(), path) && !path.IsEmpty());
        MARLIN_REQUIRE(path.AsView().EndsWith(name));
        Physical = path.AsView();
        Mount = Vfs::Mount("/AssetIndexTest", MakeUnique<DirectoryMount>(Physical, true), 0);
    }

    ~ScratchContent()
    {
        (void)Vfs::Unmount(Mount);
        (void)Vfs::Delete(Virtual.AsView());
    }

    MARLIN_NONCOPYABLE(ScratchContent);

    static void Write(StringView path, StringView text)
    {
        StringView parent;
        StringView name;
        if (path.SplitLast('/', parent, name))
        {
            (void)Vfs::CreateDirectory(parent);
        }
        MARLIN_REQUIRE(Vfs::WriteAll(path, text));
    }
};

bool WaitFor(const auto &condition)
{
    for (int32 i = 0; i < 3000; ++i)
    {
        if (condition())
        {
            return true;
        }
        PlatformThread::Sleep(2);
    }
    return false;
}
} // namespace

MARLIN_TEST(AssetIndex, ParseAsset)
{
    AssetFileRecord record;
    StringBuilder<256> errors;
    MARLIN_REQUIRE(AssetScanner::ParseAsset("/Project/Content/Rock.masset", Rock, record, errors));
    MARLIN_EXPECT(errors.IsEmpty());
    MARLIN_REQUIRE(record.IsAsset && record.Objects.Count() == 3);
    const AssetObjectRecord &texture = record.Objects[0];
    MARLIN_EXPECT(texture.Uuid == Albedo && texture.Name == "Rock_Albedo" && texture.Type == "TextureSource");
    MARLIN_EXPECT(texture.Links.IsEmpty() && texture.BinaryLinks.Count() == 1);
    const AssetObjectRecord &material = record.Objects[1];
    MARLIN_EXPECT(material.Links.Count() == 2 && material.Links[0] == Albedo && material.Links[1] == BigGuid);
    MARLIN_EXPECT(material.Tags.Count() == 2 && material.Tags[1] == "Wet");
    MARLIN_EXPECT(record.Objects[2].Links.Count() == 1 && record.Objects[2].Links[0] == RockGuid);
    MARLIN_EXPECT(record.Objects[2].Line == 19);
}

MARLIN_TEST(AssetIndex, QueriesAndPersistence)
{
    AssetIndex index;
    AssetFileRecord record;
    StringBuilder<256> errors;
    MARLIN_REQUIRE(AssetScanner::ParseAsset("/Project/Content/Rocks/Rock.masset", Rock, record, errors));
    record.Path = "/Project/Content/Rocks/Rock.masset";
    record.Size = Rock.Length();
    index.Update(record);
    AssetFileRecord binary;
    binary.Path = "/Project/Content/Rocks/Rock.png";
    binary.BinaryGuid = {0x7c1e0000, 0, 0, 1};
    index.Update(binary);
    (void)index.Commit();

    AssetObjectInfo info;
    MARLIN_REQUIRE(index.Find(BigGuid, info));
    MARLIN_EXPECT(info.Name == "Rock_Big" && info.Path == record.Path && info.Line == 19);
    List<AssetObjectInfo> results;
    AssetQuery query;
    query.Type = "Material";
    query.Name = "rock*";
    index.Query(query, results);
    MARLIN_EXPECT(results.Count() == 2);
    results.Clear();
    query.Tag = "Wet";
    query.PathPrefix = "/Project/Content/Rocks";
    index.Query(query, results);
    MARLIN_EXPECT(results.Count() == 1 && results[0].Uuid == RockGuid);
    results.Clear();
    index.GetReferencers(Albedo, results);
    MARLIN_EXPECT(results.Count() == 1 && results[0].Uuid == RockGuid);
    List<Guid> objects;
    List<Guid> binaries;
    index.GetDependencies(RockGuid, objects, binaries);
    MARLIN_EXPECT(objects.Count() == 2 && binaries.Count() == 1);
    AssetBinaryInfo png;
    MARLIN_EXPECT(index.FindBinary(binary.BinaryGuid, png) && png.Path == binary.Path);

    StringBuilder<512> directory;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", directory) && !directory.IsEmpty());
    const String snapshot = Format("{0}/IndexTest.snapshot", directory.AsView());
    const String log = Format("{0}/IndexTest.log", directory.AsView());
    // Files are removed through the VFS, by their /Temp path, never by the native one.
    (void)Vfs::Delete("/Temp/IndexTest.log");
    MARLIN_REQUIRE(index.Save(snapshot.AsView()));
    Hash128 canonical;
    MARLIN_EXPECT(index.GetCanonicalHash(RockGuid, canonical) && canonical == record.Objects[1].Canonical);
    MARLIN_EXPECT(record.Objects[1].Canonical != record.Objects[2].Canonical);
    AssetFileRecord moved = record;
    moved.Path = "/Project/Content/Moved.masset";
    MARLIN_REQUIRE(AssetIndex::AppendLog(log.AsView(), record.Path.AsView(), nullptr));
    MARLIN_REQUIRE(AssetIndex::AppendLog(log.AsView(), moved.Path.AsView(), &moved));

    AssetIndex loaded;
    MARLIN_REQUIRE(loaded.Load(snapshot.AsView()) && loaded.ReplayLog(log.AsView()));
    MARLIN_REQUIRE(loaded.Find(BigGuid, info));
    MARLIN_EXPECT(info.Path == "/Project/Content/Moved.masset");
    MARLIN_EXPECT(loaded.GetCounts().Files == 2 && loaded.GetCounts().Objects == 3);
    loaded.Compact();
    MARLIN_EXPECT(loaded.GetCounts().Objects == 3 && loaded.Find(Albedo, info));
    (void)Vfs::Delete("/Temp/IndexTest.snapshot");
    (void)Vfs::Delete("/Temp/IndexTest.log");
}

MARLIN_TEST(AssetIndex, ScannerAndBinaryRenames)
{
    const ScratchContent scratch("AssetScannerTest");
    ScratchContent::Write("/AssetIndexTest/Content/Rocks/Rock.masset", Rock);
    ScratchContent::Write("/AssetIndexTest/Content/Rocks/Rock.png", "pixels");
    ScratchContent::Write("/AssetIndexTest/Content/Other.png", "other pixels");
    AssetIndex index;
    List<String> folders;
    folders.Add("/AssetIndexTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    MARLIN_EXPECT(changed.Count() == 3 && index.GetCounts().Objects == 3 && index.GetCounts().BinaryFiles == 2);
    String files;
    MARLIN_REQUIRE(Vfs::ReadAll("/AssetIndexTest/Content/.mfiles", files));
    MARLIN_EXPECT(files.AsView().Contains("Path = \"Rocks/Rock.png\""));

    AssetFileRecord png;
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/Rocks/Rock.png", png));
    const Guid pngGuid = png.BinaryGuid;
    changed.Clear();
    scanner.ScanAll(changed);
    MARLIN_EXPECT(changed.IsEmpty());

    // Written again with the same text, as a save without edits or a checkout does: a new time, nothing changed.
    FileInfo before;
    MARLIN_REQUIRE(Vfs::GetInfo("/AssetIndexTest/Content/Rocks/Rock.masset", before));
    for (uint32 waited = 0; waited < 2000; waited += 10)
    {
        ScratchContent::Write("/AssetIndexTest/Content/Rocks/Rock.masset", Rock);
        FileInfo after;
        if (Vfs::GetInfo("/AssetIndexTest/Content/Rocks/Rock.masset", after) &&
            after.ModifiedTime != before.ModifiedTime)
        {
            break;
        }
        PlatformThread::Sleep(10);
    }
    changed.Clear();
    scanner.ScanPath("/AssetIndexTest/Content/Rocks/Rock.masset", changed);
    MARLIN_EXPECT(changed.IsEmpty() && index.GetCounts().Objects == 3);
    changed.Clear();
    scanner.ScanAll(changed);
    MARLIN_EXPECT(changed.IsEmpty());

    MARLIN_REQUIRE(PlatformFile::Move(Format("{0}/Content/Rocks/Rock.png", scratch.Physical).CStr(),
                                      Format("{0}/Content/Moved.png", scratch.Physical).CStr(), false));
    changed.Clear();
    scanner.ScanAll(changed);
    AssetFileRecord moved;
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/Moved.png", moved));
    MARLIN_EXPECT(moved.BinaryGuid == pngGuid);
    MARLIN_EXPECT(!index.GetRecord("/AssetIndexTest/Content/Rocks/Rock.png", png));

    ScratchContent::Write("/AssetIndexTest/Content/Rocks/Rock.masset", "!Version \"1.0\"\n");
    changed.Clear();
    scanner.ScanPath("/AssetIndexTest/Content/Rocks/Rock.masset", changed);
    MARLIN_EXPECT(changed.Count() == 1 && index.GetCounts().Objects == 0);
    MARLIN_REQUIRE(Vfs::Delete("/AssetIndexTest/Content/Other.png"));
    changed.Clear();
    scanner.ScanPath("/AssetIndexTest/Content/Other.png", changed);
    MARLIN_EXPECT(changed.Count() == 1 && index.GetCounts().BinaryFiles == 1);
}

MARLIN_TEST(AssetIndex, CommitsAndFolds)
{
    AssetIndex index;
    const uint64 empty = index.Commit();
    MARLIN_EXPECT(index.Commit() == empty);
    const auto add = [&](uint32 i) {
        AssetFileRecord record;
        record.Path = Format("/Project/Content/Folder{0}/File{1}.masset", i % 7, i);
        record.IsAsset = true;
        record.Objects.AddDefaulted().Uuid = Guid{i + 1, 0, 0, 7};
        index.Update(Move(record));
    };
    for (uint32 i = 0; i < 10; ++i)
    {
        add(i);
    }
    const uint64 first = index.Commit();
    MARLIN_EXPECT(first == empty + 1 && index.GetCounts().Files == 10);
    // A commit with nothing new publishes nothing.
    MARLIN_EXPECT(index.Commit() == first);
    usize below = 0;
    index.ForEachFileBelow("/Project/Content/Folder3", [&](StringView) { ++below; });
    MARLIN_EXPECT(below == 1);
    MARLIN_REQUIRE(index.Remove("/Project/Content/Folder3/File3.masset"));
    below = 0;
    index.ForEachFileBelow("/Project/Content/Folder3", [&](StringView) { ++below; });
    MARLIN_EXPECT(below == 0);

    // Past the fold threshold the changes become the base, and the delta is empty again.
    for (uint32 i = 10; i < AssetIndex::FoldThreshold + 20; ++i)
    {
        add(i);
    }
    (void)index.Commit();
    MARLIN_EXPECT(index.GetCounts().Files == AssetIndex::FoldThreshold + 19);
    AssetObjectInfo info;
    MARLIN_EXPECT(index.Find(Guid{5, 0, 0, 7}, info) && !index.Find(Guid{4, 0, 0, 7}, info));
    index.WithImages([&](Span<const uint8> base, Span<const uint8> delta, uint64, StringView) {
        MARLIN_EXPECT(!base.IsEmpty() && delta.IsEmpty());
    });
    below = 0;
    index.ForEachFileBelow("/Project/Content/Folder3",
                           [&](StringView path) { below += path.StartsWith("/Project/Content/Folder3/") ? 1 : 0; });
    MARLIN_EXPECT(below == (AssetIndex::FoldThreshold + 20 + 3) / 7 - 1);
    const uint64 folded = index.GetGeneration();
    index.Compact();
    MARLIN_EXPECT(index.GetGeneration() == folded);

    List<AssetObjectInfo> results;
    AssetQuery query;
    query.Limit = 3;
    index.Query(query, results);
    MARLIN_EXPECT(results.Count() == 3);
}

MARLIN_TEST(AssetIndex, ScanPathsIsOneBatch)
{
    const ScratchContent scratch("AssetBatchTest");
    ScratchContent::Write("/AssetIndexTest/Content/A.png", "same pixels");
    ScratchContent::Write("/AssetIndexTest/Content/B.png", "same pixels");
    AssetIndex index;
    List<String> folders;
    folders.Add("/AssetIndexTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    AssetFileRecord a;
    AssetFileRecord b;
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/A.png", a));
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/B.png", b));
    // Files with the same contents are two files, not a rename.
    MARLIN_EXPECT(a.BinaryGuid.IsValid() && b.BinaryGuid.IsValid() && a.BinaryGuid != b.BinaryGuid);

    // A rename next to a file with the same contents takes the Guid of the file that went away.
    MARLIN_REQUIRE(PlatformFile::Move(Format("{0}/Content/B.png", scratch.Physical).CStr(),
                                      Format("{0}/Content/C.png", scratch.Physical).CStr(), false));
    ScratchContent::Write("/AssetIndexTest/Content/D.masset", "!Version \"1.0\"\n");
    ScratchContent::Write("/AssetIndexTest/Content/E.masset", "!Version \"1.0\"\n");
    const uint64 before = index.GetGeneration();
    List<String> paths;
    paths.Add("/AssetIndexTest/Content/B.png");
    paths.Add("/AssetIndexTest/Content/C.png");
    paths.Add("/AssetIndexTest/Content/D.masset");
    paths.Add("/AssetIndexTest/Content/E.masset");
    paths.Add("/AssetIndexTest/Content/.mfiles");
    changed.Clear();
    scanner.ScanPaths(paths.AsSpan(), changed);
    MARLIN_EXPECT(index.GetGeneration() == before + 1 && changed.Count() == 4);
    AssetFileRecord c;
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/C.png", c));
    MARLIN_EXPECT(c.BinaryGuid == b.BinaryGuid && !index.GetRecord("/AssetIndexTest/Content/B.png", b));
    String files;
    MARLIN_REQUIRE(Vfs::ReadAll("/AssetIndexTest/Content/.mfiles", files));
    MARLIN_EXPECT(files.AsView().Contains("Path = \"C.png\"") && !files.AsView().Contains("B.png"));

    // The scanner's own .mfiles is not read back; a rescan of unchanged files changes nothing.
    changed.Clear();
    scanner.ScanPath("/AssetIndexTest/Content/.mfiles", changed);
    scanner.ScanAll(changed);
    MARLIN_EXPECT(changed.IsEmpty() && index.GetGeneration() == before + 1);
}

MARLIN_TEST(AssetIndex, ScannerSeesEveryLayer)
{
    const ScratchContent scratch("AssetLayerTest");
    MARLIN_REQUIRE(Vfs::CreateDirectory("/AssetIndexTest/Overlay/Content"));
    StringBuilder<512> overlayPath;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/AssetIndexTest/Overlay", overlayPath) && !overlayPath.IsEmpty());
    /// A second, higher layer of /AssetIndexTest, like a platform layer; it lives inside the scratch folder.
    struct Overlay
    {
        MountId Id = 0;
        ~Overlay()
        {
            (void)Vfs::Unmount(Id);
        }
    };
    const Overlay overlay{Vfs::Mount("/AssetIndexTest", MakeUnique<DirectoryMount>(overlayPath.AsView(), true), 10)};
    List<MountId> layers;
    Vfs::GetLayers("/AssetIndexTest/Content", layers);
    MARLIN_REQUIRE(layers.Count() == 2 && layers[0] == overlay.Id);
    const MountId base = layers[1];
    MARLIN_REQUIRE(Vfs::WriteAll(base, "/AssetIndexTest/Content/Rock.png", "base pixels"));
    MARLIN_REQUIRE(Vfs::WriteAll(base, "/AssetIndexTest/Content/Grass.png", "grass"));

    AssetIndex index;
    List<String> folders;
    folders.Add("/AssetIndexTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    AssetFileRecord rock;
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/Rock.png", rock) && rock.BinaryGuid.IsValid());
    const Guid rockGuid = rock.BinaryGuid;

    MARLIN_REQUIRE(Vfs::WriteAll(overlay.Id, "/AssetIndexTest/Content/Rock.png", "overlay pixels"));
    MARLIN_REQUIRE(Vfs::WriteAll(overlay.Id, "/AssetIndexTest/Content/Sky.png", "sky"));
    changed.Clear();
    scanner.ScanAll(changed);
    MARLIN_EXPECT(index.GetCounts().BinaryFiles == 3);
    MARLIN_REQUIRE(index.GetRecord("/AssetIndexTest/Content/Rock.png", rock));
    MARLIN_EXPECT(rock.BinaryGuid == rockGuid);

    String baseFiles;
    String overlayFiles;
    MARLIN_REQUIRE(Vfs::ReadAll(base, "/AssetIndexTest/Content/.mfiles", baseFiles));
    MARLIN_REQUIRE(Vfs::ReadAll(overlay.Id, "/AssetIndexTest/Content/.mfiles", overlayFiles));
    MARLIN_EXPECT(baseFiles.AsView().Contains("Path = \"Grass.png\"") &&
                  baseFiles.AsView().Contains("Path = \"Rock.png\"") && !baseFiles.AsView().Contains("Sky.png"));
    MARLIN_EXPECT(overlayFiles.AsView().Contains("Path = \"Sky.png\"") &&
                  overlayFiles.AsView().Contains("Path = \"Rock.png\"") &&
                  !overlayFiles.AsView().Contains("Grass.png"));
}

MARLIN_TEST(AssetIndex, ServiceAndClient)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const ScratchContent scratch("AssetServiceTest");
    ScratchContent::Write("/AssetIndexTest/Content/Rock.masset", Rock);
    AssetIndexServiceDesc desc;
    desc.Folders.Add("/AssetIndexTest/Content");
    desc.StoreDirectory = Format("{0}/Store", scratch.Physical);
    desc.PipeName = Format("MarlinTests.{0}.AssetIndex", PlatformProcess::CurrentId());
    AssetIndexService service(loop, desc);
    MARLIN_REQUIRE(service.Start());

    AssetIndexClient client(loop);
    MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()));
    MARLIN_REQUIRE(client.WaitUntilReady(10000));
    AssetObjectInfo info;
    MARLIN_REQUIRE(client.Find(BigGuid, info));
    MARLIN_EXPECT(info.Name == "Rock_Big" && info.Path == "/AssetIndexTest/Content/Rock.masset");
    MARLIN_EXPECT(client.IsShared());
    List<AssetObjectInfo> results;
    MARLIN_REQUIRE(client.GetReferencers(Albedo, results));
    MARLIN_EXPECT(results.Count() == 1);
    AssetLinks links;
    MARLIN_REQUIRE(client.GetDependencies(RockGuid, links));
    MARLIN_EXPECT(links.Objects.Count() == 2 && links.Binaries.Count() == 1);
    MARLIN_EXPECT(!client.Find(Guid{1, 2, 3, 4}, info));

    ScratchContent::Write("/AssetIndexTest/Content/New.masset", R"(!Version "1.0"
Material { !UUID = "9a0b0000-0000-0000-0000-000000000009" !Name = "New" })");
    MARLIN_EXPECT(WaitFor([&] { return client.Find(Guid{0x9a0b0000, 0, 0, 9}, info); }));
    client.Disconnect();
    service.Stop();

    ScratchContent::Write("/AssetIndexTest/Content/Offline.masset", R"(!Version "1.0"
Material { !UUID = "9a0b0000-0000-0000-0000-00000000000a" !Name = "Offline" })");
    {
        AssetIndexService again(loop, desc);
        MARLIN_REQUIRE(again.Start());
        AssetIndexClient second(loop);
        MARLIN_REQUIRE(second.ConnectPipe(desc.PipeName.AsView()) && second.WaitUntilReady(10000));
        MARLIN_EXPECT(second.Find(Guid{0x9a0b0000, 0, 0, 10}, info) && info.Name == "Offline");
        MARLIN_EXPECT(second.Find(Guid{0x9a0b0000, 0, 0, 9}, info));
        second.Disconnect();
        again.Stop();
    }

    AssetIndex reopened;
    reopened.SetSnapshotDirectory(desc.StoreDirectory.AsView());
    MARLIN_REQUIRE(reopened.LoadLatest());
    MARLIN_EXPECT(reopened.Find(Guid{0x9a0b0000, 0, 0, 9}, info) && reopened.GetCounts().Objects == 5);
}

MARLIN_TEST(AssetIndex, ServiceIsIdleOnlyWithoutClientsOrWork)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const ScratchContent scratch("AssetServiceIdleTest");
    ScratchContent::Write("/AssetIndexTest/Content/Rock.masset", Rock);
    AssetIndexServiceDesc desc;
    desc.Folders.Add("/AssetIndexTest/Content");
    desc.StoreDirectory = Format("{0}/Store", scratch.Physical);
    desc.PipeName = Format("MarlinTests.{0}.AssetIndexIdle", PlatformProcess::CurrentId());
    AssetIndexService service(loop, desc);
    MARLIN_REQUIRE(service.Start());
    MARLIN_EXPECT(WaitFor([&] {
        service.Tick();
        return service.IsIdle();
    }));

    AssetIndexClient client(loop);
    MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()) && client.WaitUntilReady(10000));
    MARLIN_EXPECT(WaitFor([&] {
        service.Tick();
        return !service.IsIdle();
    }));
    client.Disconnect();
    MARLIN_EXPECT(WaitFor([&] {
        service.Tick();
        return service.IsIdle();
    }));
    service.Stop();
}

MARLIN_TEST(AssetIndex, ClientConnectedThroughRemoteReadsTheSharedMapping)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const ScratchContent scratch("AssetRemoteTest");
    ScratchContent::Write("/AssetIndexTest/Content/Rock.masset", Rock);
    AssetIndexServiceDesc desc;
    desc.Folders.Add("/AssetIndexTest/Content");
    desc.StoreDirectory = Format("{0}/Store", scratch.Physical);
    desc.PipeName = Format("MarlinTests.{0}.AssetRemote", PlatformProcess::CurrentId());
    AssetIndexService service(loop, desc);
    MARLIN_REQUIRE(service.Start());

    AssetIndexClient client(loop);
    MARLIN_REQUIRE(client.GetRemote().ConnectPipe(desc.PipeName.AsView(), 2000));
    MARLIN_REQUIRE(client.WaitUntilReady(10000));
    AssetObjectInfo info;
    MARLIN_REQUIRE(client.Find(BigGuid, info));
    MARLIN_EXPECT(info.Name == "Rock_Big" && client.IsShared());
    Hash128 hash;
    MARLIN_EXPECT(client.GetCanonicalHash(RockGuid, hash) && hash != Hash128());
    client.Disconnect();
    MARLIN_EXPECT(!client.IsShared());
    service.Stop();
}

MARLIN_TEST(AssetIndex, SharedMapping)
{
    // Created and removed through the VFS only; the native path is just where the snapshots go.
    constexpr StringView Scratch = "/Temp/AssetIndexShared";
    (void)Vfs::Delete(Scratch);
    MARLIN_REQUIRE(Vfs::CreateDirectory(Scratch));
    StringBuilder<512> directory;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath(Scratch, directory) && !directory.IsEmpty());
    MARLIN_REQUIRE(directory.AsView().EndsWith("AssetIndexShared"));
    const String store = directory.ToString();
    const String name = Format("MarlinTests.{0}.Shared", PlatformProcess::CurrentId());
    {
        AssetIndex index;
        index.SetSnapshotDirectory(store.AsView());
        AssetFileRecord record;
        StringBuilder<256> errors;
        MARLIN_REQUIRE(AssetScanner::ParseAsset("/Project/Content/Rock.masset", Rock, record, errors));
        record.Path = "/Project/Content/Rock.masset";
        index.Update(record);
        index.Compact();
        MARLIN_REQUIRE(!index.GetBasePath().IsEmpty());

        AssetIndexPublisher publisher;
        MARLIN_REQUIRE(publisher.Open(name.AsView()) && publisher.Publish(index));
        AssetIndexReader reader;
        MARLIN_REQUIRE(reader.Open(name.AsView()));
        AssetObjectInfo info;
        bool found = false;
        MARLIN_REQUIRE(reader.Read([&](const AssetIndexView &view) { found = view.Find(BigGuid, info); }));
        MARLIN_EXPECT(found && info.Name == "Rock_Big");

        AssetFileRecord added;
        added.Path = "/Project/Content/New.masset";
        added.IsAsset = true;
        added.Objects.AddDefaulted().Uuid = Guid{9, 9, 9, 9};
        index.Update(added);
        MARLIN_REQUIRE(index.Remove("/Project/Content/Rock.masset"));
        (void)index.Commit();
        MARLIN_REQUIRE(publisher.Publish(index));
        MARLIN_REQUIRE(reader.Read([&](const AssetIndexView &view) {
            found = view.Find(Guid{9, 9, 9, 9}, info) && !view.Find(BigGuid, info);
        }));
        MARLIN_EXPECT(found && reader.GetGeneration() == index.GetGeneration());

        index.Compact();
        MARLIN_REQUIRE(publisher.Publish(index));
        MARLIN_REQUIRE(reader.Read([&](const AssetIndexView &view) {
            found = view.Find(Guid{9, 9, 9, 9}, info) && view.GetDelta().FileCount() == 0;
        }));
        MARLIN_EXPECT(found);
        publisher.Close();
        MARLIN_EXPECT(!reader.Read([](const AssetIndexView &) {}));
    }
    (void)Vfs::Delete(Scratch);
}

MARLIN_TEST(AssetIndex, ChangeJournal)
{
    const ScratchContent scratch("AssetJournalTest");
    List<String> folders;
    folders.Add("/AssetIndexTest/Content");
    AssetJournal journal;
    if (!journal.Open(folders.AsSpan()))
    {
        Testing::Print("The volume keeps no change journal; skipped\n");
        return;
    }
    ScratchContent::Write("/AssetIndexTest/Content/Live/A.masset", "!Version \"1.0\"\n");
    List<String> paths;
    MARLIN_REQUIRE(journal.Poll(paths));
    MARLIN_EXPECT(paths.Contains(String("/AssetIndexTest/Content/Live/A.masset")));
    MARLIN_EXPECT(paths.Contains(String("/AssetIndexTest/Content/Live")));
    const String cursor = Format("{0}/journal.txt", scratch.Physical);
    MARLIN_REQUIRE(journal.Save(cursor.AsView()));

    ScratchContent::Write("/AssetIndexTest/Content/Live/B.masset", "!Version \"1.0\"\n");
    MARLIN_REQUIRE(Vfs::Delete("/AssetIndexTest/Content/Live/A.masset"));
    AssetJournal restarted;
    MARLIN_REQUIRE(restarted.Open(folders.AsSpan()) && restarted.Load(cursor.AsView()));
    paths.Clear();
    MARLIN_REQUIRE(restarted.Poll(paths));
    MARLIN_EXPECT(paths.Contains(String("/AssetIndexTest/Content/Live/B.masset")));
    MARLIN_EXPECT(paths.Contains(String("/AssetIndexTest/Content/Live/A.masset")) ||
                  paths.Contains(String("/AssetIndexTest/Content/Live")));
    paths.Clear();
    MARLIN_REQUIRE(restarted.Poll(paths));
    MARLIN_EXPECT(paths.IsEmpty());
}
} // namespace Marlin::Tests
