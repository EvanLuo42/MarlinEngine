#include "CrashReport/CrashCommands.h"

#include "Diagnostics/Assert.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformThread.h"

namespace Marlin
{
namespace
{
uintptr _badAddress = 0;

/// Uses a frame per call that the optimizer cannot remove, until the stack runs out.
MARLIN_NOINLINE uint64 Recurse(uint64 depth)
{
    volatile uint8 frame[4096];
    frame[depth % sizeof(frame)] = static_cast<uint8>(depth);
    if (depth == ~uint64{0})
    {
        return 0;
    }
    return Recurse(depth + 1) + frame[depth % sizeof(frame)];
}
} // namespace

void CrashCommands::Crash(const String &kind)
{
    if (MARLIN_BUILD_SHIPPING)
    {
        return;
    }
    const StringView name = kind.AsView();
    if (name != "AccessViolation" && name != "Assert" && name != "Fatal" && name != "StackOverflow" && name != "Hang")
    {
        MARLIN_LOG(LogCore, Error, "Debug.Crash takes AccessViolation, Assert, Fatal, StackOverflow or Hang");
        return;
    }
    MARLIN_LOG(LogCore, Warning, "Debug.Crash {0}", kind);
    if (name == "AccessViolation")
    {
        *reinterpret_cast<volatile uint32 *>(_badAddress) = 0;
    }
    else if (name == "Assert")
    {
        MARLIN_ASSERT(kind.IsEmpty());
    }
    else if (name == "Fatal")
    {
        MARLIN_FATAL("Debug.Crash Fatal");
    }
    else if (name == "StackOverflow")
    {
        (void)Recurse(0);
    }
    else
    {
        // The calling thread never returns, so the heartbeat stops while the process keeps running.
        while (true)
        {
            PlatformThread::Sleep(1000);
        }
    }
}
} // namespace Marlin
