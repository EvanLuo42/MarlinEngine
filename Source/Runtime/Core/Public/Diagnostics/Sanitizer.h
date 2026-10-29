//! AddressSanitizer annotations for memory and fiber stacks that the sanitizer cannot see on its own.
//!
//! Every function here does nothing in builds without AddressSanitizer.

#pragma once

#include "CoreTypes.h"

#if MARLIN_ASAN
extern "C"
{
    void __asan_poison_memory_region(const volatile void *address, Marlin::usize size);
    void __asan_unpoison_memory_region(const volatile void *address, Marlin::usize size);
    void __sanitizer_start_switch_fiber(void **fakeStackSave, const void *stackBottom, Marlin::usize stackSize);
    void __sanitizer_finish_switch_fiber(void *fakeStackSave, const void **stackBottomOld, Marlin::usize *stackSizeOld);
}
#endif

/// Annotations for AddressSanitizer.
///
/// # Thread safety
/// Poisoning and unpoisoning are safe from any thread, but ranges that may be touched concurrently must not share a
/// shadow byte.
namespace Marlin::Sanitizer
{
/// Bytes per AddressSanitizer shadow byte; ranges that threads poison or unpoison concurrently must not share one.
inline constexpr usize Granularity = 8;

/// Makes any access to the range an error.
///
/// Use it for memory an allocator holds but has not handed out.
MARLIN_FORCEINLINE void PoisonMemory([[maybe_unused]] const void *address, [[maybe_unused]] usize size)
{
#if MARLIN_ASAN
    __asan_poison_memory_region(address, size);
#endif
}

/// Makes the range accessible again.
MARLIN_FORCEINLINE void UnpoisonMemory([[maybe_unused]] const void *address, [[maybe_unused]] usize size)
{
#if MARLIN_ASAN
    __asan_unpoison_memory_region(address, size);
#endif
}

/// Call right before switching to the stack `[bottom, bottom + size)`.
///
/// `fakeStack` is then passed to `FinishSwitchFiber` once this stack runs again.
MARLIN_FORCEINLINE void StartSwitchFiber([[maybe_unused]] void **fakeStack, [[maybe_unused]] const void *bottom,
                                         [[maybe_unused]] usize size)
{
#if MARLIN_ASAN
    __sanitizer_start_switch_fiber(fakeStack, bottom, size);
#endif
}

/// Call first thing on the stack switched to.
///
/// Pass what its own `StartSwitchFiber` saved, or null on its first run.
MARLIN_FORCEINLINE void FinishSwitchFiber([[maybe_unused]] void *fakeStack)
{
#if MARLIN_ASAN
    __sanitizer_finish_switch_fiber(fakeStack, nullptr, nullptr);
#endif
}
} // namespace Marlin::Sanitizer
