//! A non-recursive lock that spins in user space for short critical sections.
//!
//! A waiter pauses and then yields the thread instead of sleeping in the OS. The lock is all-zero when unlocked,
//! so a global `SpinLock` can be constant-initialized.

#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "CoreTypes.h"
#include "HAL/PlatformArch.h"
#include "HAL/PlatformThread.h"

namespace Marlin
{
/// A non-recursive lock that spins in user space instead of sleeping in the OS.
///
/// Use it for critical sections shorter than a context switch. It is all-zero when unlocked, so a global `SpinLock`
/// can be constant-initialized. The lock records no owner, so double unlock or unlocking a lock the caller does not
/// hold is undefined. The lock is non-copyable and non-movable.
///
/// # Thread safety
/// Safe to use from any thread; it serializes access to the data it guards by convention.
///
/// # Performance
/// The spin loop pauses for its first 64 iterations and then yields the thread on every later iteration. Holding
/// the lock across a blocking call wastes a core.
class SpinLock
{
public:
    constexpr SpinLock() = default;
    ~SpinLock() = default;
    MARLIN_NONCOPYABLE(SpinLock);

    /// Acquires the lock, spinning until it is free.
    void Lock() noexcept
    {
        uint32 spins = 0;
        while (_locked.Exchange(true, MemoryOrder::Acquire))
        {
            while (_locked.Load(MemoryOrder::Relaxed))
            {
                if (++spins < 64)
                {
                    PlatformArch::Pause();
                }
                else
                {
                    PlatformThread::YieldThread();
                }
            }
        }
    }

    /// Tries to acquire the lock without waiting.
    ///
    /// Returns true if this call acquired the lock, false if another thread holds it.
    [[nodiscard]] bool TryLock() noexcept
    {
        return !_locked.Load(MemoryOrder::Relaxed) && !_locked.Exchange(true, MemoryOrder::Acquire);
    }

    /// Releases the lock.
    ///
    /// # Preconditions
    /// The caller holds the lock. Unlocking it twice or without holding it is undefined.
    void Unlock() noexcept
    {
        _locked.Store(false, MemoryOrder::Release);
    }

private:
    Atomic<bool> _locked{false};
};
} // namespace Marlin
