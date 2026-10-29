#include "HAL/PlatformTime.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin::PlatformTime
{
uint64 Ticks()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64>(counter.QuadPart);
}

uint64 TicksPerSecond()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<uint64>(frequency.QuadPart);
}

float64 Seconds()
{
    const uint64 ticks = Ticks();
    const uint64 frequency = TicksPerSecond();
    const uint64 seconds = ticks / frequency;
    const uint64 remainder = ticks % frequency;
    return static_cast<float64>(seconds) + static_cast<float64>(remainder) / static_cast<float64>(frequency);
}

int64 UtcNow()
{
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    return Microsoft::FileTimeToTicks(now);
}

int64 LocalUtcOffset()
{
    DYNAMIC_TIME_ZONE_INFORMATION zone;
    const DWORD state = GetDynamicTimeZoneInformation(&zone);
    LONG bias = zone.Bias;
    if (state == TIME_ZONE_ID_DAYLIGHT)
    {
        bias += zone.DaylightBias;
    }
    else if (state == TIME_ZONE_ID_STANDARD)
    {
        bias += zone.StandardBias;
    }
    constexpr int64 TicksPerMinute = 600000000;
    return -static_cast<int64>(bias) * TicksPerMinute;
}
} // namespace Marlin::PlatformTime
