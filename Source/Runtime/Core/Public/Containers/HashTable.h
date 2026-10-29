//! The open-addressing hash table underneath `HashMap` and `HashSet`.
//!
//! One control byte per slot holds seven bits of the hash or a marker, and probing compares a group of sixteen
//! control bytes at once. The type is internal to the container library.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Hash/Hash.h"
#include "Math/Simd.h"
#include "Memory/Allocator.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"
#include "Templates/Utility.h"

namespace Marlin::Private
{
/// An open-addressing table in the style of SwissTable: one control byte per slot holds 7 bits of the hash or a
/// marker, and probing tests a group of control bytes at once. `TTraits` gives the entry type and its key.
///
/// Slots and control bytes are allocated in one block, and capacities are powers of two. Live entries stay at or
/// below 7/8 of the slots, and the table rehashes on insertion when it runs out of room.
///
/// # Thread safety
/// A single table is not synchronized.
///
/// # Performance
/// Lookups, insertions and removals average O(1). A rehash moves every entry and allocates a new block.
template <typename TTraits, typename TAllocator> class HashTable
{
public:
    /// The entry type stored in each occupied slot.
    using Entry = TTraits::Entry;
    /// The key type used for hashing and lookup.
    using Key = TTraits::Key;

private:
    using Storage = TAllocator::template ForType<Entry>;

    static constexpr usize GroupWidth = 16;
    static constexpr uint8 Empty = 0x80;
    static constexpr uint8 Deleted = 0xFE;

    /// Sixteen control bytes tested in one vector compare; bit i of a match is slot i of the group.
    struct Group
    {
        Simd::Byte16 Bytes;

        [[nodiscard]] uint32 Match(uint8 h2) const
        {
            return Simd::MoveMask(Simd::Equal(Bytes, Simd::Byte16(h2)));
        }

        [[nodiscard]] uint32 MatchEmpty() const
        {
            return Simd::MoveMask(Simd::Equal(Bytes, Simd::Byte16(Empty)));
        }

        /// Empty and Deleted are the only control bytes with the high bit set.
        [[nodiscard]] uint32 MatchEmptyOrDeleted() const
        {
            return Simd::MoveMask(Bytes);
        }
    };

    static usize FirstIndex(uint32 bits)
    {
        return static_cast<usize>(CountTrailingZeros(bits));
    }

public:
    /// Creates an empty table.
    HashTable() = default;

    /// Creates an empty table that allocates through `allocator`.
    explicit HashTable(const TAllocator &allocator) : _storage(allocator)
    {
    }

    /// Copies the entries of `other` with freshly computed hashes.
    HashTable(const HashTable &other)
    {
        Reserve(other._count);
        for (const Entry &entry : other)
        {
            InsertUnique(Hasher<Key>::Get(TTraits::KeyOf(entry)), entry);
        }
    }

    /// Takes the storage of `other` and leaves it empty.
    HashTable(HashTable &&other) noexcept
        : _slots(Exchange(other._slots, nullptr)), _controls(Exchange(other._controls, nullptr)),
          _capacity(Exchange(other._capacity, 0)), _count(Exchange(other._count, 0)),
          _growthLeft(Exchange(other._growthLeft, 0))
    {
    }

    /// Destroys the entries and frees the storage.
    ~HashTable()
    {
        DestroyAll();
        FreeTable();
    }

    /// Replaces the entries with a copy of `other`.
    HashTable &operator=(const HashTable &other)
    {
        if (this != &other)
        {
            HashTable(other).Swap(*this);
        }
        return *this;
    }

    /// Frees the current storage, then takes the storage of `other` and leaves it empty.
    HashTable &operator=(HashTable &&other) noexcept
    {
        if (this != &other)
        {
            HashTable(Move(other)).Swap(*this);
        }
        return *this;
    }

    /// Exchanges the contents of both tables.
    void Swap(HashTable &other) noexcept
    {
        Marlin::Swap(_slots, other._slots);
        Marlin::Swap(_controls, other._controls);
        Marlin::Swap(_capacity, other._capacity);
        Marlin::Swap(_count, other._count);
        Marlin::Swap(_growthLeft, other._growthLeft);
    }

    /// Returns the number of entries.
    [[nodiscard]] usize Count() const
    {
        return _count;
    }

    /// Returns whether there are no entries.
    [[nodiscard]] bool IsEmpty() const
    {
        return _count == 0;
    }

    /// Returns the number of slots, which is zero until the first insertion.
    [[nodiscard]] usize Capacity() const
    {
        return _capacity;
    }

    /// Destroys every entry and marks every slot empty, keeping the allocation.
    void Clear()
    {
        DestroyAll();
        if (_capacity > 0)
        {
            MemSet(_controls, Empty, _capacity + GroupWidth);
        }
        _count = 0;
        _growthLeft = MaxLoad(_capacity);
    }

    /// Grows the capacity so that `count` entries fit below the load limit.
    void Reserve(usize count)
    {
        if (count > MaxLoad(_capacity))
        {
            Rehash(CapacityFor(count));
        }
    }

    /// Returns the entry for `key`, or null when it is absent.
    ///
    /// The pointer is invalidated by an insertion that rehashes the table.
    template <typename TKey> [[nodiscard]] Entry *Find(const TKey &key) const
    {
        return _count == 0 ? nullptr : FindHashed(key, Hasher<RemoveCvRef<TKey>>::Get(key));
    }

    /// Returns the entry for `key`, using `hash` instead of hashing it again.
    ///
    /// # Preconditions
    /// `hash` is what [`Hasher`] gives for `key`. An incorrect hash can miss an entry.
    template <typename TKey> [[nodiscard]] Entry *FindHashed(const TKey &key, uint64 hash) const
    {
        if (_count == 0)
        {
            return nullptr;
        }
        const auto h2 = static_cast<uint8>(hash & 0x7F);
        usize position = (hash >> 7) & (_capacity - 1);
        for (usize step = GroupWidth;; step += GroupWidth)
        {
            const Group group = LoadGroup(position);
            for (uint32 bits = group.Match(h2); bits != 0; bits &= bits - 1)
            {
                const usize index = (position + FirstIndex(bits)) & (_capacity - 1);
                if (TTraits::KeyOf(_slots[index]) == key)
                {
                    return _slots + index;
                }
            }
            if (group.MatchEmpty() != 0)
            {
                return nullptr;
            }
            position = (position + step) & (_capacity - 1);
        }
    }

    /// Returns the entry for `key` and whether it was inserted.
    ///
    /// A new entry is constructed from `args` only when `key` is absent.
    template <typename TKey, typename... TArgs> Pair<Entry *, bool> FindOrInsert(const TKey &key, TArgs &&...args)
    {
        return FindOrInsertHashed(key, Hasher<RemoveCvRef<TKey>>::Get(key), Forward<TArgs>(args)...);
    }

    /// Returns the entry for `key` and whether it was inserted, using `hash` instead of hashing it again.
    ///
    /// # Preconditions
    /// `hash` is what [`Hasher`] gives for `key`. An incorrect hash can insert a duplicate entry.
    template <typename TKey, typename... TArgs>
    Pair<Entry *, bool> FindOrInsertHashed(const TKey &key, uint64 hash, TArgs &&...args)
    {
        if (_count > 0)
        {
            const auto h2 = static_cast<uint8>(hash & 0x7F);
            usize position = (hash >> 7) & (_capacity - 1);
            for (usize step = GroupWidth;; step += GroupWidth)
            {
                const Group group = LoadGroup(position);
                for (uint32 bits = group.Match(h2); bits != 0; bits &= bits - 1)
                {
                    const usize index = (position + FirstIndex(bits)) & (_capacity - 1);
                    if (TTraits::KeyOf(_slots[index]) == key)
                    {
                        return {_slots + index, false};
                    }
                }
                if (group.MatchEmpty() != 0)
                {
                    break;
                }
                position = (position + step) & (_capacity - 1);
            }
        }
        return {InsertUnique(hash, Forward<TArgs>(args)...), true};
    }

    /// Removes the entry for `key`; returns whether it existed.
    template <typename TKey> bool Remove(const TKey &key)
    {
        const Entry *entry = Find(key);
        if (entry == nullptr)
        {
            return false;
        }
        RemoveAt(static_cast<usize>(entry - _slots));
        return true;
    }

    /// Removes every entry that `predicate` accepts; returns how many were removed.
    ///
    /// `predicate` receives a `const Entry &`.
    template <typename TPredicate> usize RemoveIf(TPredicate predicate)
    {
        usize removed = 0;
        for (usize index = 0; index < _capacity; ++index)
        {
            if (IsFull(_controls[index]) && predicate(static_cast<const Entry &>(_slots[index])))
            {
                RemoveAt(index);
                ++removed;
            }
        }
        return removed;
    }

    /// Iterates over the occupied slots in slot order.
    template <typename TEntry> class Iterator
    {
    public:
        /// Creates an iterator at `index`, advancing to the first occupied slot.
        Iterator(TEntry *slots, const uint8 *controls, usize index, usize capacity)
            : _slots(slots), _controls(controls), _index(index), _capacity(capacity)
        {
            SkipEmpty();
        }

        /// Returns the entry at the current slot.
        TEntry &operator*() const
        {
            return _slots[_index];
        }

        /// Returns a pointer to the entry at the current slot.
        TEntry *operator->() const
        {
            return _slots + _index;
        }

        /// Advances to the next occupied slot.
        Iterator &operator++()
        {
            ++_index;
            SkipEmpty();
            return *this;
        }

        /// Returns whether both iterators are at the same slot.
        bool operator==(const Iterator &other) const
        {
            return _index == other._index;
        }

    private:
        void SkipEmpty()
        {
            while (_index < _capacity && !IsFull(_controls[_index]))
            {
                ++_index;
            }
        }

        TEntry *_slots;
        const uint8 *_controls;
        usize _index;
        usize _capacity;
    };

    /// Returns an iterator over the occupied slots.
    [[nodiscard]] Iterator<Entry> begin()
    {
        return {_slots, _controls, 0, _capacity};
    }

    /// Returns the end iterator.
    [[nodiscard]] Iterator<Entry> end()
    {
        return {_slots, _controls, _capacity, _capacity};
    }

    /// Returns an iterator over the occupied slots.
    [[nodiscard]] Iterator<const Entry> begin() const
    {
        return {_slots, _controls, 0, _capacity};
    }

    /// Returns the end iterator.
    [[nodiscard]] Iterator<const Entry> end() const
    {
        return {_slots, _controls, _capacity, _capacity};
    }

private:
    static bool IsFull(uint8 control)
    {
        return (control & 0x80) == 0;
    }

    static usize MaxLoad(usize capacity)
    {
        return capacity - (capacity / 8);
    }

    static usize CapacityFor(usize count)
    {
        usize capacity = 16;
        while (MaxLoad(capacity) < count)
        {
            capacity *= 2;
        }
        return capacity;
    }

    [[nodiscard]] Group LoadGroup(usize position) const
    {
        Group group{};
        MemCopy(&group.Bytes, _controls + position, sizeof(group.Bytes));
        return group;
    }

    void SetControl(usize index, uint8 control)
    {
        _controls[index] = control;
        if (index < GroupWidth)
        {
            _controls[_capacity + index] = control;
        }
    }

    [[nodiscard]] usize FindInsertSlot(uint64 hash) const
    {
        usize position = (hash >> 7) & (_capacity - 1);
        for (usize step = GroupWidth;; step += GroupWidth)
        {
            const uint32 bits = LoadGroup(position).MatchEmptyOrDeleted();
            if (bits != 0)
            {
                return (position + FirstIndex(bits)) & (_capacity - 1);
            }
            position = (position + step) & (_capacity - 1);
        }
    }

    template <typename... TArgs> Entry *InsertUnique(uint64 hash, TArgs &&...args)
    {
        if (_growthLeft == 0)
        {
            if (_capacity == 0)
            {
                Rehash(CapacityFor(1));
            }
            else
            {
                Rehash(_count >= MaxLoad(_capacity) / 2 ? _capacity * 2 : _capacity);
            }
        }
        const usize index = FindInsertSlot(hash);
        if (_controls[index] == Empty)
        {
            --_growthLeft;
        }
        SetControl(index, static_cast<uint8>(hash & 0x7F));
        ConstructAt(_slots + index, Forward<TArgs>(args)...);
        ++_count;
        return _slots + index;
    }

    void RemoveAt(usize index)
    {
        DestroyAt(_slots + index);
        --_count;
        const usize before = (index - GroupWidth) & (_capacity - 1);
        const bool wasNeverFull = LoadGroup(index).MatchEmpty() != 0 && LoadGroup(before).MatchEmpty() != 0 &&
                                  (static_cast<usize>(CountLeadingZeros(LoadGroup(before).MatchEmpty())) - 16) +
                                          FirstIndex(LoadGroup(index).MatchEmpty()) <
                                      GroupWidth;
        SetControl(index, wasNeverFull ? Empty : Deleted);
        _growthLeft += wasNeverFull ? 1 : 0;
    }

    void Rehash(usize capacity)
    {
        Entry *oldSlots = _slots;
        const uint8 *oldControls = _controls;
        const usize oldCapacity = _capacity;

        const usize controlBytes = capacity + GroupWidth;
        const usize units = capacity + DivideRoundUp(controlBytes, sizeof(Entry));
        _slots = _storage.Allocate(units);
        _controls = reinterpret_cast<uint8 *>(_slots + capacity);
        _capacity = capacity;
        MemSet(_controls, Empty, controlBytes);
        _growthLeft = MaxLoad(capacity) - _count;

        for (usize index = 0; index < oldCapacity; ++index)
        {
            if (IsFull(oldControls[index]))
            {
                const uint64 hash = Hasher<Key>::Get(TTraits::KeyOf(oldSlots[index]));
                const usize target = FindInsertSlot(hash);
                SetControl(target, static_cast<uint8>(hash & 0x7F));
                RelocateRange(_slots + target, oldSlots + index, 1);
            }
        }
        if (oldSlots != nullptr)
        {
            _storage.Free(oldSlots, oldCapacity + DivideRoundUp(oldCapacity + GroupWidth, sizeof(Entry)));
        }
    }

    void DestroyAll()
    {
        if constexpr (!IsTriviallyDestructible<Entry>)
        {
            for (usize index = 0; index < _capacity; ++index)
            {
                if (IsFull(_controls[index]))
                {
                    DestroyAt(_slots + index);
                }
            }
        }
    }

    void FreeTable()
    {
        if (_slots != nullptr)
        {
            _storage.Free(_slots, _capacity + DivideRoundUp(_capacity + GroupWidth, sizeof(Entry)));
            _slots = nullptr;
            _controls = nullptr;
        }
    }

    MARLIN_NO_UNIQUE_ADDRESS Storage _storage;
    Entry *_slots = nullptr;
    uint8 *_controls = nullptr;
    usize _capacity = 0;
    usize _count = 0;
    usize _growthLeft = 0;
};
} // namespace Marlin::Private
