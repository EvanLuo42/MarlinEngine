#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetIndexService.h"
#include "AssetPipeline/AssetIndexShared.h"
#include "CoreTypes.h"
#include "Remote/RemoteClient.h"
#include "String/String.h"

namespace Marlin
{
/// Talks to the project's MarlinAssetIndexer, starting it if none runs (Documentation/Architecture/06 §2). The
/// editor, development games, converters and tools all use it; calls block for their answer.
class ASSETPIPELINE_API AssetIndexClient
{
public:
    explicit AssetIndexClient(NetLoop &loop) : _remote(loop)
    {
    }

    /// Connects to the indexer of the project in *projectRoot*; with *launch*, starts MarlinAssetIndexer from the
    /// executable's folder for *projectFile* if none answers, and waits for it.
    bool Connect(StringView projectRoot, StringView projectFile, bool launch, uint32 timeout = 15000);
    void Disconnect();
    [[nodiscard]] bool IsConnected() const
    {
        return _remote.IsConnected();
    }

    /// Connects to an indexer by its pipe name, without starting one.
    bool ConnectPipe(StringView pipe, uint32 timeout = 200);

    /// Queries read the shared mapping when the indexer publishes one, else they go through the pipe; the mapping
    /// follows the pipe the client is connected to, also when it was connected through GetRemote().
    bool Find(const Guid &uuid, AssetObjectInfo &info);
    bool GetCanonicalHash(const Guid &uuid, Hash128 &hash);
    bool GetBinaryHash(const Guid &uuid, Hash128 &hash);
    [[nodiscard]] bool IsShared() const
    {
        return _reader.IsOpen();
    }
    bool FindBinary(const Guid &uuid, AssetBinaryInfo &info);
    bool FindBinaryAt(StringView path, AssetBinaryInfo &info);
    bool Query(const AssetQuery &query, List<AssetObjectInfo> &results);
    bool GetReferences(const Guid &uuid, AssetLinks &links);
    bool GetReferencers(const Guid &uuid, List<AssetObjectInfo> &results);
    bool GetDependencies(const Guid &uuid, AssetLinks &links);
    bool GetStatus(AssetIndexStatus &status);
    /// Runs *visitor* with the index as the indexer last published it, read in this process; false without a shared
    /// mapping. For walking many files at once, such as an asset browser does.
    bool ReadView(FunctionRef<void(const AssetIndexView &view)> visitor)
    {
        return ReadShared(visitor);
    }
    /// The generation ReadView last saw; a newer one means the index changed.
    [[nodiscard]] uint64 GetViewGeneration() const
    {
        return _reader.GetGeneration();
    }
    bool Rescan();
    /// Blocks until the first full scan has finished.
    bool WaitUntilReady(uint32 timeout);

    /// For calls without a typed wrapper and for AssetIndex.Changed.
    [[nodiscard]] RemoteClient &GetRemote()
    {
        return _remote;
    }

private:
    template <typename TResult> bool Call(StringView method, const Guid *uuid, TResult &result);
    /// Reads the shared mapping, opening it first if the indexer had not published one when this connected.
    bool ReadShared(FunctionRef<void(const AssetIndexView &view)> visitor);

    RemoteClient _remote;
    AssetIndexReader _reader;
};

/// Resolves through the indexer daemon, for converter workers.
class ASSETPIPELINE_API ClientSourceResolver final : public IAssetSourceResolver
{
public:
    explicit ClientSourceResolver(AssetIndexClient &client) : _client(&client)
    {
    }

    bool FindObject(const Guid &uuid, AssetObjectInfo &info) override
    {
        return _client->Find(uuid, info);
    }

    bool FindBinary(const Guid &uuid, AssetBinaryInfo &info) override
    {
        return _client->FindBinary(uuid, info);
    }

    bool FindBinaryAt(StringView path, AssetBinaryInfo &info) override
    {
        return _client->FindBinaryAt(path, info);
    }

    bool GetCanonicalHash(const Guid &uuid, Hash128 &hash) override
    {
        return _client->GetCanonicalHash(uuid, hash);
    }

    bool GetBinaryHash(const Guid &uuid, Hash128 &hash) override
    {
        return _client->GetBinaryHash(uuid, hash);
    }

private:
    AssetIndexClient *_client;
};
} // namespace Marlin
