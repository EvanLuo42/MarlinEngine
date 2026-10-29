#include "Diagnostics/Profiler.h"

namespace Marlin
{
namespace Private
{
const ProfilerHooks *_profilerHooks = nullptr;
} // namespace Private

namespace Profiler
{
void SetHooks(const ProfilerHooks *hooks)
{
    Private::_profilerHooks = hooks;
}

const ProfilerHooks *GetHooks()
{
    return Private::_profilerHooks;
}
} // namespace Profiler
} // namespace Marlin
