//! A dense set of 32-bit keys with a paged sparse index.
//!
//! `SparseSet` keeps the keys densely in a `List` and maps each key to its dense index through pages of 4096
//! slots. Adds, removals and lookups are O(1); iteration follows the dense order.

#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"

namespace Marlin
{
/// A set of 32-bit keys with O(1) add, remove and lookup and densely packed iteration.
///
/// The sparse side is paged, so large, scattered keys cost memory only for the pages they touch. Removing a key
/// moves the last key into its place, so dense indices change and iteration order is not stable. `TAllocator` is
/// the allocation policy for the dense keys and the page table.
///
/// # Thread safety
/// A single set is not synchronized.
///
/// # Performance
/// Add, remove, lookup and contains are O(1). Each touched page allocates 4096 slots (16 KiB) and initializes them
/// on first use. The page table grows to the highest key divided by 4096.
///
/// # Examples
///
/// ```cpp
/// SparseSet<> entities;
/// uint32 first = entities.Add(5);       // -> 0
/// uint32 second = entities.Add(100000); // -> 1
/// uint32 moved = entities.Remove(5);    // -> 0
/// bool has = entities.Contains(100000); // -> true
/// ```
template <typename TAllocator = HeapAllocator> class SparseSet
{
public:
    /// The value returned when a key is not in the set.
    static constexpr uint32 NotFound = ~0u;

    /// Returns the number of keys.
    [[nodiscard]] usize Count() const
    {
        return _dense.Count();
    }

    /// Returns whether there are no keys.
    [[nodiscard]] bool IsEmpty() const
    {
        return _dense.IsEmpty();
    }

    /// Returns whether `key` is in the set.
    [[nodiscard]] bool Contains(uint32 key) const
    {
        return IndexOf(key) != NotFound;
    }

    /// Returns the dense index of `key`, or [`NotFound`] when it is absent.
    ///
    /// Does not allocate and does not change the set.
    [[nodiscard]] uint32 IndexOf(uint32 key) const
    {
        const usize page = key / PageSize;
        if (page >= _pages.Count() || !_pages[page].IsValid())
        {
            return NotFound;
        }
        const uint32 index = (*_pages[page])[key % PageSize];
        return index;
    }

    /// Returns the dense index of `key`, adding it at the end when it is absent.
    ///
    /// The first key added to a page allocates and initializes that page.
    uint32 Add(uint32 key)
    {
        const uint32 existing = IndexOf(key);
        if (existing != NotFound)
        {
            return existing;
        }
        const auto index = static_cast<uint32>(_dense.Count());
        SlotFor(key) = index;
        _dense.Add(key);
        return index;
    }

    /// Removes `key`, moving the last key into its place.
    ///
    /// Returns the dense index that the move filled, or [`NotFound`] when no key moved: either `key` was absent or
    /// it was the last dense key.
    uint32 Remove(uint32 key)
    {
        const uint32 index = IndexOf(key);
        if (index == NotFound)
        {
            return NotFound;
        }
        const uint32 last = _dense.Last();
        _dense.RemoveAtSwap(index);
        SlotFor(key) = NotFound;
        if (last != key)
        {
            SlotFor(last) = index;
            return index;
        }
        return NotFound;
    }

    /// Removes every key and keeps the allocated pages.
    void Clear()
    {
        for (const uint32 key : _dense)
        {
            SlotFor(key) = NotFound;
        }
        _dense.Clear();
    }

    /// Returns the keys in dense order.
    ///
    /// The span points into the set and is invalidated by any insertion or removal.
    [[nodiscard]] Span<const uint32> Keys() const
    {
        return _dense.AsSpan();
    }

    /// Returns a pointer to the first dense key, for range-based `for`.
    [[nodiscard]] const uint32 *begin() const
    {
        return _dense.begin();
    }

    /// Returns one past the last dense key, for range-based `for`.
    [[nodiscard]] const uint32 *end() const
    {
        return _dense.end();
    }

private:
    static constexpr uint32 PageSize = 4096;

    struct Page
    {
        uint32 Slots[PageSize];

        Page()
        {
            for (uint32 &slot : Slots)
            {
                slot = NotFound;
            }
        }

        uint32 &operator[](uint32 index)
        {
            return Slots[index];
        }

        const uint32 &operator[](uint32 index) const
        {
            return Slots[index];
        }
    };

    uint32 &SlotFor(uint32 key)
    {
        const usize page = key / PageSize;
        if (page >= _pages.Count())
        {
            _pages.Resize(page + 1);
        }
        if (!_pages[page].IsValid())
        {
            _pages[page] = MakeUnique<Page>();
        }
        return (*_pages[page])[key % PageSize];
    }

    List<uint32, TAllocator> _dense;
    List<UniquePtr<Page>, TAllocator> _pages;
};
} // namespace Marlin
