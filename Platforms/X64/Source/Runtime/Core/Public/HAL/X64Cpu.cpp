#include "HAL/PlatformCpu.h"

#include "Memory/MemoryOps.h"

#define MARLIN_BASELINE_X64 __attribute__((target("arch=x86-64")))

namespace Marlin::PlatformCpu
{
namespace
{
struct CpuidResult
{
    uint32 Eax;
    uint32 Ebx;
    uint32 Ecx;
    uint32 Edx;
};

MARLIN_BASELINE_X64 CpuidResult Cpuid(uint32 leaf, uint32 subleaf)
{
    CpuidResult result = {};
    __asm__ volatile("cpuid"
                     : "=a"(result.Eax), "=b"(result.Ebx), "=c"(result.Ecx), "=d"(result.Edx)
                     : "a"(leaf), "c"(subleaf));
    return result;
}

__attribute__((target("arch=x86-64,xsave"))) uint64 ReadXcr0()
{
    return __builtin_ia32_xgetbv(0);
}

MARLIN_BASELINE_X64 bool HasBits(uint32 value, uint32 bits)
{
    return (value & bits) == bits;
}
} // namespace

MARLIN_BASELINE_X64 bool SupportsRequiredIsa()
{
    if (Cpuid(0, 0).Eax < 7 || Cpuid(0x80000000u, 0).Eax < 0x80000001u)
    {
        return false;
    }
    const CpuidResult features = Cpuid(1, 0);
    constexpr uint32 Sse3 = 1u << 0;
    constexpr uint32 Ssse3 = 1u << 9;
    constexpr uint32 Fma = 1u << 12;
    constexpr uint32 Cx16 = 1u << 13;
    constexpr uint32 Sse41 = 1u << 19;
    constexpr uint32 Sse42 = 1u << 20;
    constexpr uint32 Movbe = 1u << 22;
    constexpr uint32 Popcnt = 1u << 23;
    constexpr uint32 Xsave = 1u << 26;
    constexpr uint32 Osxsave = 1u << 27;
    constexpr uint32 Avx = 1u << 28;
    constexpr uint32 F16c = 1u << 29;
    if (!HasBits(features.Ecx,
                 Sse3 | Ssse3 | Fma | Cx16 | Sse41 | Sse42 | Movbe | Popcnt | Xsave | Osxsave | Avx | F16c))
    {
        return false;
    }
    constexpr uint64 SseAndAvxState = 0x6;
    if ((ReadXcr0() & SseAndAvxState) != SseAndAvxState)
    {
        return false;
    }
    constexpr uint32 Bmi1 = 1u << 3;
    constexpr uint32 Avx2 = 1u << 5;
    constexpr uint32 Bmi2 = 1u << 8;
    constexpr uint32 Lahf = 1u << 0;
    constexpr uint32 Lzcnt = 1u << 5;
    return HasBits(Cpuid(7, 0).Ebx, Bmi1 | Avx2 | Bmi2) && HasBits(Cpuid(0x80000001u, 0).Ecx, Lahf | Lzcnt);
}

usize GetBrand(char *buffer, usize capacity)
{
    if (Cpuid(0x80000000u, 0).Eax < 0x80000004u)
    {
        return 0;
    }
    char brand[49] = {};
    for (uint32 i = 0; i < 3; ++i)
    {
        const CpuidResult part = Cpuid(0x80000002u + i, 0);
        MemCopy(brand + (static_cast<usize>(i) * 16), &part, sizeof(part));
    }
    const char *start = brand;
    while (*start == ' ')
    {
        ++start;
    }
    usize length = 0;
    while (start[length] != '\0')
    {
        ++length;
    }
    while (length > 0 && start[length - 1] == ' ')
    {
        --length;
    }
    if (length < capacity)
    {
        MemCopy(buffer, start, length);
        buffer[length] = '\0';
    }
    return length;
}
} // namespace Marlin::PlatformCpu
