#include "GlobalStoreAssetStore/GlobalStoreAssetStore.h"

#include "Asset/Asset.h"
#include "Concurrency/Atomic.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "Engine/StartupProgress.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "Memory/MemoryOps.h"
#include "Net/NetSubsystem.h"
#include "Remote/RemoteLogStream.h"
#include "String/StringBuilder.h"

namespace Marlin
{
MARLIN_DEFINE_LOG_CATEGORY(LogAssetIndexer);

bool GlobalStoreAssetStoreSubsystem::Enabled = true;
int32 GlobalStoreAssetStoreSubsystem::Priority = -100;
bool GlobalStoreAssetStoreSubsystem::LaunchIndexer = true;
bool GlobalStoreAssetStoreSubsystem::ShowIndexerLog = true;

namespace
{
/// What the first Get of a result asks for: the header and table of all but huge results, and with them the first
/// stored blocks, which for a small asset are all of its block. One whole block plus room for the header and a
/// table of a few hundred blocks, so a first block stored uncompressed still arrives whole.
constexpr uint64 OpenGetSize = ConvertedResultBlockSize + (uint64{4} << 10);

/// The segments of one read in flight; the last to arrive ends the read.
struct SourceRead final : RefCounted
{
    Atomic<uint32> Remaining{0};
    Atomic<bool> Failed{false};
    Function<void(bool succeeded)> Done;
};

/// An OpenConvertedResult in flight.
struct OpenJob final : RefCounted
{
    SharedPtr<GlobalStoreConnection> Connection;
    Hash128 Key;
    const CompressionCodecSet *Codecs = nullptr;
    Function<void(const FileRegion *stream)> Done;
};

bool Inside(uint64 offset, uint64 size, uint64 total)
{
    return offset <= total && size <= total - offset;
}

/// Parses the start of the value *bytes* and ends *job*.
void FinishOpen(const SharedPtr<OpenJob> &job, Span<const uint8> bytes)
{
    ConvertedResultReader reader;
    if (!reader.Parse(job->Key, bytes))
    {
        MARLIN_LOG(LogAsset, Warning, "GlobalStore holds no converted result under {0}", FormatResultKey(job->Key));
        job->Done(nullptr);
        return;
    }
    if (!reader.CanDecode(*job->Codecs))
    {
        MARLIN_LOG(LogAsset, Warning, "The converted result {0} is in {1}, which this process does not link",
                   FormatResultKey(job->Key), reader.GetCodecName());
        job->Done(nullptr);
        return;
    }
    const CompressionCodec *codec = reader.FindCodec(*job->Codecs);
    const SharedPtr<ConvertedResultCompression> compression(new ConvertedResultCompression(Move(reader), codec));
    const ConvertedResultHeader &header = compression->GetHeader();

    // Blocks of the asset block that arrived with the table need no read of their own.
    const uint64 blockSize = header.BlockSize;
    const uint64 blockEnd = header.AssetBlockOffset + header.AssetBlockSize;
    for (uint64 index = blockSize != 0 ? header.AssetBlockOffset / blockSize : 0;
         blockSize != 0 && index * blockSize < blockEnd; ++index)
    {
        uint64 offset = 0;
        uint32 stored = 0;
        uint32 logical = 0;
        if (!compression->GetBlock(index, offset, stored, logical) || !Inside(offset, stored, bytes.Count()))
        {
            break;
        }
        const SharedPtr<CachedRegionBlock> block(new CachedRegionBlock());
        block->Bytes.ResizeUninitialized(logical);
        if (!compression->Decompress(index, bytes.Slice(static_cast<usize>(offset), stored), block->Bytes.AsSpan()))
        {
            break;
        }
        compression->CacheBlock(index, SharedPtr<const CachedRegionBlock>(block.Get()));
    }

    FileRegion stream;
    stream.Size = header.LogicalSize;
    stream.Compression = SharedPtr<const IRegionCompression>(compression.Get());
    stream.Source = SharedPtr<const IRegionSource>(new GlobalStoreRegionSource(job->Connection, job->Key));
    job->Done(&stream);
}
} // namespace

GlobalStoreRegionSource::GlobalStoreRegionSource(SharedPtr<GlobalStoreConnection> connection, const Hash128 &key)
    : _connection(Move(connection)), _key(key)
{
}

void GlobalStoreRegionSource::Read(uint64 offset, uint64 size, uint8 *buffer, Function<void(bool succeeded)> done) const
{
    if (size == 0)
    {
        done(true);
        return;
    }
    const SharedPtr<SourceRead> read(new SourceRead());
    const uint64 segments = (size + SegmentSize - 1) / SegmentSize;
    read->Remaining.Store(static_cast<uint32>(segments));
    read->Done = Move(done);
    // Every segment goes out at once; the connection answers them in order, so they stream back to back.
    for (uint64 start = 0; start < size; start += SegmentSize)
    {
        const uint64 length = size - start < SegmentSize ? size - start : SegmentSize;
        _connection->Client.GetAsync(_key, offset + start, length,
                                     [read, target = buffer + start, length](GlobalStoreReply &reply) {
                                         if (reply.Status == GlobalStoreStatus::Ok && reply.Bytes.Count() == length)
                                         {
                                             MemCopy(target, reply.Bytes.Data(), static_cast<usize>(length));
                                         }
                                         else
                                         {
                                             read->Failed.Store(true);
                                         }
                                         if (read->Remaining.FetchSub(1, MemoryOrder::AcquireRelease) == 1)
                                         {
                                             read->Done(!read->Failed.Load());
                                         }
                                     });
    }
}

ConvertedResultCompression::ConvertedResultCompression(ConvertedResultReader reader, const CompressionCodec *codec)
    : _reader(Move(reader)), _codec(codec)
{
}

uint32 ConvertedResultCompression::GetBlockSize() const
{
    return _reader.GetHeader().BlockSize;
}

bool ConvertedResultCompression::GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const
{
    return _reader.GetBlock(index, offset, storedSize, logicalSize);
}

bool ConvertedResultCompression::Decompress(uint64 index, Span<const uint8> stored, Span<uint8> output) const
{
    return _reader.DecompressBlock(_codec, index, stored, output);
}

SharedPtr<const CachedRegionBlock> ConvertedResultCompression::FindCachedBlock(uint64 index) const
{
    return _cache.Find(index);
}

void ConvertedResultCompression::CacheBlock(uint64 index, SharedPtr<const CachedRegionBlock> block) const
{
    _cache.Add(index, Move(block));
}

void OpenConvertedResult(SharedPtr<GlobalStoreConnection> connection, const Hash128 &key,
                         const CompressionCodecSet &codecs, Function<void(const FileRegion *stream)> done)
{
    const SharedPtr<OpenJob> job(new OpenJob());
    job->Connection = Move(connection);
    job->Key = key;
    job->Codecs = &codecs;
    job->Done = Move(done);
    job->Connection->Client.GetAsync(key, 0, OpenGetSize, [job](GlobalStoreReply &reply) {
        if (reply.Status != GlobalStoreStatus::Ok || reply.Bytes.Count() < sizeof(ConvertedResultHeader))
        {
            job->Done(nullptr);
            return;
        }
        ConvertedResultHeader header;
        MemCopy(&header, reply.Bytes.Data(), sizeof(header));
        const uint64 tableEnd = ConvertedResultReader::GetTableEnd(header.BlockCount);
        if (reply.Bytes.Count() >= tableEnd || tableEnd > reply.TotalSize)
        {
            FinishOpen(job, reply.Bytes.AsSpan());
            return;
        }
        // A table larger than the first Get, for a result of hundreds of MiB: one more round trip.
        job->Connection->Client.GetAsync(job->Key, 0, tableEnd, [job](GlobalStoreReply &table) {
            if (table.Status != GlobalStoreStatus::Ok)
            {
                job->Done(nullptr);
                return;
            }
            FinishOpen(job, table.Bytes.AsSpan());
        });
    });
}

bool MakeAssetLocation(const FileRegion &stream, const ConvertedResultLocation &located, AssetLocation &location)
{
    if (!Inside(located.BlockOffset, located.BlockSize, stream.Size) ||
        !Inside(located.BulkOffset, located.BulkSize, stream.Size))
    {
        return false;
    }
    location.Block = stream;
    location.Block.Offset = stream.Offset + located.BlockOffset;
    location.Block.Size = located.BlockSize;
    if (located.BulkSize != 0)
    {
        location.Bulk = stream;
        location.Bulk.Offset = stream.Offset + located.BulkOffset;
        location.Bulk.Size = located.BulkSize;
    }
    return true;
}

void LocateConvertedResult(const SharedPtr<GlobalStoreConnection> &connection, StringView what,
                           RemoteResponse &response, AssetLocateCallback done)
{
    if (!response.Succeeded)
    {
        MARLIN_LOG(LogAsset, Warning, "{0} has no converted data: {1}", what, response.ErrorMessage);
        done(nullptr);
        return;
    }
    ConvertedResultLocation located;
    StringBuilder<256> problem;
    Hash128 key;
    if (!Json::Read(response.Result, located, problem) || !ParseResultKey(located.Key.AsView(), key))
    {
        MARLIN_LOG(LogAsset, Warning, "{0}: the indexer answered with no result key", what);
        done(nullptr);
        return;
    }
    OpenConvertedResult(connection, key, Engine::GetCompressionCodecs(),
                        [name = String(what), located = Move(located), done = Move(done)](const FileRegion *stream) {
                            AssetLocation location;
                            if (stream == nullptr || !MakeAssetLocation(*stream, located, location))
                            {
                                MARLIN_LOG(LogAsset, Warning, "{0}: the indexer named a result GlobalStore cannot give",
                                           name);
                                done(nullptr);
                                return;
                            }
                            done(&location);
                        });
}

GlobalStoreAssetStore::GlobalStoreAssetStore(SharedPtr<IndexerConnection> indexer,
                                             SharedPtr<GlobalStoreConnection> store, ConversionTarget target)
    : _indexer(Move(indexer)), _store(Move(store)), _target(Move(target))
{
}

void GlobalStoreAssetStore::Locate(const Guid &guid, AssetLocateCallback done)
{
    AssetLocateParams params;
    params.Uuid = guid;
    params.Target = _target;
    StringBuilder<256> text;
    JsonWriter writer(text);
    Json::Write(writer, params);
    JsonValue value;
    JsonError error;
    if (!Json::Parse(text.AsView(), value, error))
    {
        done(nullptr);
        return;
    }
    _indexer->Client.CallAsync("Asset.Locate", value,
                               [store = _store, guid, done = Move(done)](RemoteResponse &response) mutable {
                                   StringBuilder<64> name;
                                   name.Append("Asset ");
                                   guid.FormatTo(name, {});
                                   LocateConvertedResult(store, name.AsView(), response, Move(done));
                               });
}

void GlobalStoreAssetStoreSubsystem::Initialize(SubsystemContext &context)
{
    const EngineEnvironment &environment = Engine::GetEnvironment();
    NetSubsystem *net = context.Collection->Get<NetSubsystem>();
    if (!Enabled || !environment.HasProject() || net == nullptr)
    {
        return;
    }
    _indexer = SharedPtr<IndexerConnection>(new IndexerConnection(net->GetLoop()));
    StartupProgress::SetStatus("Starting asset indexer");
    if (!AssetIndexer::Connect(_indexer->Client, environment.ProjectRoot.AsView(), environment.ProjectFile.AsView(),
                               LaunchIndexer, 15000))
    {
        MARLIN_LOG(LogAsset, Warning, "No asset indexer serves {0}; assets load only from packages",
                   environment.ProjectName);
        _indexer.Reset();
        return;
    }
    // No server named: the machine's GlobalStore, started here if the indexer has not started it yet. A named server
    // that does not answer falls back to it for this run (06 §3.3).
    String host;
    (void)Config::TryGet("Engine.GlobalStore.Host", host);
    uint32 port = GlobalStoreDefaultPort;
    (void)Config::TryGet("Engine.GlobalStore.Port", port);
    _store = SharedPtr<GlobalStoreConnection>(new GlobalStoreConnection(net->GetLoop()));
    StartupProgress::SetStatus("Starting GlobalStore");
    bool connected = !host.IsEmpty() && _store->Client.Connect(host.AsView(), static_cast<uint16>(port));
    if (!connected && !host.IsEmpty())
    {
        MARLIN_LOG(LogAsset, Warning,
                   "GlobalStore {0}:{1} is unreachable; loading from the GlobalStore on this machine", host, port);
    }
    connected =
        connected || _store->Client.ConnectLocal(environment.ExecutableDirectory.AsView(), static_cast<uint16>(port));
    if (!connected)
    {
        MARLIN_LOG(LogAsset, Warning, "No GlobalStore answers on port {0}; assets load only from packages", port);
        _store.Reset();
        _indexer->Client.Disconnect();
        _indexer.Reset();
        return;
    }
    _target.Platform = environment.Platform;
    _target.Kind = environment.TargetName == "Server" ? TargetKind::Server : TargetKind::Client;
    _storeId = Assets::AddStore(MakeUnique<GlobalStoreAssetStore>(_indexer, _store, _target), Priority);
    // Asset.Locate and Shader.Locate say when they convert first, which a splash screen shows (09 §4.6).
    _indexer->Client.OnNotification("Asset.Converting", [](const JsonValue &params) {
        const JsonValue *uuid = params.Find("Uuid");
        const JsonValue *name = params.Find("Name");
        const JsonValue *shader = params.Find("Shader");
        Guid guid;
        if (uuid != nullptr && Guid::TryParse(uuid->AsString(), guid))
        {
            Assets::NoteConverting(guid, name != nullptr ? name->AsString() : StringView(),
                                   shader != nullptr && shader->AsBool() ? AssetLoadStage::Compiling
                                                                         : AssetLoadStage::Converting);
        }
    });
    EnableHotReload(*_indexer);
    if (ShowIndexerLog)
    {
        EnableIndexerLog(*_indexer);
    }
}

void EnableHotReload(IndexerConnection &connection)
{
    connection.Client.OnNotification("Asset.Changed", [](const JsonValue &params) {
        const JsonValue *uuids = params.Find("Uuids");
        if (uuids == nullptr || !uuids->IsArray())
        {
            return;
        }
        for (const JsonValue &text : uuids->GetElements())
        {
            Guid uuid;
            AssetSlot *slot = Guid::TryParse(text.AsString(), uuid) ? Assets::FindSlot(uuid) : nullptr;
            if (slot != nullptr && slot->GetState() == AssetState::Loaded)
            {
                Assets::Reload(*slot);
            }
        }
    });
    JsonValue subscribe = JsonValue::MakeObject();
    subscribe.Set("Stream", JsonValue::MakeString("Asset.Changed"));
    connection.Client.CallAsync("Remote.Subscribe", subscribe, [](RemoteResponse &response) {
        if (!response.Succeeded)
        {
            MARLIN_LOG(LogAsset, Warning, "Hot reload is off: {0}", response.ErrorMessage);
        }
    });
}

void EnableIndexerLog(IndexerConnection &connection)
{
    connection.Client.OnNotification(RemoteLogStream::Stream, [](const JsonValue &entry) {
        RemoteLogStream::Relog(LogAssetIndexer, entry, "MarlinAssetIndexer");
    });
    JsonValue subscribe = JsonValue::MakeObject();
    subscribe.Set("Stream", JsonValue::MakeString(RemoteLogStream::Stream));
    connection.Client.CallAsync("Remote.Subscribe", subscribe, [](RemoteResponse &response) {
        if (!response.Succeeded)
        {
            MARLIN_LOG(LogAsset, Warning, "The indexer's log does not show here: {0}", response.ErrorMessage);
        }
    });
    // What the indexer logged before this subscribed is not sent again; where it stands now is.
    connection.Client.CallAsync("AssetIndex.Status", JsonValue(), [](RemoteResponse &response) {
        if (!response.Succeeded)
        {
            return;
        }
        const auto number = [&](StringView key) {
            const JsonValue *value = response.Result.Find(key);
            return value != nullptr ? static_cast<uint64>(value->AsNumber()) : uint64{0};
        };
        const JsonValue *counts = response.Result.Find("Counts");
        const JsonValue *files = counts != nullptr ? counts->Find("Files") : nullptr;
        const JsonValue *objects = counts != nullptr ? counts->Find("Objects") : nullptr;
        const JsonValue *scanning = response.Result.Find("Scanning");
        MARLIN_LOG(LogAssetIndexer, Info,
                   "Asset index: {0} files, {1} objects{2}; read {3} of {4} changed files; converting {5} "
                   "({6} queued); {7} converted, {8} from the cache, {9} failed; shaders {10} of {11} warmed",
                   files != nullptr ? static_cast<uint64>(files->AsNumber()) : uint64{0},
                   objects != nullptr ? static_cast<uint64>(objects->AsNumber()) : uint64{0},
                   scanning != nullptr && scanning->AsBool() ? ", first scan running" : "", number("FilesRead"),
                   number("FilesToRead"), number("ConversionsRunning"), number("ConversionsQueued"),
                   number("Converted"), number("ConversionCacheHits"), number("ConversionsFailed"),
                   number("ShadersWarmed"), number("ShadersToWarm"));
    });
}

void GlobalStoreAssetStoreSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    if (_indexer.IsValid())
    {
        _indexer->Client.ProcessPending();
    }
}

void GlobalStoreAssetStoreSubsystem::Deinitialize()
{
    if (_storeId != 0)
    {
        Assets::RemoveStore(_storeId);
        _storeId = 0;
    }
    if (_indexer.IsValid())
    {
        _indexer->Client.Disconnect();
        _indexer.Reset();
    }
    if (_store.IsValid())
    {
        _store->Client.Disconnect();
        _store.Reset();
    }
}
} // namespace Marlin
