#include "GlobalStoreScriptStore/GlobalStoreScriptStore.h"

#include "Asset/Asset.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "ScriptVM/ScriptDomain.h"
#include "ScriptVM/ScriptModuleAsset.h"
#include "String/StringBuilder.h"

namespace Marlin
{
GlobalStoreScriptStore::GlobalStoreScriptStore(SharedPtr<IndexerConnection> indexer,
                                               SharedPtr<GlobalStoreConnection> store, ConversionTarget target)
    : _indexer(Move(indexer)), _store(Move(store)), _target(Move(target))
{
}

void GlobalStoreScriptStore::Locate(const Guid &guid, AssetLocateCallback done)
{
    ScriptLocateParams params;
    for (const StringView package : {StringView("Engine"), StringView("Project")})
    {
        if (GetScriptModuleGuid(package) == guid)
        {
            params.Package = String(package);
        }
    }
    if (params.Package.IsEmpty())
    {
        done(nullptr);
        return;
    }
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
    _indexer->Client.CallAsync(
        "Script.Locate", value,
        [store = _store, package = params.Package, done = Move(done)](RemoteResponse &response) mutable {
            if (!response.Succeeded)
            {
                MARLIN_LOG(LogScript, Error, "The {0} scripts do not compile:\n{1}", package, response.ErrorMessage);
                done(nullptr);
                return;
            }
            LocateConvertedResult(store, package.AsView(), response, Move(done));
        });
}

void GlobalStoreScriptStoreSubsystem::Initialize(SubsystemContext &context)
{
    const GlobalStoreAssetStoreSubsystem *assets = context.Collection->Get<GlobalStoreAssetStoreSubsystem>();
    if (assets == nullptr || !assets->GetIndexer().IsValid() || !assets->GetGlobalStore().IsValid())
    {
        return;
    }
    _store = Assets::AddStore(
        MakeUnique<GlobalStoreScriptStore>(assets->GetIndexer(), assets->GetGlobalStore(), assets->GetTarget()),
        GlobalStoreAssetStoreSubsystem::Priority + 1);
}

void GlobalStoreScriptStoreSubsystem::Deinitialize()
{
    if (_store != 0)
    {
        Assets::RemoveStore(_store);
        _store = 0;
    }
}
} // namespace Marlin
