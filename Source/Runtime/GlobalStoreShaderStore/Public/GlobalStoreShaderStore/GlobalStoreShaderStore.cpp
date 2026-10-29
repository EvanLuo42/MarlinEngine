#include "GlobalStoreShaderStore/GlobalStoreShaderStore.h"

#include "Asset/Asset.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "Shader/ShaderLibrary.h"
#include "String/StringBuilder.h"

namespace Marlin
{
GlobalStoreShaderStore::GlobalStoreShaderStore(SharedPtr<IndexerConnection> indexer,
                                               SharedPtr<GlobalStoreConnection> store, ConversionTarget target,
                                               const ShaderLibrary &library)
    : _indexer(Move(indexer)), _store(Move(store)), _target(Move(target)), _library(library)
{
}

void GlobalStoreShaderStore::Locate(const Guid &guid, AssetLocateCallback done)
{
    ShaderLocateParams params;
    if (!_library.FindEntryPoint(guid, params.EntryPoint))
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
        "Shader.Locate", value,
        [store = _store, entryPoint = params.EntryPoint, done = Move(done)](RemoteResponse &response) mutable {
            if (!response.Succeeded)
            {
                MARLIN_LOG(LogShader, Error, "{0} did not compile:\n{1}", entryPoint, response.ErrorMessage);
                done(nullptr);
                return;
            }
            LocateConvertedResult(store, entryPoint.AsView(), response, Move(done));
        });
}

void GlobalStoreShaderStoreSubsystem::Initialize(SubsystemContext &context)
{
    const GlobalStoreAssetStoreSubsystem *assets = context.Collection->Get<GlobalStoreAssetStoreSubsystem>();
    const ShaderLibrary *library = context.Collection->Get<ShaderLibrary>();
    if (assets == nullptr || library == nullptr || !assets->GetIndexer().IsValid() ||
        !assets->GetGlobalStore().IsValid())
    {
        return;
    }
    _store = Assets::AddStore(MakeUnique<GlobalStoreShaderStore>(assets->GetIndexer(), assets->GetGlobalStore(),
                                                                 assets->GetTarget(), *library),
                              GlobalStoreAssetStoreSubsystem::Priority + 1);
}

void GlobalStoreShaderStoreSubsystem::Deinitialize()
{
    if (_store != 0)
    {
        Assets::RemoveStore(_store);
        _store = 0;
    }
}
} // namespace Marlin
