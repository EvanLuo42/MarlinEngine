#pragma once

#include "Asset/AssetRef.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "Templates/Function.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
/// Where a converted asset is stored: its block, and its bulk data if it has any.
struct AssetLocation
{
    FileRegion Block;
    FileRegion Bulk;
    /// The store knows the asset was removed, as a patch's tombstone does: the stores below it are not asked.
    bool Deleted = false;
    /// The Guids the block references, if the store knows them without reading it, as a package's dependency table
    /// does: the table starts loading them while the block is read. Empty for stores that do not know; only valid
    /// during the callback. The block's own list stays what references are fixed up from.
    Span<const Guid> Dependencies;
};

/// Gets where an asset is, or null if the store has none.
using AssetLocateCallback = Function<void(const AssetLocation *location)>;

/// Finds converted assets by Guid, such as a package's catalog or the derived data cache. Called from any thread,
/// never with the asset table locked.
class ASSET_API IAssetStore
{
public:
    IAssetStore() = default;
    virtual ~IAssetStore() = default;
    MARLIN_NONCOPYABLE(IAssetStore);

    /// Calls *done* exactly once, at once or later from any thread: a catalog answers inline, a store that must
    /// ask another process, such as the indexer converting on a miss, answers when it hears back.
    virtual void Locate(const Guid &guid, AssetLocateCallback done) = 0;
    /// Names the store in logs.
    [[nodiscard]] virtual StringView GetName() const = 0;
};

using AssetStoreId = uint32;
using AssetReloadId = uint32;

/// Where a load in flight is.
enum class AssetLoadStage : uint8
{
    /// Waiting for a store to say where it is.
    Locating,
    /// A store is converting it first, such as the indexer on a miss.
    Converting,
    /// The same for a shader being compiled.
    Compiling,
    /// Located; its block is being read.
    Loading,
};

/// The IoPriority::Critical loads since the process started, for a splash screen (Documentation/Architecture/09 §4.6):
/// the dependencies of a critical load are critical too.
struct CriticalLoadProgress
{
    uint32 Requested = 0;
    /// Loaded or failed.
    uint32 Finished = 0;
    /// One load still in flight to name, a converting or compiling one first; Pending is false when none is.
    bool Pending = false;
    Guid PendingGuid;
    AssetLoadStage PendingStage = AssetLoadStage::Locating;
    /// What the store called it, such as a file or a shader's entry point; empty if it said nothing.
    String PendingName;
};

/// The asset table: one slot per Guid, loaded from the stores and freed at a safe point once unreferenced
/// (Documentation/Architecture/06 §4.2). AssetSubsystem drives it from the main loop.
namespace Assets
{
/// Checks every asset type's fields and starts accepting loads; types with fields that cannot live in a block are
/// reported and cannot load.
ASSET_API void Initialize();
/// Waits for loads in flight, frees every unreferenced asset and reports the ones still referenced.
ASSET_API void Shutdown();
[[nodiscard]] ASSET_API bool IsInitialized();

/// While the engine initializes, stores are still being added by the subsystems that own them, in whatever order the
/// subsystems initialize: between HoldLocates and ReleaseLocates loads are accepted but no store is asked, and their
/// searches start with every store at ReleaseLocates. AssetSubsystem holds from its Initialize to its Start. Waiting
/// for a held load fails at once instead of blocking.
ASSET_API void HoldLocates();
ASSET_API void ReleaseLocates();

/// Stores are searched from the highest priority down, such as a patch before the base package.
ASSET_API AssetStoreId AddStore(UniquePtr<IAssetStore> store, int32 priority);
ASSET_API void RemoveStore(AssetStoreId id);

/// Starts loading *slot* if it is unloaded, along with everything it references.
ASSET_API void Load(AssetSlot &slot, IoPriority priority = IoPriority::Streaming);

template <typename T> AssetRef<T> Load(const Guid &guid, IoPriority priority = IoPriority::Streaming)
{
    AssetRef<T> ref = AssetRef<T>::FromGuid(guid);
    if (ref.IsValid())
    {
        Load(*ref.GetSlot(), priority);
    }
    return ref;
}

/// Blocks until *slot* has loaded or failed; true if it loaded. Loading screens and tools only.
ASSET_API bool Wait(AssetSlot &slot, uint32 timeout = InfiniteTimeout);

/// Loads and waits; Get on the result is null if loading failed.
template <typename T> AssetRef<T> LoadSync(const Guid &guid)
{
    AssetRef<T> ref = Load<T>(guid, IoPriority::Critical);
    if (ref.IsValid())
    {
        (void)Wait(*ref.GetSlot());
    }
    return ref;
}

/// Calls *callback* on the main thread, in ProcessFrame, once *slot* has loaded or failed.
ASSET_API void OnCompleted(AssetSlot &slot, AssetCallback callback);

/// Loads the converted data of *slot* again, as hot reload does (Documentation/Architecture/06 §6). The old object
/// stays in use while the new one loads; at the start of the next frame after that the slot switches to the new one,
/// so every AssetRef sees it, the reload watchers run, and the old one is freed at the end of that frame. If loading
/// fails the old object stays. Does nothing unless the slot is loaded and not already reloading.
ASSET_API void Reload(AssetSlot &slot);

/// Calls *callback* on the main thread each time *slot* switches to reloaded data, until UnwatchReload.
[[nodiscard]] ASSET_API AssetReloadId WatchReload(AssetSlot &slot, AssetCallback callback);
ASSET_API void UnwatchReload(AssetReloadId id);

/// The slot of *guid* if it exists, without a reference; only valid until the next CollectGarbage.
[[nodiscard]] ASSET_API AssetSlot *FindSlot(const Guid &guid);

/// A store tells the table that *guid*, still locating, is being converted or compiled under *name*; any thread.
ASSET_API void NoteConverting(const Guid &guid, StringView name, AssetLoadStage stage = AssetLoadStage::Converting);
/// Any thread.
ASSET_API void GetCriticalProgress(CriticalLoadProgress &progress);

/// Where the bulk data of a loaded asset is, for the asset to read itself.
[[nodiscard]] ASSET_API FileRegion GetBulk(const AssetSlot &slot);

/// Notes the Guid of each asset as it starts loading, so the packager can lay packages out in load order
/// (Documentation/Architecture/06 §5).
ASSET_API void StartRecordingLoadOrder();
/// The Guids noted since StartRecordingLoadOrder; an asset freed and loaded again appears again.
[[nodiscard]] ASSET_API List<Guid> StopRecordingLoadOrder();

/// Runs completion callbacks; the main loop calls it at the start of each frame.
ASSET_API void ProcessFrame();
/// Frees the assets nothing references any more; the main loop calls it at the end of each frame.
ASSET_API void CollectGarbage();

[[nodiscard]] ASSET_API usize GetSlotCount();
} // namespace Assets

template <typename T> AssetRef<T> SoftAssetRef<T>::GetIfLoaded() const
{
    AssetSlot *slot = _guid.IsValid() ? Assets::FindSlot(_guid) : nullptr;
    if (slot == nullptr || slot->GetState() != AssetState::Loaded)
    {
        return {};
    }
    AssetRef<T> ref = AssetRef<T>::FromGuid(_guid);
    return ref.IsLoaded() ? ref : AssetRef<T>();
}
} // namespace Marlin
