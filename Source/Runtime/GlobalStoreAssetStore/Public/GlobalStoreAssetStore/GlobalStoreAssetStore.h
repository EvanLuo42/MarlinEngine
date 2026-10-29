#pragma once

#include "Asset/Assets.h"
#include "Compression/CompressionCodec.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Engine/Subsystem.h"
#include "GlobalStore/ConvertedResult.h"
#include "GlobalStore/GlobalStoreClient.h"
#include "GlobalStoreAssetStore/AssetLocate.h"
#include "Memory/SharedPtr.h"
#include "Remote/RemoteClient.h"
#include "Vfs/Vfs.h"
#include "GlobalStoreAssetStore/GlobalStoreAssetStore.gen.h"

namespace Marlin
{
/// A connection to the asset indexer, shared by the stores and their owner so the owner can cut it at shutdown while
/// lookups still hold the stores.
struct IndexerConnection final : RefCounted
{
    explicit IndexerConnection(NetLoop &loop) : Client(loop)
    {
    }

    RemoteClient Client;
};

/// A connection to GlobalStore, shared by the stores, the regions they hand out and the reads in flight.
struct GlobalStoreConnection final : RefCounted
{
    explicit GlobalStoreConnection(NetLoop &loop) : Client(loop)
    {
    }

    GlobalStoreClient Client;
};

/// The bytes of one GlobalStore value as a FileRegion source (Documentation/Architecture/02 §5.5): a read is cut into
/// segments that are all requested at once, pipelined on the connection, and land in the read's buffer.
class GLOBALSTOREASSETSTORE_API GlobalStoreRegionSource final : public IRegionSource
{
public:
    /// The bytes one Get asks for at most.
    static constexpr uint64 SegmentSize = uint64{1} << 20;

    GlobalStoreRegionSource(SharedPtr<GlobalStoreConnection> connection, const Hash128 &key);

    void Read(uint64 offset, uint64 size, uint8 *buffer, Function<void(bool succeeded)> done) const override;

private:
    SharedPtr<GlobalStoreConnection> _connection;
    Hash128 _key;
};

/// The blocks of a ConvertedResult (06 §3.3) as an IRegionCompression: reads fetch and decompress only the blocks a
/// range covers, and keep the ones they need only part of, as for a package.
class GLOBALSTOREASSETSTORE_API ConvertedResultCompression final : public IRegionCompression
{
public:
    /// *reader* has parsed the result; *codec* is its codec, null if every block is stored.
    ConvertedResultCompression(ConvertedResultReader reader, const CompressionCodec *codec);

    [[nodiscard]] const ConvertedResultHeader &GetHeader() const
    {
        return _reader.GetHeader();
    }

    [[nodiscard]] uint32 GetBlockSize() const override;
    bool GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const override;
    bool Decompress(uint64 index, Span<const uint8> stored, Span<uint8> output) const override;
    [[nodiscard]] SharedPtr<const CachedRegionBlock> FindCachedBlock(uint64 index) const override;
    void CacheBlock(uint64 index, SharedPtr<const CachedRegionBlock> block) const override;

private:
    ConvertedResultReader _reader;
    const CompressionCodec *_codec;
    mutable RegionBlockCache _cache;
};

/// Gets the header and block table of the result *key* and calls *done* with the region of its whole logical stream,
/// sourced from GlobalStore, or null if it is missing, not a converted result or in a codec *codecs* lacks. Blocks of
/// the asset block that came with the table are kept, so a small asset needs no second round trip.
GLOBALSTOREASSETSTORE_API void OpenConvertedResult(SharedPtr<GlobalStoreConnection> connection, const Hash128 &key,
                                                   const CompressionCodecSet &codecs,
                                                   Function<void(const FileRegion *stream)> done);

/// The block and bulk regions of *located* within *stream*, a region OpenConvertedResult gave; false if they lie
/// outside it.
GLOBALSTOREASSETSTORE_API bool MakeAssetLocation(const FileRegion &stream, const ConvertedResultLocation &located,
                                                 AssetLocation &location);

/// Answers a Locate from an indexer's Asset.Locate or Shader.Locate *response*: opens the result it names and calls
/// *done* with its location, or null. *what* names the object in logs.
GLOBALSTOREASSETSTORE_API void LocateConvertedResult(const SharedPtr<GlobalStoreConnection> &connection,
                                                     StringView what, RemoteResponse &response,
                                                     AssetLocateCallback done);

/// The development-time asset store (Documentation/Architecture/06 §4.2, GlobalStore streaming): asks the indexer's
/// Asset.Locate where an object's converted result is, which converts it first on a miss, then reads it straight
/// from GlobalStore, nothing written to disk. Lookups answer when the indexer and GlobalStore do, on IO threads, so a
/// conversion holds up only the assets waiting for it.
class GLOBALSTOREASSETSTORE_API GlobalStoreAssetStore final : public IAssetStore
{
public:
    GlobalStoreAssetStore(SharedPtr<IndexerConnection> indexer, SharedPtr<GlobalStoreConnection> store,
                          ConversionTarget target);

    void Locate(const Guid &guid, AssetLocateCallback done) override;

    [[nodiscard]] StringView GetName() const override
    {
        return "GlobalStore";
    }

private:
    SharedPtr<IndexerConnection> _indexer;
    SharedPtr<GlobalStoreConnection> _store;
    ConversionTarget _target;
};

/// Subscribes *connection* to the indexer's Asset.Changed and reloads each loaded asset it names (06 §6); if the
/// subscription fails, hot reload stays off.
GLOBALSTOREASSETSTORE_API void EnableHotReload(IndexerConnection &connection);

/// The records of the indexer and its converter workers, as they arrive on its Log.Stream.
MARLIN_DECLARE_LOG_CATEGORY(GLOBALSTOREASSETSTORE_API, LogAssetIndexer, Info, Warning);

/// Subscribes *connection* to the indexer's Log.Stream and logs each record here under LogAssetIndexer, so what the
/// indexer scans and converts shows in this process's console; records below Info are not sent.
GLOBALSTOREASSETSTORE_API void EnableIndexerLog(IndexerConnection &connection);

/// Adds the GlobalStore store to the asset table when the engine runs with a project, connecting to the project's
/// indexer, starting it if none runs, and to the GlobalStore of Engine.GlobalStore; reloads loaded assets the indexer
/// reports stale (Asset.Changed, 06 §6). Linked only outside Shipping.
MCLASS(Config = "Engine.GlobalStoreAssetStore", Dependencies = "NetSubsystem, AssetSubsystem")
class GLOBALSTOREASSETSTORE_API GlobalStoreAssetStoreSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Off to load only from packages, as a packaged development build would.
    MPROPERTY()
    static bool Enabled;
    /// Below every package, so mounted packages win.
    MPROPERTY()
    static int32 Priority;
    /// Starts MarlinAssetIndexer when none serves the project.
    MPROPERTY()
    static bool LaunchIndexer;
    /// Shows the indexer's and converter workers' log records here (LogAssetIndexer).
    MPROPERTY()
    static bool ShowIndexerLog;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// The connection to the indexer; null when assets load only from packages.
    [[nodiscard]] const SharedPtr<IndexerConnection> &GetIndexer() const
    {
        return _indexer;
    }

    /// The connection to GlobalStore; null when assets load only from packages.
    [[nodiscard]] const SharedPtr<GlobalStoreConnection> &GetGlobalStore() const
    {
        return _store;
    }

    [[nodiscard]] const ConversionTarget &GetTarget() const
    {
        return _target;
    }

private:
    SharedPtr<IndexerConnection> _indexer;
    SharedPtr<GlobalStoreConnection> _store;
    ConversionTarget _target;
    AssetStoreId _storeId = 0;
};
} // namespace Marlin
