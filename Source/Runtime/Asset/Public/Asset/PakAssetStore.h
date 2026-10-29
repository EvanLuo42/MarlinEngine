#pragma once

#include "Asset/Assets.h"
#include "CoreTypes.h"
#include "Memory/SharedPtr.h"
#include "Vfs/PakFile.h"

namespace Marlin
{
/// A package's catalog as an asset store (Documentation/Architecture/06 §5): a binary search, then a
/// compressed region of the package that the read decompresses, and the dependencies the package lists for it so the
/// table can start them at once. Answers inline.
class ASSET_API PakAssetStore final : public IAssetStore
{
public:
    explicit PakAssetStore(SharedPtr<const PakFile> pak);

    void Locate(const Guid &guid, AssetLocateCallback done) override;

    [[nodiscard]] StringView GetName() const override
    {
        return _pak->GetChunk();
    }

private:
    SharedPtr<const PakFile> _pak;
};
} // namespace Marlin
