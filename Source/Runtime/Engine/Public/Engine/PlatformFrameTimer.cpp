#include "Engine/FramePacer.h"

#include "HAL/PlatformThread.h"

namespace Marlin
{
PlatformFrameTimer::PlatformFrameTimer() = default;

PlatformFrameTimer::~PlatformFrameTimer() = default;

void PlatformFrameTimer::Sleep(float64 seconds)
{
    // Whole milliseconds, rounded down so the sleep ends before the pacer's deadline.
    const auto milliseconds = static_cast<uint32>(seconds * 1000);
    if (milliseconds > 0)
    {
        PlatformThread::Sleep(milliseconds);
    }
    else
    {
        PlatformThread::YieldThread();
    }
}
} // namespace Marlin
