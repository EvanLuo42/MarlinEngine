//! Low-level operations on uninitialized storage: copy, fill, construct, move and destroy ranges.
//!
//! These functions do not allocate and do not check counts; the caller provides valid storage.

#pragma once

#include "CoreTypes.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// Copies `size` bytes from `source` to `target`.
///
/// The ranges must not overlap; use `MemMove` when they may.
inline void MemCopy(void *target, const void *source, usize size) noexcept
{
    __builtin_memcpy(target, source, size);
}

/// Copies `size` bytes from `source` to `target`; the ranges may overlap.
inline void MemMove(void *target, const void *source, usize size) noexcept
{
    __builtin_memmove(target, source, size);
}

/// Fills `size` bytes at `target` with `value`.
inline void MemSet(void *target, uint8 value, usize size) noexcept
{
    __builtin_memset(target, value, size);
}

/// Fills `size` bytes at `target` with zero.
inline void MemZero(void *target, usize size) noexcept
{
    __builtin_memset(target, 0, size);
}

/// Compares `size` bytes at `a` and `b`, treating them as unsigned bytes.
///
/// Returns a negative value, zero or a positive value when `a` is less than, equal to or greater than `b`.
[[nodiscard]] inline int32 MemCompare(const void *a, const void *b, usize size) noexcept
{
    return __builtin_memcmp(a, b, size);
}

/// Constructs a `T` at `target` with `args` and returns `target`.
///
/// # Preconditions
/// `target` points to uninitialized storage large enough for a `T`.
///
/// # Examples
///
/// ```cpp
/// alignas(int32) uint8 storage[sizeof(int32)];
/// int32 *value = ConstructAt(reinterpret_cast<int32 *>(storage), 42);
/// int32 read = *value; // -> 42
/// DestroyAt(value);
/// ```
template <typename T, typename... TArgs> constexpr T *ConstructAt(T *target, TArgs &&...args)
{
    return ::new (static_cast<void *>(target)) T(Forward<TArgs>(args)...);
}

/// Destroys the `T` at `target`.
///
/// # Preconditions
/// `target` points to a live `T`.
template <typename T> constexpr void DestroyAt(T *target) noexcept
{
    target->~T();
}

/// Value-initializes `count` elements at `target`.
///
/// # Preconditions
/// `target` points to uninitialized storage for `count` `T` objects.
template <typename T> constexpr void DefaultConstructRange(T *target, usize count)
{
    if constexpr (IsZeroConstructible<T>)
    {
        if (!__builtin_is_constant_evaluated())
        {
            MemZero(target, count * sizeof(T));
            return;
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        ::new (static_cast<void *>(target + i)) T();
    }
}

/// Copy-constructs `count` elements at `target` from `source`.
///
/// # Preconditions
/// `target` points to uninitialized storage for `count` `T` objects, and the ranges do not overlap.
template <typename T> constexpr void CopyConstructRange(T *target, const T *source, usize count)
{
    if constexpr (IsTriviallyCopyable<T>)
    {
        if (!__builtin_is_constant_evaluated())
        {
            if (count > 0)
            {
                MemCopy(target, source, count * sizeof(T));
            }
            return;
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        ::new (static_cast<void *>(target + i)) T(source[i]);
    }
}

/// Move-constructs `count` elements at `target` from `source` and leaves the sources in a moved-from state.
///
/// The sources are not destroyed. Use `RelocateRange` to destroy them as well.
///
/// # Preconditions
/// `target` points to uninitialized storage for `count` `T` objects, and the ranges do not overlap.
template <typename T> constexpr void MoveConstructRange(T *target, T *source, usize count)
{
    if constexpr (IsTriviallyCopyable<T>)
    {
        if (!__builtin_is_constant_evaluated())
        {
            if (count > 0)
            {
                MemCopy(target, source, count * sizeof(T));
            }
            return;
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        ::new (static_cast<void *>(target + i)) T(Move(source[i]));
    }
}

/// Destroys `count` elements at `target`.
///
/// Does nothing for trivially destructible types.
///
/// # Preconditions
/// `target` points to `count` live `T` objects.
template <typename T> constexpr void DestroyRange(T *target, usize count) noexcept
{
    if constexpr (!IsTriviallyDestructible<T>)
    {
        for (usize i = 0; i < count; ++i)
        {
            target[i].~T();
        }
    }
}

/// Moves `count` elements to uninitialized `target` and destroys the sources.
///
/// The ranges may overlap only when `T` is trivially relocatable.
///
/// # Preconditions
/// `target` points to uninitialized storage for `count` `T` objects.
template <typename T> constexpr void RelocateRange(T *target, T *source, usize count)
{
    if constexpr (IsTriviallyRelocatable<T>)
    {
        if (!__builtin_is_constant_evaluated())
        {
            if (count > 0)
            {
                MemMove(target, source, count * sizeof(T));
            }
            return;
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        ::new (static_cast<void *>(target + i)) T(Move(source[i]));
        source[i].~T();
    }
}
} // namespace Marlin
