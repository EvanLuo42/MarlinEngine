//! Values that are local to the OS thread.
//!
//! `ThreadLocal` is declared explicitly because a job can resume on another thread after any wait. Use it for
//! thread-affine caches; use `FiberLocal` for state that must follow a job.

#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "HAL/PlatformThread.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
/// A value per OS thread.
///
/// The first `Get` on a thread constructs a `T` for that thread. The value is never destroyed and lives until the
/// process exits. Use it for thread-affine caches, not for job state; a job can resume on another thread after a
/// wait, so job state belongs in `FiberLocal`.
///
/// `T` must be default constructible. The object is non-copyable and non-movable.
///
/// # Errors
/// The first use allocates a platform TLS slot. Running out of slots is reported by an assertion in builds with
/// assertions; without them the behavior is undefined.
///
/// # Thread safety
/// Different threads see different values, and the first-use TLS slot allocation is atomic. Concurrent access to one
/// thread's `T` follows `T`'s own rules.
///
/// # Examples
///
/// ```cpp
/// ThreadLocal<uint32> scratch;
/// scratch.Get() = 7;
/// const uint32 value = scratch.Get(); // -> 7
/// ```
template <typename T> class ThreadLocal
{
public:
    /// Creates the object without allocating a TLS slot; the slot is taken on first use.
    constexpr ThreadLocal() = default;
    /// Does not destroy any thread's value.
    ~ThreadLocal() = default;
    MARLIN_NONCOPYABLE(ThreadLocal);

    /// Returns this thread's value, constructing it on first use.
    ///
    /// The first use on any thread allocates a platform TLS slot.
    [[nodiscard]] T &Get()
    {
        const uint32 slot = Slot();
        void *value = PlatformTls::Get(slot);
        if (MARLIN_UNLIKELY(value == nullptr))
        {
            value = ConstructAt(static_cast<T *>(Memory::Allocate(sizeof(T), alignof(T))));
            PlatformTls::Set(slot, value);
        }
        return *static_cast<T *>(value);
    }

    /// Returns this thread's value, or null if this thread has not called `Get`.
    ///
    /// It does not construct a value, but the first call still allocates the object's TLS slot.
    [[nodiscard]] T *TryGet()
    {
        return static_cast<T *>(PlatformTls::Get(Slot()));
    }

private:
    /// One past the PlatformTls slot, so zero means none yet.
    uint32 Slot()
    {
        uint32 slot = _slot.Load(MemoryOrder::Acquire);
        if (MARLIN_UNLIKELY(slot == 0))
        {
            const uint32 allocated = PlatformTls::Allocate();
            MARLIN_VERIFY(allocated != PlatformTls::InvalidSlot);
            uint32 expected = 0;
            if (_slot.CompareExchange(expected, allocated + 1))
            {
                slot = allocated + 1;
            }
            else
            {
                PlatformTls::Free(allocated);
                slot = expected;
            }
        }
        return slot - 1;
    }

    Atomic<uint32> _slot{0};
};
} // namespace Marlin
