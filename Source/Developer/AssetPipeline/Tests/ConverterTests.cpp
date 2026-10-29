#include "Testing/Testing.h"

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetIndexClient.h"
#include "AssetPipeline/AssetIndexService.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConverterPool.h"
#include "AssetPipeline/ConversionStore.h"
#include "AssetPipeline/GlobalStoreWarming.h"
#include "GlobalStore/ConvertedResult.h"
#include "ConverterTestTypes.h"
#include "GlobalStoreAssetStore/GlobalStoreAssetStore.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/MemoryOps.h"
#include "Net/NetLoop.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
int32 TestWordsConverter::Runs = 0;
bool TestMarkedWordsConverter::Enabled = false;

bool TestWordsConverter::Convert(ConversionContext &context)
{
    ++Runs;
    TestWordsSource source;
    if (!context.ReadSource(source))
    {
        return false;
    }
    TestWordsAsset result;
    StringBuilder<256> upper;
    for (const char c : source.Text)
    {
        upper.Append(Ascii::ToUpper(c));
    }
    result.Text = FrozenString(upper.AsView());
    if (source.Table.IsValid())
    {
        const MarlinTextObject *table = context.ReadDependency(source.Table.GetGuid());
        TestTableAsset values;
        MarlinTextDiagnostics diagnostics;
        MarlinTextReadOptions options;
        options.IgnoreTypeName = true;
        if (table == nullptr ||
            !MarlinTextSerializer::ReadObject(*table, TypeOf<TestTableAsset>(), &values, diagnostics, options))
        {
            context.Error("the table cannot be read");
            return false;
        }
        for (const int32 value : values.Values)
        {
            result.TableSum += value;
        }
    }
    context.SetResult(result);
    return true;
}

bool TestMarkedWordsConverter::ShouldCreate(const ExtensionContext & /*context*/)
{
    return Enabled;
}

bool TestMarkedWordsConverter::Convert(ConversionContext &context)
{
    AssetConverter *wrapped = context.GetReplaced(*this);
    if (wrapped == nullptr || !wrapped->Convert(context))
    {
        return false;
    }
    TestWordsSource source;
    (void)context.ReadSource(source);
    TestWordsAsset result;
    result.Text = FrozenString(Format("{0}!", source.Text).AsView());
    context.SetResult(result);
    return true;
}

namespace
{
constexpr StringView Content = R"(!Version "1.0"

TestTableAsset
{
    !UUID = "7ab1e000-0000-0000-0000-000000000001"
    !Name = "Table"
    Values = [1, 2, 3]
}

TestWordsSource
{
    !UUID = "7ab1e000-0000-0000-0000-000000000002"
    !Name = "Words"
    Text = "hello"
    Table = <Table#7ab1e000-0000-0000-0000-000000000001>
}
)";

const Guid TableGuid = {0x7ab1e000, 0, 0, 1};
const Guid WordsGuid = {0x7ab1e000, 0, 0, 2};

/// Content in a scratch folder at /ConverterTest, a scanned index of it, and a GlobalStore in this process with its
/// data there. The folder is /Temp/ConverterTest, created and removed only through the VFS, inside the /Temp mount,
/// so that a failed lookup can never become an empty path or the root of a drive; its native path only serves to
/// mount it.
struct ConversionFixture
{
    String Physical;
    MountId Mount = 0;
    AssetIndex Index;
    List<String> Folders;
    UniquePtr<AssetScanner> Scanner;
    UniquePtr<ConversionStore> Store;

    static constexpr StringView Scratch = "/Temp/ConverterTest";

    ConversionFixture()
    {
        (void)Vfs::Delete(Scratch);
        MARLIN_REQUIRE(Vfs::CreateDirectory(Format("{0}/Content", Scratch).AsView()));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath(Scratch, path) && !path.IsEmpty());
        MARLIN_REQUIRE(path.AsView().EndsWith("ConverterTest"));
        Physical = path.AsView();
        Mount = Vfs::Mount("/ConverterTest", MakeUnique<DirectoryMount>(Physical, true), 0);
        Folders.Add("/ConverterTest/Content");
        Scanner = MakeUnique<AssetScanner>(Index, Folders);
        // A GlobalStore of its own, in this process, with its data in the scratch folder.
        MARLIN_REQUIRE(Vfs::CreateDirectory(Format("{0}/GlobalStore", Scratch).AsView()));
        ConversionStoreDesc store;
        store.InProcessDirectory = Format("{0}/GlobalStore", Physical);
        Store = MakeUnique<ConversionStore>(Move(store));
    }

    ~ConversionFixture()
    {
        Store.Reset();
        (void)Vfs::Unmount(Mount);
        (void)Vfs::Delete(Scratch);
    }

    MARLIN_NONCOPYABLE(ConversionFixture);

    void Write(StringView text)
    {
        MARLIN_REQUIRE(Vfs::WriteAll("/ConverterTest/Content/Words.masset", text));
        List<String> changed;
        Scanner->ScanAll(changed);
    }

    /// Reads the block a conversion stored in *store* back into an object of *type*.
    static bool ReadBack(ConversionStore &store, const ConversionResult &result, const TypeInfo &type, void *object)
    {
        List<uint8> bytes;
        List<uint8> bulk;
        if (!store.ReadResult(result.Key, bytes, bulk) || bytes.Count() != result.Location.BlockSize)
        {
            return false;
        }
        const Span<const uint8> block = bytes.AsSpan();
        StringBuilder<128> error;
        AssetBlockHeader header;
        if (!AssetBlock::Validate(block, error))
        {
            return false;
        }
        MemCopy(&header, block.Data(), sizeof(header));
        if (header.TypeHash != AssetBlock::GetTypeHash(type))
        {
            return false;
        }
        const uint8 *image = block.Data() + header.ImageOffset;
        for (const FieldInfo *field : type.GetFields())
        {
            if (field->GetType().GetKind() == TypeKind::Int32)
            {
                MemCopy(field->GetValue(object), image + field->GetOffset(), sizeof(int32));
            }
        }
        return true;
    }
};

ConversionTarget Windows()
{
    ConversionTarget target;
    target.Platform = "Windows";
    return target;
}
} // namespace

MARLIN_TEST(AssetConversion, CacheHitsFromTheIndexAlone)
{
    TestMarkedWordsConverter::Enabled = false;
    ConversionFixture fixture;
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);
    AssetConversion conversion(resolver, *fixture.Store);

    TestWordsConverter::Runs = 0;
    const ConversionResult first = conversion.Convert(WordsGuid, Windows());
    MARLIN_REQUIRE(first.Succeeded);
    MARLIN_EXPECT(!first.CacheHit && TestWordsConverter::Runs == 1);
    MARLIN_EXPECT(first.Inputs.Count() == 1 && first.Inputs[0].Uuid == TableGuid);
    TestWordsAsset words;
    MARLIN_REQUIRE(ConversionFixture::ReadBack(*fixture.Store, first, TypeOf<TestWordsAsset>(), &words));
    MARLIN_EXPECT(words.TableSum == 6);

    // The source file is gone, but the index still knows its hashes: a hit needs nothing else.
    MARLIN_REQUIRE(Vfs::Delete("/ConverterTest/Content/Words.masset"));
    const ConversionResult second = conversion.Convert(WordsGuid, Windows());
    MARLIN_EXPECT(second.Succeeded && second.CacheHit && second.Key == first.Key && TestWordsConverter::Runs == 1);

    ConversionTarget server = Windows();
    server.Kind = TargetKind::Server;
    fixture.Write(Content);
    const ConversionResult other = conversion.Convert(WordsGuid, server);
    MARLIN_EXPECT(other.Succeeded && !other.CacheHit && other.Key != first.Key);

    const ConversionResult table = conversion.Convert(TableGuid, Windows());
    MARLIN_EXPECT(table.Succeeded && !table.CacheHit);
}

MARLIN_TEST(AssetConversion, DependencyChangesMiss)
{
    TestMarkedWordsConverter::Enabled = false;
    ConversionFixture fixture;
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);
    AssetConversion conversion(resolver, *fixture.Store);
    const ConversionResult before = conversion.Convert(WordsGuid, Windows());
    MARLIN_REQUIRE(before.Succeeded);

    String edited(Content);
    (void)edited.Replace("Values = [1, 2, 3]", "Values = [1, 2, 3, 4]");
    fixture.Write(edited.AsView());
    TestWordsConverter::Runs = 0;
    const ConversionResult after = conversion.Convert(WordsGuid, Windows());
    MARLIN_REQUIRE(after.Succeeded);
    MARLIN_EXPECT(!after.CacheHit && after.Key != before.Key && TestWordsConverter::Runs == 1);
    TestWordsAsset words;
    MARLIN_REQUIRE(ConversionFixture::ReadBack(*fixture.Store, after, TypeOf<TestWordsAsset>(), &words));
    MARLIN_EXPECT(words.TableSum == 10);

    String moved(edited);
    (void)moved.Replace("<Table#", "</Elsewhere/Content:Table#");
    fixture.Write(moved.AsView());
    const ConversionResult hinted = conversion.Convert(WordsGuid, Windows());
    MARLIN_EXPECT(hinted.Succeeded && hinted.CacheHit && hinted.Key == after.Key);
}

MARLIN_TEST(AssetConversion, ReferencedPathsStayInTheMount)
{
    String path;
    constexpr StringView Scene = "/Project/Content/Scenes/Town.gltf";
    MARLIN_EXPECT(ResolveReferencedPath(Scene, "Town.bin", path) && path == "/Project/Content/Scenes/Town.bin");
    MARLIN_EXPECT(ResolveReferencedPath(Scene, "./Data\\Town.bin", path) &&
                  path == "/Project/Content/Scenes/Data/Town.bin");
    MARLIN_EXPECT(ResolveReferencedPath(Scene, "../../Shared/Town.bin", path) && path == "/Project/Shared/Town.bin");
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "../../../Engine/Town.bin", path));
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "/Project/Content/Town.bin", path));
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "C:/Town.bin", path));
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "https://example.com/Town.bin", path));
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "Data/", path));
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "..", path));
    MARLIN_EXPECT(!ResolveReferencedPath(Scene, "", path));
}

MARLIN_TEST(AssetConversion, ReferencedBinariesAreInputs)
{
    ConversionFixture fixture;
    MARLIN_REQUIRE(Vfs::CreateDirectory("/ConverterTest/Content/Scene"));
    MARLIN_REQUIRE(Vfs::WriteAll("/ConverterTest/Content/Scene/Town.gltf", "{}"));
    MARLIN_REQUIRE(Vfs::WriteAll("/ConverterTest/Content/Scene/Town.bin", "buffer"));
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);

    AssetBinaryInfo buffer;
    MARLIN_REQUIRE(fixture.Index.FindBinaryAt("/ConverterTest/Content/Scene/Town.bin", buffer));
    MARLIN_EXPECT(buffer.Uuid.IsValid() && buffer.Path == "/ConverterTest/Content/Scene/Town.bin");
    AssetBinaryInfo asset;
    MARLIN_EXPECT(!fixture.Index.FindBinaryAt("/ConverterTest/Content/Words.masset", asset));

    ConversionContext context(resolver, Windows());
    List<uint8> bytes;
    MARLIN_REQUIRE(context.ReadReferencedBinary("/ConverterTest/Content/Scene/Town.gltf", "Town.bin", 0,
                                                ISourceFileReader::ToEnd, bytes));
    MARLIN_EXPECT(bytes.Count() == 6 && bytes[0] == 'b');
    const Span<const DependencyInput> inputs = context.GetInputs();
    MARLIN_EXPECT(inputs.Count() == 1 && inputs[0].Kind == DependencyInputKind::Binary &&
                  inputs[0].Uuid == buffer.Uuid);

    MARLIN_EXPECT(!context.ReadReferencedBinary("/ConverterTest/Content/Scene/Town.gltf", "Missing.bin", 0,
                                                ISourceFileReader::ToEnd, bytes));
    MARLIN_EXPECT(context.HasFailed());
}

MARLIN_TEST(AssetConversion, SourcesAheadOfTheIndexAreNotCached)
{
    TestMarkedWordsConverter::Enabled = false;
    ConversionFixture fixture;
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);
    AssetConversion conversion(resolver, *fixture.Store);

    // Edited on disk but not scanned yet: what the converter would read is not what the key covers.
    String edited(Content);
    (void)edited.Replace("Text = \"hello\"", "Text = \"goodbye\"");
    MARLIN_REQUIRE(Vfs::WriteAll("/ConverterTest/Content/Words.masset", edited.AsView()));
    TestWordsConverter::Runs = 0;
    const ConversionResult stale = conversion.Convert(WordsGuid, Windows());
    MARLIN_EXPECT(!stale.Succeeded && TestWordsConverter::Runs == 0);
    MARLIN_REQUIRE(stale.Stale.Count() == 1);
    MARLIN_EXPECT(stale.Stale[0] == "/ConverterTest/Content/Words.masset");

    // Once the index has caught up, the conversion runs; nothing was cached for the stale attempt.
    List<String> changed;
    fixture.Scanner->ScanAll(changed);
    const ConversionResult fresh = conversion.Convert(WordsGuid, Windows());
    MARLIN_REQUIRE(fresh.Succeeded);
    MARLIN_EXPECT(!fresh.CacheHit && fresh.Stale.IsEmpty() && TestWordsConverter::Runs == 1);
}

MARLIN_TEST(AssetConversion, ReplacingConverterWraps)
{
    ConversionFixture fixture;
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);
    TestMarkedWordsConverter::Enabled = false;
    ConversionResult plain;
    {
        AssetConversion conversion(resolver, *fixture.Store);
        MARLIN_EXPECT(conversion.FindConverter("TestWordsSource")->GetType().GetName() == "TestWordsConverter");
        plain = conversion.Convert(WordsGuid, Windows());
    }
    TestMarkedWordsConverter::Enabled = true;
    {
        AssetConversion conversion(resolver, *fixture.Store);
        const AssetConverter *chosen = conversion.FindConverter("TestWordsSource");
        MARLIN_REQUIRE(chosen != nullptr && chosen->GetType().GetName() == "TestMarkedWordsConverter");
        MARLIN_EXPECT(conversion.GetReplaced(*chosen)->GetType().GetName() == "TestWordsConverter");
        TestWordsConverter::Runs = 0;
        const ConversionResult marked = conversion.Convert(WordsGuid, Windows());
        MARLIN_REQUIRE(marked.Succeeded);
        MARLIN_EXPECT(!marked.CacheHit && marked.Key != plain.Key && TestWordsConverter::Runs == 1);
    }
    TestMarkedWordsConverter::Enabled = false;
}

namespace
{
/// A second store on the fixture's GlobalStore, as another machine on the studio server.
ConversionStoreDesc SameServer(const ConversionFixture &fixture)
{
    ConversionStoreDesc desc;
    desc.Host = "127.0.0.1";
    desc.Port = fixture.Store->GetDesc().Port;
    return desc;
}
} // namespace

MARLIN_TEST(AssetConversion, OtherMachinesHitWithoutConverting)
{
    TestMarkedWordsConverter::Enabled = false;
    ConversionFixture fixture;
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);
    TestWordsConverter::Runs = 0;
    ConversionResult first;
    {
        AssetConversion conversion(resolver, *fixture.Store);
        first = conversion.Convert(WordsGuid, Windows());
    }
    MARLIN_REQUIRE(first.Succeeded && !first.CacheHit && TestWordsConverter::Runs == 1);

    ConversionStore other(SameServer(fixture));
    AssetConversion conversion(resolver, other);
    const ConversionResult second = conversion.Convert(WordsGuid, Windows());
    MARLIN_REQUIRE(second.Succeeded);
    MARLIN_EXPECT(second.CacheHit && second.Key == first.Key && TestWordsConverter::Runs == 1);
    MARLIN_EXPECT(second.Location.Key == FormatResultKey(first.Key) &&
                  second.Location.BlockSize == first.Location.BlockSize);
    TestWordsAsset words;
    MARLIN_REQUIRE(ConversionFixture::ReadBack(other, second, TypeOf<TestWordsAsset>(), &words));
    MARLIN_EXPECT(words.TableSum == 6);
}

MARLIN_TEST(AssetConversion, ConvertManyChecksInOneBatch)
{
    TestMarkedWordsConverter::Enabled = false;
    ConversionFixture fixture;
    fixture.Write(Content);
    IndexSourceResolver resolver(fixture.Index);
    AssetConversion conversion(resolver, *fixture.Store);
    const Guid both[] = {WordsGuid, TableGuid};
    List<ConversionResult> results;
    conversion.ConvertMany(both, Windows(), results, false);
    MARLIN_REQUIRE(results.Count() == 2);
    MARLIN_EXPECT(results[0].Missing && results[1].Missing && !results[0].Succeeded);

    TestWordsConverter::Runs = 0;
    conversion.ConvertMany(both, Windows(), results);
    MARLIN_EXPECT(results[0].Succeeded && !results[0].CacheHit && results[1].Succeeded && TestWordsConverter::Runs == 1);
    conversion.ConvertMany(both, Windows(), results);
    MARLIN_EXPECT(results[0].CacheHit && results[1].CacheHit && TestWordsConverter::Runs == 1);
}

MARLIN_TEST(AssetConversion, ConvertedResultsCompressPerBlock)
{
    // A compressible block, and bulk that spans several blocks: the first bulk block compresses, the last, random,
    // does not and is stored as it is.
    List<uint8> block;
    for (uint32 i = 0; i < 1000; ++i)
    {
        block.Add(static_cast<uint8>(i % 3));
    }
    List<uint8> bulk;
    uint32 state = 12345;
    for (uint32 i = 0; i < ConvertedResultBlockSize * 2 + 100; ++i)
    {
        state = state * 1664525u + 1013904223u;
        bulk.Add(i < ConvertedResultBlockSize ? static_cast<uint8>((i / 64) % 7) : static_cast<uint8>(state >> 24));
    }
    const Hash128 key = ConversionStore::ResultKey("test");
    CompressionCodecSet codecs;
    codecs.Build({});
    for (const StringView name : {StringView(), StringView("Zstd"), StringView("Lz4")})
    {
        const CompressionCodec *codec = name.IsEmpty() ? nullptr : codecs.Find(name);
        if (!name.IsEmpty() && codec == nullptr)
        {
            continue;
        }
        List<uint8> value;
        ConvertedResultHeader header;
        ConvertedResultWriter::Write(key, block.AsSpan(), bulk.AsSpan(), codec, 0, value, &header);
        MARLIN_EXPECT(header.AssetBlockSize == block.Count() && header.BulkSize == bulk.Count());
        MARLIN_EXPECT(header.BulkOffset % ConvertedResultBulkAlignment == 0 && header.BulkOffset >= block.Count());
        MARLIN_EXPECT(header.BlockCount == (header.LogicalSize + ConvertedResultBlockSize - 1) / ConvertedResultBlockSize);

        ConvertedResultReader reader;
        MARLIN_REQUIRE(reader.Parse(key, value.AsSpan()));
        MARLIN_EXPECT(reader.CanDecode(codecs));
        MARLIN_EXPECT(reader.IsStored(header.BlockCount - 1));
        MARLIN_EXPECT(codec == nullptr ? reader.IsStored(0) : !reader.IsStored(0));
        if (codec != nullptr)
        {
            MARLIN_EXPECT(value.Count() < header.LogicalSize);
            CompressionCodecSet none;
            MARLIN_EXPECT(!reader.CanDecode(none));
        }
        List<uint8> logical;
        MARLIN_REQUIRE(ConvertedResultReader::Decode(codecs, key, value.AsSpan(), logical));
        MARLIN_EXPECT(MemCompare(logical.Data(), block.Data(), block.Count()) == 0);
        MARLIN_EXPECT(MemCompare(logical.Data() + header.BulkOffset, bulk.Data(), bulk.Count()) == 0);
        MARLIN_EXPECT(!reader.Parse(ConversionStore::ResultKey("other"), value.AsSpan()));
    }
}

namespace
{
/// An external converter worker in this process, pulling jobs from the indexer as MarlinConverter does.
struct InProcessWorker
{
    NetLoop &Loop;
    String Pipe;
    ConversionStoreDesc Store;
    ThreadHandle Thread;
    Atomic<bool> Stopping{false};
    Atomic<int32> Conversions{0};
    UniquePtr<AssetIndexClient> Client;

    InProcessWorker(NetLoop &loop, StringView pipe, ConversionStoreDesc store)
        : Loop(loop), Pipe(pipe), Store(Move(store))
    {
        Client = MakeUnique<AssetIndexClient>(Loop);
        MARLIN_REQUIRE(Client->ConnectPipe(Pipe.AsView()));
        ThreadDesc desc;
        desc.Name = "Test Converter";
        desc.Entry = [](void *argument) { static_cast<InProcessWorker *>(argument)->Run(); };
        desc.Argument = this;
        Thread = PlatformThread::Create(desc);
    }

    ~InProcessWorker()
    {
        Stopping.Store(true);
        Client->Disconnect();
        PlatformThread::Join(Thread);
    }

    MARLIN_NONCOPYABLE(InProcessWorker);

    void Run()
    {
        // Batched, as MarlinConverter is: up to four jobs per take, all their outcomes in one finish.
        ClientSourceResolver resolver(*Client);
        ConversionStore store(Store);
        AssetConversion conversion(resolver, store);
        JsonValue take = JsonValue::MakeObject();
        take.Set("Max", JsonValue::MakeUnsigned(4));
        while (!Stopping.Load())
        {
            RemoteResponse response;
            if (!Client->GetRemote().CallSync("Converter.Take", take, response, 60000))
            {
                break;
            }
            const JsonValue *jobs = response.Result.Find("Jobs");
            if (jobs == nullptr || jobs->Count() == 0)
            {
                continue;
            }
            StringBuilder<4096> text;
            JsonWriter writer(text);
            writer.BeginObject().Key("Outcomes").BeginArray();
            for (const JsonValue &element : jobs->GetElements())
            {
                ConversionJob job;
                StringBuilder<256> error;
                MARLIN_REQUIRE(Json::Read(element, job, error) && job.Job != 0);
                const ConversionResult result = conversion.Convert(job.Uuid, job.Target);
                if (result.Succeeded && !result.CacheHit)
                {
                    Conversions.FetchAdd(1);
                }
                ConversionOutcome outcome;
                outcome.Job = job.Job;
                outcome.Succeeded = result.Succeeded;
                outcome.CacheHit = result.CacheHit;
                outcome.Key = FormatResultKey(result.Key);
                outcome.Location = result.Location;
                outcome.Errors = result.Errors;
                for (const DependencyInput &input : result.Inputs)
                {
                    outcome.Inputs.Add(input.Uuid);
                }
                Json::Write(writer, outcome);
            }
            writer.EndArray().EndObject();
            JsonValue params;
            JsonError parseError;
            if (!Json::Parse(text.AsView(), params, parseError) ||
                !Client->GetRemote().CallSync("Converter.Finish", params, response))
            {
                break;
            }
        }
    }
};
} // namespace

MARLIN_TEST(AssetConversion, DevelopmentLoadsThroughTheIndexer)
{
    TestMarkedWordsConverter::Enabled = false;
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    ConversionFixture fixture;
    fixture.Write(Content);
    AssetIndexServiceDesc desc;
    desc.Folders = fixture.Folders;
    desc.StoreDirectory = Format("{0}/Store", fixture.Physical);
    desc.PipeName = Format("MarlinTests.{0}.DevelopmentLoad", PlatformProcess::CurrentId());
    desc.ProjectFile = Format("{0}/Test.mproject", fixture.Physical);
    desc.MaxConverters = 0;
    {
        AssetIndexService service(loop, desc);
        MARLIN_REQUIRE(service.Start());
        InProcessWorker worker(loop, desc.PipeName.AsView(), SameServer(fixture));

        // Loads as the engine does in development: located by the indexer, read from GlobalStore.
        Assets::Initialize();
        SharedPtr<IndexerConnection> connection(new IndexerConnection(loop));
        MARLIN_REQUIRE(connection->Client.ConnectPipe(desc.PipeName.AsView()));
        SharedPtr<GlobalStoreConnection> globalStore(new GlobalStoreConnection(loop));
        MARLIN_REQUIRE(globalStore->Client.Connect("127.0.0.1", fixture.Store->GetDesc().Port));
        const AssetStoreId store =
            Assets::AddStore(MakeUnique<GlobalStoreAssetStore>(connection, globalStore, Windows()), 0);
        {
            const AssetRef<TestWordsAsset> words = Assets::LoadSync<TestWordsAsset>(WordsGuid);
            MARLIN_REQUIRE(words.Get() != nullptr);
            MARLIN_EXPECT(words->Text.AsView() == "HELLO" && words->TableSum == 6);
            const AssetRef<TestTableAsset> table = Assets::LoadSync<TestTableAsset>(TableGuid);
            MARLIN_REQUIRE(table.Get() != nullptr);
            MARLIN_EXPECT(table->Values.Count() == 3 && table->Values[2] == 3);
        }
        Assets::CollectGarbage();
        MARLIN_EXPECT(worker.Conversions.Load() == 2);
        {
            const AssetRef<TestWordsAsset> again = Assets::LoadSync<TestWordsAsset>(WordsGuid);
            MARLIN_EXPECT(again.Get() != nullptr && again->TableSum == 6);
        }
        MARLIN_EXPECT(worker.Conversions.Load() == 2);
        MARLIN_EXPECT(!Assets::LoadSync<TestWordsAsset>(Guid{0x7ab1e000, 0, 0, 99}).IsLoaded());

        const AssetRef<TestWordsAsset> words = Assets::LoadSync<TestWordsAsset>(WordsGuid);
        MARLIN_REQUIRE(words.IsLoaded());
        EnableHotReload(*connection);
        RemoteResponse subscribed;
        MARLIN_REQUIRE(connection->Client.CallSync("Remote.ListStreams", JsonValue(), subscribed));
        String edited(Content);
        (void)edited.Replace("Text = \"hello\"", "Text = \"world\"");
        MARLIN_REQUIRE(Vfs::WriteAll("/ConverterTest/Content/Words.masset", edited.AsView()));
        for (uint32 waited = 0; words->Text.AsView() != "WORLD" && waited < 10000; waited += 5)
        {
            PlatformThread::Sleep(5);
            connection->Client.ProcessPending();
            Assets::ProcessFrame();
        }
        MARLIN_EXPECT(words->Text.AsView() == "WORLD" && words->TableSum == 6);
        MARLIN_EXPECT(worker.Conversions.Load() == 3);
        Assets::CollectGarbage();
        Assets::RemoveStore(store);
        connection->Client.Disconnect();
        globalStore->Client.Disconnect();
        Assets::Shutdown();
        service.Stop();
    }
    loop.Stop();
}

MARLIN_TEST(AssetConversion, IndexerKeepsLocatedObjectsAcrossRestarts)
{
    TestMarkedWordsConverter::Enabled = false;
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    ConversionFixture fixture;
    fixture.Write(Content);
    AssetIndexServiceDesc desc;
    desc.Folders = fixture.Folders;
    desc.StoreDirectory = Format("{0}/Store", fixture.Physical);
    desc.PipeName = Format("MarlinTests.{0}.KeepLocated", PlatformProcess::CurrentId());
    desc.ProjectFile = Format("{0}/Test.mproject", fixture.Physical);
    desc.MaxConverters = 0;
    desc.GlobalStoreHost = "127.0.0.1";
    desc.GlobalStorePort = fixture.Store->GetDesc().Port;
    JsonValue params = JsonValue::MakeObject();
    {
        StringBuilder<64> uuid;
        WordsGuid.FormatTo(uuid, {});
        params.Set("Uuid", JsonValue::MakeString(uuid.AsView()));
        JsonValue target = JsonValue::MakeObject();
        target.Set("Platform", JsonValue::MakeString("Windows"));
        params.Set("Target", Move(target));
    }
    String key;
    {
        AssetIndexService service(loop, desc);
        MARLIN_REQUIRE(service.Start());
        InProcessWorker worker(loop, desc.PipeName.AsView(), SameServer(fixture));
        AssetIndexClient client(loop);
        MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()));
        RemoteResponse response;
        MARLIN_REQUIRE(client.GetRemote().CallSync("Asset.Locate", params, response, 20000));
        const JsonValue *found = response.Result.Find("Key");
        MARLIN_REQUIRE(found != nullptr && found->IsString());
        key = String(found->AsString());
        client.Disconnect();
        service.Stop();
    }
    // No worker this time: only a location kept from the last run can answer.
    AssetIndexService service(loop, desc);
    MARLIN_REQUIRE(service.Start());
    AssetIndexClient client(loop);
    MARLIN_REQUIRE(client.ConnectPipe(desc.PipeName.AsView()));
    RemoteResponse response;
    MARLIN_REQUIRE(client.GetRemote().CallSync("Asset.Locate", params, response, 5000));
    const JsonValue *found = response.Result.Find("Key");
    MARLIN_EXPECT(found != nullptr && found->IsString() && found->AsString() == key.AsView());
    client.Disconnect();
    service.Stop();
    loop.Stop();
}

MARLIN_TEST(AssetConversion, WarmingConvertsEveryObjectForEveryTarget)
{
    TestMarkedWordsConverter::Enabled = false;
    ConversionFixture fixture;
    fixture.Write(Content);
    GlobalStoreWarmingDesc desc;
    desc.Targets.Add(Windows());
    ConversionTarget console;
    console.Platform = "PS5";
    desc.Targets.Add(console);
    desc.Threads = 3;
    desc.Store = SameServer(fixture);
    StringBuilder<4096> failures;
    const GlobalStoreWarmingStats first = WarmGlobalStore(fixture.Index, desc, &failures);
    MARLIN_EXPECT(first.Conversions == 4 && first.CacheHits == 0 && first.Failures == 0);
    MARLIN_EXPECT(failures.Length() == 0);
    const GlobalStoreWarmingStats second = WarmGlobalStore(fixture.Index, desc, &failures);
    MARLIN_EXPECT(second.Conversions == 0 && second.CacheHits == 4 && second.Failures == 0);
}

MARLIN_TEST(AssetConversion, ConverterPoolBatchesTakeAndFinish)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    {
        RemoteServerDesc serverDesc;
        serverDesc.PipeName = Format("MarlinTests.{0}.ConverterPool", PlatformProcess::CurrentId());
        RemoteServer server(loop, serverDesc);
        ConverterPool pool(server, {}, serverDesc.PipeName.AsView(), 0);
        MARLIN_REQUIRE(server.Start());
        Atomic<int32> finished{0};
        for (uint32 i = 0; i < 3; ++i)
        {
            pool.Request(Guid{0x7ab1e000, 0, 1, i}, Windows(), ConversionPriority::Background,
                         [&finished](const ConversionOutcome &outcome) {
                             if (outcome.Succeeded)
                             {
                                 finished.FetchAdd(1);
                             }
                         });
        }
        // The same object and target again shares the job.
        pool.Request(Guid{0x7ab1e000, 0, 1, 0}, Windows(), ConversionPriority::Urgent,
                     [&finished](const ConversionOutcome &outcome) {
                         if (outcome.Succeeded)
                         {
                             finished.FetchAdd(1);
                         }
                     });

        RemoteClient worker(loop);
        MARLIN_REQUIRE(worker.ConnectPipe(serverDesc.PipeName.AsView()));
        JsonValue take = JsonValue::MakeObject();
        take.Set("Max", JsonValue::MakeUnsigned(2));
        RemoteResponse response;
        MARLIN_REQUIRE(worker.CallSync("Converter.Take", take, response));
        const JsonValue *jobs = response.Result.Find("Jobs");
        MARLIN_REQUIRE(jobs != nullptr && jobs->Count() == 2);
        StringBuilder<1024> text;
        JsonWriter writer(text);
        writer.BeginObject().Key("Outcomes").BeginArray();
        List<Guid> taken;
        for (const JsonValue &element : jobs->GetElements())
        {
            ConversionJob job;
            StringBuilder<128> error;
            MARLIN_REQUIRE(Json::Read(element, job, error));
            taken.Add(job.Uuid);
            ConversionOutcome outcome;
            outcome.Job = job.Job;
            outcome.Succeeded = true;
            Json::Write(writer, outcome);
        }
        writer.EndArray().EndObject();
        // The urgent request moved its job to the front.
        MARLIN_EXPECT(taken[0] == (Guid{0x7ab1e000, 0, 1, 0}));
        JsonValue outcomes;
        JsonError parseError;
        MARLIN_REQUIRE(Json::Parse(text.AsView(), outcomes, parseError));
        MARLIN_REQUIRE(worker.CallSync("Converter.Finish", outcomes, response));
        MARLIN_EXPECT(response.Result.IsArray() && response.Result.Count() == 2 &&
                      response.Result.GetElements()[0].AsBool() && response.Result.GetElements()[1].AsBool());
        MARLIN_EXPECT(finished.Load() == 3);

        // A take without params gets one job as before, and a lone outcome finishes it.
        MARLIN_REQUIRE(worker.CallSync("Converter.Take", JsonValue(), response));
        ConversionJob single;
        StringBuilder<128> error;
        MARLIN_REQUIRE(Json::Read(response.Result, single, error) && single.Job != 0);
        ConversionOutcome outcome;
        outcome.Job = single.Job;
        outcome.Succeeded = true;
        StringBuilder<512> lone;
        JsonWriter loneWriter(lone);
        Json::Write(loneWriter, outcome);
        MARLIN_REQUIRE(Json::Parse(lone.AsView(), outcomes, parseError));
        MARLIN_REQUIRE(worker.CallSync("Converter.Finish", outcomes, response));
        MARLIN_EXPECT(response.Result.AsBool() && finished.Load() == 4 && pool.IsIdle());
        worker.Disconnect();
        pool.Shutdown();
        server.Stop();
    }
    loop.Stop();
}
} // namespace Marlin::Tests
