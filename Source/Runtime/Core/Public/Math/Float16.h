//! An IEEE 754 binary16 value for compact storage.
//!
//! Convert to `float32` before doing arithmetic. Conversions round to nearest even.

#pragma once

#include "CoreTypes.h"
#include "Templates/Bit.h"

namespace Marlin
{
/// An IEEE 754 binary16 value, for storage; convert to `float32` for math.
///
/// Conversions round to nearest even. On x64 they use the F16C instructions.
///
/// # Performance
/// On x64, conversion uses F16C. Converting to `float32` is exact because every binary16 value is representable as a
/// `float32`.
///
/// # Examples
///
/// ```cpp
/// const Float16 half(1.5f);
/// const uint16 bits = half.Bits; // -> 0x3E00
/// const float32 value = Float16::FromBits(0x3C00).ToFloat(); // -> 1.0
/// ```
struct Float16
{
    /// The raw IEEE 754 binary16 bits. The default bits, 0, encode positive zero.
    uint16 Bits = 0;

    /// Creates positive zero.
    constexpr Float16() = default;

    /// Converts `value` to binary16, rounding to nearest even.
    ///
    /// Values that overflow round to infinity; values below half the smallest subnormal round to zero, and values
    /// in the subnormal range round to the nearest subnormal. All rounding is to nearest even.
    constexpr explicit Float16(float32 value) : Bits(BitCast<uint16>(static_cast<_Float16>(value)))
    {
    }

    /// Creates a value from raw bits without conversion.
    [[nodiscard]] static constexpr Float16 FromBits(uint16 bits)
    {
        Float16 result;
        result.Bits = bits;
        return result;
    }

    /// Converts to `float32`; the result is exact.
    [[nodiscard]] constexpr float32 ToFloat() const
    {
        return static_cast<float32>(BitCast<_Float16>(Bits));
    }

    /// Converts to `float32`, as `ToFloat`.
    constexpr explicit operator float32() const
    {
        return ToFloat();
    }

    /// Compares the raw bits exactly; NaNs with equal bits compare equal.
    constexpr bool operator==(const Float16 &) const = default;

    /// The largest finite binary16 value, 65504, as a `float32`.
    static constexpr float32 Max = 65504.0f;
};

static_assert(sizeof(Float16) == 2);
} // namespace Marlin
