//! Comparison function objects, sorting and binary search over contiguous ranges.
//!
//! The algorithms take a pointer and a count instead of iterators, allocate nothing, and need no standard
//! library.

#pragma once

#include "CoreTypes.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// A comparator that orders with `operator<`.
///
/// The call operator is a template, so values of different types can be compared as long as `operator<` accepts
/// them.
struct Less
{
    template <typename T, typename TOther> constexpr bool operator()(const T &a, const TOther &b) const
    {
        return a < b;
    }
};

/// A comparator that orders in descending order.
///
/// Returns `b < a`, so it uses the same `operator<` as [Less] with the operands reversed.
struct Greater
{
    template <typename T, typename TOther> constexpr bool operator()(const T &a, const TOther &b) const
    {
        return b < a;
    }
};

namespace Private
{
template <typename T, typename TLess> constexpr void InsertionSort(T *data, usize count, TLess &less)
{
    for (usize i = 1; i < count; ++i)
    {
        T value = Move(data[i]);
        usize j = i;
        for (; j > 0 && less(value, data[j - 1]); --j)
        {
            data[j] = Move(data[j - 1]);
        }
        data[j] = Move(value);
    }
}

template <typename T, typename TLess> constexpr void SiftDown(T *data, usize root, usize count, TLess &less)
{
    while (true)
    {
        usize child = (2 * root) + 1;
        if (child >= count)
        {
            return;
        }
        if (child + 1 < count && less(data[child], data[child + 1]))
        {
            ++child;
        }
        if (!less(data[root], data[child]))
        {
            return;
        }
        Swap(data[root], data[child]);
        root = child;
    }
}

template <typename T, typename TLess> constexpr void HeapSort(T *data, usize count, TLess &less)
{
    for (usize i = count / 2; i-- > 0;)
    {
        SiftDown(data, i, count, less);
    }
    for (usize end = count; end-- > 1;)
    {
        Swap(data[0], data[end]);
        SiftDown(data, 0, end, less);
    }
}

template <typename T, typename TLess> constexpr void IntroSort(T *data, usize count, uint32 depth, TLess &less)
{
    while (count > 16)
    {
        if (depth-- == 0)
        {
            HeapSort(data, count, less);
            return;
        }
        T *middle = data + (count / 2);
        T *last = data + count - 1;
        if (less(*middle, *data))
        {
            Swap(*middle, *data);
        }
        if (less(*last, *middle))
        {
            Swap(*last, *middle);
            if (less(*middle, *data))
            {
                Swap(*middle, *data);
            }
        }
        Swap(*middle, data[count - 2]);
        T &pivot = data[count - 2];
        usize left = 0;
        usize right = count - 2;
        while (true)
        {
            while (less(data[++left], pivot))
            {
            }
            while (less(pivot, data[--right]))
            {
            }
            if (left >= right)
            {
                break;
            }
            Swap(data[left], data[right]);
        }
        Swap(data[left], data[count - 2]);
        if (left < count - left)
        {
            IntroSort(data, left, depth, less);
            data += left + 1;
            count -= left + 1;
        }
        else
        {
            IntroSort(data + left + 1, count - left - 1, depth, less);
            count = left;
        }
    }
}
} // namespace Private

/// Sorts `count` elements of `data` in place, in ascending order by `less`; not stable.
///
/// Introsort: quicksort with a median-of-three pivot that switches to heapsort when the recursion gets too
/// deep, followed by an insertion pass over the resulting blocks of at most 16 elements. The worst case is
/// O(n log n) comparisons and moves. Equal elements are not guaranteed to keep their order; use [StableSort]
/// to preserve it.
///
/// # Preconditions
/// `data` points to `count` elements, or `count` is zero and `data` may be null. `less` must be a strict weak
/// ordering; with any other comparator the sort can read or write outside `data`.
///
/// # Performance
/// Does not allocate.
///
/// # Examples
///
/// ```cpp
/// int32 values[] = {3, 1, 4, 2};
/// Sort(values, 4);
/// int32 first = values[0]; // -> 1
/// ```
template <typename T, typename TLess = Less> constexpr void Sort(T *data, usize count, TLess less = {})
{
    uint32 depth = 0;
    for (usize n = count; n > 1; n >>= 1)
    {
        depth += 2;
    }
    Private::IntroSort(data, count, depth, less);
    Private::InsertionSort(data, count, less);
}

/// Sorts `count` elements of `data` in place by `less`, keeping equal elements in their original order.
///
/// Sorts runs of 16 elements with insertion sort, then merges the runs in place. It never allocates.
/// Comparisons stay O(n log n), but merging shifts elements, so the number of moves can reach O(n^2) for a
/// large range.
///
/// # Preconditions
/// Same as [Sort].
///
/// # Performance
/// No allocation; O(n log n) comparisons and O(n^2) moves in the worst case.
template <typename T, typename TLess = Less> constexpr void StableSort(T *data, usize count, TLess less = {})
{
    constexpr usize Run = 16;
    for (usize start = 0; start < count; start += Run)
    {
        Private::InsertionSort(data + start, Min(Run, count - start), less);
    }
    for (usize width = Run; width < count; width *= 2)
    {
        for (usize start = 0; start + width < count; start += 2 * width)
        {
            usize middle = start + width;
            const usize end = Min(start + (2 * width), count);
            usize left = start;
            while (left < middle && middle < end)
            {
                if (!less(data[middle], data[left]))
                {
                    ++left;
                    continue;
                }
                T value = Move(data[middle]);
                for (usize i = middle; i > left; --i)
                {
                    data[i] = Move(data[i - 1]);
                }
                data[left] = Move(value);
                ++left;
                ++middle;
            }
        }
    }
}

/// Returns the first index in `data[0, count)` whose element is not less than `value`.
///
/// Binary search for the lower bound: every element before the result compares less than `value`, and the
/// elements from the result on do not.
///
/// # Preconditions
/// The range must be partitioned with respect to `value` under `less`, normally because it is sorted by
/// `less`. `data` points to `count` elements, or `count` is zero and `data` may be null.
///
/// # Performance
/// O(log n) comparisons; no allocation.
///
/// # Examples
///
/// ```cpp
/// const int32 values[] = {1, 3, 3, 7};
/// usize first = LowerBound(values, 4, 3); // -> 1
/// usize end = UpperBound(values, 4, 3);   // -> 3
/// ```
template <typename T, typename TValue, typename TLess = Less>
[[nodiscard]] constexpr usize LowerBound(const T *data, usize count, const TValue &value, TLess less = {})
{
    usize first = 0;
    while (count > 0)
    {
        const usize half = count / 2;
        if (less(data[first + half], value))
        {
            first += half + 1;
            count -= half + 1;
        }
        else
        {
            count = half;
        }
    }
    return first;
}

/// Returns the first index in `data[0, count)` whose element is greater than `value`.
///
/// Binary search for the upper bound: every element before the result compares not greater than `value`, and
/// the elements from the result on compare greater.
///
/// # Preconditions
/// Same as [LowerBound].
///
/// # Performance
/// O(log n) comparisons; no allocation.
template <typename T, typename TValue, typename TLess = Less>
[[nodiscard]] constexpr usize UpperBound(const T *data, usize count, const TValue &value, TLess less = {})
{
    usize first = 0;
    while (count > 0)
    {
        const usize half = count / 2;
        if (!less(value, data[first + half]))
        {
            first += half + 1;
            count -= half + 1;
        }
        else
        {
            count = half;
        }
    }
    return first;
}

/// Reverses `count` elements of `data` in place.
///
/// # Preconditions
/// `data` points to `count` elements, or `count` is zero and `data` may be null. `T` must be move constructible
/// and move assignable.
///
/// # Performance
/// Performs `count / 2` swaps; no allocation.
template <typename T> constexpr void Reverse(T *data, usize count)
{
    for (usize i = 0; i < count / 2; ++i)
    {
        Swap(data[i], data[count - 1 - i]);
    }
}
} // namespace Marlin
