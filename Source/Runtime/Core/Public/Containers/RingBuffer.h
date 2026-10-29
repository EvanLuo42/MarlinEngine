//! A double-ended queue backed by a circular buffer.
//!
//! `RingBuffer` keeps its capacity at a power of two, so wrapped indices are a mask. Pushing at either end is
//! O(1) amortized; the elements are contiguous only logically.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Allocator.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// A growable double-ended queue in one circular buffer whose capacity is a power of two.
///
/// `T` is the element type and `TAllocator` the allocation policy. Elements are indexed from the front, so index 0
/// is the element that [`PopFront`] returns next. The buffer grows by doubling and never shrinks. The allocator
/// policy is copied into the buffer and does not extend the lifetime of any storage it refers to.
///
/// # Thread safety
/// A single buffer is not synchronized.
///
/// # Performance
/// `PushBack`, `PushFront`, `PopFront` and `PopBack` are O(1) amortized. Indexing is O(1).
///
/// # Examples
///
/// ```cpp
/// RingBuffer<int32> queue;
/// queue.PushBack(1);
/// queue.PushFront(0);
/// int32 front = queue.Front();     // -> 0
/// int32 back = queue.Back();       // -> 1
/// int32 popped = queue.PopFront(); // -> 0
/// ```
template <typename T, typename TAllocator = HeapAllocator> class RingBuffer
{
    using Storage = TAllocator::template ForType<T>;

public:
    /// Creates an empty buffer.
    RingBuffer() = default;

    /// Creates an empty buffer that allocates through `allocator`.
    explicit RingBuffer(const TAllocator &allocator) : _storage(allocator)
    {
    }

    /// Copies the elements of `other`, front first.
    RingBuffer(const RingBuffer &other)
    {
        Reserve(other._count);
        for (usize i = 0; i < other._count; ++i)
        {
            PushBack(other[i]);
        }
    }

    /// Takes the storage of `other` and leaves it empty.
    ///
    /// Available only when the allocator keeps no inline storage, because a moved buffer must not point into another
    /// object.
    RingBuffer(RingBuffer &&other) noexcept
        requires(Storage::InlineCapacity == 0)
        : _data(Exchange(other._data, nullptr)), _capacity(Exchange(other._capacity, 0)),
          _head(Exchange(other._head, 0)), _count(Exchange(other._count, 0))
    {
    }

    /// Destroys the elements and frees the buffer.
    ~RingBuffer()
    {
        Clear();
        if (_data != nullptr)
        {
            _storage.Free(_data, _capacity);
        }
    }

    /// Replaces the elements with a copy of `other`, front first.
    RingBuffer &operator=(const RingBuffer &other)
    {
        if (this != &other)
        {
            Clear();
            Reserve(other._count);
            for (usize i = 0; i < other._count; ++i)
            {
                PushBack(other[i]);
            }
        }
        return *this;
    }

    /// Destroys the current elements, then takes the storage of `other` and leaves it empty.
    ///
    /// Available only when the allocator keeps no inline storage.
    RingBuffer &operator=(RingBuffer &&other) noexcept
        requires(Storage::InlineCapacity == 0)
    {
        if (this != &other)
        {
            this->~RingBuffer();
            ConstructAt(this, Move(other));
        }
        return *this;
    }

    /// Returns the number of elements.
    [[nodiscard]] usize Count() const
    {
        return _count;
    }

    /// Returns whether there are no elements.
    [[nodiscard]] bool IsEmpty() const
    {
        return _count == 0;
    }

    /// Returns how many elements the buffer can hold without allocating.
    [[nodiscard]] usize Capacity() const
    {
        return _capacity;
    }

    /// Returns the element at `index`, counting from the front.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] T &operator[](usize index)
    {
        MARLIN_ASSERT(index < _count);
        return _data[(_head + index) & (_capacity - 1)];
    }

    /// Returns the element at `index`, counting from the front.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] const T &operator[](usize index) const
    {
        MARLIN_ASSERT(index < _count);
        return _data[(_head + index) & (_capacity - 1)];
    }

    /// Returns the first element.
    ///
    /// # Preconditions
    /// The buffer is not empty, checked by an assertion in builds with assertions.
    [[nodiscard]] T &Front()
    {
        return (*this)[0];
    }

    /// Returns the last element.
    ///
    /// # Preconditions
    /// The buffer is not empty, checked by an assertion in builds with assertions.
    [[nodiscard]] T &Back()
    {
        return (*this)[_count - 1];
    }

    /// Appends an element constructed from `args` and returns it.
    ///
    /// Growing reallocates the buffer and invalidates references to elements.
    template <typename... TArgs> T &PushBack(TArgs &&...args)
    {
        Grow();
        T *element = ConstructAt(_data + ((_head + _count) & (_capacity - 1)), Forward<TArgs>(args)...);
        ++_count;
        return *element;
    }

    /// Prepends an element constructed from `args` and returns it.
    ///
    /// Growing reallocates the buffer and invalidates references to elements.
    template <typename... TArgs> T &PushFront(TArgs &&...args)
    {
        Grow();
        _head = (_head - 1) & (_capacity - 1);
        T *element = ConstructAt(_data + _head, Forward<TArgs>(args)...);
        ++_count;
        return *element;
    }

    /// Removes and returns the first element.
    ///
    /// # Preconditions
    /// The buffer is not empty, checked by an assertion in builds with assertions.
    T PopFront()
    {
        MARLIN_ASSERT(_count > 0);
        T value = Move(_data[_head]);
        DestroyAt(_data + _head);
        _head = (_head + 1) & (_capacity - 1);
        --_count;
        return value;
    }

    /// Removes and returns the last element.
    ///
    /// # Preconditions
    /// The buffer is not empty, checked by an assertion in builds with assertions.
    T PopBack()
    {
        MARLIN_ASSERT(_count > 0);
        T *slot = _data + ((_head + _count - 1) & (_capacity - 1));
        T value = Move(*slot);
        DestroyAt(slot);
        --_count;
        return value;
    }

    /// Destroys the elements and keeps the capacity.
    void Clear()
    {
        for (usize i = 0; i < _count; ++i)
        {
            DestroyAt(&(*this)[i]);
        }
        _head = 0;
        _count = 0;
    }

    /// Grows the capacity to at least `capacity`, rounded up to a power of two.
    void Reserve(usize capacity)
    {
        if (capacity > _capacity)
        {
            Reallocate(RoundUpToPowerOfTwo(capacity));
        }
    }

private:
    void Grow()
    {
        if (_count == _capacity)
        {
            Reallocate(_capacity == 0 ? 8 : _capacity * 2);
        }
    }

    void Reallocate(usize capacity)
    {
        T *data = _storage.Allocate(capacity);
        for (usize i = 0; i < _count; ++i)
        {
            T &element = (*this)[i];
            ConstructAt(data + i, Move(element));
            DestroyAt(&element);
        }
        if (_data != nullptr)
        {
            _storage.Free(_data, _capacity);
        }
        _data = data;
        _capacity = capacity;
        _head = 0;
    }

    MARLIN_NO_UNIQUE_ADDRESS Storage _storage;
    T *_data = nullptr;
    usize _capacity = 0;
    usize _head = 0;
    usize _count = 0;
};
} // namespace Marlin
