//! Bit manipulation: reinterpretation, population counts, rotations, alignment and a 128-bit multiply.

#pragma once

#include "CoreTypes.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// Reinterprets the bits of `value` as a `TTo`.
///
/// # Preconditions
/// `TTo` and `TFrom` must have the same size and be trivially copyable; a size mismatch is a compile-time
/// error.
///
/// # Examples
///
/// ```cpp
/// float32 one = BitCast<float32>(uint32{0x3F800000}); // -> 1.0f
/// ```
template <typename TTo, typename TFrom> [[nodiscard]] constexpr TTo BitCast(const TFrom &value) noexcept
{
    static_assert(sizeof(TTo) == sizeof(TFrom));
    return __builtin_bit_cast(TTo, value);
}

/// Returns the number of set bits in `value`.
template <UnsignedIntegral T> [[nodiscard]] constexpr int32 PopCount(T value) noexcept
{
    if constexpr (sizeof(T) <= 4)
    {
        return __builtin_popcount(value);
    }
    else
    {
        return __builtin_popcountll(value);
    }
}

/// Returns the number of zero bits above the highest set bit; returns the bit width of `T` for zero.
template <UnsignedIntegral T> [[nodiscard]] constexpr int32 CountLeadingZeros(T value) noexcept
{
    constexpr int32 Bits = sizeof(T) * 8;
    if (value == 0)
    {
        return Bits;
    }
    if constexpr (sizeof(T) <= 4)
    {
        return __builtin_clz(value) - (32 - Bits);
    }
    else
    {
        return __builtin_clzll(value);
    }
}

/// Returns the number of zero bits below the lowest set bit; returns the bit width of `T` for zero.
template <UnsignedIntegral T> [[nodiscard]] constexpr int32 CountTrailingZeros(T value) noexcept
{
    if (value == 0)
    {
        return sizeof(T) * 8;
    }
    if constexpr (sizeof(T) <= 4)
    {
        return __builtin_ctz(value);
    }
    else
    {
        return __builtin_ctzll(value);
    }
}

/// Returns the index of the highest set bit, or -1 for zero.
template <UnsignedIntegral T> [[nodiscard]] constexpr int32 FloorLog2(T value) noexcept
{
    return static_cast<int32>(sizeof(T) * 8) - 1 - CountLeadingZeros(value);
}

/// Returns whether `value` has exactly one bit set; zero is not a power of two.
template <UnsignedIntegral T> [[nodiscard]] constexpr bool IsPowerOfTwo(T value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

/// Returns the smallest power of two not below `value`; returns 1 for zero.
///
/// # Preconditions
/// `value` must not exceed the largest power of two representable in `T`. The function does not check it.
template <UnsignedIntegral T> [[nodiscard]] constexpr T RoundUpToPowerOfTwo(T value) noexcept
{
    return value <= 1 ? T{1} : static_cast<T>(T{1} << (sizeof(T) * 8 - CountLeadingZeros(static_cast<T>(value - 1))));
}

/// Rotates the bits of `value` left by `count` positions.
///
/// `count` is reduced modulo the bit width of `T` and may be negative; a negative count rotates right.
template <UnsignedIntegral T> [[nodiscard]] constexpr T RotateLeft(T value, int32 count) noexcept
{
    constexpr uint32 Bits = sizeof(T) * 8;
    const uint32 shift = static_cast<uint32>(count) & (Bits - 1);
    const auto wide = static_cast<Conditional<(sizeof(T) <= 4), uint32, uint64>>(value);
    return shift == 0 ? value : static_cast<T>((wide << shift) | (wide >> (Bits - shift)));
}

/// Rotates the bits of `value` right by `count` positions.
///
/// `count` is reduced modulo the bit width of `T` and may be negative; a negative count rotates left.
template <UnsignedIntegral T> [[nodiscard]] constexpr T RotateRight(T value, int32 count) noexcept
{
    constexpr uint32 Bits = sizeof(T) * 8;
    const uint32 shift = static_cast<uint32>(count) & (Bits - 1);
    const auto wide = static_cast<Conditional<(sizeof(T) <= 4), uint32, uint64>>(value);
    return shift == 0 ? value : static_cast<T>((wide >> shift) | (wide << (Bits - shift)));
}

/// Reverses the byte order of `value`; values of one byte are returned unchanged.
template <Integral T> [[nodiscard]] constexpr T ByteSwap(T value) noexcept
{
    if constexpr (sizeof(T) == 1)
    {
        return value;
    }
    else if constexpr (sizeof(T) == 2)
    {
        return static_cast<T>(__builtin_bswap16(static_cast<uint16>(value)));
    }
    else if constexpr (sizeof(T) == 4)
    {
        return static_cast<T>(__builtin_bswap32(static_cast<uint32>(value)));
    }
    else
    {
        return static_cast<T>(__builtin_bswap64(static_cast<uint64>(value)));
    }
}

/// Rounds `value` up to the smallest multiple of `alignment` that is not below it.
///
/// # Preconditions
/// `alignment` must be a nonzero power of two and `value + alignment - 1` must not overflow `T`; the function
/// checks neither. Values that are already aligned are returned unchanged.
template <UnsignedIntegral T> [[nodiscard]] constexpr T AlignUp(T value, T alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Rounds `value` down to the nearest multiple of `alignment`.
///
/// # Preconditions
/// `alignment` must be a nonzero power of two; the function does not check it.
template <UnsignedIntegral T> [[nodiscard]] constexpr T AlignDown(T value, T alignment) noexcept
{
    return value & ~(alignment - 1);
}

/// Returns whether `value` is a multiple of `alignment`.
///
/// # Preconditions
/// `alignment` must be a nonzero power of two; the function does not check it.
template <UnsignedIntegral T> [[nodiscard]] constexpr bool IsAligned(T value, T alignment) noexcept
{
    return (value & (alignment - 1)) == 0;
}

/// Returns `pointer` rounded up to the next multiple of `alignment` bytes.
///
/// # Preconditions
/// `alignment` must be a nonzero power of two.
///
/// # Safety
/// If the result is dereferenced, it must lie within the allocation that `pointer` points into. Rounding up
/// can move past the end of an object.
template <typename T> [[nodiscard]] inline T *AlignPointerUp(T *pointer, usize alignment) noexcept
{
    return reinterpret_cast<T *>(AlignUp(reinterpret_cast<uintptr>(pointer), static_cast<uintptr>(alignment)));
}

/// Returns whether `pointer` is aligned to `alignment` bytes.
///
/// # Preconditions
/// `alignment` must be a nonzero power of two.
[[nodiscard]] inline bool IsPointerAligned(const void *pointer, usize alignment) noexcept
{
    return IsAligned(reinterpret_cast<uintptr>(pointer), static_cast<uintptr>(alignment));
}

/// Returns `dividend / divisor`, rounded up.
///
/// # Preconditions
/// `divisor` must not be zero, and `dividend + divisor - 1` must not overflow `T`.
template <UnsignedIntegral T> [[nodiscard]] constexpr T DivideRoundUp(T dividend, T divisor) noexcept
{
    return (dividend + divisor - 1) / divisor;
}

/// The full 128-bit product of two 64-bit unsigned values.
///
/// # Examples
///
/// ```cpp
/// Product128 product = Multiply128(0xFFFFFFFFFFFFFFFFull, 2);
/// uint64 high = product.High; // -> 1
/// ```
struct Product128
{
    /// The low 64 bits of the product.
    uint64 Low;
    /// The high 64 bits of the product.
    uint64 High;
};

/// Returns the full 128-bit product of `a` and `b`.
[[nodiscard]] constexpr Product128 Multiply128(uint64 a, uint64 b) noexcept
{
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return {static_cast<uint64>(product), static_cast<uint64>(product >> 64)};
}
} // namespace Marlin
