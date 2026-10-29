//! A bounded lock-free queue shared by any number of producers and consumers.
//!
//! The queue allocates its ring once in the constructor and never blocks: `Push` and `Pop` report a full or
//! empty queue instead of waiting.

#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

namespace Marlin
{
/// A bounded multi-producer, multi-consumer queue of trivially copyable values.
///
/// Producers and consumers may run concurrently on any number of threads. `Push` fails when the queue is full and
/// `Pop` fails when it is empty; neither waits. `T` must be trivially copyable, default constructible and copy
/// assignable. The queue is non-copyable and non-movable.
///
/// # Thread safety
/// All operations are safe to call concurrently from any thread. The queue must not be destroyed while another
/// thread is using it.
///
/// # Performance
/// `Push`, `PushRange` and `Pop` are lock-free. `PushRange` claims a batch with one compare-exchange.
///
/// # Examples
///
/// ```cpp
/// MpmcQueue<uint32> queue(64);
/// const bool pushed = queue.Push(7); // -> true
/// uint32 value = 0;
/// const bool popped = queue.Pop(value); // -> true
/// const uint32 read = value; // -> 7
/// ```
template <typename T> class MpmcQueue
{
    static_assert(IsTriviallyCopyable<T>, "MpmcQueue copies values without constructing them");

public:
    /// Creates a queue that can hold at least `capacity` values.
    ///
    /// The capacity is rounded up to a power of two and is at least 2.
    explicit MpmcQueue(usize capacity)
        : _cells(static_cast<Cell *>(Memory::Allocate(RoundedCapacity(capacity) * sizeof(Cell), alignof(Cell)))),
          _mask(RoundedCapacity(capacity) - 1)
    {
        for (usize i = 0; i <= _mask; ++i)
        {
            ConstructAt(&_cells[i]);
            _cells[i].Sequence.Store(i, MemoryOrder::Relaxed);
        }
    }

    /// Destroys the queue and frees its storage.
    ///
    /// # Preconditions
    /// No other thread is using the queue.
    ~MpmcQueue()
    {
        Memory::Free(_cells);
    }

    MARLIN_NONCOPYABLE(MpmcQueue);

    /// Pushes a copy of `value` if there is room.
    ///
    /// Returns true if the value was pushed, false if the queue is full.
    bool Push(const T &value)
    {
        usize position = _tail.Load(MemoryOrder::Relaxed);
        while (true)
        {
            Cell &cell = _cells[position & _mask];
            const usize sequence = cell.Sequence.Load(MemoryOrder::Acquire);
            const auto difference = static_cast<isize>(sequence) - static_cast<isize>(position);
            if (difference == 0)
            {
                if (_tail.CompareExchangeWeak(position, position + 1, MemoryOrder::Relaxed))
                {
                    cell.Value = value;
                    cell.Sequence.Store(position + 1, MemoryOrder::Release);
                    return true;
                }
            }
            else if (difference < 0)
            {
                return false;
            }
            else
            {
                position = _tail.Load(MemoryOrder::Relaxed);
            }
        }
    }

    /// Pushes up to `count` values produced by `make` and returns how many it pushed.
    ///
    /// Calls `make(i)` for each value it pushes, with `i` starting at zero and increasing by one. It claims room
    /// for the batch with a single compare-exchange when it succeeds and pushes a prefix of the requested values, so
    /// it can push fewer than `count` when the queue fills; it returns zero when it pushes nothing.
    ///
    /// # Preconditions
    /// `make` returns the value for index `i`.
    template <typename TMake> usize PushRange(usize count, const TMake &make)
    {
        usize position = _tail.Load(MemoryOrder::Relaxed);
        while (count != 0)
        {
            // A cell is free for its position when its sequence equals it; past the tail no producer has claimed any.
            usize free = 0;
            while (free < count &&
                   _cells[(position + free) & _mask].Sequence.Load(MemoryOrder::Acquire) == position + free)
            {
                ++free;
            }
            if (free == 0)
            {
                const usize sequence = _cells[position & _mask].Sequence.Load(MemoryOrder::Acquire);
                if (static_cast<isize>(sequence) - static_cast<isize>(position) < 0)
                {
                    return 0;
                }
                position = _tail.Load(MemoryOrder::Relaxed);
                continue;
            }
            if (_tail.CompareExchangeWeak(position, position + free, MemoryOrder::Relaxed))
            {
                for (usize i = 0; i < free; ++i)
                {
                    Cell &cell = _cells[(position + i) & _mask];
                    cell.Value = make(i);
                    cell.Sequence.Store(position + i + 1, MemoryOrder::Release);
                }
                return free;
            }
        }
        return 0;
    }

    /// Pops the oldest value into `value` if the queue is not empty.
    ///
    /// Returns true if a value was popped, false if the queue is empty. On failure `value` is unchanged.
    bool Pop(T &value)
    {
        usize position = _head.Load(MemoryOrder::Relaxed);
        while (true)
        {
            Cell &cell = _cells[position & _mask];
            const usize sequence = cell.Sequence.Load(MemoryOrder::Acquire);
            const auto difference = static_cast<isize>(sequence) - static_cast<isize>(position + 1);
            if (difference == 0)
            {
                if (_head.CompareExchangeWeak(position, position + 1, MemoryOrder::Relaxed))
                {
                    value = cell.Value;
                    cell.Sequence.Store(position + _mask + 1, MemoryOrder::Release);
                    return true;
                }
            }
            else if (difference < 0)
            {
                return false;
            }
            else
            {
                position = _head.Load(MemoryOrder::Relaxed);
            }
        }
    }

    /// Returns whether the queue appears empty.
    ///
    /// The result is a snapshot and may be stale as soon as it returns; it is not a synchronization point.
    [[nodiscard]] bool IsEmpty() const
    {
        return _head.Load(MemoryOrder::Relaxed) == _tail.Load(MemoryOrder::Relaxed);
    }

    /// Returns the capacity after rounding up to a power of two.
    [[nodiscard]] usize Capacity() const
    {
        return _mask + 1;
    }

private:
    struct Cell
    {
        Atomic<usize> Sequence;
        T Value{};
    };

    static usize RoundedCapacity(usize capacity)
    {
        return RoundUpToPowerOfTwo(capacity < 2 ? usize{2} : capacity);
    }

    static constexpr usize CacheLine = 64;

    Cell *_cells = nullptr;
    usize _mask = 0;
    alignas(CacheLine) Atomic<usize> _head{0};
    alignas(CacheLine) Atomic<usize> _tail{0};
};
} // namespace Marlin
