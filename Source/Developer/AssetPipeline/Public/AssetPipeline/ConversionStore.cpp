#include "AssetPipeline/ConversionStore.h"

#include "AssetPipeline/AssetIndex.h"
#include "Concurrency/ScopeLock.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "GlobalStoreServer/GlobalStoreServer.h"
#include "HAL/PlatformTime.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "Net/NetSubsystem.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
constexpr uint32 DependencyMagic = 0x5044444Du;
constexpr uint32 LaunchMilliseconds = 5000;
/// How long a check on whether the studio server is back may hold up a conversion.
constexpr uint32 ProbeMilliseconds = 1000;
constexpr StringView LocalHost = "127.0.0.1";

uint64 Seconds()
{
    return PlatformTime::Ticks() / PlatformTime::TicksPerSecond();
}

void WriteDependencies(Span<const DependencyInput> inputs, List<uint8> &bytes)
{
    const auto append = [&](const void *data, usize size) {
        bytes.AddRange(Span<const uint8>(static_cast<const uint8 *>(data), size));
    };
    append(&DependencyMagic, sizeof(DependencyMagic));
    const auto count = static_cast<uint32>(inputs.Count());
    append(&count, sizeof(count));
    for (const DependencyInput &input : inputs)
    {
        const auto kind = static_cast<uint8>(input.Kind);
        append(&kind, 1);
        append(&input.Uuid, sizeof(Guid));
        const auto length = static_cast<uint32>(input.Section.Length());
        append(&length, sizeof(length));
        append(input.Section.Data(), length);
    }
}

bool ReadDependencies(Span<const uint8> bytes, List<DependencyInput> &inputs)
{
    inputs.Clear();
    usize position = 0;
    const auto take = [&](void *target, usize size) {
        if (size > bytes.Count() - position)
        {
            return false;
        }
        MemCopy(target, bytes.Data() + position, size);
        position += size;
        return true;
    };
    uint32 magic = 0;
    uint32 count = 0;
    if (!take(&magic, 4) || magic != DependencyMagic || !take(&count, 4))
    {
        return false;
    }
    for (uint32 i = 0; i < count; ++i)
    {
        DependencyInput &input = inputs.AddDefaulted();
        uint8 kind = 0;
        uint32 length = 0;
        if (!take(&kind, 1) || kind > static_cast<uint8>(DependencyInputKind::SceneDecoder) ||
            !take(&input.Uuid, sizeof(Guid)) || !take(&length, 4) || length > bytes.Count() - position)
        {
            inputs.Clear();
            return false;
        }
        input.Kind = static_cast<DependencyInputKind>(kind);
        input.Section = StringView(reinterpret_cast<const char *>(bytes.Data() + position), length);
        position += length;
    }
    return true;
}

/// The location a result's header gives; false if *bytes* is not the header of a result for *key* this process can
/// decode.
bool LocationOf(const Hash128 &key, Span<const uint8> bytes, ConvertedResultLocation &location)
{
    if (bytes.Count() < sizeof(ConvertedResultHeader))
    {
        return false;
    }
    ConvertedResultHeader header;
    MemCopy(&header, bytes.Data(), sizeof(header));
    if (header.Magic != ConvertedResultMagic || header.Version != ConvertedResultVersion || header.Key != key)
    {
        return false;
    }
    usize length = 0;
    while (length < sizeof(header.Codec) && header.Codec[length] != '\0')
    {
        ++length;
    }
    if (length != 0 && Engine::GetCompressionCodecs().Find(StringView(header.Codec, length)) == nullptr)
    {
        return false;
    }
    location.Key = FormatResultKey(key);
    location.BlockOffset = header.AssetBlockOffset;
    location.BlockSize = header.AssetBlockSize;
    location.BulkOffset = header.BulkOffset;
    location.BulkSize = header.BulkSize;
    return true;
}

struct PutWaiter
{
    Event Done{EventReset::Manual};
    bool Ok = false;
};
} // namespace

ConversionStoreDesc ConversionStoreDesc::FromConfig()
{
    ConversionStoreDesc desc;
    desc.Host = Config::GetOr<String>("Engine.GlobalStore.Host", String());
    desc.Port = Config::GetOr<uint16>("Engine.GlobalStore.Port", GlobalStoreDefaultPort);
    const String codec = Config::GetOr<String>("Engine.GlobalStore.Codec", String("Zstd"));
    desc.Codec = codec.IsEmpty() || codec == "None" ? nullptr : Engine::GetCompressionCodecs().Find(codec.AsView());
    desc.Level = Config::GetOr<int32>("Engine.GlobalStore.Level", 0);
    return desc;
}

ConversionStoreDesc ConversionStoreDesc::InProcess(StringView directory)
{
    ConversionStoreDesc desc;
    (void)Vfs::CreateDirectory(directory);
    StringBuilder<512> physical;
    if (Vfs::GetPhysicalPath(directory, physical))
    {
        desc.InProcessDirectory = physical.AsView();
    }
    return desc;
}

/// A server inside this process, on a loop of its own.
struct ConversionStore::InProcess
{
    NetLoop Loop;
    UniquePtr<GlobalStoreServer> Server;
};

ConversionStore::ConversionStore(ConversionStoreDesc desc) : _desc(Move(desc))
{
    NetSubsystem *net = Engine::GetSubsystem<NetSubsystem>();
    if (net == nullptr || !net->GetLoop().IsRunning())
    {
        _ownLoop = MakeUnique<NetLoop>();
        (void)_ownLoop->Start(1);
    }
    _client = MakeUnique<GlobalStoreClient>(_ownLoop != nullptr ? *_ownLoop : net->GetLoop());
    if (!_desc.InProcessDirectory.IsEmpty())
    {
        _inProcess = MakeUnique<InProcess>();
        (void)_inProcess->Loop.Start(1);
        GlobalStoreServerDesc server;
        server.DataDirectory = _desc.InProcessDirectory;
        _inProcess->Server = MakeUnique<GlobalStoreServer>(_inProcess->Loop, Move(server));
        if (!_inProcess->Server->Start())
        {
            MARLIN_LOG(LogAssetIndex, Error, "The GlobalStore in this process cannot start in {0}",
                       _desc.InProcessDirectory);
        }
        _desc.Host = String();
        _desc.Port = _inProcess->Server->GetPort();
    }
}

ConversionStore::~ConversionStore()
{
    _client.Reset();
    if (_ownLoop != nullptr)
    {
        _ownLoop->Stop();
    }
    if (_inProcess != nullptr)
    {
        _inProcess->Server->Stop();
        _inProcess->Loop.Stop();
    }
}

GlobalStoreClient *ConversionStore::GetClient()
{
    if (_client->IsConnected() && !(_fellBack && Seconds() >= _retryAt))
    {
        return _client.Get();
    }
    const ScopeLock lock(_connectLock);
    if (_client->IsConnected() && !(_fellBack && Seconds() >= _retryAt))
    {
        return _client.Get();
    }
    return Connect() ? _client.Get() : nullptr;
}

bool ConversionStore::Connect()
{
    if (!_desc.Host.IsEmpty() && (!_fellBack || Seconds() >= _retryAt))
    {
        // Back on the studio server once it answers; while on the local one, a separate probe keeps the
        // connection in use intact if it does not.
        bool reachable = !_fellBack;
        if (_fellBack)
        {
            GlobalStoreClient probe(_ownLoop != nullptr ? *_ownLoop : Engine::GetSubsystem<NetSubsystem>()->GetLoop());
            reachable = probe.Connect(_desc.Host.AsView(), _desc.Port, ProbeMilliseconds);
        }
        if (reachable && _client->Connect(_desc.Host.AsView(), _desc.Port))
        {
            if (_fellBack)
            {
                MARLIN_LOG(LogAssetIndex, Info, "GlobalStore {0}:{1} answers again", _desc.Host, _desc.Port);
            }
            _fellBack = false;
            return true;
        }
        _retryAt = Seconds() + RetrySeconds;
        if (!_fellBack)
        {
            MARLIN_LOG(LogAssetIndex, Warning,
                       "GlobalStore {0}:{1} is unreachable; converting into the GlobalStore on this machine until it "
                       "answers",
                       _desc.Host, _desc.Port);
        }
        _fellBack = true;
        if (_client->IsConnected())
        {
            return true;
        }
    }
    return _client->Connect(LocalHost, _desc.Port) || (_inProcess == nullptr && Launch());
}

bool ConversionStore::Launch()
{
    const String executable = Format("{0}/MarlinGlobalStore{1}", Engine::GetEnvironment().ExecutableDirectory,
                                     MARLIN_PLATFORM_WINDOWS ? ".exe" : "");
    if (_client->ConnectLocal(Engine::GetEnvironment().ExecutableDirectory.AsView(), _desc.Port, LaunchMilliseconds))
    {
        return true;
    }
    MARLIN_LOG(LogAssetIndex, Error, "{0} does not answer on port {1}", executable, _desc.Port);
    return false;
}

Hash128 ConversionStore::DependencyKey(StringView text)
{
    return Xxh3Hash128(text.Data(), text.Length(), GlobalStoreDomain::DependencyRecord);
}

Hash128 ConversionStore::ResultKey(StringView text)
{
    return Xxh3Hash128(text.Data(), text.Length(), GlobalStoreDomain::ConvertedResult);
}

Hash128 ConversionStore::RedirectKey(const Hash128 &missed)
{
    StringBuilder<64> text("Redirect\n");
    text.Append(Format("{0:x}{1:x}", missed.High, missed.Low).AsView());
    return DependencyKey(text.AsView());
}

bool ConversionStore::GetDependencies(Span<const Hash128> inputKeys, List<List<DependencyInput>> &records,
                                      List<bool> &found)
{
    records.Clear();
    records.Resize(inputKeys.Count());
    found.Clear();
    found.Resize(inputKeys.Count(), false);
    GlobalStoreClient *client = GetClient();
    List<List<uint8>> values;
    List<bool> present;
    if (client == nullptr || !client->GetMany(inputKeys, values, present))
    {
        return false;
    }
    for (usize i = 0; i < inputKeys.Count(); ++i)
    {
        found[i] = present[i] && ReadDependencies(values[i].AsSpan(), records[i]);
    }
    return true;
}

bool ConversionStore::GetDependencies(const Hash128 &inputKey, List<DependencyInput> &inputs)
{
    GlobalStoreClient *client = GetClient();
    List<uint8> value;
    return client != nullptr && client->Get(inputKey, value) && ReadDependencies(value.AsSpan(), inputs);
}

bool ConversionStore::PutDependencies(const Hash128 &inputKey, Span<const DependencyInput> inputs)
{
    GlobalStoreClient *client = GetClient();
    if (client == nullptr)
    {
        return false;
    }
    List<uint8> bytes;
    WriteDependencies(inputs, bytes);
    return client->Put(inputKey, bytes.AsSpan());
}

bool ConversionStore::GetRecord(const Hash128 &key, List<uint8> &bytes)
{
    GlobalStoreClient *client = GetClient();
    return client != nullptr && client->Get(key, bytes);
}

bool ConversionStore::PutRecord(const Hash128 &key, Span<const uint8> bytes)
{
    GlobalStoreClient *client = GetClient();
    return client != nullptr && client->Put(key, bytes);
}

bool ConversionStore::HasResults(Span<const Hash128> keys, List<bool> &found)
{
    GlobalStoreClient *client = GetClient();
    if (client == nullptr)
    {
        found.Clear();
        found.Resize(keys.Count(), false);
        return false;
    }
    return client->HasMany(keys, found);
}

bool ConversionStore::LocateResults(Span<const Hash128> keys, List<ConvertedResultLocation> &locations,
                                    List<bool> &found)
{
    locations.Clear();
    locations.Resize(keys.Count());
    found.Clear();
    found.Resize(keys.Count(), false);
    GlobalStoreClient *client = GetClient();
    List<List<uint8>> headers;
    List<bool> present;
    if (client == nullptr || !client->GetMany(keys, 0, sizeof(ConvertedResultHeader), headers, present))
    {
        return false;
    }
    for (usize i = 0; i < keys.Count(); ++i)
    {
        found[i] = present[i] && LocationOf(keys[i], headers[i].AsSpan(), locations[i]);
    }
    return true;
}

bool ConversionStore::LocateResult(const Hash128 &key, ConvertedResultLocation &location)
{
    List<ConvertedResultLocation> locations;
    List<bool> found;
    if (!LocateResults(Span<const Hash128>(&key, 1), locations, found) || !found[0])
    {
        return false;
    }
    location = Move(locations[0]);
    return true;
}

bool ConversionStore::PutResult(const Hash128 &key, Span<const uint8> block, Span<const uint8> bulk,
                                ConvertedResultLocation &location)
{
    GlobalStoreClient *client = GetClient();
    if (client == nullptr)
    {
        return false;
    }
    // Written straight into the request buffer, after the room for its header, so the value is not copied again.
    List<uint8> framed;
    ConvertedResultHeader header;
    ConvertedResultWriter::Write(key, block, bulk, _desc.Codec, _desc.Level, framed, &header,
                                 GlobalStoreClient::PutHeaderSize);
    // Waited for, since whoever is told where the result is reads it next, possibly on another connection.
    PutWaiter waiter;
    client->PutAsync(key, Move(framed), [&waiter](GlobalStoreReply &reply) {
        waiter.Ok = reply.Status == GlobalStoreStatus::Ok;
        waiter.Done.Set();
    });
    (void)waiter.Done.Wait();
    if (!waiter.Ok)
    {
        return false;
    }
    location.Key = FormatResultKey(key);
    location.BlockOffset = header.AssetBlockOffset;
    location.BlockSize = header.AssetBlockSize;
    location.BulkOffset = header.BulkOffset;
    location.BulkSize = header.BulkSize;
    return true;
}

bool ConversionStore::ReadResult(const Hash128 &key, List<uint8> &block, List<uint8> &bulk)
{
    GlobalStoreClient *client = GetClient();
    List<uint8> value;
    List<uint8> logical;
    if (client == nullptr || !client->Get(key, value) ||
        !ConvertedResultReader::Decode(Engine::GetCompressionCodecs(), key, value.AsSpan(), logical))
    {
        return false;
    }
    ConvertedResultHeader header;
    MemCopy(&header, value.Data(), sizeof(header));
    block.Clear();
    block.AddRange(
        logical.AsSpan().Slice(static_cast<usize>(header.AssetBlockOffset), static_cast<usize>(header.AssetBlockSize)));
    bulk.Clear();
    bulk.AddRange(logical.AsSpan().Slice(static_cast<usize>(header.BulkOffset), static_cast<usize>(header.BulkSize)));
    return true;
}

bool ConversionStore::ReadResult(const ConvertedResultLocation &location, List<uint8> &content)
{
    Hash128 key;
    List<uint8> bulk;
    if (!ParseResultKey(location.Key.AsView(), key) || !ReadResult(key, content, bulk))
    {
        return false;
    }
    content.AddRange(bulk.AsSpan());
    return true;
}

bool ConversionStore::ExportResult(const Hash128 &key, StringView path)
{
    GlobalStoreClient *client = GetClient();
    List<uint8> value;
    List<uint8> logical;
    return client != nullptr && client->Get(key, value) &&
           ConvertedResultReader::Decode(Engine::GetCompressionCodecs(), key, value.AsSpan(), logical) &&
           Vfs::WriteAll(path, StringView(reinterpret_cast<const char *>(logical.Data()), logical.Count()));
}
} // namespace Marlin
