#include "Asset/Assets.h"

#include "Asset/AssetBlock.h"
#include "Concurrency/Jobs.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformThread.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Reflection/Reflection.h"
#include "String/String.h"
#include "String/StringBuilder.h"

MARLIN_DEFINE_LOG_CATEGORY(LogAsset);

namespace Marlin
{
namespace Private
{
const AssetLinkKind StrongAssetLink = AssetLinkKind::Strong;
const AssetLinkKind SoftAssetLink = AssetLinkKind::Soft;

/// Shared by the table and the lookups in flight, so a store can be removed while it is being asked.
struct StoreBox final : RefCounted
{
    UniquePtr<IAssetStore> Store;
};

struct StoreEntry
{
    AssetStoreId Id = 0;
    int32 Priority = 0;
    SharedPtr<StoreBox> Store;
};

/// The stores in priority order. Never changed once built: adding or removing a store builds a new list, so a load
/// takes one reference to the whole list instead of one per store.
struct StoreList final : RefCounted
{
    List<StoreEntry> Entries;
};

struct Completion
{
    AssetSlot *Slot = nullptr;
    AssetCallback Callback;
};

struct Retired
{
    Asset *Object = nullptr;
    uint8 *Block = nullptr;
};

struct CallbackBox final : RefCounted
{
    explicit CallbackBox(AssetCallback callback) : Callback(Move(callback))
    {
    }

    AssetCallback Callback;
};

struct Watcher
{
    AssetReloadId Id = 0;
    SharedPtr<CallbackBox> Box;
};

/// A run of image bytes a load copies into the object: the fields that are not Transient, adjacent ones merged.
struct CopyRange
{
    uint32 Offset = 0;
    uint32 Size = 0;
};

/// What Initialize works out once per asset type, so a load only looks it up.
struct AssetType
{
    const TypeInfo *Type = nullptr;
    uint64 LayoutHash = 0;
    List<CopyRange> Copies;
};

/// A load whose store search waits for HoldLocates to end.
struct HeldLocate
{
    AssetSlot *Slot = nullptr;
    IoPriority Priority = IoPriority::Streaming;
};

struct AssetTable
{
    Mutex Lock;
    bool Initialized = false;
    /// Between HoldLocates and ReleaseLocates: stores are still being added, so no store is asked yet.
    bool LocatesHeld = false;
    List<HeldLocate> Held;
    HashMap<Guid, AssetSlot *> Slots;
    SharedPtr<StoreList> Stores{new StoreList()};
    AssetStoreId NextStoreId = 1;
    /// Built by Initialize and not changed until Shutdown, which waits for every load: loads read it without the lock.
    HashMap<uint64, AssetType> Types;
    List<AssetSlot *> Releases;
    List<Completion> Completions;
    /// Reloaded slots whose new objects are ready, switched at the start of the next frame.
    List<AssetSlot *> Swaps;
    /// The objects and blocks reloads replaced, freed at the end of the frame.
    List<Retired> RetiredContents;
    HashMap<AssetSlot *, List<Watcher>> Watchers;
    HashMap<AssetReloadId, AssetSlot *> WatcherSlots;
    AssetReloadId NextWatcher = 1;
    Atomic<uint32> InFlight{0};
    bool RecordingLoadOrder = false;
    List<Guid> LoadOrder;
    /// Cycle searches mark the slots they visit with the current epoch instead of filling a set; the stack is reused.
    uint32 VisitEpoch = 0;
    List<AssetSlot *> VisitStack;

    struct CriticalLoad
    {
        AssetLoadStage Stage = AssetLoadStage::Locating;
        String Name;
    };

    /// The IoPriority::Critical loads in flight, and how many were started and finished (Assets::GetCriticalProgress).
    HashMap<AssetSlot *, CriticalLoad> Critical;
    uint32 CriticalRequested = 0;
    uint32 CriticalFinished = 0;

    /// Built on first use and never destroyed.
    static AssetTable &Get()
    {
        alignas(AssetTable) static uint8 storage[sizeof(AssetTable)];
        static AssetTable *table = ConstructAt(reinterpret_cast<AssetTable *>(storage));
        return *table;
    }

    static bool IsDone(const AssetSlot &slot)
    {
        const AssetState state = slot.GetState();
        return state == AssetState::Loaded || state == AssetState::Failed;
    }

    static void SetState(AssetSlot &slot, AssetState state)
    {
        slot._state.Store(static_cast<uint8>(state), MemoryOrder::Release);
    }

    /// True if *from* waits, through slots still loading, on *target*. Called with the lock held. Slots already
    /// marked with the current epoch were searched since it began without reaching *target*, so they are skipped; a
    /// search that finds *target* stops early and leaves its marks incomplete, so it starts a new epoch.
    bool WaitsOn(AssetSlot &from, AssetSlot &target)
    {
        VisitStack.Clear();
        VisitStack.Add(&from);
        while (!VisitStack.IsEmpty())
        {
            AssetSlot *slot = VisitStack.Pop();
            if (slot == &target)
            {
                ++VisitEpoch;
                return true;
            }
            if (slot->_visitEpoch == VisitEpoch)
            {
                continue;
            }
            slot->_visitEpoch = VisitEpoch;
            for (AssetSlot *dependency : slot->_dependencies)
            {
                if (dependency->GetState() == AssetState::Loading && dependency->_visitEpoch != VisitEpoch)
                {
                    VisitStack.Add(dependency);
                }
            }
        }
        return false;
    }

    /// Wakes waiters and queues callbacks of a slot that just finished. Called with the lock held; returns the
    /// dependents that are now ready to finish.
    void Complete(AssetSlot &slot, List<AssetSlot *> &ready)
    {
        if (Critical.Remove(&slot))
        {
            ++CriticalFinished;
        }
        for (Event *waiter : slot._waiters)
        {
            waiter->Set();
        }
        slot._waiters.Clear();
        for (AssetCallback &callback : slot._callbacks)
        {
            Completions.Add({&slot, Move(callback)});
        }
        slot._callbacks.Clear();
        for (AssetSlot *dependent : slot._dependents)
        {
            MARLIN_ASSERT(dependent->_pendingDependencies > 0);
            if (--dependent->_pendingDependencies == 0)
            {
                ready.Add(dependent);
            }
        }
        slot._dependents.Clear();
    }

    void Unreferenced(AssetSlot &slot);
    bool WaitFor(AssetSlot &slot, uint32 timeout);
    void AddCallback(AssetSlot &slot, AssetCallback callback);
    FileRegion BulkOf(const AssetSlot &slot);
    void Collect();

    /// Acquires the slot of each valid Guid in *guids* under one lock, with a reference each.
    void AcquireSlots(Span<const Guid> guids, List<AssetSlot *> &slots)
    {
        slots.Reserve(slots.Count() + guids.Count());
        const ScopeLock lock(Lock);
        for (const Guid &guid : guids)
        {
            if (!guid.IsValid())
            {
                continue;
            }
            AssetSlot *&slot = Slots.FindOrAdd(guid);
            if (slot == nullptr)
            {
                slot = new AssetSlot(guid);
            }
            slot->AddRef();
            slots.Add(slot);
        }
    }

    /// Visits the references in a fixed-up block's arrays, which no destructor releases: a loaded block holds one
    /// reference per entry, taken when it loads and dropped when it is freed.
    static void ForEachDataReference(uint8 *block, void (*visit)(AssetSlot &slot))
    {
        AssetBlockHeader header;
        MemCopy(&header, block, sizeof(header));
        const uint8 *fixups = block + header.FixupOffset;
        for (uint32 i = 0; i < header.FixupCount; ++i)
        {
            uint32 fixup = 0;
            MemCopy(&fixup, fixups + i * sizeof(uint32), sizeof(fixup));
            const uint32 offset = fixup & ~AssetFixupKindMask;
            if ((fixup & AssetFixupKindMask) == static_cast<uint32>(AssetFixupKind::AssetRef) &&
                offset >= header.ImageOffset + header.ImageSize)
            {
                AssetSlot *slot = nullptr;
                MemCopy(&slot, block + offset, sizeof(AssetSlot *));
                visit(*slot);
            }
        }
    }

    /// Frees what a slot holds. Called without the lock; the destructor of the object may release references.
    static void FreeContents(Asset *object, uint8 *block, bool loaded)
    {
        if (object != nullptr)
        {
            object->GetType().GetOperations().Delete(object);
        }
        if (block != nullptr && loaded)
        {
            ForEachDataReference(block, [](AssetSlot &slot) { slot.Release(); });
        }
        Memory::Free(block);
    }

    /// Drops the references a load holds on the slots its block names, once the block holds its own.
    static void ReleaseDependencies(List<AssetSlot *> &dependencies)
    {
        for (AssetSlot *dependency : dependencies)
        {
            dependency->Release();
        }
        dependencies.Clear();
    }
};
} // namespace Private

namespace Private
{
struct AssetLoader
{
    struct ReadJob
    {
        AssetSlot *Slot;
        FileRegion Bulk;
        AsyncReadRequest Request;
    };

    /// Asks the stores in priority order until one has the slot's data.
    struct LocateJob
    {
        AssetSlot *Slot;
        IoPriority Priority;
        SharedPtr<StoreList> Stores;
        usize Next = 0;
    };

    struct PendingStart
    {
        AssetSlot *Slot;
        IoPriority Priority;
    };

    /// The loads the thread is starting, while it starts them: a store that answers at once hands over dependencies
    /// that start loading in turn, and they queue here instead of nesting, however deep the graph.
    static thread_local List<PendingStart> *Starting;

    static void Fail(AssetSlot &slot, StringView reason)
    {
        AssetTable &table = AssetTable::Get();
        List<AssetSlot *> ready;
        Asset *object = nullptr;
        uint8 *block = nullptr;
        List<AssetSlot *> dependencies;
        List<AssetSlot *> preloads;
        bool reloading = false;
        {
            const ScopeLock lock(table.Lock);
            reloading = slot._reloading;
            object = Exchange(slot._loading, nullptr);
            block = Exchange(slot._pendingBlock, nullptr);
            slot._pendingBulk = {};
            slot._reloading = false;
            dependencies = Move(slot._dependencies);
            slot._dependencies.Clear();
            preloads = Move(slot._preloads);
            slot._preloads.Clear();
            if (!reloading)
            {
                AssetTable::SetState(slot, AssetState::Failed);
                table.Complete(slot, ready);
            }
        }
        if (reloading)
        {
            MARLIN_LOG(LogAsset, Warning, "Asset {0} could not be reloaded and keeps its old data: {1}", slot.GetGuid(),
                       reason);
        }
        else
        {
            MARLIN_LOG(LogAsset, Warning, "Asset {0} failed to load: {1}", slot.GetGuid(), reason);
        }
        AssetTable::FreeContents(object, block, false);
        AssetTable::ReleaseDependencies(dependencies);
        AssetTable::ReleaseDependencies(preloads);
        FinishAll(ready);
        EndLoad(slot);
    }

    static void EndLoad(AssetSlot &slot)
    {
        AssetTable::Get().InFlight.FetchSub(1, MemoryOrder::AcquireRelease);
        slot.Release();
    }

    /// Starts loading *slot* if it is unloaded; inside another start on this thread it queues behind it.
    static void Start(AssetSlot &slot, IoPriority priority)
    {
        if (slot.GetState() != AssetState::Unloaded)
        {
            return;
        }
        slot.AddRef();
        if (Starting != nullptr)
        {
            Starting->Add({&slot, priority});
            return;
        }
        List<PendingStart> pending;
        pending.Add({&slot, priority});
        Starting = &pending;
        while (!pending.IsEmpty())
        {
            const PendingStart next = pending.Pop();
            Begin(*next.Slot, next.Priority, false);
            next.Slot->Release();
        }
        Starting = nullptr;
    }

    /// Starts loading *slot*, or with *reload* loading it again into a new object while the old one stays in use.
    static void Begin(AssetSlot &slot, IoPriority priority, bool reload)
    {
        AssetTable &table = AssetTable::Get();
        auto *job = new LocateJob{&slot, priority, {}};
        {
            const ScopeLock lock(table.Lock);
            if (reload ? slot.GetState() != AssetState::Loaded || slot._reloading
                       : slot.GetState() != AssetState::Unloaded)
            {
                delete job;
                return;
            }
            if (!table.Initialized)
            {
                MARLIN_LOG(LogAsset, Error, "Asset {0} cannot load: the asset table is not initialized",
                           slot.GetGuid());
                delete job;
                return;
            }
            if (reload)
            {
                slot._reloading = true;
            }
            else
            {
                AssetTable::SetState(slot, AssetState::Loading);
                if (table.RecordingLoadOrder)
                {
                    table.LoadOrder.Add(slot.GetGuid());
                }
                if (priority == IoPriority::Critical)
                {
                    table.Critical.FindOrAdd(&slot) = {};
                    ++table.CriticalRequested;
                }
            }
            slot._priority = priority;
            slot.AddRef();
            table.InFlight.FetchAdd(1, MemoryOrder::AcquireRelease);
            if (table.LocatesHeld)
            {
                // Asked for before every store is added: the search starts with all of them (ReleaseLocates).
                table.Held.Add({&slot, priority});
                delete job;
                return;
            }
            job->Stores = table.Stores;
        }
        LocateNext(job);
    }

    /// Starts the store searches HoldLocates kept back.
    static void StartHeld(List<HeldLocate> held, const SharedPtr<StoreList> &stores)
    {
        for (const HeldLocate &entry : held)
        {
            LocateNext(new LocateJob{entry.Slot, entry.Priority, stores});
        }
    }

    static void LocateNext(LocateJob *job)
    {
        if (job->Next == job->Stores->Entries.Count())
        {
            AssetSlot &slot = *job->Slot;
            delete job;
            Fail(slot, "no asset store has it");
            return;
        }
        IAssetStore &store = *job->Stores->Entries[job->Next++].Store->Store;
        store.Locate(job->Slot->GetGuid(), [job](const AssetLocation *location) {
            if (location == nullptr)
            {
                LocateNext(job);
                return;
            }
            AssetSlot &slot = *job->Slot;
            const IoPriority priority = job->Priority;
            const AssetLocation found = *location;
            delete job;
            if (found.Deleted)
            {
                Fail(slot, "a package deletes it");
                return;
            }
            if (priority == IoPriority::Critical)
            {
                AssetTable &table = AssetTable::Get();
                const ScopeLock lock(table.Lock);
                if (AssetTable::CriticalLoad *load = table.Critical.Find(&slot))
                {
                    load->Stage = AssetLoadStage::Loading;
                }
            }
            Preload(slot, found.Dependencies, priority);
            Read(slot, priority, found);
        });
    }

    /// Starts the dependencies the store listed while the block is read, so the whole closure a package knows loads
    /// at once instead of one level per block read; *slot* holds them until its block's own list does.
    static void Preload(AssetSlot &slot, Span<const Guid> guids, IoPriority priority)
    {
        if (guids.IsEmpty())
        {
            return;
        }
        List<AssetSlot *> preloads;
        AssetTable::Get().AcquireSlots(guids, preloads);
        for (AssetSlot *dependency : preloads)
        {
            Start(*dependency, priority);
        }
        // Only this load's own steps touch it, one after another.
        slot._preloads = Move(preloads);
    }

    static void Read(AssetSlot &slot, IoPriority priority, const AssetLocation &location)
    {
        auto *job = new ReadJob{&slot, location.Bulk, {}};
        // The request comes from the completion, not the return value: a read served from cached blocks
        // completes before ReadAsync returns.
        (void)Vfs::ReadAsync(location.Block, priority, 0, Vfs::WholeFile, [job](const AsyncReadRequest &request) {
            job->Request = request;
            Jobs::Run(JobDecl{.Entry = &Process, .Argument = job, .Priority = JobPriority::Normal});
        });
    }

    static void Process(void *argument)
    {
        const auto *job = static_cast<const ReadJob *>(argument);
        AssetSlot &slot = *job->Slot;
        const FileRegion bulk = job->Bulk;
        usize size = 0;
        uint8 *block = job->Request.TakeData(size);
        delete job;
        if (block == nullptr)
        {
            Fail(slot, "its block could not be read");
            return;
        }
        StringBuilder<256> error;
        List<AssetSlot *> dependencies;
        const AssetType *type = Prepare(slot, block, size, dependencies, error);
        if (type == nullptr)
        {
            AssetTable::ReleaseDependencies(dependencies);
            Memory::Free(block);
            Fail(slot, error.AsView());
            return;
        }
        AssetBlockHeader header;
        MemCopy(&header, block, sizeof(header));

        void *raw = type->Type->New();
        if (raw == nullptr)
        {
            AssetTable::ReleaseDependencies(dependencies);
            Memory::Free(block);
            Fail(slot, "its type cannot be constructed");
            return;
        }
        auto *object = static_cast<Asset *>(type->Type->CastTo(raw, TypeOf<Asset>()));
        object->_guid = slot.GetGuid();
        const uint8 *image = block + header.ImageOffset;
        for (const CopyRange &range : type->Copies)
        {
            MemCopy(static_cast<uint8 *>(raw) + range.Offset, image + range.Offset, range.Size);
        }
        const uint8 *fixups = block + header.FixupOffset;
        for (uint32 i = 0; i < header.FixupCount; ++i)
        {
            uint32 fixup = 0;
            MemCopy(&fixup, fixups + i * sizeof(uint32), sizeof(fixup));
            const uint32 offset = fixup & ~AssetFixupKindMask;
            if ((fixup & AssetFixupKindMask) == static_cast<uint32>(AssetFixupKind::AssetRef) &&
                offset < header.ImageOffset + header.ImageSize)
            {
                AssetSlot *dependency = nullptr;
                MemCopy(&dependency, image + (offset - header.ImageOffset), sizeof(AssetSlot *));
                dependency->AddRef();
            }
        }

        AssetTable &table = AssetTable::Get();
        List<AssetSlot *> start;
        List<AssetSlot *> preloads;
        bool ready = false;
        {
            const ScopeLock lock(table.Lock);
            slot._pendingBlock = block;
            slot._pendingBulk = bulk;
            slot._loading = object;
            slot._dependencies = Move(dependencies);
            preloads = Move(slot._preloads);
            slot._preloads.Clear();
            ++table.VisitEpoch;
            for (AssetSlot *dependency : slot._dependencies)
            {
                if (dependency->GetState() == AssetState::Unloaded)
                {
                    start.Add(dependency);
                }
                if (AssetTable::IsDone(*dependency))
                {
                    continue;
                }
                if (dependency->GetState() == AssetState::Loading && table.WaitsOn(*dependency, slot))
                {
                    MARLIN_LOG(LogAsset, Debug, "Assets {0} and {1} reference each other", slot.GetGuid(),
                               dependency->GetGuid());
                    continue;
                }
                dependency->_dependents.Add(&slot);
                ++slot._pendingDependencies;
            }
            ready = slot._pendingDependencies == 0;
        }
        AssetTable::ReleaseDependencies(preloads);
        for (AssetSlot *dependency : start)
        {
            Start(*dependency, slot._priority);
        }
        if (ready)
        {
            List<AssetSlot *> slots;
            slots.Add(&slot);
            FinishAll(slots);
        }
    }

    /// Checks the block, resolves its type and dependencies and applies the fixups; null with *error* on failure.
    /// *dependencies* gets the block's dependency slots, with a reference each, even on failure.
    static const AssetType *Prepare(AssetSlot &slot, uint8 *block, usize size, List<AssetSlot *> &dependencies,
                                    StringWriter &error)
    {
        if (!AssetBlock::Validate(Span<const uint8>(block, size), error))
        {
            return nullptr;
        }
        AssetBlockHeader header;
        MemCopy(&header, block, sizeof(header));
        if (header.Object != slot.GetGuid())
        {
            error.Append("the store returned the block of another asset");
            return nullptr;
        }
        if (header.Alignment > DefaultAlignment)
        {
            error.Append("its block needs more alignment than reads give");
            return nullptr;
        }
        const AssetType *type = AssetTable::Get().Types.Find(header.TypeHash);
        if (type == nullptr)
        {
            error.Append("its type is not an asset type of this build");
            return nullptr;
        }
        if (header.LayoutHash != type->LayoutHash || header.ImageSize != type->Type->GetSize())
        {
            error.Append(Format("it was converted for another layout of {0}; convert it again", type->Type->GetName()));
            return nullptr;
        }
        // Valid Guids only, as converters write them; an invalid one would shift every index after it.
        const Span<const Guid> guids(reinterpret_cast<const Guid *>(block + header.DependencyOffset),
                                     header.DependencyCount);
        for (const Guid &guid : guids)
        {
            if (!guid.IsValid())
            {
                error.Append("it names an invalid dependency");
                return nullptr;
            }
        }
        AssetTable::Get().AcquireSlots(guids, dependencies);
        const uint8 *fixups = block + header.FixupOffset;
        for (uint32 i = 0; i < header.FixupCount; ++i)
        {
            uint32 fixup = 0;
            MemCopy(&fixup, fixups + i * sizeof(uint32), sizeof(fixup));
            uint8 *target = block + (fixup & ~AssetFixupKindMask);
            uint64 stored = 0;
            MemCopy(&stored, target, sizeof(stored));
            if ((fixup & AssetFixupKindMask) == static_cast<uint32>(AssetFixupKind::Pointer))
            {
                if (stored < header.DataOffset || stored >= uint64{header.DataOffset} + header.DataSize)
                {
                    error.Append(Format("fixup {0} points outside its data", i));
                    return nullptr;
                }
                const uint8 *const pointer = block + stored;
                MemCopy(target, &pointer, sizeof(uint8 *));
            }
            else
            {
                if (stored >= header.DependencyCount)
                {
                    error.Append(Format("fixup {0} names a missing dependency", i));
                    return nullptr;
                }
                const AssetSlot *const dependency = dependencies[static_cast<usize>(stored)];
                MemCopy(target, &dependency, sizeof(AssetSlot *));
            }
        }
        return type;
    }

    /// Switches reloaded slots to their new objects and runs their watchers; the main thread, at the start of a frame.
    static void ApplySwaps()
    {
        AssetTable &table = AssetTable::Get();
        List<AssetSlot *> swaps;
        {
            const ScopeLock lock(table.Lock);
            swaps = Move(table.Swaps);
            table.Swaps.Clear();
        }
        for (AssetSlot *slot : swaps)
        {
            List<SharedPtr<Private::CallbackBox>> watchers;
            {
                const ScopeLock lock(table.Lock);
                Asset *old = slot->_object.Exchange(Exchange(slot->_loading, nullptr));
                table.RetiredContents.Add({old, slot->_block});
                slot->_block = Exchange(slot->_pendingBlock, nullptr);
                slot->_bulk = slot->_pendingBulk;
                slot->_pendingBulk = {};
                slot->_reloading = false;
                if (const List<Private::Watcher> *watching = table.Watchers.Find(slot))
                {
                    for (const Private::Watcher &watcher : *watching)
                    {
                        watchers.Add(watcher.Box);
                    }
                }
            }
            MARLIN_LOG(LogAsset, Info, "Reloaded asset {0}", slot->GetGuid());
            for (const SharedPtr<Private::CallbackBox> &watcher : watchers)
            {
                watcher->Callback(*slot);
            }
            slot->Release();
        }
    }

    /// Finishes each slot whose dependencies are all done, and then those its completion makes ready.
    static void FinishAll(List<AssetSlot *> &ready)
    {
        AssetTable &table = AssetTable::Get();
        while (!ready.IsEmpty())
        {
            AssetSlot &slot = *ready.Pop();
            // Only this load changes its dependency list; other threads at most read it.
            bool dependencyFailed = false;
            for (const AssetSlot *dependency : slot._dependencies)
            {
                dependencyFailed = dependencyFailed || dependency->GetState() == AssetState::Failed;
            }
            if (dependencyFailed)
            {
                Fail(slot, "an asset it references failed to load");
                continue;
            }
            slot._loading->PostLoad();
            AssetTable::ForEachDataReference(slot._pendingBlock, [](AssetSlot &dependency) { dependency.AddRef(); });
            List<AssetSlot *> dependencies;
            {
                const ScopeLock lock(table.Lock);
                dependencies = Move(slot._dependencies);
                slot._dependencies.Clear();
                if (slot._reloading)
                {
                    slot.AddRef();
                    table.Swaps.Add(&slot);
                }
                else
                {
                    slot._block = Exchange(slot._pendingBlock, nullptr);
                    slot._bulk = slot._pendingBulk;
                    slot._pendingBulk = {};
                    slot._object.Store(Exchange(slot._loading, nullptr), MemoryOrder::Release);
                    AssetTable::SetState(slot, AssetState::Loaded);
                    table.Complete(slot, ready);
                }
            }
            AssetTable::ReleaseDependencies(dependencies);
            EndLoad(slot);
        }
    }
};

thread_local List<AssetLoader::PendingStart> *AssetLoader::Starting = nullptr;

void AssetTable::Unreferenced(AssetSlot &slot)
{
    const ScopeLock lock(Lock);
    if (!slot._queuedForRelease)
    {
        slot._queuedForRelease = true;
        Releases.Add(&slot);
    }
}

bool AssetTable::WaitFor(AssetSlot &slot, uint32 timeout)
{
    Event done(EventReset::Manual);
    {
        const ScopeLock lock(Lock);
        if (IsDone(slot) || slot.GetState() == AssetState::Unloaded)
        {
            return slot.GetState() == AssetState::Loaded;
        }
        if (LocatesHeld)
        {
            // It would wait forever: its search starts once the engine has started (Assets::HoldLocates).
            MARLIN_LOG(LogAsset, Error, "Waiting for asset {0} before the asset stores are open; wait from Start on",
                       slot.GetGuid());
            return false;
        }
        slot._waiters.Add(&done);
    }
    if (!done.Wait(timeout))
    {
        const ScopeLock lock(Lock);
        slot._waiters.Remove(&done);
        return false;
    }
    return slot.GetState() == AssetState::Loaded;
}

void AssetTable::AddCallback(AssetSlot &slot, AssetCallback callback)
{
    slot.AddRef();
    const ScopeLock lock(Lock);
    if (IsDone(slot))
    {
        Completions.Add({&slot, Move(callback)});
    }
    else
    {
        slot._callbacks.Add(Move(callback));
    }
}

FileRegion AssetTable::BulkOf(const AssetSlot &slot)
{
    const ScopeLock lock(Lock);
    return slot._bulk;
}

void AssetTable::Collect()
{
    List<Retired> retired;
    {
        const ScopeLock lock(Lock);
        retired = Move(RetiredContents);
        RetiredContents.Clear();
    }
    for (const Retired &entry : retired)
    {
        FreeContents(entry.Object, entry.Block, true);
    }

    while (true)
    {
        struct Freed
        {
            AssetSlot *Slot;
            Asset *Object;
            uint8 *Block;
        };
        List<Freed> freed;
        {
            const ScopeLock lock(Lock);
            if (Releases.IsEmpty())
            {
                return;
            }
            for (AssetSlot *slot : Releases)
            {
                slot->_queuedForRelease = false;
                if (slot->GetReferenceCount() != 0 || slot->GetState() == AssetState::Loading)
                {
                    continue;
                }
                Slots.Remove(slot->GetGuid());
                freed.Add({slot, slot->_object.Exchange(nullptr), Exchange(slot->_block, nullptr)});
            }
            Releases.Clear();
        }
        for (const Freed &entry : freed)
        {
            FreeContents(entry.Object, entry.Block, true);
            delete entry.Slot;
        }
    }
}

void OnSlotUnreferenced(AssetSlot &slot)
{
    AssetTable::Get().Unreferenced(slot);
}
} // namespace Private

using Private::AssetLoader;
using Private::AssetTable;

namespace Assets
{
void Initialize()
{
    AssetTable &table = AssetTable::Get();
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(TypeOf<Asset>(), types);
    const ScopeLock lock(table.Lock);
    table.Types.Clear();
    for (const TypeInfo *type : types)
    {
        if (type->GetKind() != TypeKind::Class || type->IsAbstract() || type->GetMetadata().Has("Abstract"))
        {
            continue;
        }
        StringBuilder<1024> problems;
        if (!AssetBlock::CheckImageSafe(*type, &problems))
        {
            MARLIN_LOG(LogAsset, Error, "Asset type {0} cannot be loaded; these fields cannot live in a block:\n{1}",
                       type->GetFullName(), problems.AsView());
            continue;
        }
        Private::AssetType entry;
        entry.Type = type;
        entry.LayoutHash = AssetBlock::GetLayoutHash(*type);
        for (const FieldInfo *field : type->GetFields())
        {
            if (!field->GetMetadata().Has("Transient"))
            {
                entry.Copies.Add(
                    {static_cast<uint32>(field->GetOffset()), static_cast<uint32>(field->GetType().GetSize())});
            }
        }
        entry.Copies.Sort([](const Private::CopyRange &a, const Private::CopyRange &b) { return a.Offset < b.Offset; });
        List<Private::CopyRange> merged;
        for (const Private::CopyRange &range : entry.Copies)
        {
            if (!merged.IsEmpty() && merged.Last().Offset + merged.Last().Size == range.Offset)
            {
                merged.Last().Size += range.Size;
            }
            else
            {
                merged.Add(range);
            }
        }
        entry.Copies = Move(merged);
        table.Types.Add(AssetBlock::GetTypeHash(*type), Move(entry));
    }
    table.Initialized = true;
}

void HoldLocates()
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    table.LocatesHeld = true;
}

void ReleaseLocates()
{
    AssetTable &table = AssetTable::Get();
    List<Private::HeldLocate> held;
    SharedPtr<Private::StoreList> stores;
    {
        const ScopeLock lock(table.Lock);
        table.LocatesHeld = false;
        held = Move(table.Held);
        table.Held.Clear();
        stores = table.Stores;
    }
    AssetLoader::StartHeld(Move(held), stores);
}

void Shutdown()
{
    AssetTable &table = AssetTable::Get();
    {
        const ScopeLock lock(table.Lock);
        if (!table.Initialized)
        {
            return;
        }
    }
    ReleaseLocates();
    while (table.InFlight.Load(MemoryOrder::Acquire) != 0)
    {
        PlatformThread::Sleep(1);
    }
    ProcessFrame();
    CollectGarbage();
    const ScopeLock lock(table.Lock);
    if (!table.Slots.IsEmpty())
    {
        StringBuilder<1024> leaked;
        for (const auto &entry : table.Slots)
        {
            leaked.Append(Format("\n  {0} ({1} references)", entry.Key, entry.Value->GetReferenceCount()));
        }
        MARLIN_LOG(LogAsset, Error, "{0} assets are still referenced at shutdown:{1}", table.Slots.Count(),
                   leaked.AsView());
    }
    table.Stores = SharedPtr<Private::StoreList>(new Private::StoreList());
    table.Types.Clear();
    table.Initialized = false;
}

bool IsInitialized()
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    return table.Initialized;
}

void StartRecordingLoadOrder()
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    table.LoadOrder.Clear();
    table.RecordingLoadOrder = true;
}

List<Guid> StopRecordingLoadOrder()
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    table.RecordingLoadOrder = false;
    return Move(table.LoadOrder);
}

AssetStoreId AddStore(UniquePtr<IAssetStore> store, int32 priority)
{
    AssetTable &table = AssetTable::Get();
    const SharedPtr<Private::StoreBox> box(new Private::StoreBox());
    box->Store = Move(store);
    const ScopeLock lock(table.Lock);
    const AssetStoreId id = table.NextStoreId++;
    const SharedPtr<Private::StoreList> stores(new Private::StoreList());
    stores->Entries = table.Stores->Entries;
    usize index = 0;
    while (index < stores->Entries.Count() && stores->Entries[index].Priority >= priority)
    {
        ++index;
    }
    Private::StoreEntry &entry = stores->Entries.Insert(index);
    entry.Id = id;
    entry.Priority = priority;
    entry.Store = box;
    table.Stores = stores;
    return id;
}

void RemoveStore(AssetStoreId id)
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    const SharedPtr<Private::StoreList> stores(new Private::StoreList());
    stores->Entries = table.Stores->Entries;
    stores->Entries.RemoveAll([id](const Private::StoreEntry &entry) { return entry.Id == id; });
    table.Stores = stores;
}

AssetSlot *AcquireSlot(const Guid &guid)
{
    MARLIN_ASSERT(guid.IsValid());
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    AssetSlot *&slot = table.Slots.FindOrAdd(guid);
    if (slot == nullptr)
    {
        slot = new AssetSlot(guid);
    }
    slot->AddRef();
    return slot;
}

AssetSlot *FindSlot(const Guid &guid)
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    AssetSlot *const *slot = table.Slots.Find(guid);
    return slot != nullptr ? *slot : nullptr;
}

void NoteConverting(const Guid &guid, StringView name, AssetLoadStage stage)
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    AssetSlot *const *slot = table.Slots.Find(guid);
    AssetTable::CriticalLoad *load = slot != nullptr ? table.Critical.Find(*slot) : nullptr;
    if (load != nullptr && load->Stage == AssetLoadStage::Locating)
    {
        load->Stage = stage;
        load->Name = String(name);
    }
}

void GetCriticalProgress(CriticalLoadProgress &progress)
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    progress.Requested = table.CriticalRequested;
    progress.Finished = table.CriticalFinished;
    progress.Pending = false;
    for (const auto &entry : table.Critical)
    {
        const bool converting =
            entry.Value.Stage == AssetLoadStage::Converting || entry.Value.Stage == AssetLoadStage::Compiling;
        if (!progress.Pending || converting)
        {
            progress.Pending = true;
            progress.PendingGuid = entry.Key->GetGuid();
            progress.PendingStage = entry.Value.Stage;
            progress.PendingName = entry.Value.Name;
        }
        if (converting)
        {
            break;
        }
    }
}

void Load(AssetSlot &slot, IoPriority priority)
{
    AssetLoader::Start(slot, priority);
}

bool Wait(AssetSlot &slot, uint32 timeout)
{
    return AssetTable::Get().WaitFor(slot, timeout);
}

void OnCompleted(AssetSlot &slot, AssetCallback callback)
{
    AssetTable::Get().AddCallback(slot, Move(callback));
}

FileRegion GetBulk(const AssetSlot &slot)
{
    return AssetTable::Get().BulkOf(slot);
}

void Reload(AssetSlot &slot)
{
    AssetLoader::Begin(slot, IoPriority::Streaming, true);
}

AssetReloadId WatchReload(AssetSlot &slot, AssetCallback callback)
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    const AssetReloadId id = table.NextWatcher++;
    Private::Watcher &watcher = table.Watchers.FindOrAdd(&slot).AddDefaulted();
    watcher.Id = id;
    watcher.Box = SharedPtr<Private::CallbackBox>(new Private::CallbackBox(Move(callback)));
    table.WatcherSlots.Add(id, &slot);
    return id;
}

void UnwatchReload(AssetReloadId id)
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    AssetSlot *const *slot = table.WatcherSlots.Find(id);
    if (slot == nullptr)
    {
        return;
    }
    if (List<Private::Watcher> *watchers = table.Watchers.Find(*slot))
    {
        watchers->RemoveAll([id](const Private::Watcher &watcher) { return watcher.Id == id; });
        if (watchers->IsEmpty())
        {
            table.Watchers.Remove(*slot);
        }
    }
    table.WatcherSlots.Remove(id);
}

void ProcessFrame()
{
    AssetLoader::ApplySwaps();
    AssetTable &table = AssetTable::Get();
    List<Private::Completion> completions;
    {
        const ScopeLock lock(table.Lock);
        completions = Move(table.Completions);
        table.Completions.Clear();
    }
    for (const Private::Completion &completion : completions)
    {
        completion.Callback(*completion.Slot);
        completion.Slot->Release();
    }
}

void CollectGarbage()
{
    AssetTable::Get().Collect();
}

usize GetSlotCount()
{
    AssetTable &table = AssetTable::Get();
    const ScopeLock lock(table.Lock);
    return table.Slots.Count();
}
} // namespace Assets
} // namespace Marlin
