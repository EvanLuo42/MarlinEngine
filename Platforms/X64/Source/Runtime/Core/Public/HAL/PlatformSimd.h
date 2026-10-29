//! x64 implementations of the SIMD primitives that clang builtins do not cover.
//!
//! The reciprocal and reciprocal square root estimates use the SSE instructions and carry about 12 bits; the
//! byte move mask uses `pmovmskb`. Other platform layers provide portable versions of the same names.

#pragma once

#include "CoreTypes.h"
#include "Templates/Bit.h"

/// The x64 implementations of the platform SIMD primitives.
namespace Marlin::PlatformSimd
{
/// A vector of four `float32` lanes; the same type as `Simd::Float4`.
using Float4 = float __attribute__((ext_vector_type(4)));
/// A vector of sixteen bytes; the same type as `Simd::Byte16`.
using Byte16 = uint8 __attribute__((ext_vector_type(16)));

/// Returns an approximation of `1 / value` in every lane, with about 12 bits of precision.
///
/// The result comes from a single `rcpps` instruction.
MARLIN_FORCEINLINE Float4 ReciprocalEstimate(Float4 value)
{
    return __builtin_ia32_rcpps(value);
}

/// Returns an approximation of `1 / sqrt(value)` in every lane, with about 12 bits of precision.
///
/// The result comes from a single `rsqrtps` instruction.
MARLIN_FORCEINLINE Float4 ReciprocalSqrtEstimate(Float4 value)
{
    return __builtin_ia32_rsqrtps(value);
}

/// Returns a mask whose bit `i` is the top bit of byte `i`.
///
/// Bits 16 to 31 of the result are zero.
MARLIN_FORCEINLINE uint32 MoveMask(Byte16 bytes)
{
    using Char16 = char __attribute__((vector_size(16)));
    return static_cast<uint32>(__builtin_ia32_pmovmskb128(BitCast<Char16>(bytes)));
}
} // namespace Marlin::PlatformSimd
