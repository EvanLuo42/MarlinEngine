//! An unordered set of unique keys.
//!
//! `HashSet` hashes keys through `Hasher` and stores them in an open-addressing table. Iteration visits the
//! keys in an unspecified order.

#pragma once

#include "Containers/HashTable.h"
#include "CoreTypes.h"

namespace Marlin
{
namespace Private
{
/// Adapts a key to the table's entry-and-key interface.
template <typename TKey> struct HashSetTraits
{
    using Entry = TKey;
    using Key = TKey;

    static const TKey &KeyOf(const Entry &entry)
    {
        return entry; // NOLINT(bugprone-return-const-ref-from-parameter)
    }
};
} // namespace Private

/// An unordered set of unique keys.
///
/// `TKey` is the key type and `TAllocator` the allocation policy. Keys are hashed by `Hasher` for the key type,
/// through a specialization or the key's `GetHash()` member, and need `==`; a lookup may take any type that hashes
/// and compares equal to the key type.
///
/// # Thread safety
/// A single set is not synchronized.
///
/// # Performance
/// Lookups average O(1). Insertion rehashes when the table runs out of room, which moves every key.
///
/// # Examples
///
/// ```cpp
/// HashSet<uint64> ids = {1, 2, 3};
/// bool added = ids.Add(2);        // -> false
/// bool present = ids.Contains(2); // -> true
/// bool removed = ids.Remove(2);   // -> true
/// ```
template <typename TKey, typename TAllocator = HeapAllocator> class HashSet
{
    using Table = Private::HashTable<Private::HashSetTraits<TKey>, TAllocator>;

public:
    /// Creates an empty set.
    HashSet() = default;

    /// Creates an empty set that allocates through `allocator`.
    explicit HashSet(const TAllocator &allocator) : _table(allocator)
    {
    }

    /// Creates a set from `keys`; duplicates collapse.
    HashSet(std::initializer_list<TKey> keys)
    {
        Reserve(keys.size());
        for (const TKey &key : keys)
        {
            Add(key);
        }
    }

    /// Returns the number of keys.
    [[nodiscard]] usize Count() const
    {
        return _table.Count();
    }

    /// Returns whether there are no keys.
    [[nodiscard]] bool IsEmpty() const
    {
        return _table.IsEmpty();
    }

    /// Grows the capacity so that `count` keys fit without rehashing.
    void Reserve(usize count)
    {
        _table.Reserve(count);
    }

    /// Removes every key and keeps the capacity.
    void Clear()
    {
        _table.Clear();
    }

    /// Adds `key`; returns whether it was added.
    bool Add(const TKey &key)
    {
        return _table.FindOrInsert(key, key).Second;
    }

    /// Adds `key` by moving it; returns whether it was added.
    ///
    /// `key` is moved only when it is actually added.
    bool Add(TKey &&key)
    {
        const TKey &lookup = key;
        return _table.FindOrInsert(lookup, Move(key)).Second;
    }

    /// Returns whether `key` is present.
    template <typename TLookup> [[nodiscard]] bool Contains(const TLookup &key) const
    {
        return _table.Find(key) != nullptr;
    }

    /// Returns the stored key equal to `key`, or null when it is absent.
    ///
    /// The pointer is invalidated by an insertion that grows the table.
    template <typename TLookup> [[nodiscard]] const TKey *Find(const TLookup &key) const
    {
        return _table.Find(key);
    }

    /// Removes `key`; returns whether it was present.
    template <typename TLookup> bool Remove(const TLookup &key)
    {
        return _table.Remove(key);
    }

    /// Removes every key that `predicate` accepts; returns how many were removed.
    ///
    /// `predicate` receives a `const TKey &`.
    template <typename TPredicate> usize RemoveIf(TPredicate predicate)
    {
        return _table.RemoveIf(predicate);
    }

    /// Returns an iterator over the keys, in an unspecified order.
    [[nodiscard]] auto begin() const
    {
        return _table.begin();
    }

    /// Returns the end iterator.
    [[nodiscard]] auto end() const
    {
        return _table.end();
    }

private:
    Table _table;
};
} // namespace Marlin
