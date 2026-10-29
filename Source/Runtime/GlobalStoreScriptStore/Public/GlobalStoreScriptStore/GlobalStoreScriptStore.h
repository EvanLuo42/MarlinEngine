#pragma once

#include "Asset/Assets.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "GlobalStoreAssetStore/GlobalStoreAssetStore.h"
#include "Memory/SharedPtr.h"
#include "GlobalStoreScriptStore/GlobalStoreScriptStore.gen.h"

namespace Marlin
{
/// The development-time store of script modules (MarlinScript §11.1): the Guid of the Engine or Project package's
/// module is located with the indexer's Script.Locate, which compiles the package on a miss, and read from GlobalStore
/// like an asset; other Guids it does not have. Script edits come back as Asset.Changed, which reloads the module.
class GLOBALSTORESCRIPTSTORE_API GlobalStoreScriptStore final : public IAssetStore
{
public:
    GlobalStoreScriptStore(SharedPtr<IndexerConnection> indexer, SharedPtr<GlobalStoreConnection> store,
                           ConversionTarget target);

    void Locate(const Guid &guid, AssetLocateCallback done) override;

    [[nodiscard]] StringView GetName() const override
    {
        return "GlobalStore scripts";
    }

private:
    SharedPtr<IndexerConnection> _indexer;
    SharedPtr<GlobalStoreConnection> _store;
    ConversionTarget _target;
};

/// Adds the GlobalStoreScriptStore above the GlobalStore asset store when that store is connected. Linked only
/// outside Shipping.
MCLASS(Dependencies = "GlobalStoreAssetStoreSubsystem")
class GLOBALSTORESCRIPTSTORE_API GlobalStoreScriptStoreSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

private:
    AssetStoreId _store = 0;
};
} // namespace Marlin
