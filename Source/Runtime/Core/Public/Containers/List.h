//! A growable, contiguous array with a pluggable allocator.
//!
//! `List` owns one buffer and grows it geometrically, so appending in small steps stays linear. The allocator
//! policy can keep the first elements inside the object, as `InlineList` does.

#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Allocator.h"
#include "Memory/MemoryOps.h"
#include "Templates/Algorithm.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// A growable array. Elements move in bulk when `T` is trivially relocatable.
///
/// `T` is the element type and `TAllocator` the allocation policy. Elements are stored contiguously in one buffer.
/// Adding elements may reallocate the buffer and invalidate pointers and references; removing an element
/// invalidates references to it and to the elements that move. The allocator policy is copied into the list and
/// does not extend the lifetime of any storage it refers to.
///
/// # Thread safety
/// A single list is not synchronized.
///
/// # Performance
/// `Add`, `Emplace` and insertion at the end are amortized O(1). `Insert` and `RemoveAt` in the middle are O(n).
/// `Reserve` grows to the requested capacity; other growth doubles the capacity, or the required size when that
/// is larger.
///
/// # Examples
///
/// ```cpp
/// List<int32> values;
/// values.Add(3);
/// values.Insert(0, 4);
/// int32 first = values.First();    // -> 4
/// bool removed = values.Remove(3); // -> true
/// ```
template <typename T, typename TAllocator = HeapAllocator> class List
{
    using Storage = TAllocator::template ForType<T>;

public:
    /// The index returned by a search that finds nothing.
    static constexpr usize NotFound = ~usize{0};

    /// Creates an empty list with the allocator's inline capacity.
    List() : _data(InitialData()), _capacity(Storage::InlineCapacity)
    {
    }

    /// Creates an empty list that allocates through `allocator`.
    explicit List(const TAllocator &allocator)
        : _storage(allocator), _data(InitialData()), _capacity(Storage::InlineCapacity)
    {
    }

    /// Creates a list holding `values`.
    List(std::initializer_list<T> values) : List()
    {
        AddRange(Span<const T>(values.begin(), values.size()));
    }

    /// Creates a list by copying `values`.
    explicit List(Span<const T> values) : List()
    {
        AddRange(values);
    }

    /// Copies the elements of `other`.
    List(const List &other) : List()
    {
        AddRange(Span<const T>(other._data, other._count));
    }

    /// Takes the elements of `other` and leaves it empty.
    List(List &&other) noexcept : List()
    {
        TakeFrom(other);
    }

    /// Destroys the elements and frees the buffer.
    ~List()
    {
        DestroyRange(_data, _count);
        FreeData();
    }

    /// Replaces the elements with a copy of `other`.
    List &operator=(const List &other)
    {
        if (this != &other)
        {
            Clear();
            AddRange(Span<const T>(other._data, other._count));
        }
        return *this;
    }

    /// Frees the current buffer, then takes the elements of `other` and leaves it empty.
    List &operator=(List &&other) noexcept
    {
        if (this != &other)
        {
            Clear();
            FreeData();
            _data = InitialData();
            _capacity = Storage::InlineCapacity;
            TakeFrom(other);
        }
        return *this;
    }

    /// Returns the number of elements.
    [[nodiscard]] usize Count() const
    {
        return _count;
    }

    /// Returns how many elements the list can hold without allocating.
    [[nodiscard]] usize Capacity() const
    {
        return _capacity;
    }

    /// Returns whether there are no elements.
    [[nodiscard]] bool IsEmpty() const
    {
        return _count == 0;
    }

    /// Returns a pointer to the first element.
    ///
    /// The pointer may be null when the list is empty.
    [[nodiscard]] T *Data()
    {
        return _data;
    }

    /// Returns a pointer to the first element.
    ///
    /// The pointer may be null when the list is empty.
    [[nodiscard]] const T *Data() const
    {
        return _data;
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] T &operator[](usize index)
    {
        MARLIN_ASSERT(index < _count);
        return _data[index];
    }

    /// Returns the element at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] const T &operator[](usize index) const
    {
        MARLIN_ASSERT(index < _count);
        return _data[index];
    }

    /// Returns the first element.
    ///
    /// # Preconditions
    /// The list is not empty, checked by an assertion in builds with assertions.
    [[nodiscard]] T &First()
    {
        MARLIN_ASSERT(_count > 0);
        return _data[0];
    }

    /// Returns the first element.
    ///
    /// # Preconditions
    /// The list is not empty, checked by an assertion in builds with assertions.
    [[nodiscard]] const T &First() const
    {
        MARLIN_ASSERT(_count > 0);
        return _data[0];
    }

    /// Returns the last element.
    ///
    /// # Preconditions
    /// The list is not empty, checked by an assertion in builds with assertions.
    [[nodiscard]] T &Last()
    {
        MARLIN_ASSERT(_count > 0);
        return _data[_count - 1];
    }

    /// Returns the last element.
    ///
    /// # Preconditions
    /// The list is not empty, checked by an assertion in builds with assertions.
    [[nodiscard]] const T &Last() const
    {
        MARLIN_ASSERT(_count > 0);
        return _data[_count - 1];
    }

    /// Returns a view of the elements.
    ///
    /// The view points into the list's buffer and is invalidated when that buffer is replaced.
    [[nodiscard]] Span<T> AsSpan()
    {
        return {_data, _count};
    }

    /// Returns a view of the elements.
    ///
    /// The view points into the list's buffer and is invalidated when that buffer is replaced.
    [[nodiscard]] Span<const T> AsSpan() const
    {
        return {_data, _count};
    }

    /// Converts the list to a view of its elements.
    operator Span<T>()
    {
        return AsSpan();
    }

    /// Converts the list to a view of its elements.
    operator Span<const T>() const
    {
        return AsSpan();
    }

    /// Appends a copy of `value` and returns it.
    T &Add(const T &value)
    {
        return Emplace(value);
    }

    /// Appends `value` by moving it and returns it.
    T &Add(T &&value)
    {
        return Emplace(Move(value));
    }

    /// Appends an element constructed from `args` and returns it.
    template <typename... TArgs> T &Emplace(TArgs &&...args)
    {
        if (_count == _capacity)
        {
            return EmplaceGrow(Forward<TArgs>(args)...);
        }
        T *element = ConstructAt(_data + _count, Forward<TArgs>(args)...);
        ++_count;
        return *element;
    }

    /// Appends a value-initialized element and returns it.
    T &AddDefaulted()
    {
        return Emplace();
    }

    /// Appends a copy of every element in `values`.
    void AddRange(Span<const T> values)
    {
        ReserveForGrowth(_count + values.Count());
        CopyConstructRange(_data + _count, values.Data(), values.Count());
        _count += values.Count();
    }

    /// Inserts an element constructed from `args` at `index` and returns it.
    ///
    /// # Preconditions
    /// `index <= Count()`, checked by an assertion in builds with assertions.
    ///
    /// # Performance
    /// O(n) below the end because the following elements move.
    template <typename... TArgs> T &Insert(usize index, TArgs &&...args)
    {
        MARLIN_ASSERT(index <= _count);
        if (index == _count)
        {
            return Emplace(Forward<TArgs>(args)...);
        }
        if constexpr (IsTriviallyRelocatable<T>)
        {
            alignas(T) uint8 value[sizeof(T)];
            ConstructAt(reinterpret_cast<T *>(value), Forward<TArgs>(args)...);
            if (_count == _capacity)
            {
                Reallocate(GrowCapacity(_count + 1));
            }
            MemMove(_data + index + 1, _data + index, (_count - index) * sizeof(T));
            MemCopy(_data + index, value, sizeof(T));
            ++_count;
            return _data[index];
        }
        else
        {
            T value(Forward<TArgs>(args)...);
            ReserveForGrowth(_count + 1);
            ConstructAt(_data + _count, Move(_data[_count - 1]));
            for (usize i = _count - 1; i > index; --i)
            {
                _data[i] = Move(_data[i - 1]);
            }
            _data[index] = Move(value);
            ++_count;
            return _data[index];
        }
    }

    /// Removes `count` elements starting at `index`, keeping the order of the rest.
    ///
    /// # Preconditions
    /// `index + count <= Count()`, checked by an assertion in builds with assertions.
    void RemoveAt(usize index, usize count = 1)
    {
        MARLIN_ASSERT(index + count <= _count);
        if constexpr (IsTriviallyRelocatable<T>)
        {
            DestroyRange(_data + index, count);
            MemMove(_data + index, _data + index + count, (_count - index - count) * sizeof(T));
        }
        else
        {
            for (usize i = index; i + count < _count; ++i)
            {
                _data[i] = Move(_data[i + count]);
            }
            DestroyRange(_data + _count - count, count);
        }
        _count -= count;
    }

    /// Removes the element at `index` by moving the last element into its place.
    ///
    /// O(1), but changes the order.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    void RemoveAtSwap(usize index)
    {
        MARLIN_ASSERT(index < _count);
        if (index != _count - 1)
        {
            _data[index] = Move(_data[_count - 1]);
        }
        DestroyAt(_data + _count - 1);
        --_count;
    }

    /// Removes the first element equal to `value`; returns whether one was removed.
    template <typename TValue> bool Remove(const TValue &value)
    {
        const usize index = IndexOf(value);
        if (index == NotFound)
        {
            return false;
        }
        RemoveAt(index);
        return true;
    }

    /// Removes every element that `predicate` accepts, keeping the order; returns how many were removed.
    ///
    /// `predicate` receives a `const T &`.
    template <typename TPredicate> usize RemoveAll(TPredicate predicate)
    {
        usize kept = 0;
        for (usize i = 0; i < _count; ++i)
        {
            if (!predicate(static_cast<const T &>(_data[i])))
            {
                if (kept != i)
                {
                    _data[kept] = Move(_data[i]);
                }
                ++kept;
            }
        }
        const usize removed = _count - kept;
        DestroyRange(_data + kept, removed);
        _count = kept;
        return removed;
    }

    /// Removes and returns the last element.
    ///
    /// # Preconditions
    /// The list is not empty, checked by an assertion in builds with assertions.
    T Pop()
    {
        MARLIN_ASSERT(_count > 0);
        T value = Move(_data[_count - 1]);
        DestroyAt(_data + _count - 1);
        --_count;
        return value;
    }

    /// Destroys the elements and keeps the capacity.
    void Clear()
    {
        DestroyRange(_data, _count);
        _count = 0;
    }

    /// Grows the capacity to at least `capacity`; does nothing when it already fits.
    void Reserve(usize capacity)
    {
        if (capacity > _capacity)
        {
            Reallocate(capacity);
        }
    }

    /// Resizes to `count` elements, value-initializing new ones.
    ///
    /// Shrinking destroys the trailing elements.
    void Resize(usize count)
    {
        if (count < _count)
        {
            DestroyRange(_data + count, _count - count);
        }
        else
        {
            ReserveForGrowth(count);
            DefaultConstructRange(_data + _count, count - _count);
        }
        _count = count;
    }

    /// Resizes to `count` elements, copying `value` into new ones.
    ///
    /// Shrinking destroys the trailing elements.
    void Resize(usize count, const T &value)
    {
        if (count < _count)
        {
            DestroyRange(_data + count, _count - count);
        }
        else
        {
            ReserveForGrowth(count);
            for (usize i = _count; i < count; ++i)
            {
                ConstructAt(_data + i, value);
            }
        }
        _count = count;
    }

    /// Resizes without constructing or destroying elements.
    ///
    /// Available only when `T` is trivially default-constructible; the new elements have indeterminate values.
    /// Shrinking discards the trailing elements without destroying them.
    void ResizeUninitialized(usize count)
        requires(IsTriviallyDefaultConstructible<T>)
    {
        ReserveForGrowth(count);
        _count = count;
    }

    /// Releases spare capacity, but not below the allocator's inline capacity.
    void ShrinkToFit()
    {
        if (_capacity > _count && _capacity > Storage::InlineCapacity)
        {
            Reallocate(_count);
        }
    }

    /// Returns the index of the first element equal to `value`, or [`NotFound`].
    template <typename TValue> [[nodiscard]] usize IndexOf(const TValue &value) const
    {
        for (usize i = 0; i < _count; ++i)
        {
            if (_data[i] == value)
            {
                return i;
            }
        }
        return NotFound;
    }

    /// Returns the index of the first element that `predicate` accepts, or [`NotFound`].
    template <typename TPredicate> [[nodiscard]] usize FindIndex(TPredicate predicate) const
    {
        for (usize i = 0; i < _count; ++i)
        {
            if (predicate(_data[i]))
            {
                return i;
            }
        }
        return NotFound;
    }

    /// Returns a pointer to the first element that `predicate` accepts, or null.
    template <typename TPredicate> [[nodiscard]] T *Find(TPredicate predicate)
    {
        const usize index = FindIndex(predicate);
        return index == NotFound ? nullptr : _data + index;
    }

    /// Returns whether an element equal to `value` exists.
    template <typename TValue> [[nodiscard]] bool Contains(const TValue &value) const
    {
        return IndexOf(value) != NotFound;
    }

    /// Sorts the elements in place with `less`.
    ///
    /// # Performance
    /// Unstable, O(n log n) worst case.
    template <typename TLess = Less> void Sort(TLess less = {})
    {
        Marlin::Sort(_data, _count, less);
    }

    /// Sorts the elements in place with `less`, keeping equal elements in order.
    ///
    /// # Performance
    /// O(n log n) comparisons and O(n^2) moves in the worst case; it does not allocate.
    template <typename TLess = Less> void StableSort(TLess less = {})
    {
        Marlin::StableSort(_data, _count, less);
    }

    /// Returns a pointer to the first element, for range-based `for`.
    [[nodiscard]] T *begin()
    {
        return _data;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] T *end()
    {
        return _data + _count;
    }

    /// Returns a pointer to the first element, for range-based `for`.
    [[nodiscard]] const T *begin() const
    {
        return _data;
    }

    /// Returns one past the last element, for range-based `for`.
    [[nodiscard]] const T *end() const
    {
        return _data + _count;
    }

    /// Returns whether both lists hold equal elements, in order.
    ///
    /// Available only when `T` supports `==`.
    bool operator==(const List &other) const
        requires(EqualityComparable<T>)
    {
        if (_count != other._count)
        {
            return false;
        }
        for (usize i = 0; i < _count; ++i)
        {
            if (!(_data[i] == other._data[i]))
            {
                return false;
            }
        }
        return true;
    }

private:
    T *InitialData()
    {
        return Storage::InlineCapacity > 0 ? _storage.Allocate(Storage::InlineCapacity) : nullptr;
    }

    void FreeData()
    {
        if (_data != nullptr)
        {
            _storage.Free(_data, _capacity);
        }
    }

    /// Grows geometrically, so that appending in small steps stays linear; Reserve gives exactly what it is asked.
    void ReserveForGrowth(usize required)
    {
        if (required > _capacity)
        {
            Reallocate(GrowCapacity(required));
        }
    }

    [[nodiscard]] usize GrowCapacity(usize required) const
    {
        const usize capacity = _capacity < 4 ? 4 : _capacity * 2;
        return capacity < required ? required : capacity;
    }

    void Reallocate(usize capacity)
    {
        T *data = _storage.Allocate(capacity);
        if (data != _data)
        {
            RelocateRange(data, _data, _count);
            FreeData();
        }
        _data = data;
        _capacity = capacity < Storage::InlineCapacity ? Storage::InlineCapacity : capacity;
    }

    template <typename... TArgs> MARLIN_NOINLINE T &EmplaceGrow(TArgs &&...args)
    {
        const usize capacity = GrowCapacity(_count + 1);
        T *data = _storage.Allocate(capacity);
        T *element = ConstructAt(data + _count, Forward<TArgs>(args)...);
        RelocateRange(data, _data, _count);
        FreeData();
        _data = data;
        _capacity = capacity;
        ++_count;
        return *element;
    }

    void TakeFrom(List &other)
    {
        if (other._data != nullptr && !other._storage.IsInline(other._data))
        {
            _data = other._data;
            _capacity = other._capacity;
            _count = other._count;
            other._data = other.InitialData();
            other._capacity = Storage::InlineCapacity;
            other._count = 0;
            return;
        }
        Reserve(other._count);
        RelocateRange(_data, other._data, other._count);
        _count = other._count;
        other._count = 0;
    }

    MARLIN_NO_UNIQUE_ADDRESS Storage _storage;
    T *_data = nullptr;
    usize _count = 0;
    usize _capacity = 0;
};

/// Lists that allocate on the heap can be relocated by copying their bytes; inline storage would leave dangling
/// pointers.
template <typename T> inline constexpr bool IsTriviallyRelocatable<List<T, HeapAllocator>> = true;

/// A [`List`] that stores its first `TCount` elements inside the object.
template <typename T, usize TCount> using InlineList = List<T, InlineAllocator<TCount>>;
} // namespace Marlin
