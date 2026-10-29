#include "Engine/FramePacer.h"

#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"

namespace Marlin
{
void FramePacer::Wait()
{
    if (_interval <= 0)
    {
        return;
    }
    _deadline = NextDeadline(_deadline, _interval, PlatformTime::Seconds());
    for (;;)
    {
        const float64 remaining = _deadline - PlatformTime::Seconds();
        if (remaining <= 0)
        {
            return;
        }
        if (remaining > _timer.GetSlack())
        {
            _timer.Sleep(remaining - _timer.GetSlack());
        }
        else
        {
            PlatformThread::YieldThread();
        }
    }
}
} // namespace Marlin
