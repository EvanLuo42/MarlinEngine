#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// A timer the main loop sleeps on between frames: finer than PlatformThread::Sleep where the platform has one. The
/// generic one in PlatformFrameTimer.cpp is PlatformThread::Sleep; a platform layer replaces that file.
class ENGINE_API PlatformFrameTimer
{
public:
    PlatformFrameTimer();
    ~PlatformFrameTimer();
    MARLIN_NONCOPYABLE(PlatformFrameTimer);

    /// Sleeps about *seconds*: less, or later by up to GetSlack.
    void Sleep(float64 seconds);

    /// How late Sleep may wake; FramePacer stops sleeping this long before a deadline and yields the rest.
    [[nodiscard]] float64 GetSlack() const
    {
        return _slack;
    }

private:
    uintptr _handle = 0;
    float64 _slack = 0.002;
};

/// Holds the main loop to `Engine.Loop.MaxFrameRate` (Documentation/Architecture/01 §4). Frames end on a grid of
/// absolute deadlines, so time one frame oversleeps comes off the next instead of adding up; the pacer sleeps on a
/// PlatformFrameTimer until the timer's slack before a deadline and yields the thread for the rest.
class ENGINE_API FramePacer
{
public:
    /// Zero turns pacing off.
    void SetInterval(float64 seconds)
    {
        _interval = seconds > 0 ? seconds : 0;
    }

    [[nodiscard]] float64 GetInterval() const
    {
        return _interval;
    }

    /// Starts the grid at *now*, the start of the first frame.
    void Reset(float64 now)
    {
        _deadline = now;
    }

    /// Ends a frame: waits for the next deadline, or returns at once if it has passed.
    void Wait();

    /// The deadline after *deadline* for a frame that ended at *now*: one interval on, or *now* itself if the loop
    /// fell more than a whole interval behind, so a hitch is not followed by a burst of unpaced frames.
    [[nodiscard]] static float64 NextDeadline(float64 deadline, float64 interval, float64 now)
    {
        const float64 next = deadline + interval;
        return now - next > interval ? now : next;
    }

private:
    PlatformFrameTimer _timer;
    float64 _interval = 0;
    float64 _deadline = 0;
};
} // namespace Marlin
