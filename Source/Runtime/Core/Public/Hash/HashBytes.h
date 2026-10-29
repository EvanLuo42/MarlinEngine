//! Byte-level helpers shared by the hash implementations.

#pragma once

#include "CoreTypes.h"
#include "Memory/MemoryOps.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// A byte type a hash can read in a constant expression.
///
/// That is `char`, `signed char`, `unsigned char` or `char8_t`, with or without cv-qualifiers.
template <typename T>
concept ByteLike = IsSame<RemoveCv<T>, char> || IsSame<RemoveCv<T>, signed char> ||
                   IsSame<RemoveCv<T>, unsigned char> || IsSame<RemoveCv<T>, char8_t>;

namespace Private
{
/// Reads a little-endian 64-bit value; works in constant expressions.
template <ByteLike TByte> constexpr uint64 ReadLe64(const TByte *data) noexcept
{
    if (__builtin_is_constant_evaluated())
    {
        uint64 value = 0;
        for (int32 i = 7; i >= 0; --i)
        {
            value = (value << 8) | static_cast<uint8>(data[i]);
        }
        return value;
    }
    uint64 value = 0;
    MemCopy(&value, data, sizeof(value));
    return value;
}

/// Reads a little-endian 32-bit value; works in constant expressions.
template <ByteLike TByte> constexpr uint32 ReadLe32(const TByte *data) noexcept
{
    if (__builtin_is_constant_evaluated())
    {
        uint32 value = 0;
        for (int32 i = 3; i >= 0; --i)
        {
            value = (value << 8) | static_cast<uint8>(data[i]);
        }
        return value;
    }
    uint32 value = 0;
    MemCopy(&value, data, sizeof(value));
    return value;
}

/// Reads one byte.
template <ByteLike TByte> constexpr uint8 ReadByte(const TByte *data) noexcept
{
    return static_cast<uint8>(*data);
}
} // namespace Private
} // namespace Marlin
