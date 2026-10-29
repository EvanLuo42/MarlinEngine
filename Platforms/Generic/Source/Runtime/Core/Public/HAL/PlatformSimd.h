#pragma once

#include "CoreTypes.h"

/// Portable versions of the SIMD operations that clang builtins do not cover; arch layers replace this file.
namespace Marlin::PlatformSimd
{
using Float4 = float __attribute__((ext_vector_type(4)));
using Byte16 = uint8 __attribute__((ext_vector_type(16)));

/// Approximate 1 / x.
MARLIN_FORCEINLINE Float4 ReciprocalEstimate(Float4 value)
{
    return 1.0f / value;
}

/// Approximate 1 / sqrt(x).
MARLIN_FORCEINLINE Float4 ReciprocalSqrtEstimate(Float4 value)
{
    return 1.0f / __builtin_elementwise_sqrt(value);
}

/// Bit i is the top bit of byte i.
MARLIN_FORCEINLINE uint32 MoveMask(Byte16 bytes)
{
    uint32 mask = 0;
    for (uint32 i = 0; i < 16; ++i)
    {
        mask |= static_cast<uint32>(bytes[i] >> 7) << i;
    }
    return mask;
}
} // namespace Marlin::PlatformSimd
