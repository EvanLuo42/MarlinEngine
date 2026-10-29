//! Clocks: a monotonic high-resolution counter, the wall clock in UTC, and the local offset from it.
//!
//! The monotonic clock has an arbitrary epoch and never runs backwards, so it measures durations. UTC values are
//! DateTime ticks: 100 ns intervals since 0001-01-01.

#pragma once

#include "CoreTypes.h"

namespace Marlin::PlatformTime
{
/// Returns the monotonic clock's current value, in ticks.
///
/// The epoch is arbitrary, so only differences between two readings are meaningful. The counter is high
/// resolution and does not jump when the wall clock is adjusted.
CORE_API uint64 Ticks();
/// Returns the frequency of `Ticks`, in ticks per second.
///
/// The value is constant for the life of the process.
///
/// # Examples
///
/// ```cpp
/// const uint64 start = PlatformTime::Ticks();
/// // ...work...
/// const float64 seconds = static_cast<float64>(PlatformTime::Ticks() - start) /
///                         static_cast<float64>(PlatformTime::TicksPerSecond());
/// ```
CORE_API uint64 TicksPerSecond();
/// Returns the monotonic clock's value in seconds.
///
/// The epoch is arbitrary; use the difference of two readings. Precision falls as the value grows, so use `Ticks`
/// when exact integer arithmetic matters.
CORE_API float64 Seconds();

/// Returns the current UTC time in DateTime ticks: 100 ns intervals since 0001-01-01.
CORE_API int64 UtcNow();
/// Returns the difference between local time and UTC, in DateTime ticks.
///
/// The value is negative west of Greenwich and includes the daylight-saving adjustment that is in effect.
CORE_API int64 LocalUtcOffset();
} // namespace Marlin::PlatformTime
