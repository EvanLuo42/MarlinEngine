//! Synchronization objects: a mutex, a read-write lock, an event, a semaphore and a futex-like wait on a word of
//! memory.
//!
//! The four objects synchronize threads that share data; `PlatformSync::WaitOnAddress` waits on a word of memory
//! and needs no object of its own.

#pragma once

#include "Concurrency/ScopeLock.h"
#include "CoreTypes.h"

namespace Marlin
{
/// The timeout, in milliseconds, that waits forever.
inline constexpr uint32 InfiniteTimeout = 0xFFFFFFFFu;

/// A non-recursive mutual-exclusion lock.
///
/// The object is all-zero while unlocked, so a global `Mutex` is constant-initialized and needs no dynamic
/// initialization.
///
/// # Thread safety
/// The thread that locks must be the one that unlocks. A thread that locks a lock it already holds deadlocks.
///
/// # Performance
/// An uncontended lock costs one atomic operation; a contended one may block the thread.
///
/// # Examples
///
/// ```cpp
/// Mutex mutex;
/// int32 counter = 0;
/// {
///     const ScopeLock lock(mutex);
///     ++counter;
/// }
/// ```
class CORE_API Mutex
{
public:
    constexpr Mutex() = default;
    ~Mutex() = default;
    MARLIN_NONCOPYABLE(Mutex);

    /// Blocks until the calling thread holds the lock.
    void Lock();
    /// Takes the lock without blocking; returns false when the lock is already held.
    bool TryLock();
    /// Releases the lock.
    ///
    /// # Preconditions
    /// The calling thread holds the lock. Unlocking a lock that it does not hold is undefined behavior.
    void Unlock();

private:
    void *_state = nullptr;
};

/// A lock held either by many readers at once or by one writer alone.
///
/// `Lock` and `Unlock` take exclusive access for writing; `LockShared` and `UnlockShared` take shared access for
/// reading. The lock is not recursive and a reader cannot upgrade to a writer.
///
/// # Thread safety
/// A thread that holds shared access must not wait for exclusive access; the wait never ends.
class CORE_API ReadWriteLock
{
public:
    constexpr ReadWriteLock() = default;
    ~ReadWriteLock() = default;
    MARLIN_NONCOPYABLE(ReadWriteLock);

    /// Blocks until the calling thread holds exclusive access.
    void Lock();
    /// Takes exclusive access without blocking; returns false while any reader or writer holds the lock.
    bool TryLock();
    /// Releases exclusive access.
    void Unlock();
    /// Blocks until no writer holds the lock and takes shared access.
    void LockShared();
    /// Takes shared access without blocking; returns false while a writer holds the lock.
    bool TryLockShared();
    /// Releases shared access.
    void UnlockShared();

private:
    void *_state = nullptr;
};

/// What an event does with its signal after a waiter is released.
enum class EventReset : uint8
{
    /// Releases one waiter, then resets itself.
    Auto,
    /// Stays set until `Event::Reset`.
    Manual,
};

/// A flag that threads wait on and any thread sets.
///
/// A waiter blocks until the event is set. With `EventReset::Auto` a set releases one waiter; with
/// `EventReset::Manual` the event stays set until `Reset`. A named event is shared by the processes of the user
/// session that use the same name.
///
/// # Errors
/// Construction is fatal when the OS cannot create the event or `name` is not valid UTF-8.
///
/// # Thread safety
/// `Set`, `Reset` and `Wait` may be called from any thread.
class CORE_API Event
{
public:
    /// Creates an event that starts clear.
    ///
    /// `name` is a UTF-8 name that other processes of the user session can open with the same text, or null for an
    /// unnamed event.
    explicit Event(EventReset reset = EventReset::Auto, const char *name = nullptr);
    ~Event();
    MARLIN_NONCOPYABLE(Event);

    /// Sets the event, releasing waiters according to its reset mode.
    void Set();
    /// Clears the event, so later waits block until it is set again.
    void Reset();
    /// Blocks until the event is set, or until `timeout` milliseconds pass.
    ///
    /// Returns false on timeout. An auto-reset event consumes the signal when a wait succeeds.
    bool Wait(uint32 timeout = InfiniteTimeout);

private:
    void *_handle = nullptr;
};

/// A counting semaphore.
///
/// The count starts at `initialCount`; `Wait` takes one and blocks while the count is zero, and `Release` adds more.
///
/// # Preconditions
/// `initialCount` is at most `maximumCount`. Construction is fatal when the OS rejects the counts.
///
/// # Thread safety
/// Any thread may `Wait` or `Release`.
class CORE_API Semaphore
{
public:
    /// Creates a semaphore whose count starts at `initialCount` and may not exceed `maximumCount`.
    Semaphore(uint32 initialCount, uint32 maximumCount);
    ~Semaphore();
    MARLIN_NONCOPYABLE(Semaphore);

    /// Adds `count` to the semaphore, releasing waiting threads.
    ///
    /// # Preconditions
    /// The total does not exceed `maximumCount`; the OS call is checked by an assertion.
    void Release(uint32 count = 1);
    /// Blocks until the count is above zero and takes one permit.
    ///
    /// Returns false when `timeout` milliseconds pass first.
    bool Wait(uint32 timeout = InfiniteTimeout);

private:
    void *_handle = nullptr;
};

/// Waits on a word of memory without a kernel object, like a futex.
///
/// A caller stores a new value and then wakes the address. The wait compares the word atomically when it starts, so
/// a wake that arrives before the wait is not lost.
namespace PlatformSync
{
/// Blocks while the `size` bytes at `address` equal those at `expected`, until a wake or `timeout` milliseconds
/// pass.
///
/// `size` is 1, 2, 4 or 8. The call may return early or spuriously, so callers recheck their condition in a loop.
///
/// # Errors
/// Returns false when the timeout elapses; a failed OS call also returns false.
CORE_API bool WaitOnAddress(const void *address, const void *expected, usize size, uint32 timeout = InfiniteTimeout);
/// Wakes one thread waiting on `address`. Waking an address that no thread waits on has no effect.
CORE_API void WakeOneOnAddress(const void *address);
/// Wakes every thread waiting on `address`. Waking an address that no thread waits on has no effect.
CORE_API void WakeAllOnAddress(const void *address);
} // namespace PlatformSync
} // namespace Marlin
