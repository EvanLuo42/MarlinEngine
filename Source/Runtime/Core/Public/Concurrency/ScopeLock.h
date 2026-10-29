//! RAII guards that hold a lock for the lifetime of a scope.
//!
//! Construction acquires the lock and destruction releases it, including when the scope exits early.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// A guard that acquires a lock on construction and releases it on destruction.
///
/// `TLock` must provide `Lock()` and `Unlock()`. The guard is non-copyable and non-movable, and the lock must
/// outlive it.
///
/// # Thread safety
/// The guard belongs to one thread. The underlying lock may be shared with other threads.
///
/// # Examples
///
/// ```cpp
/// SpinLock lock;
/// {
///     const ScopeLock guard(lock);
///     // The lock is held here.
/// }
/// // The lock has been released.
/// ```
template <typename TLock> class ScopeLock
{
public:
    /// Acquires `lock` by calling `Lock()` on it.
    explicit ScopeLock(TLock &lock) : _lock(lock)
    {
        _lock.Lock();
    }

    /// Releases the lock by calling `Unlock()` on it.
    ~ScopeLock()
    {
        _lock.Unlock();
    }

    MARLIN_NONCOPYABLE(ScopeLock);

private:
    TLock &_lock;
};

/// A guard that acquires the shared side of a lock on construction and releases it on destruction.
///
/// `TLock` must provide `LockShared()` and `UnlockShared()`, such as a read-write lock. The guard is
/// non-copyable and non-movable, and the lock must outlive it.
///
/// # Thread safety
/// The guard belongs to one thread. The underlying lock may be shared with other threads.
template <typename TLock> class SharedScopeLock
{
public:
    /// Acquires the shared side of `lock` by calling `LockShared()` on it.
    explicit SharedScopeLock(TLock &lock) : _lock(lock)
    {
        _lock.LockShared();
    }

    /// Releases the shared side of the lock by calling `UnlockShared()` on it.
    ~SharedScopeLock()
    {
        _lock.UnlockShared();
    }

    MARLIN_NONCOPYABLE(SharedScopeLock);

private:
    TLock &_lock;
};
} // namespace Marlin
