#pragma once

#include "Asset/Asset.h"
#include "Concurrency/Atomic.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Misc/Guid.h"
#include "Reflection/TypeOf.h"
#include "Templates/Function.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
class AssetSlot;

enum class AssetState : uint8
{
    Unloaded,
    Loading,
    Loaded,
    Failed,
};

using AssetCallback = Function<void(AssetSlot &slot)>;

namespace Private
{
struct AssetLoader;
struct AssetTable;

ASSET_API void OnSlotUnreferenced(AssetSlot &slot);
} // namespace Private

/// One Guid's entry in the asset table. References point at the slot rather than the object, so a reload swaps the
/// object under all of them. The table owns slots and frees one at a safe point once nothing references it.
class ASSET_API AssetSlot
{
public:
    explicit AssetSlot(const Guid &guid) : _guid(guid)
    {
    }
    ~AssetSlot() = default;
    MARLIN_NONCOPYABLE(AssetSlot);

    [[nodiscard]] const Guid &GetGuid() const
    {
        return _guid;
    }

    [[nodiscard]] AssetState GetState() const
    {
        return static_cast<AssetState>(_state.Load(MemoryOrder::Acquire));
    }

    /// The object once it has loaded, else null.
    [[nodiscard]] Asset *GetObject() const
    {
        return _object.Load(MemoryOrder::Acquire);
    }

    [[nodiscard]] uint32 GetReferenceCount() const
    {
        return _references.Load(MemoryOrder::Relaxed);
    }

    void AddRef()
    {
        _references.FetchAdd(1, MemoryOrder::Relaxed);
    }

    void Release()
    {
        if (_references.FetchSub(1, MemoryOrder::AcquireRelease) == 1)
        {
            Private::OnSlotUnreferenced(*this);
        }
    }

private:
    friend struct Private::AssetLoader;
    friend struct Private::AssetTable;

    Guid _guid;
    Atomic<uint32> _references{0};
    Atomic<uint8> _state{static_cast<uint8>(AssetState::Unloaded)};
    Atomic<Asset *> _object{nullptr};
    /// The object while it waits for its dependencies, before it is published.
    Asset *_loading = nullptr;

    // Owned by the table and touched under its lock.
    uint8 *_block = nullptr;
    FileRegion _bulk;
    /// The block and bulk of a load in progress, published with the object when it finishes; while a reload is in
    /// progress the published ones stay in use.
    uint8 *_pendingBlock = nullptr;
    FileRegion _pendingBulk;
    bool _reloading = false;
    IoPriority _priority = IoPriority::Streaming;
    List<AssetSlot *> _dependencies;
    /// The slots a store said the block references, started before the block arrives and held until the block's
    /// own list holds them.
    List<AssetSlot *> _preloads;
    /// The table's search epoch in which this slot was last visited looking for a reference cycle.
    uint32 _visitEpoch = 0;
    List<AssetSlot *> _dependents;
    uint32 _pendingDependencies = 0;
    List<AssetCallback> _callbacks;
    List<Event *> _waiters;
    bool _queuedForRelease = false;
};

namespace Assets
{
/// The slot of *guid*, created unloaded if it is new, with a reference the caller owns.
[[nodiscard]] ASSET_API AssetSlot *AcquireSlot(const Guid &guid);
} // namespace Assets

/// A strong reference: keeps the asset loaded. Converted blocks store it as an index into their dependencies,
/// fixed up to the slot on load. Get is null until the asset has loaded, or if it is not a T.
template <typename T> class AssetRef
{
public:
    AssetRef() = default;

    /// Adopts a reference the caller already holds on *slot*.
    static AssetRef Adopt(AssetSlot *slot)
    {
        AssetRef ref;
        ref._slot = slot;
        return ref;
    }

    /// The asset of *guid*, without loading it; an invalid Guid gives an empty reference.
    static AssetRef FromGuid(const Guid &guid)
    {
        return Adopt(guid.IsValid() ? Assets::AcquireSlot(guid) : nullptr);
    }

    AssetRef(const AssetRef &other) : _slot(other._slot)
    {
        if (_slot != nullptr)
        {
            _slot->AddRef();
        }
    }

    AssetRef(AssetRef &&other) noexcept : _slot(other._slot)
    {
        other._slot = nullptr;
    }

    template <typename TOther>
        requires IsBaseOf<T, TOther>
    AssetRef(const AssetRef<TOther> &other) : AssetRef(Adopt(other.GetSlot()))
    {
        if (_slot != nullptr)
        {
            _slot->AddRef();
        }
    }

    ~AssetRef()
    {
        Reset();
    }

    // Taking the new reference before dropping the old one makes self-assignment safe.
    AssetRef &operator=(const AssetRef &other) // NOLINT(bugprone-unhandled-self-assignment)
    {
        if (other._slot != nullptr)
        {
            other._slot->AddRef();
        }
        Reset();
        _slot = other._slot;
        return *this;
    }

    AssetRef &operator=(AssetRef &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            _slot = other._slot;
            other._slot = nullptr;
        }
        return *this;
    }

    void Reset()
    {
        if (_slot != nullptr)
        {
            AssetSlot *slot = _slot;
            _slot = nullptr;
            slot->Release();
        }
    }

    [[nodiscard]] T *Get() const
    {
        Asset *object = _slot != nullptr ? _slot->GetObject() : nullptr;
        if (object == nullptr)
        {
            return nullptr;
        }
        // Most references name the exact type, which needs no walk up the hierarchy.
        const TypeInfo &type = object->GetType();
        return &type == &TypeOf<T>() || type.IsA(TypeOf<T>()) ? static_cast<T *>(object) : nullptr;
    }

    [[nodiscard]] T *operator->() const
    {
        T *object = Get();
        MARLIN_ASSERT(object != nullptr);
        return object;
    }

    [[nodiscard]] T &operator*() const
    {
        return *operator->();
    }

    /// True if it refers to an asset, loaded or not.
    [[nodiscard]] bool IsValid() const
    {
        return _slot != nullptr;
    }

    [[nodiscard]] bool IsLoaded() const
    {
        return Get() != nullptr;
    }

    [[nodiscard]] AssetState GetState() const
    {
        return _slot != nullptr ? _slot->GetState() : AssetState::Unloaded;
    }

    [[nodiscard]] Guid GetGuid() const
    {
        return _slot != nullptr ? _slot->GetGuid() : Guid();
    }

    [[nodiscard]] AssetSlot *GetSlot() const
    {
        return _slot;
    }

    bool operator==(const AssetRef &other) const
    {
        return _slot == other._slot;
    }

private:
    AssetSlot *_slot = nullptr;
};

/// A weak reference by Guid: never loads by itself. Use it for content loaded on demand or streamed.
template <typename T> class SoftAssetRef
{
public:
    SoftAssetRef() = default;

    explicit SoftAssetRef(const Guid &guid) : _guid(guid)
    {
    }

    SoftAssetRef(const AssetRef<T> &ref) : _guid(ref.GetGuid())
    {
    }

    [[nodiscard]] bool IsValid() const
    {
        return _guid.IsValid();
    }

    [[nodiscard]] const Guid &GetGuid() const
    {
        return _guid;
    }

    /// A strong reference to the asset if it is loaded, else an empty one.
    [[nodiscard]] AssetRef<T> GetIfLoaded() const;

    bool operator==(const SoftAssetRef &) const = default;

private:
    Guid _guid;
};

static_assert(sizeof(AssetRef<Asset>) == sizeof(void *));
static_assert(sizeof(SoftAssetRef<Asset>) == sizeof(Guid));

/// Which reference a TypeKind::ObjectLink type is; its TypeInfo::GetExtension points at one of these.
enum class AssetLinkKind : uint8
{
    Strong,
    Soft,
};

namespace Private
{
ASSET_API extern const AssetLinkKind StrongAssetLink;
ASSET_API extern const AssetLinkKind SoftAssetLink;

template <typename T> struct ContainerType<AssetRef<T>>
{
    using TRef = AssetRef<T>;

    static constexpr LinkOperations Operations{
        .GetGuid = [](const void *link, Guid &guid) { guid = static_cast<const TRef *>(link)->GetGuid(); },
        .SetGuid = [](void *link, const Guid &guid) { *static_cast<TRef *>(link) = TRef::FromGuid(guid); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TRef>("AssetRef", TypeKind::ObjectLink);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        description.Extension = &StrongAssetLink;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TRef, ContainerType>();
    }
};

template <typename T> struct ContainerType<SoftAssetRef<T>>
{
    using TRef = SoftAssetRef<T>;

    static constexpr LinkOperations Operations{
        .GetGuid = [](const void *link, Guid &guid) { guid = static_cast<const TRef *>(link)->GetGuid(); },
        .SetGuid = [](void *link, const Guid &guid) { *static_cast<TRef *>(link) = TRef(guid); },
    };

    static constexpr TypeDescription Describe()
    {
        TypeDescription description = DescribeContainer<TRef>("SoftAssetRef", TypeKind::ObjectLink);
        description.Element = &TypeOf<T>;
        description.ContainerOperations = &Operations;
        description.Extension = &SoftAssetLink;
        return description;
    }

    static const TypeInfo &Get()
    {
        return InternContainer<TRef, ContainerType>();
    }
};
} // namespace Private
} // namespace Marlin
