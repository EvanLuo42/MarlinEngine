#include "Engine/FramePacer.h"

#include "HAL/MicrosoftHeaders.h"
#include "HAL/PlatformThread.h"

#include <timeapi.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Marlin
{
// A high-resolution waitable timer (Windows 10 1803 and later) wakes within a fraction of a millisecond, where Sleep
// rounds up to the system tick, 15.6 ms by default. Without one, a 1 ms timer period makes Sleep accurate enough.
PlatformFrameTimer::PlatformFrameTimer()
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr)
    {
        _handle = reinterpret_cast<uintptr>(timer);
        _slack = 0.001;
        return;
    }
    (void)timeBeginPeriod(1);
    _slack = 0.002;
}

PlatformFrameTimer::~PlatformFrameTimer()
{
    if (_handle != 0)
    {
        CloseHandle(reinterpret_cast<HANDLE>(_handle));
        return;
    }
    (void)timeEndPeriod(1);
}

void PlatformFrameTimer::Sleep(float64 seconds)
{
    if (_handle == 0)
    {
        const auto milliseconds = static_cast<uint32>(seconds * 1000);
        milliseconds > 0 ? PlatformThread::Sleep(milliseconds) : PlatformThread::YieldThread();
        return;
    }
    // A negative due time is relative, in 100 ns units.
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7);
    const auto timer = reinterpret_cast<HANDLE>(_handle);
    if (due.QuadPart == 0 || !SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
    {
        PlatformThread::YieldThread();
        return;
    }
    (void)WaitForSingleObject(timer, INFINITE);
}
} // namespace Marlin
