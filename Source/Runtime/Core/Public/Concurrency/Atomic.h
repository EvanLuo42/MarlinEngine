//! Atomic types and memory ordering for lock-free code.
//!
//! [Atomic] owns a value and [AtomicRef] wraps an existing object; both map to the compiler's atomic builtins. The
//! memory order passed to each operation controls how other memory accesses may move around it.

#pragma once

#include "CoreTypes.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// The ordering of a memory operation.
///
/// The values are passed to the compiler's atomic builtins and to [AtomicThreadFence] and [AtomicSignalFence].
enum class MemoryOrder : uint8
{
    /// No ordering: the operation is atomic, but other memory accesses may move around it.
    Relaxed = __ATOMIC_RELAXED,
    /// Prevents later memory accesses from moving before the operation.
    Acquire = __ATOMIC_ACQUIRE,
    /// Prevents earlier memory accesses from moving after the operation.
    Release = __ATOMIC_RELEASE,
    /// Combines acquire and release ordering for read-modify-write operations.
    AcquireRelease = __ATOMIC_ACQ_REL,
    /// Acquire and release ordering, plus a single total order shared by all such operations.
    SequentiallyConsistent = __ATOMIC_SEQ_CST,
};

/// Orders memory accesses across threads.
///
/// A relaxed fence has no effect. The other orders constrain accesses around the fence.
inline void AtomicThreadFence(MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
{
    __atomic_thread_fence(static_cast<int32>(order));
}

/// Orders memory against a signal handler or fiber switch on the same thread, without emitting a hardware fence.
///
/// Use [AtomicThreadFence] to order accesses between threads.
inline void AtomicSignalFence(MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
{
    __atomic_signal_fence(static_cast<int32>(order));
}

namespace Private
{
/// Returns the order of the load a failed compare-exchange performs for `order`.
///
/// The failure order is never a release and is no stronger than `order`.
constexpr int32 AtomicFailureOrder(MemoryOrder order) noexcept
{
    switch (order)
    {
    case MemoryOrder::Release:
        return __ATOMIC_RELAXED;
    case MemoryOrder::AcquireRelease:
        return __ATOMIC_ACQUIRE;
    default:
        return static_cast<int32>(order);
    }
}
} // namespace Private

/// A value that can be read and written atomically.
///
/// `T` must be trivially copyable, and the target must implement operations of `sizeof(T)` without a lock. On the
/// supported targets the largest such value is 16 bytes. A default-constructed atomic value-initializes its value;
/// the value constructor initializes it explicitly. Every operation takes a [MemoryOrder] and defaults to
/// `MemoryOrder::SequentiallyConsistent`. Copies and moves are deleted.
///
/// # Thread safety
/// Safe to use from any thread. Accessing the value without an atomic operation while another thread accesses it is
/// a data race.
///
/// # Examples
///
/// ```cpp
/// Atomic<uint32> counter{0};
/// counter.FetchAdd(1);
/// const uint32 previous = counter.Exchange(5); // -> 1
/// ```
template <typename T> class Atomic
{
    static_assert(IsTriviallyCopyable<T>, "Atomic needs a trivially copyable type");
    static_assert(__atomic_always_lock_free(sizeof(T), nullptr), "Atomic needs a lock-free size");

public:
    /// Creates an atomic holding a value-initialized `T`.
    constexpr Atomic() noexcept = default;

    /// Creates an atomic holding `value`.
    constexpr explicit Atomic(T value) noexcept : _value(value)
    {
    }

    ~Atomic() = default;
    Atomic(const Atomic &) = delete;
    Atomic &operator=(const Atomic &) = delete;
    Atomic(Atomic &&) = delete;
    Atomic &operator=(Atomic &&) = delete;

    /// Returns the current value, using `order`.
    [[nodiscard]] T Load(MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
    {
        T result;
        __atomic_load(&_value, &result, static_cast<int32>(order));
        return result;
    }

    /// Replaces the value with `value`, using `order`.
    void Store(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
    {
        __atomic_store(&_value, &value, static_cast<int32>(order));
    }

    /// Replaces the value with `value` and returns the previous value.
    T Exchange(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
    {
        T result;
        __atomic_exchange(&_value, &value, &result, static_cast<int32>(order));
        return result;
    }

    /// Replaces the value with `desired` if it equals `expected`.
    ///
    /// Returns true on success, leaving `expected` unchanged. On failure, writes the current value to `expected`
    /// and returns false.
    bool CompareExchange(T &expected, T desired, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
    {
        return __atomic_compare_exchange(&_value, &expected, &desired, false, static_cast<int32>(order),
                                         Private::AtomicFailureOrder(order));
    }

    /// Like `CompareExchange`, but may fail spuriously.
    ///
    /// Use it in a loop that retries; on failure it writes the current value to `expected` like `CompareExchange`.
    bool CompareExchangeWeak(T &expected, T desired, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
    {
        return __atomic_compare_exchange(&_value, &expected, &desired, true, static_cast<int32>(order),
                                         Private::AtomicFailureOrder(order));
    }

    /// Adds `value` and returns the previous value.
    ///
    /// Only available when `T` is integral.
    T FetchAdd(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsIntegral<T>)
    {
        return __atomic_fetch_add(&_value, value, static_cast<int32>(order));
    }

    /// Subtracts `value` and returns the previous value.
    ///
    /// Only available when `T` is integral.
    T FetchSub(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsIntegral<T>)
    {
        return __atomic_fetch_sub(&_value, value, static_cast<int32>(order));
    }

    /// Applies a bitwise AND with `value` and returns the previous value.
    ///
    /// Only available when `T` is integral.
    T FetchAnd(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsIntegral<T>)
    {
        return __atomic_fetch_and(&_value, value, static_cast<int32>(order));
    }

    /// Applies a bitwise OR with `value` and returns the previous value.
    ///
    /// Only available when `T` is integral.
    T FetchOr(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsIntegral<T>)
    {
        return __atomic_fetch_or(&_value, value, static_cast<int32>(order));
    }

    /// Applies a bitwise XOR with `value` and returns the previous value.
    ///
    /// Only available when `T` is integral.
    T FetchXor(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsIntegral<T>)
    {
        return __atomic_fetch_xor(&_value, value, static_cast<int32>(order));
    }

    /// Advances the pointer by `count` elements and returns the previous pointer.
    ///
    /// A negative `count` moves the pointer backwards. Only available when `T` is a pointer.
    T FetchAdd(isize count, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsPointer<T>)
    {
        return __atomic_fetch_add(&_value, count * static_cast<isize>(sizeof(*_value)), static_cast<int32>(order));
    }

    /// Moves the pointer back by `count` elements and returns the previous pointer.
    ///
    /// A negative `count` moves the pointer forwards. Only available when `T` is a pointer.
    T FetchSub(isize count, MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept
        requires(IsPointer<T>)
    {
        return __atomic_fetch_sub(&_value, count * static_cast<isize>(sizeof(*_value)), static_cast<int32>(order));
    }

private:
    alignas(sizeof(T)) T _value{};
};

/// Atomic access to an object that must stay a plain object, such as a word inside a byte buffer.
///
/// Every concurrent access to the object must go through an `AtomicRef`; mixing atomic and direct accesses is a data
/// race. Copies of the view refer to the same object.
///
/// # Safety
/// The object must be aligned to `sizeof(T)` and must stay alive while any `AtomicRef` refers to it. `T` has the
/// same trivially copyable and lock-free requirements as for [Atomic].
template <typename T> class AtomicRef
{
    static_assert(IsTriviallyCopyable<T>, "AtomicRef needs a trivially copyable type");
    static_assert(__atomic_always_lock_free(sizeof(T), nullptr), "AtomicRef needs a lock-free size");

public:
    /// Creates a view of `object`.
    explicit AtomicRef(T &object) noexcept : _object(&object)
    {
    }

    /// Returns the current value, using `order`.
    [[nodiscard]] T Load(MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
    {
        T result;
        __atomic_load(_object, &result, static_cast<int32>(order));
        return result;
    }

    /// Replaces the value with `value`, using `order`.
    void Store(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
    {
        __atomic_store(_object, &value, static_cast<int32>(order));
    }

    /// Replaces the value with `value` and returns the previous value.
    T Exchange(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
    {
        T result;
        __atomic_exchange(_object, &value, &result, static_cast<int32>(order));
        return result;
    }

    /// Replaces the value with `desired` if it equals `expected`.
    ///
    /// Returns true on success, leaving `expected` unchanged. On failure, writes the current value to `expected`
    /// and returns false.
    bool CompareExchange(T &expected, T desired, MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
    {
        return __atomic_compare_exchange(_object, &expected, &desired, false, static_cast<int32>(order),
                                         Private::AtomicFailureOrder(order));
    }

    /// Like `CompareExchange`, but may fail spuriously.
    ///
    /// Use it in a loop that retries; on failure it writes the current value to `expected` like `CompareExchange`.
    bool CompareExchangeWeak(T &expected, T desired,
                             MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
    {
        return __atomic_compare_exchange(_object, &expected, &desired, true, static_cast<int32>(order),
                                         Private::AtomicFailureOrder(order));
    }

    /// Adds `value` and returns the previous value.
    ///
    /// Only available when `T` is integral.
    T FetchAdd(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) const noexcept
        requires(IsIntegral<T>)
    {
        return __atomic_fetch_add(_object, value, static_cast<int32>(order));
    }

private:
    T *_object;
};
} // namespace Marlin
