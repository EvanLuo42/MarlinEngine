#include "Testing/Testing.h"

#include "Hash/Hash.h"
#include "Hash/Hash128.h"
#include "Hash/RapidHash.h"
#include "Hash/Sha1.h"
#include "Hash/Xxh3.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Bit.h"
#include "Templates/NumericLimits.h"
#include "Templates/TypeTraits.h"

namespace Marlin::Tests
{
static_assert(IsSame<RemoveCvRef<const int32 &>, int32>);
static_assert(NumericLimits<int32>::Max() == 2147483647 && NumericLimits<int32>::Min() == -2147483647 - 1);
static_assert(NumericLimits<uint16>::Max() == 65535 && NumericLimits<int8>::Min() == -128);
static_assert(NumericLimits<int64>::Max() == 0x7FFFFFFFFFFFFFFFll);
static_assert(PopCount(0xF0u) == 4 && CountLeadingZeros(uint8{1}) == 7 && CountTrailingZeros(uint64{8}) == 3);
static_assert(RoundUpToPowerOfTwo(17u) == 32u && RoundUpToPowerOfTwo(32u) == 32u && FloorLog2(1024u) == 10);
static_assert(AlignUp<usize>(13, 8) == 16 && RotateLeft(0x80000000u, 1) == 1u && ByteSwap(0x1234u) == 0x34120000u);
static_assert(Xxh3Hash64("", 0) == 0x2D06800538D394C2ull);
static_assert(Xxh3Hash128("", 0) == Hash128{0x6001C324468D497Full, 0x99AA06D3014798D8ull});
static_assert(RapidHash("hello", 5) != RapidHash("hellp", 5));
static_assert(Integral<uint8> && !Integral<float32> && Invocable<int32 (*)(int32), int32>);

namespace
{
constexpr const char *Fox = "The quick brown fox jumps over the lazy dog";

struct SanityBuffer
{
    uint8 Bytes[3000] = {};

    SanityBuffer()
    {
        uint64 generator = 2654435761u;
        for (uint8 &byte : Bytes)
        {
            byte = static_cast<uint8>(generator >> 56);
            generator *= 11400714785074694797ull;
        }
    }
};
} // namespace

MARLIN_TEST(Hash, KnownValues)
{
    const SanityBuffer buffer;
    MARLIN_EXPECT(Xxh3Hash64(buffer.Bytes, 2367) == 0xCB37AEB9E5D361EDull);
    MARLIN_EXPECT(Xxh3Hash64(buffer.Bytes, 2367, 11400714785074694797ull) == 0xD2DB3415B942B42Aull);
    const Hash128 vector = Xxh3Hash128(buffer.Bytes, 2367, 2654435761u);
    MARLIN_EXPECT(vector.Low == 0x6F5360AE69C2F406ull && vector.High == 0xD23AAE4B76C31ECBull);
    MARLIN_EXPECT(GetHash(0.0f) == GetHash(-0.0f) && GetHash(1) != GetHash(2) && GetHash(Hash128{1, 2}) == 3);
}

/// Every length up to 3000 bytes at every alignment mod 7 and five seeds, folded into one value. The pinned values
/// here were taken from the implementation that matched the reference xxHash and rapidhash on all of these inputs.
MARLIN_TEST(Hash, AllLengthsDigest)
{
    const SanityBuffer buffer;
    const uint64 seeds[] = {0, 1, 2654435761u, 11400714785074694797ull, ~0ull};
    uint64 digest = 0;
    for (const uint64 seed : seeds)
    {
        for (usize length = 0; length <= sizeof(buffer.Bytes); ++length)
        {
            const uint8 *data = buffer.Bytes + (length % 7);
            const usize size = length + (length % 7) <= sizeof(buffer.Bytes) ? length : sizeof(buffer.Bytes) - 7;
            const Hash128 wide = Xxh3Hash128(data, size, seed);
            digest = HashCombine(digest, Xxh3Hash64(data, size, seed));
            digest = HashCombine(digest, wide.Low ^ RotateLeft(wide.High, 17));
            digest = HashCombine(digest, RapidHash(data, size, seed));
        }
    }
    MARLIN_EXPECTF(digest == 0x93C14C0135480DAEull, "digest 0x{0:X16}", digest);
}

MARLIN_TEST(Hash, Constexpr)
{
    constexpr uint64 Xxh = Xxh3Hash64(Fox, 43, 7);
    constexpr uint64 Rapid = RapidHash(Fox, 43, 7);
    constexpr Hash128 Wide = Xxh3Hash128(Fox, 43);
    MARLIN_EXPECT(Xxh == Xxh3Hash64(Fox, StringView(Fox).Length(), 7));
    MARLIN_EXPECT(Rapid == RapidHash(Fox, StringView(Fox).Length(), 7));
    MARLIN_EXPECT(Wide == Xxh3Hash128(Fox, StringView(Fox).Length()));
    static_assert(Xxh == 0xCCEFEB269F944E83ull && Rapid == 0x9E78BF347733BD3Cull);
    static_assert(Wide.Low == 0x24A1CC2E3A8A7651ull && Wide.High == 0xDDD650205CA3E7FAull);
}
} // namespace Marlin::Tests

namespace Marlin::Tests
{
MARLIN_TEST(Hash, Sha1)
{
    const auto hex = [](StringView text) {
        uint8 digest[Sha1::DigestSize];
        Sha1::Hash(Span<const uint8>(reinterpret_cast<const uint8 *>(text.Data()), text.Length()), digest);
        char out[Sha1::DigestSize * 2];
        for (usize i = 0; i < Sha1::DigestSize; ++i)
        {
            out[i * 2] = "0123456789abcdef"[digest[i] >> 4];
            out[i * 2 + 1] = "0123456789abcdef"[digest[i] & 0xF];
        }
        return String(StringView(out, sizeof(out)));
    };
    MARLIN_EXPECT(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    MARLIN_EXPECT(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    MARLIN_EXPECT(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                  "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}
} // namespace Marlin::Tests
