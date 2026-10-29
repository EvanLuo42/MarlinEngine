//! Easing curves for UI animation.
//!
//! Each curve maps linear progress in [0, 1] to eased progress. They are plain function pointers, so an animation can
//! name a curve in code or evaluate one stored in an asset.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// A pointer to a curve that maps linear progress in [0, 1] to eased progress.
///
/// A curve returns 0 at 0 and 1 at 1, and may overshoot in between. The editor UI stores one in `UITransition`; game
/// UI is to keep curves as assets that evaluate with these functions.
using EasingFunction = float32 (*)(float32 t);

/// Easing curves that map progress in [0, 1] to eased progress.
///
/// The functions do not clamp `t`; values outside [0, 1] are extrapolated.
namespace Easing
{
/// Returns `t` unchanged.
constexpr float32 Linear(float32 t)
{
    return t;
}

/// Starts quickly and decelerates into the end.
///
/// Most UI motion uses this curve. It returns `1 - (1 - t)^3`.
///
/// # Examples
///
/// ```cpp
/// const float32 progress = Easing::EaseOutCubic(0.5f); // -> 0.875
/// ```
constexpr float32 EaseOutCubic(float32 t)
{
    const float32 u = 1 - t;
    return 1 - u * u * u;
}

/// Starts slowly and accelerates.
///
/// It returns `t^3`.
constexpr float32 EaseInCubic(float32 t)
{
    return t * t * t;
}

/// Accelerates out of the start and decelerates into the end.
///
/// The curve is symmetric about `(0.5, 0.5)`.
constexpr float32 EaseInOutCubic(float32 t)
{
    if (t < 0.5f)
    {
        return 4 * t * t * t;
    }
    const float32 u = -2 * t + 2;
    return 1 - u * u * u * 0.5f;
}

/// Overshoots past 1 before settling at 1.
///
/// The overshoot makes the curve suitable for elements that pop open.
constexpr float32 EaseOutBack(float32 t)
{
    constexpr float32 c1 = 1.70158f;
    constexpr float32 c3 = c1 + 1;
    const float32 u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}
} // namespace Easing
} // namespace Marlin
