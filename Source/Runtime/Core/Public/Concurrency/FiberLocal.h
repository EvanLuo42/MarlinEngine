//! Values that belong to the running job rather than the OS thread.
//!
//! A `FiberLocal` follows a job across fiber switches and thread migrations, and starts zeroed in every job.
//! Outside jobs, the value is per thread.

#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/ExecutionContext.h"
#include "CoreTypes.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
namespace Private
{
/// Allocates a slot for a [FiberLocal] and returns its index.
///
/// Slots are never reused. The function asserts in builds with assertions when
/// [ExecutionContext::MaxFiberLocals] slots are in use.
///
/// # Thread safety
/// Safe to call from any thread.
CORE_API uint32 AllocateFiberLocalSlot();
/// Returns how many fiber-local slots must be saved or cleared to cover every `FiberLocal`.
///
/// The value is one past the last allocated slot, only grows, and is capped at
/// [ExecutionContext::MaxFiberLocals].
///
/// # Thread safety
/// Safe to call from any thread.
CORE_API uint32 GetFiberLocalSlotCount();
} // namespace Private

/// A value local to the running job that follows it across fibers and threads.
///
/// Every job starts with all fiber-local values zeroed, and a value set in one job is invisible to another. Outside
/// jobs, the value is per thread. On a worker, the value survives `Jobs::Wait` because it is stored in the
/// execution context, not in the thread.
///
/// `T` must be trivially copyable and no larger than a pointer. First use allocates a slot that is never released,
/// and the context holds [ExecutionContext::MaxFiberLocals] slots in total. The object is non-copyable and
/// non-movable.
///
/// # Thread safety
/// Safe to use from any thread or job. Each execution context has its own value, and first-use slot allocation is
/// atomic. Values are not shared between jobs.
///
/// # Examples
///
/// ```cpp
/// FiberLocal<int32> scratch;
/// scratch.Set(5);
/// const int32 value = scratch.Get(); // -> 5
/// ```
template <typename T> class FiberLocal
{
    static_assert(sizeof(T) <= sizeof(void *) && IsTriviallyCopyable<T>, "FiberLocal holds pointer-sized values");

public:
    /// Creates the object without allocating a slot; the slot is taken on first use.
    constexpr FiberLocal() = default;
    ~FiberLocal() = default;
    MARLIN_NONCOPYABLE(FiberLocal);

    /// Returns this job's value.
    [[nodiscard]] T Get() const
    {
        const void *raw = GetExecutionContext().FiberLocals[Slot()];
        T value{};
        MemCopy(&value, &raw, sizeof(T));
        return value;
    }

    /// Sets this job's value.
    void Set(T value)
    {
        void *raw = nullptr;
        MemCopy(&raw, &value, sizeof(T));
        GetExecutionContext().FiberLocals[Slot()] = raw;
    }

private:
    /// Taken on first use, so a global FiberLocal needs no static constructor.
    uint32 Slot() const
    {
        uint32 slot = _slot.Load(MemoryOrder::Acquire);
        if (MARLIN_UNLIKELY(slot == 0))
        {
            uint32 expected = 0;
            const uint32 allocated = Private::AllocateFiberLocalSlot();
            slot = _slot.CompareExchange(expected, allocated) ? allocated : expected;
        }
        return slot;
    }

    mutable Atomic<uint32> _slot{0};
};
} // namespace Marlin
