//! An unordered map and the key-value pair type it stores.
//!
//! `HashMap` hashes keys through `Hasher` and stores entries in an open-addressing table. Iteration visits the
//! entries in an unspecified order.

#pragma once

#include "Containers/HashTable.h"
#include "CoreTypes.h"

namespace Marlin
{
/// A key and the value mapped to it.
///
/// The type is an aggregate, so it supports brace initialization and structured bindings.
template <typename TKey, typename TValue> struct KeyValuePair
{
    /// The key that identifies the entry.
    TKey Key;
    /// The value stored for the key.
    TValue Value;
};

namespace Private
{
/// Adapts a key-value pair to the table's entry-and-key interface.
template <typename TKey, typename TValue> struct HashMapTraits
{
    using Entry = KeyValuePair<TKey, TValue>;
    using Key = TKey;

    static const TKey &KeyOf(const Entry &entry)
    {
        return entry.Key;
    }
};
} // namespace Private

/// Whether a key-value pair can be relocated by copying its bytes; true when both the key and the value can be.
template <typename TKey, typename TValue>
inline constexpr bool IsTriviallyRelocatable<KeyValuePair<TKey, TValue>> =
    IsTriviallyRelocatable<TKey> && IsTriviallyRelocatable<TValue>;

/// An unordered map from keys to values.
///
/// `TKey` is the key type, `TValue` the mapped type and `TAllocator` the allocation policy. Keys are hashed by
/// `Hasher` for the key type, through a specialization or the key's `GetHash()` member, and need `==`; a lookup may
/// take any type that hashes and compares equal to the key type, such as a `StringView` for `String` keys.
///
/// # Thread safety
/// A single map is not synchronized.
///
/// # Performance
/// Lookups average O(1). Insertion rehashes when the table runs out of room, which moves every entry.
///
/// # Examples
///
/// ```cpp
/// HashMap<uint64, int32> counts;
/// counts.Add(1, 10);
/// counts.Set(1, 12);
/// int32 one = *counts.Find(1);     // -> 12
/// int32 two = counts.FindOrAdd(2); // -> 0
/// bool removed = counts.Remove(2); // -> true
/// ```
template <typename TKey, typename TValue, typename TAllocator = HeapAllocator> class HashMap
{
    using Table = Private::HashTable<Private::HashMapTraits<TKey, TValue>, TAllocator>;

public:
    /// The key-value pair type stored in the map.
    using Entry = KeyValuePair<TKey, TValue>;

    /// Creates an empty map.
    HashMap() = default;

    /// Creates an empty map that allocates through `allocator`.
    explicit HashMap(const TAllocator &allocator) : _table(allocator)
    {
    }

    /// Creates a map from `entries`.
    ///
    /// Later entries with a key equal to an earlier one replace its value.
    HashMap(std::initializer_list<Entry> entries)
    {
        Reserve(entries.size());
        for (const Entry &entry : entries)
        {
            Set(entry.Key, entry.Value);
        }
    }

    /// Returns the number of entries.
    [[nodiscard]] usize Count() const
    {
        return _table.Count();
    }

    /// Returns whether there are no entries.
    [[nodiscard]] bool IsEmpty() const
    {
        return _table.IsEmpty();
    }

    /// Grows the capacity so that `count` entries fit without rehashing.
    void Reserve(usize count)
    {
        _table.Reserve(count);
    }

    /// Removes every entry and keeps the capacity.
    void Clear()
    {
        _table.Clear();
    }

    /// Returns the value for `key`, or null when it is absent.
    ///
    /// The pointer is invalidated by an insertion that grows the table.
    template <typename TLookup> [[nodiscard]] TValue *Find(const TLookup &key)
    {
        Entry *entry = _table.Find(key);
        return entry != nullptr ? &entry->Value : nullptr;
    }

    /// Returns the value for `key`, or null when it is absent.
    ///
    /// The pointer is invalidated by an insertion that grows the table.
    template <typename TLookup> [[nodiscard]] const TValue *Find(const TLookup &key) const
    {
        const Entry *entry = _table.Find(key);
        return entry != nullptr ? &entry->Value : nullptr;
    }

    /// Returns whether `key` is present.
    template <typename TLookup> [[nodiscard]] bool Contains(const TLookup &key) const
    {
        return _table.Find(key) != nullptr;
    }

    /// Returns the value for `key`, using `hash` instead of hashing it again.
    ///
    /// # Preconditions
    /// `hash` is what [`Hasher`] gives for `key`. An incorrect hash can miss an entry.
    template <typename TLookup> [[nodiscard]] TValue *FindHashed(const TLookup &key, uint64 hash)
    {
        Entry *entry = _table.FindHashed(key, hash);
        return entry != nullptr ? &entry->Value : nullptr;
    }

    /// Adds `key` with `value` and returns the stored value.
    ///
    /// # Preconditions
    /// `key` is absent. The absence is checked by an assertion in builds with assertions; an existing entry keeps its
    /// value.
    TValue &Add(const TKey &key, TValue value)
    {
        const auto [entry, added] = _table.FindOrInsert(key, key, Move(value));
        MARLIN_ASSERT(added);
        return entry->Value;
    }

    /// Adds `key` with `value`, using `hash` instead of hashing it again, and returns the stored value.
    ///
    /// # Preconditions
    /// `key` is absent and `hash` is what [`Hasher`] gives for it. The absence is checked by an assertion in builds
    /// with assertions; an existing entry keeps its value.
    TValue &AddHashed(const TKey &key, uint64 hash, TValue value)
    {
        const auto [entry, added] = _table.FindOrInsertHashed(key, hash, key, Move(value));
        MARLIN_ASSERT(added);
        return entry->Value;
    }

    /// Adds `key` with `value` unless it is present; returns whether it was added.
    bool TryAdd(const TKey &key, TValue value)
    {
        return _table.FindOrInsert(key, key, Move(value)).Second;
    }

    /// Adds `key` or replaces its value; returns the stored value.
    TValue &Set(const TKey &key, TValue value)
    {
        const auto [entry, added] = _table.FindOrInsert(key, key, Move(value));
        if (!added)
        {
            entry->Value = Move(value);
        }
        return entry->Value;
    }

    /// Returns the value for `key`, adding a value-initialized one when it is absent.
    TValue &FindOrAdd(const TKey &key)
    {
        return _table.FindOrInsert(key, key, TValue()).First->Value;
    }

    /// Returns the value for `key`, adding a value-initialized one when it is absent.
    TValue &operator[](const TKey &key)
    {
        return FindOrAdd(key);
    }

    /// Removes `key`; returns whether it was present.
    template <typename TLookup> bool Remove(const TLookup &key)
    {
        return _table.Remove(key);
    }

    /// Removes every entry that `predicate` accepts; returns how many were removed.
    ///
    /// `predicate` receives a `const Entry &`.
    template <typename TPredicate> usize RemoveIf(TPredicate predicate)
    {
        return _table.RemoveIf(predicate);
    }

    /// Returns an iterator over the entries, in an unspecified order.
    [[nodiscard]] auto begin()
    {
        return _table.begin();
    }

    /// Returns the end iterator.
    [[nodiscard]] auto end()
    {
        return _table.end();
    }

    /// Returns an iterator over the entries, in an unspecified order.
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
