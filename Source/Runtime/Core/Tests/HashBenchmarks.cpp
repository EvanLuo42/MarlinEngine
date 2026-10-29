#include "Testing/Testing.h"

#include "Containers/HashMap.h"
#include "HAL/PlatformTime.h"
#include "Hash/RapidHash.h"
#include "Hash/Xxh3.h"
#include "Memory/Memory.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
volatile uint64 _hashSink = 0;

template <typename TFunction> float64 BestOf(TFunction function)
{
    float64 best = 1e30;
    for (int32 i = 0; i < 7; ++i)
    {
        const float64 start = PlatformTime::Seconds();
        function();
        const float64 elapsed = PlatformTime::Seconds() - start;
        best = elapsed < best ? elapsed : best;
    }
    return best;
}
} // namespace

MARLIN_TEST(Benchmark, Hash, Benchmark)
{
    constexpr usize Size = usize{16} << 20;
    auto *buffer = static_cast<uint8 *>(Memory::Allocate(Size));
    for (usize i = 0; i < Size; ++i)
    {
        buffer[i] = static_cast<uint8>(i * 131 + (i >> 7));
    }
    const float64 xxh64 = BestOf([&] { _hashSink = _hashSink + Xxh3Hash64(buffer, Size); });
    const float64 xxh128 = BestOf([&] { _hashSink = _hashSink + Xxh3Hash128(buffer, Size).Low; });
    const float64 rapid = BestOf([&] { _hashSink = _hashSink + RapidHash(buffer, Size); });
    Testing::Print(Format("XXH3-64  16 MiB: {0:F2} GB/s\n", Size / xxh64 / 1e9));
    Testing::Print(Format("XXH3-128 16 MiB: {0:F2} GB/s\n", Size / xxh128 / 1e9));
    Testing::Print(Format("rapidhash 16 MiB: {0:F2} GB/s\n", Size / rapid / 1e9));

    for (const usize length : {usize{8}, usize{32}, usize{100}, usize{240}, usize{1024}})
    {
        const float64 shortXxh = BestOf([&] {
            uint64 sum = 0;
            for (usize i = 0; i < 1000000; ++i)
            {
                sum += Xxh3Hash64(buffer + (i & 1023), length);
            }
            _hashSink = _hashSink + sum;
        });
        const float64 shortRapid = BestOf([&] {
            uint64 sum = 0;
            for (usize i = 0; i < 1000000; ++i)
            {
                sum += RapidHash(buffer + (i & 1023), length);
            }
            _hashSink = _hashSink + sum;
        });
        Testing::Print(Format("{0,5} bytes: XXH3 {1,6:F2} ns  rapidhash {2,6:F2} ns\n", length, shortXxh * 1000,
                              shortRapid * 1000));
    }
    Memory::Free(buffer);

    static uint32 keys[1 << 20];
    uint32 state = 7;
    for (uint32 &key : keys)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        key = state;
    }
    HashMap<uint32, uint32> map;
    for (const uint32 key : keys)
    {
        map.Set(key, key);
    }
    const float64 hit = BestOf([&] {
        uint64 sum = 0;
        for (const uint32 key : keys)
        {
            sum += *map.Find(key);
        }
        _hashSink = _hashSink + sum;
    });
    const float64 miss = BestOf([&] {
        uint64 found = 0;
        for (const uint32 key : keys)
        {
            found += map.Find(key ^ 0x80000001u) != nullptr;
        }
        _hashSink = _hashSink + found;
    });
    Testing::Print(
        Format("HashMap 1M find hit {0:F2} ns, miss {1:F2} ns\n", hit * 1e9 / (1 << 20), miss * 1e9 / (1 << 20)));
}
} // namespace Marlin::Tests
