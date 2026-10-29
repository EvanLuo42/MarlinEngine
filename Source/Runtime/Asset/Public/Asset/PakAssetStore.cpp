#include "Asset/PakAssetStore.h"

namespace Marlin
{
PakAssetStore::PakAssetStore(SharedPtr<const PakFile> pak) : _pak(Move(pak))
{
}

void PakAssetStore::Locate(const Guid &guid, AssetLocateCallback done)
{
    const PakAssetEntry *entry = _pak->FindAsset(guid);
    if (entry == nullptr)
    {
        done(nullptr);
        return;
    }
    AssetLocation location;
    if (entry->BlockSize == 0)
    {
        location.Deleted = true;
        done(&location);
        return;
    }
    location.Block = _pak->GetRegion(entry->BlockOffset, entry->BlockSize);
    location.Dependencies = _pak->GetDependencies(*entry);
    if (entry->BulkSize != 0)
    {
        location.Bulk = _pak->GetRegion(entry->BulkOffset, entry->BulkSize);
    }
    done(&location);
}
} // namespace Marlin
