#include "EditorUI/UIAnimation.h"

namespace Marlin
{
namespace UIAnimation
{
namespace
{
/// Made on first use, in an owner of its own that lives as long as the process.
Signal<float64> &Clock()
{
    static Signal<float64> clock;
    if (!clock.IsValid())
    {
        Reactive::RunInOwner(Reactive::CreateOwner(nullptr), [] { clock = CreateSignal<float64>(0); });
    }
    return clock;
}
} // namespace

float64 Now()
{
    return Clock().Get();
}

float64 PeekNow()
{
    return Clock().Peek();
}

void SetTime(float64 seconds)
{
    Clock().Set(seconds);
}
} // namespace UIAnimation
} // namespace Marlin
