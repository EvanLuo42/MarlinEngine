#include "Testing/Testing.h"

#include "Lz4Codec/Lz4Codec.h"
#include "Memory/MemoryOps.h"

namespace Marlin::Tests
{
MARLIN_TEST(Lz4Codec, RoundTrip)
{
    CompressionCodecSet codecs;
    codecs.Build({});
    const CompressionCodec *codec = codecs.Find("lz4");
    MARLIN_REQUIRE(codec != nullptr && codec->GetName() == "Lz4");

    List<uint8> repetitive;
    for (uint32 i = 0; i < 200000; ++i)
    {
        repetitive.Add(static_cast<uint8>((i % 251) ^ (i / 1000)));
    }
    for (const int32 level : {0, 9})
    {
        List<uint8> compressed;
        MARLIN_REQUIRE(codec->Compress(repetitive.AsSpan(), level, compressed));
        MARLIN_EXPECT(compressed.Count() < repetitive.Count() / 4);
        List<uint8> restored;
        restored.ResizeUninitialized(repetitive.Count());
        MARLIN_REQUIRE(codec->Decompress(compressed.AsSpan(), restored.AsSpan()));
        MARLIN_EXPECT(MemCompare(restored.Data(), repetitive.Data(), repetitive.Count()) == 0);
        compressed[compressed.Count() / 2] ^= 0x55;
        restored.ResizeUninitialized(repetitive.Count() - 1);
        MARLIN_EXPECT(!codec->Decompress(compressed.AsSpan(), restored.AsSpan()));
    }

    List<uint8> noise;
    uint32 state = 12345;
    for (uint32 i = 0; i < 4096; ++i)
    {
        state = state * 1664525u + 1013904223u;
        noise.Add(static_cast<uint8>(state >> 24));
    }
    List<uint8> compressed;
    MARLIN_EXPECT(!codec->Compress(noise.AsSpan(), 0, compressed) && compressed.IsEmpty());
}

MARLIN_TEST(Lz4Codec, ReusedContextsGiveTheSameOutput)
{
    CompressionCodecSet codecs;
    codecs.Build({});
    const CompressionCodec *codec = codecs.Find("lz4");
    MARLIN_REQUIRE(codec != nullptr);
    // The thread keeps its contexts between calls: interleaved calls on different inputs and levels must not leak
    // state into each other.
    List<uint8> first;
    List<uint8> second;
    for (uint32 i = 0; i < 50000; ++i)
    {
        first.Add(static_cast<uint8>(i % 97));
        second.Add(static_cast<uint8>((i / 7) % 13));
    }
    for (const int32 level : {0, 9})
    {
        List<uint8> once;
        List<uint8> other;
        List<uint8> again;
        MARLIN_REQUIRE(codec->Compress(first.AsSpan(), level, once));
        MARLIN_REQUIRE(codec->Compress(second.AsSpan(), level, other));
        MARLIN_REQUIRE(codec->Compress(first.AsSpan(), level, again));
        MARLIN_EXPECT(once == again);
        List<uint8> restored;
        restored.ResizeUninitialized(second.Count());
        MARLIN_REQUIRE(codec->Decompress(other.AsSpan(), restored.AsSpan()));
        MARLIN_EXPECT(restored == second);
        restored.ResizeUninitialized(first.Count());
        MARLIN_REQUIRE(codec->Decompress(again.AsSpan(), restored.AsSpan()));
        MARLIN_EXPECT(restored == first);
    }
}
} // namespace Marlin::Tests
