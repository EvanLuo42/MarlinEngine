//! A 128-bit hash value with value semantics and ordering.

#pragma once

#include "CoreTypes.h"

#include <compare>

namespace Marlin
{
/// A 128-bit hash, split into low and high halves.
struct Hash128
{
    /// The low 64 bits.
    uint64 Low = 0;
    /// The high 64 bits.
    uint64 High = 0;

    /// Returns whether both halves are equal.
    constexpr bool operator==(const Hash128 &) const = default;
    /// Orders by `High`, then by `Low`.
    constexpr std::strong_ordering operator<=>(const Hash128 &other) const
    {
        if (const auto order = High <=> other.High; order != 0)
        {
            return order;
        }
        return Low <=> other.Low;
    }

    /// Returns a 64-bit hash of the value, for use in hash tables.
    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return Low ^ High;
    }
};
} // namespace Marlin
