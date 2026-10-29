//! A map stored as a sorted array.
//!
//! `SortedMap` keeps one `KeyValuePair` per entry in a `List` ordered by key, so iteration is in key order and
//! a lookup binary-searches the array. Insertion and removal move elements and cost O(n).

#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Templates/Algorithm.h"

namespace Marlin
{
/// A map kept as a sorted array: compact, iterates in key order, binary-searches lookups; inserts are O(n).
///
/// `TKey` is the key type, `TValue` the mapped type, `TLess` the ordering and `TAllocator` the allocation policy.
/// Two keys are treated as equal when neither is less than the other. The allocator policy is copied into the map.
///
/// # Thread safety
/// A single map is not synchronized.
///
/// # Performance
/// Lookups are O(log n) and allocation-free. Insertion and removal move every later entry, so O(n) worst case.
///
/// # Examples
///
/// ```cpp
/// SortedMap<int32, int32> ranks;
/// ranks.Set(20, 1);
/// ranks.Set(10, 2);
/// const int32 *first = ranks.Find(10); // -> 2
/// int32 key = ranks.Entries()[0].Key;  // -> 10
/// ```
template <typename TKey, typename TValue, typename TLess = Less, typename TAllocator = HeapAllocator> class SortedMap
{
public:
    /// The key-value pair type stored in the map.
    using Entry = KeyValuePair<TKey, TValue>;

    /// Creates an empty map.
    SortedMap() = default;

    /// Creates an empty map that allocates through `allocator`.
    explicit SortedMap(const TAllocator &allocator) : _entries(allocator)
    {
    }

    /// Returns the number of entries.
    [[nodiscard]] usize Count() const
    {
        return _entries.Count();
    }

    /// Returns whether there are no entries.
    [[nodiscard]] bool IsEmpty() const
    {
        return _entries.IsEmpty();
    }

    /// Grows the capacity so that `count` entries fit without allocating.
    void Reserve(usize count)
    {
        _entries.Reserve(count);
    }

    /// Removes every entry and keeps the capacity.
    void Clear()
    {
        _entries.Clear();
    }

    /// Returns the value for `key`, or null when it is absent.
    ///
    /// The pointer is invalidated by any insertion or removal.
    template <typename TLookup> [[nodiscard]] TValue *Find(const TLookup &key)
    {
        const usize index = IndexOf(key);
        return index < _entries.Count() ? &_entries[index].Value : nullptr;
    }

    /// Returns the value for `key`, or null when it is absent.
    ///
    /// The pointer is invalidated by any insertion or removal.
    template <typename TLookup> [[nodiscard]] const TValue *Find(const TLookup &key) const
    {
        const usize index = IndexOf(key);
        return index < _entries.Count() ? &_entries[index].Value : nullptr;
    }

    /// Returns whether `key` is present.
    template <typename TLookup> [[nodiscard]] bool Contains(const TLookup &key) const
    {
        return IndexOf(key) < _entries.Count();
    }

    /// Adds `key` or replaces its value; returns the stored value.
    TValue &Set(const TKey &key, TValue value)
    {
        const usize index = Lower(key);
        if (index < _entries.Count() && !_less(key, _entries[index].Key))
        {
            _entries[index].Value = Move(value);
            return _entries[index].Value;
        }
        return _entries.Insert(index, Entry{key, Move(value)}).Value;
    }

    /// Returns the value for `key`, adding a value-initialized one when it is absent.
    TValue &FindOrAdd(const TKey &key)
    {
        const usize index = Lower(key);
        if (index < _entries.Count() && !_less(key, _entries[index].Key))
        {
            return _entries[index].Value;
        }
        return _entries.Insert(index, Entry{key, TValue()}).Value;
    }

    /// Returns the value for `key`, adding a value-initialized one when it is absent.
    TValue &operator[](const TKey &key)
    {
        return FindOrAdd(key);
    }

    /// Removes `key`; returns whether it was present.
    template <typename TLookup> bool Remove(const TLookup &key)
    {
        const usize index = IndexOf(key);
        if (index >= _entries.Count())
        {
            return false;
        }
        _entries.RemoveAt(index);
        return true;
    }

    /// Returns the entries in key order.
    ///
    /// The span points into the map and is invalidated by any insertion or removal.
    [[nodiscard]] Span<const Entry> Entries() const
    {
        return _entries.AsSpan();
    }

    /// Returns an iterator over the entries, in key order.
    [[nodiscard]] auto begin()
    {
        return _entries.begin();
    }

    /// Returns the end iterator.
    [[nodiscard]] auto end()
    {
        return _entries.end();
    }

    /// Returns an iterator over the entries, in key order.
    [[nodiscard]] auto begin() const
    {
        return _entries.begin();
    }

    /// Returns the end iterator.
    [[nodiscard]] auto end() const
    {
        return _entries.end();
    }

private:
    template <typename TLookup> [[nodiscard]] usize Lower(const TLookup &key) const
    {
        return LowerBound(_entries.Data(), _entries.Count(), key,
                          [this](const Entry &entry, const TLookup &value) { return _less(entry.Key, value); });
    }

    /// Count() when absent.
    template <typename TLookup> [[nodiscard]] usize IndexOf(const TLookup &key) const
    {
        const usize index = Lower(key);
        return index < _entries.Count() && !_less(key, _entries[index].Key) ? index : _entries.Count();
    }

    List<Entry, TAllocator> _entries;
    MARLIN_NO_UNIQUE_ADDRESS TLess _less;
};
} // namespace Marlin
