//! x64 processor primitives: a spin-wait hint and the time-stamp counter.
//!
//! A spin loop calls `Pause` between attempts, and a measurement reads `ReadCycleCounter` before and after the
//! work. Neither function touches memory.

#pragma once

#include "CoreTypes.h"

/// The size of an x64 cache line, in bytes.
///
/// Use it to keep a counter or a lock on a line of its own instead of sharing one with unrelated data.
#define MARLIN_CACHE_LINE_SIZE 64

namespace Marlin::PlatformArch
{
/// Pauses briefly as a hint to the processor inside a spin loop.
///
/// It reduces contention and power on a simultaneous-multithreading core. It does not yield to the OS and gives
/// no timing guarantee.
MARLIN_FORCEINLINE void Pause()
{
    __builtin_ia32_pause();
}

/// Returns the current value of the processor's time-stamp counter.
///
/// The counter ticks at a constant rate on current x64 processors, so the difference of two readings measures
/// elapsed time. The read is not serialized, so surrounding instructions may be reordered around it and very short
/// measurements are approximate.
MARLIN_FORCEINLINE uint64 ReadCycleCounter()
{
    return __builtin_ia32_rdtsc();
}
} // namespace Marlin::PlatformArch
