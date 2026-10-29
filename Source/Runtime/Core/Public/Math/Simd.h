//! Four-lane SIMD math built on clang's vector extensions.
//!
//! Arithmetic and comparisons work lane-wise, and the same source compiles to SSE/AVX on x64 and NEON on Arm64.
//! Approximate reciprocals and the byte move mask come from `PlatformSimd`.

#pragma once

#include "CoreTypes.h"
#include "HAL/PlatformSimd.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

namespace Marlin::Simd
{
/// A vector of four `float32` lanes.
///
/// Arithmetic and comparison operators apply lane-wise. The type comes from clang's vector extensions, so the
/// same source compiles to SSE/AVX on x64 and NEON on Arm64.
using Float4 = float __attribute__((ext_vector_type(4)));
/// A vector of four `int32` lanes.
using Int4 = int32 __attribute__((ext_vector_type(4)));
/// A vector of four `uint32` lanes.
using UInt4 = uint32 __attribute__((ext_vector_type(4)));
/// A vector of four `float64` lanes.
using Double4 = float64 __attribute__((ext_vector_type(4)));
/// A vector of sixteen bytes.
using Byte16 = uint8 __attribute__((ext_vector_type(16)));

/// A comparison result with all bits set in a true lane and zero in a false lane.
using Mask4 = Int4;

/// Returns a vector with every lane set to `value`.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Splat(float32 value)
{
    return Float4(value);
}

/// Loads four `float32` values into a vector.
///
/// # Preconditions
/// `values` points to at least four readable `float32` values. The pointer is not required to be aligned.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Load(const float32 *values)
{
    Float4 result;
    MemCopy(&result, values, sizeof(result));
    return result;
}

/// Loads three `float32` values and sets the fourth lane to `w`.
///
/// # Preconditions
/// `values` points to at least three readable `float32` values. The pointer is not required to be aligned.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Load3(const float32 *values, float32 w = 0)
{
    return Float4{values[0], values[1], values[2], w};
}

/// Stores the four lanes of `vector` to `values`.
///
/// # Preconditions
/// `values` points to four writable `float32` values. The pointer is not required to be aligned.
MARLIN_FORCEINLINE void Store(float32 *values, Float4 vector)
{
    MemCopy(values, &vector, sizeof(vector));
}

/// Stores the `x`, `y` and `z` lanes of `vector` to `values`.
///
/// # Preconditions
/// `values` points to three writable `float32` values. The pointer is not required to be aligned.
MARLIN_FORCEINLINE void Store3(float32 *values, Float4 vector)
{
    values[0] = vector.x;
    values[1] = vector.y;
    values[2] = vector.z;
}

/// Returns a vector built by selecting lanes of `a` and `b`.
///
/// `TX`, `TY`, `TZ` and `TW` are the lanes of the result in order. Indices 0 to 3 select lanes of `a` and 4 to
/// 7 select lanes of `b`.
///
/// # Examples
///
/// ```cpp
/// const Simd::Float4 a{1, 2, 3, 4};
/// const Simd::Float4 b{5, 6, 7, 8};
/// const Simd::Float4 mixed = Simd::Shuffle<3, 0, 4, 7>(a, b); // -> (4, 1, 5, 8)
/// ```
template <int32 TX, int32 TY, int32 TZ, int32 TW> [[nodiscard]] MARLIN_FORCEINLINE Float4 Shuffle(Float4 a, Float4 b)
{
    return __builtin_shufflevector(a, b, TX, TY, TZ, TW);
}

/// Returns a vector whose lanes are selected from `value` by `TX`, `TY`, `TZ` and `TW`.
///
/// Indices 0 to 3 select `x`, `y`, `z` and `w`; lanes may repeat.
///
/// # Examples
///
/// ```cpp
/// const Simd::Float4 value{1, 2, 3, 4};
/// const Simd::Float4 reversed = Simd::Swizzle<3, 2, 1, 0>(value); // -> (4, 3, 2, 1)
/// ```
template <int32 TX, int32 TY, int32 TZ, int32 TW> [[nodiscard]] MARLIN_FORCEINLINE Float4 Swizzle(Float4 value)
{
    return __builtin_shufflevector(value, value, TX, TY, TZ, TW);
}

/// Returns the componentwise minimum of `a` and `b`.
///
/// Matches the SSE instruction: a lane where either input is NaN takes the value from `b`.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Min(Float4 a, Float4 b)
{
    return a < b ? a : b;
}

/// Returns the componentwise maximum of `a` and `b`.
///
/// A lane where either input is NaN takes the value from `b`.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Max(Float4 a, Float4 b)
{
    return a > b ? a : b;
}

/// Returns `value` limited to the range from `low` to `high` in every lane.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Clamp(Float4 value, Float4 low, Float4 high)
{
    return Min(Max(value, low), high);
}

/// Returns the absolute value of every lane.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Abs(Float4 value)
{
    return __builtin_elementwise_abs(value);
}

/// Returns the square root of every lane.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Sqrt(Float4 value)
{
    return __builtin_elementwise_sqrt(value);
}

/// Returns every lane rounded down to the nearest integer.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Floor(Float4 value)
{
    return __builtin_elementwise_floor(value);
}

/// Returns every lane rounded up to the nearest integer.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Ceil(Float4 value)
{
    return __builtin_elementwise_ceil(value);
}

/// Returns every lane rounded to the nearest integer, with halfway cases rounded to even.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Round(Float4 value)
{
    return __builtin_elementwise_roundeven(value);
}

/// Returns `a * b + c` in every lane with a single rounding.
[[nodiscard]] MARLIN_FORCEINLINE Float4 MultiplyAdd(Float4 a, Float4 b, Float4 c)
{
    return __builtin_elementwise_fma(a, b, c);
}

/// Returns an approximation of `1 / value` in every lane.
///
/// The x64 implementation uses a hardware estimate with about 12 bits of precision; the portable
/// implementation divides.
[[nodiscard]] MARLIN_FORCEINLINE Float4 ReciprocalEstimate(Float4 value)
{
    return PlatformSimd::ReciprocalEstimate(value);
}

/// Returns an approximation of `1 / sqrt(value)` in every lane.
///
/// The x64 implementation uses a hardware estimate with about 12 bits of precision; the portable
/// implementation divides by the square root.
[[nodiscard]] MARLIN_FORCEINLINE Float4 ReciprocalSqrtEstimate(Float4 value)
{
    return PlatformSimd::ReciprocalSqrtEstimate(value);
}

/// Returns an approximation of `1 / sqrt(value)` in every lane.
///
/// The estimate is refined by one Newton step, giving about 23 bits of precision.
[[nodiscard]] MARLIN_FORCEINLINE Float4 ReciprocalSqrt(Float4 value)
{
    const Float4 estimate = ReciprocalSqrtEstimate(value);
    return estimate * (1.5f - 0.5f * value * estimate * estimate);
}

/// Returns lanes from `ifTrue` where `mask` is set and from `ifFalse` where it is clear.
///
/// Each lane of `mask` is expected to be zero or all bits set, as a comparison produces; a partial mask blends
/// the bits of both inputs.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Select(Mask4 mask, Float4 ifTrue, Float4 ifFalse)
{
    const Int4 bits = (mask & BitCast<Int4>(ifTrue)) | (~mask & BitCast<Int4>(ifFalse));
    return BitCast<Float4>(bits);
}

/// Returns a 4-bit mask with bit `i` set when lane `i` of `mask` has its sign bit set.
///
/// A comparison puts all bits set in a true lane, so the result is the set of true comparisons.
///
/// # Examples
///
/// ```cpp
/// const Simd::Mask4 mask = Simd::Float4{1, -1, 2, -2} < Simd::Splat(0.0f);
/// const uint32 bits = Simd::MoveMask(mask); // -> 0b1010
/// ```
[[nodiscard]] MARLIN_FORCEINLINE uint32 MoveMask(Mask4 mask)
{
    const UInt4 bits = BitCast<UInt4>(mask) >> 31;
    return bits.x | (bits.y << 1) | (bits.z << 2) | (bits.w << 3);
}

/// Returns whether any lane of `mask` has its sign bit set.
[[nodiscard]] MARLIN_FORCEINLINE bool AnyTrue(Mask4 mask)
{
    return MoveMask(mask) != 0;
}

/// Returns whether all four lanes of `mask` have their sign bit set.
[[nodiscard]] MARLIN_FORCEINLINE bool AllTrue(Mask4 mask)
{
    return MoveMask(mask) == 0xF;
}

/// Returns a byte mask with `0xFF` where `a` and `b` are equal and `0` where they differ.
[[nodiscard]] MARLIN_FORCEINLINE Byte16 Equal(Byte16 a, Byte16 b)
{
    return BitCast<Byte16>(a == b);
}

/// Returns a 16-bit mask with bit `i` set when the top bit of byte `i` is set.
///
/// The x64 implementation uses one `pmovmskb` instruction.
[[nodiscard]] MARLIN_FORCEINLINE uint32 MoveMask(Byte16 bytes)
{
    return PlatformSimd::MoveMask(bytes);
}

/// Returns the sum of all four lanes in every lane.
[[nodiscard]] MARLIN_FORCEINLINE Float4 HorizontalSum(Float4 value)
{
    const Float4 pairs = value + Swizzle<1, 0, 3, 2>(value);
    return pairs + Swizzle<2, 3, 0, 1>(pairs);
}

/// Returns the four-lane dot product of `a` and `b` in every lane.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Dot4(Float4 a, Float4 b)
{
    return HorizontalSum(a * b);
}

/// Returns the dot product of the `x`, `y` and `z` lanes in every lane.
///
/// The `w` lanes of the inputs are ignored.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Dot3(Float4 a, Float4 b)
{
    const Float4 product = a * b;
    return Splat(product.x + product.y + product.z);
}

/// Returns the cross product of the `x`, `y` and `z` lanes.
///
/// The `w` lane of the result is zero and the `w` lanes of the inputs are ignored.
[[nodiscard]] MARLIN_FORCEINLINE Float4 Cross3(Float4 a, Float4 b)
{
    const Float4 result =
        Swizzle<1, 2, 0, 3>(a) * Swizzle<2, 0, 1, 3>(b) - Swizzle<2, 0, 1, 3>(a) * Swizzle<1, 2, 0, 3>(b);
    return Float4{result.x, result.y, result.z, 0};
}
} // namespace Marlin::Simd
