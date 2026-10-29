#pragma once

#include "Asset/Assets.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "GlobalStoreAssetStore/GlobalStoreAssetStore.h"
#include "Memory/SharedPtr.h"
#include "GlobalStoreShaderStore/GlobalStoreShaderStore.gen.h"

namespace Marlin
{
class ShaderLibrary;

/// The development-time store of global shaders (MarlinLanguage.md §1.6): a Guid the ShaderLibrary was asked for is
/// located with the indexer's Shader.Locate by its entry point, which compiles it on a miss, and read from GlobalStore
/// like an asset; other Guids it does not have. Source edits come back as Asset.Changed, which reloads the shader.
class GLOBALSTORESHADERSTORE_API GlobalStoreShaderStore final : public IAssetStore
{
public:
    GlobalStoreShaderStore(SharedPtr<IndexerConnection> indexer, SharedPtr<GlobalStoreConnection> store,
                           ConversionTarget target, const ShaderLibrary &library);

    void Locate(const Guid &guid, AssetLocateCallback done) override;

    [[nodiscard]] StringView GetName() const override
    {
        return "GlobalStore shaders";
    }

private:
    SharedPtr<IndexerConnection> _indexer;
    SharedPtr<GlobalStoreConnection> _store;
    ConversionTarget _target;
    const ShaderLibrary &_library;
};

/// Adds the GlobalStoreShaderStore above the GlobalStore asset store when that store is connected. Linked only
/// outside Shipping.
MCLASS(Dependencies = "GlobalStoreAssetStoreSubsystem, ShaderLibrary")
class GLOBALSTORESHADERSTORE_API GlobalStoreShaderStoreSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

private:
    AssetStoreId _store = 0;
};
} // namespace Marlin
