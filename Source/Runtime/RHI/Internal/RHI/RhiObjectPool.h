#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "Concurrency/SpinLock.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiHandle.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// The objects behind one kind of RHI handle, for backends. Slots live in pages that never move, so looking up a
/// handle takes no lock; allocating and freeing take a spin lock. A freed slot's generation moves on, which makes
/// old handles stale. Free reuses slots last-in first-out, so a resource view's index, which is its slot, comes
/// back only after the backend frees it, once the GPU is done with it.
template <typename T, typename THandle, uint32 TPageSize = 1024> class RhiObjectPool
{
    static_assert((TPageSize & (TPageSize - 1)) == 0, "TPageSize must be a power of two");

public:
    /// At most *capacity* live objects; slot zero is never handed out.
    explicit RhiObjectPool(uint32 capacity) : _pageCount((capacity + TPageSize) / TPageSize)
    {
        _pages = static_cast<Atomic<Page *> *>(Memory::Allocate(sizeof(Atomic<Page *>) * _pageCount));
        for (uint32 i = 0; i < _pageCount; ++i)
        {
            ConstructAt(&_pages[i], nullptr);
        }
        _nextUnused = 1;
    }

    ~RhiObjectPool()
    {
        for (uint32 i = 0; i < _pageCount; ++i)
        {
            Page *page = _pages[i].Load(MemoryOrder::Relaxed);
            if (page == nullptr)
            {
                continue;
            }
            for (Slot &slot : page->Slots)
            {
                if ((slot.Generation.Load(MemoryOrder::Relaxed) & 1) != 0)
                {
                    DestroyAt(slot.Object());
                }
            }
            Memory::Free(page);
        }
        Memory::Free(_pages);
    }

    MARLIN_NONCOPYABLE(RhiObjectPool);

    /// Constructs an object from *args*; an invalid handle when the pool is full.
    template <typename... TArgs> THandle Allocate(TArgs &&...args)
    {
        uint32 index = 0;
        {
            const ScopeLock lock(_lock);
            if (_freeHead != 0)
            {
                index = _freeHead;
                _freeHead = GetSlot(index).NextFree;
            }
            else if (_nextUnused < _pageCount * TPageSize)
            {
                index = _nextUnused++;
                EnsurePage(index / TPageSize);
            }
            else
            {
                return {};
            }
        }
        Slot &slot = GetSlot(index);
        ConstructAt(slot.Object(), Forward<TArgs>(args)...);
        // Odd generations are live; the release store publishes the object to lock-free lookups.
        const uint32 generation = slot.Generation.Load(MemoryOrder::Relaxed) + 1;
        slot.Generation.Store(generation, MemoryOrder::Release);
        _liveCount.FetchAdd(1, MemoryOrder::Relaxed);
        return {index, generation};
    }

    /// The object, or null if *handle* is invalid or stale.
    [[nodiscard]] T *Find(THandle handle) const
    {
        if (handle.Index == 0 || handle.Index >= _pageCount * TPageSize)
        {
            return nullptr;
        }
        Page *page = _pages[handle.Index / TPageSize].Load(MemoryOrder::Acquire);
        if (page == nullptr)
        {
            return nullptr;
        }
        Slot &slot = page->Slots[handle.Index % TPageSize];
        return slot.Generation.Load(MemoryOrder::Acquire) == handle.Generation ? slot.Object() : nullptr;
    }

    /// The object of a handle that must be live.
    [[nodiscard]] T &Get(THandle handle) const
    {
        T *object = Find(handle);
        MARLIN_ASSERT(object != nullptr);
        return *object;
    }

    /// Destroys the object; false if *handle* was already stale.
    bool Free(THandle handle)
    {
        T *object = Find(handle);
        if (object == nullptr)
        {
            return false;
        }
        Slot &slot = GetSlot(handle.Index);
        uint32 expected = handle.Generation;
        if (!slot.Generation.CompareExchange(expected, handle.Generation + 1, MemoryOrder::AcquireRelease))
        {
            return false;
        }
        DestroyAt(object);
        _liveCount.FetchSub(1, MemoryOrder::Relaxed);
        const ScopeLock lock(_lock);
        slot.NextFree = _freeHead;
        _freeHead = handle.Index;
        return true;
    }

    /// Calls *visitor(handle, object)* for every live object; nothing may be allocated or freed meanwhile.
    template <typename TVisitor> void ForEach(TVisitor visitor) const
    {
        for (uint32 index = 1; index < _nextUnused; ++index)
        {
            Slot &slot = GetSlot(index);
            const uint32 generation = slot.Generation.Load(MemoryOrder::Acquire);
            if ((generation & 1) != 0)
            {
                visitor(THandle{index, generation}, *slot.Object());
            }
        }
    }

    [[nodiscard]] uint32 GetLiveCount() const
    {
        return _liveCount.Load(MemoryOrder::Relaxed);
    }

    [[nodiscard]] uint32 GetCapacity() const
    {
        return _pageCount * TPageSize;
    }

private:
    struct Slot
    {
        alignas(T) uint8 Storage[sizeof(T)];
        /// Even while free, odd while live; zero before first use, so no live handle has generation zero.
        Atomic<uint32> Generation{0};
        uint32 NextFree = 0;

        T *Object()
        {
            return reinterpret_cast<T *>(Storage);
        }
    };

    struct Page
    {
        Slot Slots[TPageSize];
    };

    Slot &GetSlot(uint32 index) const
    {
        return _pages[index / TPageSize].Load(MemoryOrder::Acquire)->Slots[index % TPageSize];
    }

    void EnsurePage(uint32 pageIndex)
    {
        if (_pages[pageIndex].Load(MemoryOrder::Relaxed) != nullptr)
        {
            return;
        }
        auto *page = static_cast<Page *>(Memory::Allocate(sizeof(Page), alignof(Page)));
        for (Slot &slot : page->Slots)
        {
            ConstructAt(&slot);
        }
        _pages[pageIndex].Store(page, MemoryOrder::Release);
    }

    Atomic<Page *> *_pages = nullptr;
    uint32 _pageCount;
    SpinLock _lock;
    uint32 _freeHead = 0;
    uint32 _nextUnused = 0;
    Atomic<uint32> _liveCount{0};
};
} // namespace Marlin
