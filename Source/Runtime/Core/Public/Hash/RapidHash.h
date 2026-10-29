//! rapidhash V3 by Nicolas De Carli (https://github.com/Nicoshev/rapidhash), MIT license; bit-exact with its
//! default, unprotected variant.
//!
//! The fast hash for in-memory tables; not stable across engine versions.

#pragma once

#include "CoreTypes.h"
#include "Hash/HashBytes.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace Private
{
inline constexpr uint64 RapidSecret[8] = {
    0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull,
    0xa0761d6478bd642full, 0xe7037ed1a0b428dbull, 0x90ed1765281c388cull, 0xaaaaaaaaaaaaaaaaull,
};

constexpr void RapidMultiply(uint64 &a, uint64 &b) noexcept
{
    const Product128 product = Multiply128(a, b);
    a = product.Low;
    b = product.High;
}
} // namespace Private

/// Mixes two 64-bit values into one.
[[nodiscard]] constexpr uint64 RapidMix(uint64 a, uint64 b) noexcept
{
    Private::RapidMultiply(a, b);
    return a ^ b;
}

/// Hashes `size` bytes at `data` with `seed`.
///
/// The result is not stable across engine versions; use [`Xxh3Hash64`] for persisted keys.
///
/// # Performance
/// O(size); does not allocate.
template <ByteLike TByte>
[[nodiscard]] constexpr uint64 RapidHash(const TByte *data, usize size, uint64 seed = 0) noexcept
{
    using Private::ReadByte;
    using Private::ReadLe32;
    using Private::ReadLe64;
    constexpr const uint64 *Secret = Private::RapidSecret;

    const TByte *p = data;
    seed ^= RapidMix(seed ^ Secret[2], Secret[1]);
    uint64 a = 0;
    uint64 b = 0;
    usize i = size;
    if (size <= 16)
    {
        if (size >= 4)
        {
            seed ^= size;
            if (size >= 8)
            {
                a = ReadLe64(p);
                b = ReadLe64(p + size - 8);
            }
            else
            {
                a = ReadLe32(p);
                b = ReadLe32(p + size - 4);
            }
        }
        else if (size > 0)
        {
            a = (static_cast<uint64>(ReadByte(p)) << 45) | ReadByte(p + size - 1);
            b = ReadByte(p + (size >> 1));
        }
    }
    else
    {
        if (size > 112)
        {
            uint64 see1 = seed;
            uint64 see2 = seed;
            uint64 see3 = seed;
            uint64 see4 = seed;
            uint64 see5 = seed;
            uint64 see6 = seed;
            const auto round = [&](const TByte *q) {
                seed = RapidMix(ReadLe64(q) ^ Secret[0], ReadLe64(q + 8) ^ seed);
                see1 = RapidMix(ReadLe64(q + 16) ^ Secret[1], ReadLe64(q + 24) ^ see1);
                see2 = RapidMix(ReadLe64(q + 32) ^ Secret[2], ReadLe64(q + 40) ^ see2);
                see3 = RapidMix(ReadLe64(q + 48) ^ Secret[3], ReadLe64(q + 56) ^ see3);
                see4 = RapidMix(ReadLe64(q + 64) ^ Secret[4], ReadLe64(q + 72) ^ see4);
                see5 = RapidMix(ReadLe64(q + 80) ^ Secret[5], ReadLe64(q + 88) ^ see5);
                see6 = RapidMix(ReadLe64(q + 96) ^ Secret[6], ReadLe64(q + 104) ^ see6);
            };
            while (i > 224)
            {
                round(p);
                round(p + 112);
                p += 224;
                i -= 224;
            }
            if (i > 112)
            {
                round(p);
                p += 112;
                i -= 112;
            }
            seed ^= see1;
            see2 ^= see3;
            see4 ^= see5;
            seed ^= see6;
            see2 ^= see4;
            seed ^= see2;
        }
        if (i > 16)
        {
            seed = RapidMix(ReadLe64(p) ^ Secret[2], ReadLe64(p + 8) ^ seed);
            if (i > 32)
            {
                seed = RapidMix(ReadLe64(p + 16) ^ Secret[2], ReadLe64(p + 24) ^ seed);
                if (i > 48)
                {
                    seed = RapidMix(ReadLe64(p + 32) ^ Secret[1], ReadLe64(p + 40) ^ seed);
                    if (i > 64)
                    {
                        seed = RapidMix(ReadLe64(p + 48) ^ Secret[1], ReadLe64(p + 56) ^ seed);
                        if (i > 80)
                        {
                            seed = RapidMix(ReadLe64(p + 64) ^ Secret[2], ReadLe64(p + 72) ^ seed);
                            if (i > 96)
                            {
                                seed = RapidMix(ReadLe64(p + 80) ^ Secret[1], ReadLe64(p + 88) ^ seed);
                            }
                        }
                    }
                }
            }
        }
        a = ReadLe64(p + i - 16) ^ i;
        b = ReadLe64(p + i - 8);
    }
    a ^= Secret[1];
    b ^= seed;
    Private::RapidMultiply(a, b);
    return RapidMix(a ^ Secret[7], b ^ Secret[1] ^ i);
}

/// Hashes `size` bytes at `data` with `seed`.
[[nodiscard]] inline uint64 RapidHash(const void *data, usize size, uint64 seed = 0) noexcept
{
    return RapidHash(static_cast<const uint8 *>(data), size, seed);
}
} // namespace Marlin
