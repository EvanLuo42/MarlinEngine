#include "Memory/HeapInternal.h"

#include "Diagnostics/Assert.h"
#include "HAL/PlatformMemory.h"
#include "Memory/MemoryOps.h"

namespace Marlin::Heap
{
SmallPage _emptyPage;
thread_local ThreadHeap *_threadHeap = nullptr;
SpinLock _sharedHeapLock;

namespace
{
thread_local bool _threadExiting = false;
SpinLock _abandonedLock;
ThreadHeap *_abandonedHeaps = nullptr;
ThreadHeap *_sharedHeap = nullptr;

ThreadHeap *NewThreadHeap()
{
    const usize size = AlignUp(sizeof(ThreadHeap), OsPageSize);
    void *memory = PlatformMemory::Reserve(size);
    if (memory == nullptr || !PlatformMemory::Commit(memory, size))
    {
        MARLIN_FATAL("Out of memory for a thread heap");
    }
    _reservedBytes.FetchAdd(size, MemoryOrder::Relaxed);
    _committedBytes.FetchAdd(size, MemoryOrder::Relaxed);
    auto *heap = ConstructAt(static_cast<ThreadHeap *>(memory));
    for (auto &tagPages : heap->Direct)
    {
        for (SmallPage *&page : tagPages)
        {
            page = &_emptyPage;
        }
    }
    return heap;
}

/// Hands the thread's heap to the pool when the thread exits; a later thread adopts it with its pages and pools.
struct ThreadHeapRelease
{
    ThreadHeapRelease() = default;
    ThreadHeapRelease(const ThreadHeapRelease &) = delete;
    ThreadHeapRelease &operator=(const ThreadHeapRelease &) = delete;
    ThreadHeapRelease(ThreadHeapRelease &&) = delete;
    ThreadHeapRelease &operator=(ThreadHeapRelease &&) = delete;

    ~ThreadHeapRelease()
    {
        _threadExiting = true;
        ThreadHeap *heap = _threadHeap;
        if (heap == nullptr)
        {
            return;
        }
        FlushAllStats(heap);
        _threadHeap = nullptr;
        const ScopeLock<SpinLock> lock(_abandonedLock);
        heap->NextAbandoned = _abandonedHeaps;
        _abandonedHeaps = heap;
    }

    void Register()
    {
        _registered = true;
    }

private:
    bool _registered = false;
};

thread_local ThreadHeapRelease _threadHeapRelease;
} // namespace

void PageList::PushFront(SmallPage *page)
{
    page->Previous = nullptr;
    page->Next = Head;
    if (Head != nullptr)
    {
        Head->Previous = page;
    }
    Head = page;
}

void PageList::Remove(SmallPage *page)
{
    if (page->Previous != nullptr)
    {
        page->Previous->Next = page->Next;
    }
    else
    {
        Head = page->Next;
    }
    if (page->Next != nullptr)
    {
        page->Next->Previous = page->Previous;
    }
    page->Next = nullptr;
    page->Previous = nullptr;
}

ThreadHeap *GetOrCreateThreadHeap()
{
    ThreadHeap *heap = _threadHeap;
    if (heap != nullptr || _threadExiting)
    {
        return heap;
    }
    _threadHeapRelease.Register();
    {
        const ScopeLock<SpinLock> lock(_abandonedLock);
        heap = _abandonedHeaps;
        if (heap != nullptr)
        {
            _abandonedHeaps = heap->NextAbandoned;
        }
    }
    if (heap == nullptr)
    {
        heap = NewThreadHeap();
    }
    heap->NextAbandoned = nullptr;
    _threadHeap = heap;
    return heap;
}

ThreadHeap *GetSharedHeap()
{
    if (_sharedHeap == nullptr)
    {
        _sharedHeap = NewThreadHeap();
    }
    return _sharedHeap;
}
} // namespace Marlin::Heap
