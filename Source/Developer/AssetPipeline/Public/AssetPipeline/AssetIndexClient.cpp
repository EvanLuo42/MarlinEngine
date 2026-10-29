#include "AssetPipeline/AssetIndexClient.h"

#include "AssetPipeline/AssetIndexService.h"

#include "Engine/Engine.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonSerializer.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
uint64 Milliseconds()
{
    return PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond();
}

JsonValue GuidParams(const Guid &uuid)
{
    StringBuilder<40> text;
    uuid.FormatTo(text, {});
    JsonValue params = JsonValue::MakeObject();
    params.Set("Uuid", JsonValue::MakeString(text.AsView()));
    return params;
}
} // namespace

bool AssetIndexClient::Connect(StringView projectRoot, StringView projectFile, bool launch, uint32 timeout)
{
    if (!AssetIndexer::Connect(_remote, projectRoot, projectFile, launch, timeout))
    {
        return false;
    }
    (void)_reader.Open(_remote.GetPipeName());
    return true;
}

bool AssetIndexClient::ConnectPipe(StringView pipe, uint32 timeout)
{
    _reader.Close();
    if (!_remote.ConnectPipe(pipe, timeout))
    {
        return false;
    }
    (void)_reader.Open(pipe);
    return true;
}

void AssetIndexClient::Disconnect()
{
    _reader.Close();
    _remote.Disconnect();
}

bool AssetIndexClient::ReadShared(FunctionRef<void(const AssetIndexView &view)> visitor)
{
    if (!_reader.IsOpen() && (_remote.GetPipeName().IsEmpty() || !_reader.Open(_remote.GetPipeName())))
    {
        return false;
    }
    return _reader.Read(visitor);
}

bool AssetIndexClient::GetCanonicalHash(const Guid &uuid, Hash128 &hash)
{
    bool found = false;
    if (ReadShared([&](const AssetIndexView &view) { found = view.GetCanonicalHash(uuid, hash); }))
    {
        return found;
    }
    AssetHashResult result;
    if (!Call("AssetIndex.CanonicalHash", &uuid, result))
    {
        return false;
    }
    hash.Low = result.Low;
    hash.High = result.High;
    return true;
}

bool AssetIndexClient::GetBinaryHash(const Guid &uuid, Hash128 &hash)
{
    bool found = false;
    if (ReadShared([&](const AssetIndexView &view) { found = view.GetBinaryHash(uuid, hash); }))
    {
        return found;
    }
    AssetHashResult result;
    if (!Call("AssetIndex.BinaryHash", &uuid, result))
    {
        return false;
    }
    hash.Low = result.Low;
    hash.High = result.High;
    return true;
}

template <typename TResult> bool AssetIndexClient::Call(StringView method, const Guid *uuid, TResult &result)
{
    RemoteResponse response;
    if (!_remote.CallSync(method, uuid != nullptr ? GuidParams(*uuid) : JsonValue(), response))
    {
        return false;
    }
    StringBuilder<256> error;
    return Json::Read(response.Result, result, error);
}

bool AssetIndexClient::Find(const Guid &uuid, AssetObjectInfo &info)
{
    bool found = false;
    if (ReadShared([&](const AssetIndexView &view) { found = view.Find(uuid, info); }))
    {
        return found;
    }
    return Call("AssetIndex.Find", &uuid, info);
}

bool AssetIndexClient::FindBinary(const Guid &uuid, AssetBinaryInfo &info)
{
    bool found = false;
    if (ReadShared([&](const AssetIndexView &view) { found = view.FindBinary(uuid, info); }))
    {
        return found;
    }
    return Call("AssetIndex.FindBinary", &uuid, info);
}

bool AssetIndexClient::FindBinaryAt(StringView path, AssetBinaryInfo &info)
{
    bool found = false;
    if (ReadShared([&](const AssetIndexView &view) { found = view.FindBinaryAt(path, info); }))
    {
        return found;
    }
    JsonValue params = JsonValue::MakeObject();
    params.Set("Path", JsonValue::MakeString(path));
    RemoteResponse response;
    StringBuilder<256> error;
    return _remote.CallSync("AssetIndex.FindBinaryAt", params, response) && Json::Read(response.Result, info, error);
}

bool AssetIndexClient::Query(const AssetQuery &query, List<AssetObjectInfo> &results)
{
    if (ReadShared([&](const AssetIndexView &view) { view.Query(query, results); }))
    {
        return true;
    }
    StringBuilder<512> text;
    JsonWriter writer(text);
    Json::Write(writer, query);
    JsonValue params;
    JsonError parseError;
    RemoteResponse response;
    StringBuilder<256> error;
    return Json::Parse(text.AsView(), params, parseError) && _remote.CallSync("AssetIndex.Query", params, response) &&
           Json::Read(response.Result, results, error);
}

bool AssetIndexClient::GetReferences(const Guid &uuid, AssetLinks &links)
{
    if (ReadShared([&](const AssetIndexView &view) { view.GetReferences(uuid, links.Objects, links.Binaries); }))
    {
        return true;
    }
    return Call("AssetIndex.References", &uuid, links);
}

bool AssetIndexClient::GetReferencers(const Guid &uuid, List<AssetObjectInfo> &results)
{
    if (ReadShared([&](const AssetIndexView &view) { view.GetReferencers(uuid, results); }))
    {
        return true;
    }
    return Call("AssetIndex.Referencers", &uuid, results);
}

bool AssetIndexClient::GetDependencies(const Guid &uuid, AssetLinks &links)
{
    if (ReadShared([&](const AssetIndexView &view) { view.GetDependencies(uuid, links.Objects, links.Binaries); }))
    {
        return true;
    }
    return Call("AssetIndex.Dependencies", &uuid, links);
}

bool AssetIndexClient::GetStatus(AssetIndexStatus &status)
{
    return Call("AssetIndex.Status", nullptr, status);
}

bool AssetIndexClient::Rescan()
{
    RemoteResponse response;
    return _remote.CallSync("AssetIndex.Rescan", JsonValue(), response);
}

bool AssetIndexClient::WaitUntilReady(uint32 timeout)
{
    const uint64 deadline = Milliseconds() + timeout;
    AssetIndexStatus status;
    while (GetStatus(status))
    {
        if (!status.Scanning)
        {
            if (!_reader.IsOpen() && !_remote.GetPipeName().IsEmpty())
            {
                (void)_reader.Open(_remote.GetPipeName());
            }
            return true;
        }
        if (Milliseconds() >= deadline)
        {
            return false;
        }
        PlatformThread::Sleep(20);
    }
    return false;
}
} // namespace Marlin
