//! XXH3 by Yann Collet (https://github.com/Cyan4973/xxHash), BSD 2-clause license; bit-exact with XXH3_64bits and
//! XXH3_128bits with the default secret.
//!
//! Stable across versions and platforms, so it is fit for persisted keys.

#pragma once

#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Hash/HashBytes.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace Private::Xxh3
{
inline constexpr uint8 Secret[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4,
    0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e,
    0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21, 0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43,
    0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb,
    0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19,
    0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8, 0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7,
    0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78,
    0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16,
    0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f,
    0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

inline constexpr usize SecretSize = sizeof(Secret);
inline constexpr usize SecretSizeMin = 136;
inline constexpr usize StripeLength = 64;
inline constexpr usize SecretConsumeRate = 8;
inline constexpr usize MidsizeStartOffset = 3;
inline constexpr usize MidsizeLastOffset = 17;
inline constexpr usize LastAccumulatorStart = 7;
inline constexpr usize MergeAccumulatorsStart = 11;

/// PRIME32_1..3 and PRIME64_1..5 of the reference.
inline constexpr uint32 Prime32A = 0x9E3779B1u;
inline constexpr uint32 Prime32B = 0x85EBCA77u;
inline constexpr uint32 Prime32C = 0xC2B2AE3Du;
inline constexpr uint64 Prime64A = 0x9E3779B185EBCA87ull;
inline constexpr uint64 Prime64B = 0xC2B2AE3D27D4EB4Full;
inline constexpr uint64 Prime64C = 0x165667B19E3779F9ull;
inline constexpr uint64 Prime64D = 0x85EBCA77C2B2AE63ull;
inline constexpr uint64 Prime64E = 0x27D4EB2F165667C5ull;
inline constexpr uint64 PrimeMx1 = 0x165667919E3779F9ull;
inline constexpr uint64 PrimeMx2 = 0x9FB21C651E98DF25ull;

constexpr uint64 Avalanche64(uint64 hash) noexcept
{
    hash ^= hash >> 33;
    hash *= Prime64B;
    hash ^= hash >> 29;
    hash *= Prime64C;
    hash ^= hash >> 32;
    return hash;
}

constexpr uint64 Avalanche(uint64 hash) noexcept
{
    hash ^= hash >> 37;
    hash *= PrimeMx1;
    hash ^= hash >> 32;
    return hash;
}

constexpr uint64 Rrmxmx(uint64 hash, uint64 length) noexcept
{
    hash ^= RotateLeft(hash, 49) ^ RotateLeft(hash, 24);
    hash *= PrimeMx2;
    hash ^= (hash >> 35) + length;
    hash *= PrimeMx2;
    return hash ^ (hash >> 28);
}

constexpr uint64 MultiplyFold64(uint64 a, uint64 b) noexcept
{
    const Product128 product = Multiply128(a, b);
    return product.Low ^ product.High;
}

template <ByteLike TByte, ByteLike TSecret>
constexpr uint64 Mix16(const TByte *input, const TSecret *secret, uint64 seed) noexcept
{
    return MultiplyFold64(ReadLe64(input) ^ (ReadLe64(secret) + seed),
                          ReadLe64(input + 8) ^ (ReadLe64(secret + 8) - seed));
}

template <ByteLike TByte, ByteLike TSecret>
constexpr Hash128 Mix32(Hash128 accumulator, const TByte *input1, const TByte *input2, const TSecret *secret,
                        uint64 seed) noexcept
{
    accumulator.Low += Mix16(input1, secret, seed);
    accumulator.Low ^= ReadLe64(input2) + ReadLe64(input2 + 8);
    accumulator.High += Mix16(input2, secret + 16, seed);
    accumulator.High ^= ReadLe64(input1) + ReadLe64(input1 + 8);
    return accumulator;
}

struct Accumulators
{
    uint64 Values[8] = {Prime32C, Prime64A, Prime64B, Prime64C, Prime64D, Prime32B, Prime64E, Prime32A};
};

template <ByteLike TByte, ByteLike TSecret>
constexpr void Accumulate512(Accumulators &accumulators, const TByte *input, const TSecret *secret) noexcept
{
    for (usize i = 0; i < 8; ++i)
    {
        const uint64 value = ReadLe64(input + (8 * i));
        const uint64 key = value ^ ReadLe64(secret + (8 * i));
        accumulators.Values[i ^ 1] += value;
        accumulators.Values[i] += (key & 0xFFFFFFFFull) * (key >> 32);
    }
}

template <ByteLike TSecret> constexpr void Scramble(Accumulators &accumulators, const TSecret *secret) noexcept
{
    for (usize i = 0; i < 8; ++i)
    {
        uint64 value = accumulators.Values[i];
        value ^= value >> 47;
        value ^= ReadLe64(secret + (8 * i));
        value *= Prime32A;
        accumulators.Values[i] = value;
    }
}

using UInt64x4 = uint64 __attribute__((ext_vector_type(4)));

MARLIN_FORCEINLINE UInt64x4 LoadVector(const void *data) noexcept
{
    UInt64x4 value;
    MemCopy(&value, data, sizeof(value));
    return value;
}

/// One stripe on two four-lane halves: the scalar Accumulate512 with lanes i and i ^ 1 swapped by shuffles.
MARLIN_FORCEINLINE void AccumulateVector(UInt64x4 &low, UInt64x4 &high, const void *input, const void *secret) noexcept
{
    const auto *bytes = static_cast<const uint8 *>(input);
    const auto *keys = static_cast<const uint8 *>(secret);
    const UInt64x4 dataLow = LoadVector(bytes);
    const UInt64x4 dataHigh = LoadVector(bytes + 32);
    const UInt64x4 keyLow = dataLow ^ LoadVector(keys);
    const UInt64x4 keyHigh = dataHigh ^ LoadVector(keys + 32);
    low += (keyLow & 0xFFFFFFFFull) * (keyLow >> 32) + __builtin_shufflevector(dataLow, dataLow, 1, 0, 3, 2);
    high += (keyHigh & 0xFFFFFFFFull) * (keyHigh >> 32) + __builtin_shufflevector(dataHigh, dataHigh, 1, 0, 3, 2);
}

MARLIN_FORCEINLINE void ScrambleVector(UInt64x4 &low, UInt64x4 &high, const void *secret) noexcept
{
    const auto *keys = static_cast<const uint8 *>(secret);
    low = ((low ^ (low >> 47)) ^ LoadVector(keys)) * Prime32A;
    high = ((high ^ (high >> 47)) ^ LoadVector(keys + 32)) * Prime32A;
}

/// HashLong at run time, with the accumulators in vector registers.
inline Accumulators HashLongVector(const void *data, usize length, const void *secretData, usize secretSize) noexcept
{
    const auto *input = static_cast<const uint8 *>(data);
    const auto *secret = static_cast<const uint8 *>(secretData);
    Accumulators accumulators;
    UInt64x4 low = LoadVector(accumulators.Values);
    UInt64x4 high = LoadVector(accumulators.Values + 4);
    const usize stripesPerBlock = (secretSize - StripeLength) / SecretConsumeRate;
    const usize blockLength = StripeLength * stripesPerBlock;
    const usize blocks = (length - 1) / blockLength;
    for (usize block = 0; block < blocks; ++block)
    {
        const uint8 *blockInput = input + (block * blockLength);
        for (usize stripe = 0; stripe < stripesPerBlock; ++stripe)
        {
            AccumulateVector(low, high, blockInput + (stripe * StripeLength), secret + (stripe * SecretConsumeRate));
        }
        ScrambleVector(low, high, secret + secretSize - StripeLength);
    }
    const usize stripes = ((length - 1) - (blockLength * blocks)) / StripeLength;
    for (usize stripe = 0; stripe < stripes; ++stripe)
    {
        AccumulateVector(low, high, input + (blocks * blockLength) + (stripe * StripeLength),
                         secret + (stripe * SecretConsumeRate));
    }
    AccumulateVector(low, high, input + length - StripeLength,
                     secret + secretSize - StripeLength - LastAccumulatorStart);
    MemCopy(accumulators.Values, &low, sizeof(low));
    MemCopy(accumulators.Values + 4, &high, sizeof(high));
    return accumulators;
}

template <ByteLike TByte, ByteLike TSecret>
constexpr Accumulators HashLong(const TByte *input, usize length, const TSecret *secret, usize secretSize) noexcept
{
    if (!__builtin_is_constant_evaluated())
    {
        return HashLongVector(input, length, secret, secretSize);
    }
    Accumulators accumulators;
    const usize stripesPerBlock = (secretSize - StripeLength) / SecretConsumeRate;
    const usize blockLength = StripeLength * stripesPerBlock;
    const usize blocks = (length - 1) / blockLength;
    for (usize block = 0; block < blocks; ++block)
    {
        for (usize stripe = 0; stripe < stripesPerBlock; ++stripe)
        {
            Accumulate512(accumulators, input + (block * blockLength) + (stripe * StripeLength),
                          secret + (stripe * SecretConsumeRate));
        }
        Scramble(accumulators, secret + secretSize - StripeLength);
    }
    const usize stripes = ((length - 1) - (blockLength * blocks)) / StripeLength;
    for (usize stripe = 0; stripe < stripes; ++stripe)
    {
        Accumulate512(accumulators, input + (blocks * blockLength) + (stripe * StripeLength),
                      secret + (stripe * SecretConsumeRate));
    }
    Accumulate512(accumulators, input + length - StripeLength,
                  secret + secretSize - StripeLength - LastAccumulatorStart);
    return accumulators;
}

template <ByteLike TSecret>
constexpr uint64 MergeAccumulators(const Accumulators &accumulators, const TSecret *secret, uint64 start) noexcept
{
    uint64 result = start;
    for (usize i = 0; i < 4; ++i)
    {
        result += MultiplyFold64(accumulators.Values[2 * i] ^ ReadLe64(secret + (16 * i)),
                                 accumulators.Values[(2 * i) + 1] ^ ReadLe64(secret + (16 * i) + 8));
    }
    return Avalanche(result);
}

struct SeededSecret
{
    uint8 Bytes[SecretSize] = {};
};

constexpr SeededSecret MakeSecret(uint64 seed) noexcept
{
    SeededSecret result;
    for (usize i = 0; i < SecretSize / 16; ++i)
    {
        const uint64 low = ReadLe64(Secret + (16 * i)) + seed;
        const uint64 high = ReadLe64(Secret + (16 * i) + 8) - seed;
        for (usize b = 0; b < 8; ++b)
        {
            result.Bytes[(16 * i) + b] = static_cast<uint8>(low >> (8 * b));
            result.Bytes[(16 * i) + 8 + b] = static_cast<uint8>(high >> (8 * b));
        }
    }
    return result;
}

template <ByteLike TByte> constexpr uint64 Hash64Short(const TByte *input, usize length, uint64 seed) noexcept
{
    const uint8 *secret = Secret;
    if (length > 8)
    {
        const uint64 bitflip1 = (ReadLe64(secret + 24) ^ ReadLe64(secret + 32)) + seed;
        const uint64 bitflip2 = (ReadLe64(secret + 40) ^ ReadLe64(secret + 48)) - seed;
        const uint64 low = ReadLe64(input) ^ bitflip1;
        const uint64 high = ReadLe64(input + length - 8) ^ bitflip2;
        const uint64 accumulator = length + ByteSwap(low) + high + MultiplyFold64(low, high);
        return Avalanche(accumulator);
    }
    if (length >= 4)
    {
        seed ^= static_cast<uint64>(ByteSwap(static_cast<uint32>(seed))) << 32;
        const uint32 input1 = ReadLe32(input);
        const uint32 input2 = ReadLe32(input + length - 4);
        const uint64 bitflip = (ReadLe64(secret + 8) ^ ReadLe64(secret + 16)) - seed;
        const uint64 input64 = input2 + (static_cast<uint64>(input1) << 32);
        return Rrmxmx(input64 ^ bitflip, length);
    }
    if (length > 0)
    {
        const uint32 combined = (static_cast<uint32>(ReadByte(input)) << 16) |
                                (static_cast<uint32>(ReadByte(input + (length >> 1))) << 24) |
                                static_cast<uint32>(ReadByte(input + length - 1)) | (static_cast<uint32>(length) << 8);
        const uint64 bitflip = (ReadLe32(secret) ^ ReadLe32(secret + 4)) + seed;
        return Avalanche64(static_cast<uint64>(combined) ^ bitflip);
    }
    return Avalanche64(seed ^ (ReadLe64(secret + 56) ^ ReadLe64(secret + 64)));
}

template <ByteLike TByte> constexpr uint64 Hash64Medium(const TByte *input, usize length, uint64 seed) noexcept
{
    const uint8 *secret = Secret;
    uint64 accumulator = length * Prime64A;
    if (length <= 128)
    {
        if (length > 32)
        {
            if (length > 64)
            {
                if (length > 96)
                {
                    accumulator += Mix16(input + 48, secret + 96, seed);
                    accumulator += Mix16(input + length - 64, secret + 112, seed);
                }
                accumulator += Mix16(input + 32, secret + 64, seed);
                accumulator += Mix16(input + length - 48, secret + 80, seed);
            }
            accumulator += Mix16(input + 16, secret + 32, seed);
            accumulator += Mix16(input + length - 32, secret + 48, seed);
        }
        accumulator += Mix16(input, secret, seed);
        accumulator += Mix16(input + length - 16, secret + 16, seed);
        return Avalanche(accumulator);
    }
    const usize rounds = length / 16;
    for (usize i = 0; i < 8; ++i)
    {
        accumulator += Mix16(input + (16 * i), secret + (16 * i), seed);
    }
    uint64 accumulatorEnd = Mix16(input + length - 16, secret + SecretSizeMin - MidsizeLastOffset, seed);
    accumulator = Avalanche(accumulator);
    for (usize i = 8; i < rounds; ++i)
    {
        accumulatorEnd += Mix16(input + (16 * i), secret + (16 * (i - 8)) + MidsizeStartOffset, seed);
    }
    return Avalanche(accumulator + accumulatorEnd);
}

template <ByteLike TByte> constexpr Hash128 Hash128Short(const TByte *input, usize length, uint64 seed) noexcept
{
    const uint8 *secret = Secret;
    if (length > 8)
    {
        const uint64 bitflipLow = (ReadLe64(secret + 32) ^ ReadLe64(secret + 40)) - seed;
        const uint64 bitflipHigh = (ReadLe64(secret + 48) ^ ReadLe64(secret + 56)) + seed;
        const uint64 inputLow = ReadLe64(input);
        uint64 inputHigh = ReadLe64(input + length - 8);
        Product128 m = Multiply128(inputLow ^ inputHigh ^ bitflipLow, Prime64A);
        m.Low += static_cast<uint64>(length - 1) << 54;
        inputHigh ^= bitflipHigh;
        m.High += inputHigh + (static_cast<uint64>(static_cast<uint32>(inputHigh)) * (Prime32B - 1));
        m.Low ^= ByteSwap(m.High);
        Product128 h = Multiply128(m.Low, Prime64B);
        h.High += m.High * Prime64B;
        return {Avalanche(h.Low), Avalanche(h.High)};
    }
    if (length >= 4)
    {
        seed ^= static_cast<uint64>(ByteSwap(static_cast<uint32>(seed))) << 32;
        const uint32 inputLow = ReadLe32(input);
        const uint32 inputHigh = ReadLe32(input + length - 4);
        const uint64 input64 = inputLow + (static_cast<uint64>(inputHigh) << 32);
        const uint64 bitflip = (ReadLe64(secret + 16) ^ ReadLe64(secret + 24)) + seed;
        Product128 m = Multiply128(input64 ^ bitflip, Prime64A + (length << 2));
        m.High += m.Low << 1;
        m.Low ^= m.High >> 3;
        m.Low ^= m.Low >> 35;
        m.Low *= PrimeMx2;
        m.Low ^= m.Low >> 28;
        return {m.Low, Avalanche(m.High)};
    }
    if (length > 0)
    {
        const uint32 combinedLow = (static_cast<uint32>(ReadByte(input)) << 16) |
                                   (static_cast<uint32>(ReadByte(input + (length >> 1))) << 24) |
                                   static_cast<uint32>(ReadByte(input + length - 1)) |
                                   (static_cast<uint32>(length) << 8);
        const uint32 combinedHigh = RotateLeft(ByteSwap(combinedLow), 13);
        const uint64 bitflipLow = (ReadLe32(secret) ^ ReadLe32(secret + 4)) + seed;
        const uint64 bitflipHigh = (ReadLe32(secret + 8) ^ ReadLe32(secret + 12)) - seed;
        return {
            Avalanche64(static_cast<uint64>(combinedLow) ^ bitflipLow),
            Avalanche64(static_cast<uint64>(combinedHigh) ^ bitflipHigh),
        };
    }
    return {
        Avalanche64(seed ^ ReadLe64(secret + 64) ^ ReadLe64(secret + 72)),
        Avalanche64(seed ^ ReadLe64(secret + 80) ^ ReadLe64(secret + 88)),
    };
}

constexpr Hash128 Finish128(Hash128 accumulator, usize length, uint64 seed) noexcept
{
    const uint64 low = accumulator.Low + accumulator.High;
    const uint64 high = (accumulator.Low * Prime64A) + (accumulator.High * Prime64D) + ((length - seed) * Prime64B);
    return {Avalanche(low), 0 - Avalanche(high)};
}

template <ByteLike TByte> constexpr Hash128 Hash128Medium(const TByte *input, usize length, uint64 seed) noexcept
{
    const uint8 *secret = Secret;
    Hash128 accumulator = {length * Prime64A, 0};
    if (length <= 128)
    {
        for (usize i = (length - 1) / 32 + 1; i-- > 0;)
        {
            accumulator =
                Mix32(accumulator, input + (16 * i), input + length - (16 * (i + 1)), secret + (32 * i), seed);
        }
        return Finish128(accumulator, length, seed);
    }
    for (usize i = 32; i < 160; i += 32)
    {
        accumulator = Mix32(accumulator, input + i - 32, input + i - 16, secret + i - 32, seed);
    }
    accumulator = {Avalanche(accumulator.Low), Avalanche(accumulator.High)};
    for (usize i = 160; i <= length; i += 32)
    {
        accumulator = Mix32(accumulator, input + i - 32, input + i - 16, secret + MidsizeStartOffset + i - 160, seed);
    }
    accumulator = Mix32(accumulator, input + length - 16, input + length - 32,
                        secret + SecretSizeMin - MidsizeLastOffset - 16, 0 - seed);
    return Finish128(accumulator, length, seed);
}
} // namespace Private::Xxh3

/// Hashes `size` bytes at `data` with `seed`, as XXH3-64.
///
/// # Performance
/// O(size); does not allocate.
template <ByteLike TByte>
[[nodiscard]] constexpr uint64 Xxh3Hash64(const TByte *data, usize size, uint64 seed = 0) noexcept
{
    using namespace Private::Xxh3;
    if (size <= 16)
    {
        return Hash64Short(data, size, seed);
    }
    if (size <= 240)
    {
        return Hash64Medium(data, size, seed);
    }
    if (seed == 0)
    {
        return MergeAccumulators(HashLong(data, size, Secret, SecretSize), Secret + MergeAccumulatorsStart,
                                 size * Prime64A);
    }
    const SeededSecret secret = MakeSecret(seed);
    return MergeAccumulators(HashLong(data, size, secret.Bytes, SecretSize), secret.Bytes + MergeAccumulatorsStart,
                             size * Prime64A);
}

/// Hashes `size` bytes at `data` with `seed`, as XXH3-128.
///
/// # Performance
/// O(size); does not allocate.
template <ByteLike TByte>
[[nodiscard]] constexpr Hash128 Xxh3Hash128(const TByte *data, usize size, uint64 seed = 0) noexcept
{
    using namespace Private::Xxh3;
    if (size <= 16)
    {
        return Hash128Short(data, size, seed);
    }
    if (size <= 240)
    {
        return Hash128Medium(data, size, seed);
    }
    const SeededSecret seeded = seed == 0 ? SeededSecret{} : MakeSecret(seed);
    const uint8 *secret = seed == 0 ? Secret : seeded.Bytes;
    const Accumulators accumulators = HashLong(data, size, secret, SecretSize);
    return {
        MergeAccumulators(accumulators, secret + MergeAccumulatorsStart, size * Prime64A),
        MergeAccumulators(accumulators, secret + SecretSize - StripeLength - MergeAccumulatorsStart,
                          ~(size * Prime64B)),
    };
}

/// Hashes `size` bytes at `data`; see the byte overload.
[[nodiscard]] inline uint64 Xxh3Hash64(const void *data, usize size, uint64 seed = 0) noexcept
{
    return Xxh3Hash64(static_cast<const uint8 *>(data), size, seed);
}

/// Hashes `size` bytes at `data`; see the byte overload.
[[nodiscard]] inline Hash128 Xxh3Hash128(const void *data, usize size, uint64 seed = 0) noexcept
{
    return Xxh3Hash128(static_cast<const uint8 *>(data), size, seed);
}
} // namespace Marlin
