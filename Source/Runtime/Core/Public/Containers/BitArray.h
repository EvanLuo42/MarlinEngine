//! Growable bit sets that pack 64 bits to a word.
//!
//! Bits are indexed from zero. The bit count is independent of the backing words, and unused high bits of the
//! last word are always zero.

#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Templates/Bit.h"

namespace Marlin
{
/// A growable array of bits, packed 64 to a word.
///
/// The backing storage is a [`List`] of `uint64`, and the count is the number of addressable bits. `TAllocator`
/// selects where the words are allocated. Bit positions are checked by assertions in builds with assertions.
///
/// # Thread safety
/// A single instance is not synchronized.
///
/// # Performance
/// `Set`, `Clear` and `Toggle` are O(1). `CountSet` and `Find` scan the words, and `Resize` reallocates and fills
/// the words it adds.
///
/// # Examples
///
/// ```cpp
/// BitArray<> bits(130);
/// bits.Set(3);
/// bits.Set(129);
/// usize first = bits.Find(true);   // -> 3
/// usize next = bits.Find(true, 4); // -> 129
/// usize count = bits.CountSet();   // -> 2
/// ```
template <typename TAllocator = HeapAllocator> class BitArray
{
public:
    /// The index returned by [`Find`] when no matching bit exists.
    static constexpr usize NotFound = ~usize{0};

    /// Creates an empty array.
    BitArray() = default;

    /// Creates an array of `count` bits, all set to `value`.
    explicit BitArray(usize count, bool value = false)
    {
        Resize(count, value);
    }

    /// Returns the number of bits.
    [[nodiscard]] usize Count() const
    {
        return _count;
    }

    /// Returns whether there are no bits.
    [[nodiscard]] bool IsEmpty() const
    {
        return _count == 0;
    }

    /// Returns the bit at `index`.
    ///
    /// Same as [`Get`].
    [[nodiscard]] bool operator[](usize index) const
    {
        return Get(index);
    }

    /// Returns the bit at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    [[nodiscard]] bool Get(usize index) const
    {
        MARLIN_ASSERT(index < _count);
        return (_words[index / 64] >> (index % 64)) & 1;
    }

    /// Sets the bit at `index` to `value`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    void Set(usize index, bool value = true)
    {
        MARLIN_ASSERT(index < _count);
        const uint64 mask = uint64{1} << (index % 64);
        _words[index / 64] = value ? _words[index / 64] | mask : _words[index / 64] & ~mask;
    }

    /// Clears the bit at `index`, leaving it false.
    ///
    /// The index is checked as in [`Set`].
    void Clear(usize index)
    {
        Set(index, false);
    }

    /// Flips the bit at `index`.
    ///
    /// # Preconditions
    /// `index < Count()`, checked by an assertion in builds with assertions.
    void Toggle(usize index)
    {
        MARLIN_ASSERT(index < _count);
        _words[index / 64] ^= uint64{1} << (index % 64);
    }

    /// Appends one bit with `value`.
    void Add(bool value)
    {
        Resize(_count + 1);
        Set(_count - 1, value);
    }

    /// Resizes to `count` bits, setting new bits to `value`.
    ///
    /// Existing bits keep their positions and values. Shrinking discards the trailing bits. `value` defaults to
    /// false.
    void Resize(usize count, bool value = false)
    {
        const usize oldCount = _count;
        _words.Resize(DivideRoundUp(count, usize{64}), value ? ~uint64{0} : 0);
        _count = count;
        if (count > oldCount && oldCount % 64 != 0)
        {
            const uint64 tail = ~uint64{0} << (oldCount % 64);
            uint64 &word = _words[oldCount / 64];
            word = value ? word | tail : word & ~tail;
        }
        ClearUnused();
    }

    /// Sets every bit to `value`.
    void SetAll(bool value)
    {
        for (uint64 &word : _words)
        {
            word = value ? ~uint64{0} : 0;
        }
        ClearUnused();
    }

    /// Returns the number of set bits.
    [[nodiscard]] usize CountSet() const
    {
        usize count = 0;
        for (const uint64 word : _words)
        {
            count += static_cast<usize>(PopCount(word));
        }
        return count;
    }

    /// Returns the index of the first bit equal to `value` at or after `start`, or [`NotFound`].
    ///
    /// `start` is not checked; a start at or past `Count()` finds nothing.
    [[nodiscard]] usize Find(bool value, usize start = 0) const
    {
        for (usize word = start / 64; word < _words.Count(); ++word)
        {
            uint64 bits = value ? _words[word] : ~_words[word];
            if (word == start / 64)
            {
                bits &= ~uint64{0} << (start % 64);
            }
            if (bits != 0)
            {
                const usize index = (word * 64) + static_cast<usize>(CountTrailingZeros(bits));
                return index < _count ? index : NotFound;
            }
        }
        return NotFound;
    }

    /// Returns the backing words.
    ///
    /// The span holds `(Count() + 63) / 64` read-only words, and unused high bits of the last word are zero. It is
    /// invalidated by any operation that changes the bit count.
    [[nodiscard]] Span<const uint64> Words() const
    {
        return _words.AsSpan();
    }

private:
    void ClearUnused()
    {
        if (_count % 64 != 0)
        {
            _words.Last() &= (uint64{1} << (_count % 64)) - 1;
        }
    }

    List<uint64, TAllocator> _words;
    usize _count = 0;
};
} // namespace Marlin
