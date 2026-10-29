//! CPU and GPU profiling hooks.
//!
//! Profiling is compiled into every configuration but Shipping; the hook interface is the only thing Core knows
//! about a profiler backend.

#pragma once

#include "CoreTypes.h"

/// Whether profiling is compiled in.
///
/// Defined as `!MARLIN_BUILD_SHIPPING`.
#define MARLIN_PROFILING (!MARLIN_BUILD_SHIPPING)

namespace Marlin
{
/// Where a zone is in the source; one static instance per `MARLIN_PROFILE_SCOPE`.
struct ProfileZone
{
    /// The zone name passed to the macro.
    const char *Name;
    /// The enclosing function, from `__func__`.
    const char *Function;
    /// The source file, from `__FILE__`.
    const char *File;
    /// The source line.
    int32 Line;
};

/// The callbacks a profiler backend installs, such as a Tracy client.
///
/// Any callback may be null. Core calls them and knows nothing else about the backend.
struct ProfilerHooks
{
    /// Called when a scope begins.
    void (*BeginZone)(const ProfileZone *zone) = nullptr;
    /// Called when a scope ends.
    void (*EndZone)(const ProfileZone *zone) = nullptr;
    /// Called at the end of a frame; a null `name` is the main frame.
    void (*FrameMark)(const char *name) = nullptr;
    /// Records `value` over time under `name`.
    void (*Plot)(const char *name, float64 value) = nullptr;
    /// Records a message of `length` bytes.
    void (*Message)(const char *text, usize length) = nullptr;
    /// The job system switched to `fiber`, or back to the thread's own stack for null; `name` is stable per fiber.
    void (*FiberSwitch)(const void *fiber, const char *name) = nullptr;
    /// A finished GPU zone on `track`, such as a queue, with times already on the `PlatformTime::Ticks` clock;
    /// `depth` is its nesting. `name` is stable.
    void (*GpuZone)(const char *track, const char *name, uint32 depth, uint64 beginTicks, uint64 endTicks) = nullptr;
};

namespace Profiler
{
/// Installs `hooks`, or removes the backend when `hooks` is null.
///
/// Install before workers start and remove after they stop.
///
/// # Thread safety
/// Not synchronized with the threads that call the hooks.
CORE_API void SetHooks(const ProfilerHooks *hooks);
/// Returns the installed hooks, or null.
CORE_API const ProfilerHooks *GetHooks();
} // namespace Profiler

namespace Private
{
CORE_API extern const ProfilerHooks *_profilerHooks;

/// Calls `BeginZone` on construction and `EndZone` on destruction.
class ProfileScope
{
public:
    explicit ProfileScope(const ProfileZone *zone) : _zone(zone)
    {
        const ProfilerHooks *hooks = _profilerHooks;
        if (hooks != nullptr && hooks->BeginZone != nullptr)
        {
            hooks->BeginZone(zone);
        }
    }

    ~ProfileScope()
    {
        const ProfilerHooks *hooks = _profilerHooks;
        if (hooks != nullptr && hooks->EndZone != nullptr)
        {
            hooks->EndZone(_zone);
        }
    }

    MARLIN_NONCOPYABLE(ProfileScope);

private:
    const ProfileZone *_zone;
};
} // namespace Private
} // namespace Marlin

#if MARLIN_PROFILING
/// Times the enclosing scope as a CPU zone.
///
/// # Examples
///
/// ```cpp
/// void Step()
/// {
///     MARLIN_PROFILE_SCOPE("Physics.Step");
/// }
/// ```
#define MARLIN_PROFILE_SCOPE(name)                                                                                     \
    static constexpr ::Marlin::ProfileZone MARLIN_CONCAT(_profileZone, __LINE__){name, __func__, __FILE__, __LINE__};  \
    const ::Marlin::Private::ProfileScope MARLIN_CONCAT(_profileScope, __LINE__)(&MARLIN_CONCAT(_profileZone, __LINE__))
/// Marks the end of a frame; a null `name` is the main frame.
#define MARLIN_PROFILE_FRAME(name) ::Marlin::Private::ProfileFrame(name)
/// Records `value` over time under `name`.
#define MARLIN_PROFILE_PLOT(name, value) ::Marlin::Private::ProfilePlot(name, static_cast<::Marlin::float64>(value))
#else
#define MARLIN_PROFILE_SCOPE(name) ((void)0)
#define MARLIN_PROFILE_FRAME(name) ((void)0)
#define MARLIN_PROFILE_PLOT(name, value) ((void)0)
#endif

namespace Marlin::Private
{
/// Calls the frame-mark hook, if any.
inline void ProfileFrame(const char *name)
{
    const ProfilerHooks *hooks = _profilerHooks;
    if (hooks != nullptr && hooks->FrameMark != nullptr)
    {
        hooks->FrameMark(name);
    }
}

/// Calls the plot hook, if any.
inline void ProfilePlot(const char *name, float64 value)
{
    const ProfilerHooks *hooks = _profilerHooks;
    if (hooks != nullptr && hooks->Plot != nullptr)
    {
        hooks->Plot(name, value);
    }
}

/// For the RHI's GPU timings.
inline void ProfileGpuZone(const char *track, const char *name, uint32 depth, uint64 beginTicks, uint64 endTicks)
{
    const ProfilerHooks *hooks = _profilerHooks;
    if (hooks != nullptr && hooks->GpuZone != nullptr)
    {
        hooks->GpuZone(track, name, depth, beginTicks, endTicks);
    }
}

/// For the job system's fiber switches.
inline void ProfileFiberSwitch(const void *fiber, const char *name)
{
    const ProfilerHooks *hooks = _profilerHooks;
    if (hooks != nullptr && hooks->FiberSwitch != nullptr)
    {
        hooks->FiberSwitch(fiber, name);
    }
}
} // namespace Marlin::Private
